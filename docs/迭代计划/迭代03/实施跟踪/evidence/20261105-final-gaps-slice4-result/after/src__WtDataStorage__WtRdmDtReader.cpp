#include "WtRdmDtReader.h"
#include "RTBlockPublish.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/DateMappingClient.hpp"

#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSUtils/WTSCmpBudgetConfig.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <rapidjson/document.h>
namespace rj = rapidjson;

// 卡11=B：历史块缓存槽位，按需创建共享 owner
template <typename Map>
static typename Map::mapped_type::element_type& his_slot(Map& m, const std::string& key)
{
	typename Map::mapped_type& ptr = m[key];
	if (!ptr)
		ptr = std::make_shared<typename Map::mapped_type::element_type>();
	return *ptr;
}

template <typename Map>
static wtp::WTSSliceOwnerPtr his_owner(Map& m, const std::string& key)
{
	auto it = m.find(key);
	if (it == m.end())
		return wtp::WTSSliceOwnerPtr();
	return it->second;
}

// Compare HFT records by their action timestamp.
template <typename T>
static bool hft_time_less(const T& lhs, const T& rhs)
{
	if (lhs.action_date != rhs.action_date)
		return lhs.action_date < rhs.action_date;
	return lhs.action_time < rhs.action_time;
}

// R132-001: a history block keeps its 12-byte packed header in front of the
// records, so records left in `_buffer` sit at a 4-byte offset and must not be
// accessed as T (8-byte fields). Move any pending payload once into the pair's
// naturally aligned vector, keep only the header in `_buffer`, and serve every
// read and slice from `_items` (slices own the pair, card 11). A trailing
// partial record is ignored exactly as the former count arithmetic did.
template <typename T, typename Pair>
static bool align_his_items(Pair& pair, std::size_t headerSize)
{
	static_assert(std::is_trivially_copyable<T>::value, "history records must be byte-copyable");
	if (pair._buffer.size() <= headerSize)
		return true;
	const std::size_t count = (pair._buffer.size() - headerSize) / sizeof(T);
	try
	{
		std::vector<T> items(count);
		if (count != 0)
			std::memcpy(items.data(), pair._buffer.data() + headerSize, count * sizeof(T));
		pair._items.swap(items);
		pair._buffer.resize(headerSize);
		pair._buffer.shrink_to_fit();
	}
	catch (const std::exception&)
	{
		return false;
	}
	pair._block = reinterpret_cast<decltype(pair._block)>(&pair._buffer[0]);
	return true;
}

template <typename T>
static bool find_rt_hft_end_index(T* items, std::size_t count, const T& end, std::size_t& endIndex)
{
	if (items == NULL || count == 0)
		return false;

	// `count` is the caller-provided range. upper_bound includes duplicates at the
	// requested endpoint; return false rather than stepping before begin().
	T* it = std::upper_bound(items, items + count, end, [](const T& value, const T& item) {
		return hft_time_less(value, item);
	});
	if (it == items)
		return false;
	--it;

	endIndex = (std::size_t)(it - items);
	return true;
}

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_rdmreader_log(IRdmDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	// I03-NEW-038: messages can carry caller-supplied codes and configured
	// paths; bound the formatter writes to the per-thread buffer.
	static thread_local char buffer[512] = { 0 };
	try
	{
		const std::size_t max_output_size = sizeof(buffer) - 1;
		const auto formatted = fmt::format_to_n(buffer, max_output_size, format, args...);
		if (formatted.size > max_output_size)
		{
			constexpr char truncation_marker[] = "...[truncated]";
			constexpr std::size_t marker_size = sizeof(truncation_marker) - 1;
			std::memcpy(buffer + max_output_size - marker_size, truncation_marker, marker_size);
			buffer[max_output_size] = '\0';
		}
		else
		{
			buffer[formatted.size] = '\0';
		}
	}
	catch (...)
	{
		constexpr char fallback[] = "WtRdmDtReader log formatting failed";
		std::memcpy(buffer, fallback, sizeof(fallback));
	}

	sink->reader_log(ll, buffer);
}

// Decision card 1 (A+C): trading dates come from the typed client over the
// guarded legacy IBaseDataMgr virtuals. A failure is logged and the caller
// stops before creating a slice or touching a cache.
inline bool rdm_trading_date(IRdmDtReaderSink* sink, wtp::DateMappingClient& mapping, const char* stdPID,
	uint32_t uDate, uint32_t uTime, uint32_t& tradingDate)
{
	const wtp::DateMappingResult32 result = mapping.calcTradingDate(stdPID, uDate, uTime, false);
	if (result.status != wtp::DateMappingStatus::Ok)
	{
		pipe_rdmreader_log(sink, LL_ERROR, "Mapping trading date of {} at {}.{} failed: {}",
			stdPID == NULL ? "" : stdPID, uDate, uTime, wtp::dateMappingStatusName(result.status));
		return false;
	}
	tradingDate = result.value;
	return true;
}

#if defined(WT_RDM_DT_READER_TEST_HOOKS)
namespace
{
thread_local wtp::RdmDtReaderBufferRebuildHook g_rdm_dt_reader_buffer_rebuild_hook = nullptr;
thread_local uint16_t g_rdm_dt_reader_transform_failure_type = 0;
thread_local wtp::RdmDtReaderTransformFailureStage g_rdm_dt_reader_transform_failure_stage =
	wtp::RdmDtReaderTransformFailureStage::None;
}

void wtp::setRdmDtReaderBufferRebuildHook(wtp::RdmDtReaderBufferRebuildHook hook)
{
	g_rdm_dt_reader_buffer_rebuild_hook = hook;
}

void wtp::setRdmDtReaderTransformFailure(uint16_t blockType,
	wtp::RdmDtReaderTransformFailureStage stage)
{
	g_rdm_dt_reader_transform_failure_type = blockType;
	g_rdm_dt_reader_transform_failure_stage = stage;
}

static void maybe_fail_rdm_dt_reader_transform(uint16_t blockType,
	wtp::RdmDtReaderTransformFailureStage stage)
{
	if (g_rdm_dt_reader_transform_failure_type == blockType
		&& g_rdm_dt_reader_transform_failure_stage == stage)
	{
		g_rdm_dt_reader_transform_failure_type = 0;
		g_rdm_dt_reader_transform_failure_stage = wtp::RdmDtReaderTransformFailureStage::None;
		throw std::runtime_error("injected RDM block transform failure");
	}
}

static void notify_rdm_dt_reader_buffer_rebuilt(uint16_t blockType,
	uintptr_t oldDataAddress, uintptr_t newDataAddress,
	std::size_t oldCapacity, std::size_t newCapacity, std::size_t rebuiltSize,
	uint16_t rebuiltVersion)
{
	if (g_rdm_dt_reader_buffer_rebuild_hook != nullptr)
		g_rdm_dt_reader_buffer_rebuild_hook(blockType, oldDataAddress, newDataAddress,
			oldCapacity, newCapacity, rebuiltSize, rebuiltVersion);
}
#endif

extern "C"
{
	EXPORT_FLAG IRdmDtReader* createRdmDtReader()
	{
		IRdmDtReader* ret = new WtRdmDtReader();
		return ret;
	}

	EXPORT_FLAG void deleteRdmDtReader(IRdmDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}

	// K31=A: optional DateMappingV1 injection (see Includes/DateMappingBinding.h).
	EXPORT_FLAG int bindRdmDtReaderDateMappingV1(IRdmDtReader* reader, const wtp::DateMappingV1* table)
	{
		if (reader == NULL)
			return 0;
		return static_cast<WtRdmDtReader*>(reader)->bindDateMapping(table) ? 1 : 0;
	}
};

/*
 *	处理块数据
 */
extern bool proc_block_data(std::string& content, bool isBar, bool bKeepHead = true);

static bool proc_his_kline_block(std::string& content, bool allowEmpty = false)
{
	if (content.size() < sizeof(HisKlineBlock))
		return false;

	const BlockHeader* header = (const BlockHeader*)content.data();
	if (header->is_compressed() && content.size() < sizeof(HisKlineBlockV2))
		return false;

	try
	{
		if (header->is_old_version())
		{
			std::string oldPayload;
			if (header->is_compressed())
			{
				const HisKlineBlockV2* compressed = (const HisKlineBlockV2*)content.data();
				if (compressed->_size != content.size() - sizeof(HisKlineBlockV2))
					return false;
				oldPayload = WTSCmpHelper::uncompress_data(compressed->_data, (std::size_t)compressed->_size);
			}
			else
			{
				oldPayload.assign(content.data() + sizeof(HisKlineBlock), content.size() - sizeof(HisKlineBlock));
			}

			if (oldPayload.size() % sizeof(WTSBarStructOld) != 0)
				return false;
		}

		if (!proc_block_data(content, true, false))
			return false;
	}
	catch (const std::exception&)
	{
		return false;
	}

	return (allowEmpty && content.empty()) ||
		(!content.empty() && content.size() % sizeof(WTSBarStruct) == 0);
}

WtRdmDtReader::WtRdmDtReader()
	: _base_data_mgr(NULL)
	, _hot_mgr(NULL)
	, _stopped(false)
{
}


WtRdmDtReader::~WtRdmDtReader()
{
	{
		std::lock_guard<std::mutex> lock(_stop_mtx);
		_stopped = true;
	}
	_stop_cv.notify_all();
	if (_thrd_check)
		_thrd_check->join();
}

void WtRdmDtReader::init(WTSVariant* cfg, IRdmDtReaderSink* sink)
{
	_sink = sink;

	_base_data_mgr = _sink->get_basedata_mgr();
	_hot_mgr = _sink->get_hot_mgr();

	if (cfg == NULL)
		return ;

	// BUG-R132-001 (K37=B): this module's decompression budget comes from its own store
	// config (keys next to path); an invalid value fails closed for compressed blocks.
	std::string budgetError;
	if (wt_cmp_budget::apply(cfg, budgetError) == wt_cmp_budget::ApplyResult::Invalid)
		pipe_rdmreader_log(_sink, LL_ERROR, "{}; decompression in this storage module is refused", budgetError);

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	bool bAdjLoaded = false;
	
	if (!bAdjLoaded && cfg->has("adjfactor"))
		loadStkAdjFactorsFromFile(cfg->getCString("adjfactor"));

	_thrd_check.reset(new StdThread([this]() {
		while(!_stopped)
		{
			{
				std::unique_lock<std::mutex> stopLock(_stop_mtx);
				if (_stop_cv.wait_for(stopLock, std::chrono::seconds(5), [this]() { return _stopped.load(); }))
					break;
			}
			uint64_t now = TimeUtils::getLocalTimeNow();
			std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);

			for(auto& m : _rt_tick_map)
			{
				//如果5分钟之内没有访问，则释放掉
				TickBlockPair& tPair = (TickBlockPair&)m.second;
				if(now > tPair._last_time + 300000 && tPair._block != NULL)
				{	
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}

			for (auto& m : _rt_ordque_map)
			{
				//如果5分钟之内没有访问，则释放掉
				OrdQueBlockPair& tPair = (OrdQueBlockPair&)m.second;
				if (now > tPair._last_time + 300000 && tPair._block != NULL)
				{
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}

			for (auto& m : _rt_orddtl_map)
			{
				//如果5分钟之内没有访问，则释放掉
				OrdDtlBlockPair& tPair = (OrdDtlBlockPair&)m.second;
				if (now > tPair._last_time + 300000 && tPair._block != NULL)
				{
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}

			for (auto& m : _rt_trans_map)
			{
				//如果5分钟之内没有访问，则释放掉
				TransBlockPair& tPair = (TransBlockPair&)m.second;
				if (now > tPair._last_time + 300000 && tPair._block != NULL)
				{
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}

			for (auto& m : _rt_min1_map)
			{
				//如果5分钟之内没有访问，则释放掉
				RTKlineBlockPair& tPair = (RTKlineBlockPair&)m.second;
				if (now > tPair._last_time + 300000 && tPair._block != NULL)
				{
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}

			for (auto& m : _rt_min5_map)
			{
				//如果5分钟之内没有访问，则释放掉
				RTKlineBlockPair& tPair = (RTKlineBlockPair&)m.second;
				if (now > tPair._last_time + 300000 && tPair._block != NULL)
				{
					StdUniqueLock lock(*tPair._mtx);
					tPair._block = NULL;
					tPair._file.reset();
				}
			}
		}
	}));
}


bool WtRdmDtReader::loadStkAdjFactorsFromFile(const char* adjfile)
{
	if (!StdFile::exists(adjfile))
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Adjusting factors file {} not exists", adjfile);
		return false;
	}

	WTSVariant* doc = WTSCfgLoader::load_from_file(adjfile);
	if (doc == NULL)
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Loading adjusting factors file {} failed", adjfile);
		return false;
	}

	uint32_t stk_cnt = 0;
	uint32_t fct_cnt = 0;
	for (const std::string& exchg : doc->memberNames())
	{
		WTSVariant* itemExchg = doc->get(exchg);
		for (const std::string& code : itemExchg->memberNames())
		{
			WTSVariant* ayFacts = itemExchg->get(code);
			if (!ayFacts->isArray())
				continue;

			/*
			 *	By Wesley @ 2021.12.21
			 *	先检查code的格式是不是包含PID，如STK.600000
			 *	如果包含PID，则直接格式化，如果不包含，则强制为STK
			 */
			bool bHasPID = (code.find('.') != std::string::npos);

			std::string key;
			if (bHasPID)
				key = fmt::format("{}.{}", exchg, code);
			else
				key = fmt::format("{}.STK.{}", exchg, code);

			stk_cnt++;

			AdjFactorList& fctrLst = _adj_factors[key];
			for (uint32_t i = 0; i < ayFacts->size(); i++)
			{
				WTSVariant* fItem = ayFacts->get(i);
				AdjFactor adjFact;
				adjFact._date = fItem->getUInt32("date");
				adjFact._factor = fItem->getDouble("factor");

				fctrLst.emplace_back(adjFact);
				fct_cnt++;
			}

			//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
			AdjFactor adjFact;
			adjFact._date = 19900101;
			adjFact._factor = 1;
			fctrLst.emplace_back(adjFact);

			std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
				return left._date < right._date;
			});
		}
	}

	pipe_rdmreader_log(_sink, LL_INFO, "{} adjusting factors of {} tickers loaded", fct_cnt, stk_cnt);
	doc->release();
	return true;
}

WTSTickSlice* WtRdmDtReader::readTickSliceByDate(const char* stdCode, uint32_t uDate )
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;
	bool isToday = (uDate == curTDate);

	//这里改成小于等于，主要针对盘后读取的情况
	//如果已经做了收盘作业，实时数据就读不到了
	if (uDate <= curTDate)
	{
		std::string curCode = cInfo._code;
		std::string hotCode;
		if (commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if(strlen(ruleTag) > 0)
			{
				curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, uDate);
				pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, uDate, stdCode, curCode.c_str());
				hotCode = cInfo._product;
				hotCode += "_";
				hotCode += ruleTag;
			}
		}

		std::string key = fmt::format("{}-{}", stdCode, uDate);

		auto it = _his_tick_map.find(key);
		bool bHasHisTick = (it != _his_tick_map.end());
		if (!bHasHisTick)
		{
			for (;;)
			{
				std::string filename;
				bool bHitHot = false;
				if (!hotCode.empty())
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << uDate << "/" << hotCode << ".dsb";
					filename = ss.str();
					if (StdFile::exists(filename.c_str()))
					{
						bHitHot = true;
					}
				}

				if (!bHitHot)
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << uDate << "/" << curCode << ".dsb";
					filename = ss.str();
					if (!StdFile::exists(filename.c_str()))
					{
						break;
					}
				}

				HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					return NULL;
				}

				if (!proc_block_data(tBlkPair._buffer, false, true))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Processing his tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					return NULL;
				}
				tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
				bHasHisTick = true;
				break;
			}
		}

		while (bHasHisTick)
		{
			HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
			if (tBlkPair._block == NULL)
				break;

			// R132-001: records are served from the pair's aligned copy, never from
			// the packed history buffer (12-byte header leaves them misaligned).
			if (!align_his_items<WTSTickStruct>(tBlkPair, sizeof(HisTickBlock)))
				break;
			WTSTickStruct* const hisRecs = tBlkPair._items.data();

			uint32_t tcnt = (uint32_t)tBlkPair._items.size();
			if (tcnt <= 0)
				break;

			WTSTickSlice* slice = WTSTickSlice::create(stdCode, hisRecs, tcnt);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_tick_map, key));
			return slice;

			break;
		}
	}
	
	while(isToday)
	{
		std::string curCode = cInfo._code;
		if(commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if (strlen(ruleTag) > 0)
				curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), curTDate);
			//else if (cInfo.isHot())
			//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, curTDate);
			//else if (cInfo.isSecond())
			//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, curTDate);
		}
		

		TickBlockPair* tPair = getRTTickBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL || tPair->_block->_size == 0)
			break;

		StdUniqueLock lock(*tPair->_mtx);
		RTTickBlock* tBlock = tPair->_block;
		// Decision K36=A: published records inside this mapping (card 13 visibleRecords).
		const uint32_t visible = wt_rtpub::visibleRecords(tBlock, tPair->_file->size(), sizeof(RTTickBlock), sizeof(WTSTickStruct));
		if (visible == 0)
			break;
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, visible);
		if (slice != NULL)
			slice->addOwner(tPair->_file);
		return slice;
	}

	return NULL;
}

WTSTickSlice* WtRdmDtReader::readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	if (etime != 0 && stime > etime)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	pipe_rdmreader_log(_sink, LL_DEBUG, "Reading ticks of {} between {} and {}", stdCode, stime, etime);

	WTSSessionInfo* sInfo = commInfo->getSessionInfo();

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	uint32_t lDate, lTime, lSecs;
	//20190807124533900
	lDate = (uint32_t)(stime / 1000000000);
	lTime = (uint32_t)(stime % 1000000000) / 100000;
	lSecs = (uint32_t)(stime % 100000);

	// One client: etime=0 and the current date share one clock snapshot.
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, lDate, lTime, beginTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	bool hasToday = (endTDate >= curTDate);

	WTSTickSlice* slice = WTSTickSlice::create(stdCode, NULL, 0);
	if (slice == NULL)	// I03-NEW-083: rejected (e.g. overlong) code
		return NULL;

	WTSTickStruct sTick;
	sTick.action_date = lDate;
	sTick.action_time = lTime * 100000 + lSecs;
	
	uint32_t nowTDate = beginTDate;
	while(nowTDate < curTDate)
	{
		std::string curCode = cInfo._code;
		std::string hotCode;
		if(commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if (strlen(ruleTag) > 0)
			{
				curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, nowTDate);

				pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, curCode.c_str());
				hotCode = cInfo._product;
				hotCode += "_";
				hotCode += ruleTag;
			}
		}
		
		std::string key = fmt::format("{}-{}", stdCode, nowTDate);

		auto it = _his_tick_map.find(key);
		bool bHasHisTick = (it != _his_tick_map.end());
		if(!bHasHisTick)
		{
			for(;;)
			{
				std::string filename;
				bool bHitHot = false;
				if (!hotCode.empty())
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << nowTDate << "/" << hotCode << ".dsb";
					filename = ss.str();
					if (StdFile::exists(filename.c_str()))
					{
						bHitHot = true;
					}
				}

				if (!bHitHot)
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << nowTDate << "/" << curCode << ".dsb";
					filename = ss.str();
					pipe_rdmreader_log(_sink, LL_DEBUG, "Reading ticks from {}...", filename);
					if (!StdFile::exists(filename.c_str()))
					{
						break;
					}
				}

				HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					slice->release();
					return NULL;
				}

				if (!proc_block_data(tBlkPair._buffer, false, true))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Processing his tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					slice->release();
					return NULL;
				}
				tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
				bHasHisTick = true;
				break;
			}
		}
		
		while(bHasHisTick)
		{
			//比较时间的对象
			WTSTickStruct eTick;
			if(nowTDate == endTDate)
			{
				eTick.action_date = rDate;
				eTick.action_time = rTime * 100000 + rSecs;
			}
			else
			{
				eTick.action_date = nowTDate;
				eTick.action_time = sInfo->getCloseTime() * 100000 + 59999;
			}

			HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
			if (tBlkPair._block == NULL)
				break;

			// R132-001: records are served from the pair's aligned copy, never from
			// the packed history buffer (12-byte header leaves them misaligned).
			if (!align_his_items<WTSTickStruct>(tBlkPair, sizeof(HisTickBlock)))
				break;
			WTSTickStruct* const hisRecs = tBlkPair._items.data();

			uint32_t tcnt = (uint32_t)tBlkPair._items.size();
			if (tcnt <= 0)
				break;

			// The range end is inclusive. upper_bound keeps all ticks sharing the
			// requested timestamp and compares action_date before action_time.
			WTSTickStruct* pEnd = std::upper_bound(hisRecs, hisRecs + tcnt, eTick,
				[](const WTSTickStruct& value, const WTSTickStruct& item) {
					return hft_time_less(value, item);
				});
			std::size_t eCount = pEnd - hisRecs;
			if (eCount == 0)
				break;

			if (beginTDate != nowTDate)
			{
				//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
				//WTSTickSlice* slice = WTSTickSlice::create(stdCode, hisRecs, eIdx + 1);
				//ayTicks->append(slice, false);
				slice->appendBlock(hisRecs, eCount);
				slice->addOwner(his_owner(_his_tick_map, key));
			}
			else
			{
				//如果交易日相同，则查找起始的位置
				WTSTickStruct* pTick = std::lower_bound(hisRecs, pEnd, sTick, hft_time_less<WTSTickStruct>);

				std::size_t sIdx = pTick - hisRecs;
				//WTSTickSlice* slice = WTSTickSlice::create(stdCode, hisRecs + sIdx, eIdx - sIdx + 1);
				//ayTicks->append(slice, false);
				if (sIdx < eCount)
				{
					slice->appendBlock(hisRecs + sIdx, eCount - sIdx);
					slice->addOwner(his_owner(_his_tick_map, key));
				}
			}

			break;
		}
		
		nowTDate = TimeUtils::getNextDate(nowTDate);
	}

	while(hasToday)
	{
		std::string curCode = cInfo._code;
		if (commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if (strlen(ruleTag) > 0)
				curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, curTDate);
		}

		TickBlockPair* tPair = getRTTickBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL || tPair->_block->_size == 0)
			break;

		StdUniqueLock lock(*tPair->_mtx);
		RTTickBlock* tBlock = tPair->_block;
		WTSTickStruct eTick;
		if (curTDate == endTDate)
		{
			eTick.action_date = rDate;
			eTick.action_time = rTime * 100000 + rSecs;
		}
		else
		{
			eTick.action_date = curTDate;
			eTick.action_time = sInfo->getCloseTime() * 100000 + 59999;
		}

		// Decision K36=A: all published records inside this mapping are visible (card 13
		// visibleRecords); use them for both the inclusive finite cutoff and latest sentinel.
		const std::size_t recordCount = wt_rtpub::visibleRecords(tBlock, tPair->_file->size(), sizeof(RTTickBlock), sizeof(WTSTickStruct));
		if (recordCount == 0)
			break;
		std::size_t eCount = 0;
		if (etime == 0)
		{
			// A future trading-date start lies beyond the latest current RT snapshot.
			if (beginTDate > curTDate)
				break;
			eCount = recordCount;
		}
		else
		{
			std::size_t eIdx = 0;
			if (!find_rt_hft_end_index(tBlock->_ticks, recordCount, eTick, eIdx))
				break;
			eCount = eIdx + 1;
		}
		if (eCount == 0)
			break;

		if (beginTDate != curTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, eIdx + 1);
			//ayTicks->append(slice, false);
			slice->appendBlock(tBlock->_ticks, eCount);
			slice->addOwner(tPair->_file);
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + eCount, sTick,
				hft_time_less<WTSTickStruct>);

			std::size_t sIdx = pTick - tBlock->_ticks;
			//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, eIdx - sIdx + 1);
			//ayTicks->append(slice, false);
			if (sIdx < eCount)
			{
				slice->appendBlock(tBlock->_ticks + sIdx, eCount - sIdx);
				slice->addOwner(tPair->_file);
			}
		}
		break;
	}

	return slice;
}

WTSOrdQueSlice* WtRdmDtReader::readOrdQueSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	if (etime != 0 && stime > etime)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	uint32_t lDate, lTime, lSecs;
	//20190807124533900
	lDate = (uint32_t)(stime / 1000000000);
	lTime = (uint32_t)(stime % 1000000000) / 100000;
	lSecs = (uint32_t)(stime % 100000);

	// One client: etime=0 and the current date share one clock snapshot.
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, lDate, lTime, beginTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, endTDate);
	}

	//比较时间的对象
	WTSOrdQueStruct eTick;
	eTick.action_date = rDate;
	eTick.action_time = rTime * 100000 + rSecs;

	WTSOrdQueStruct sTick;
	sTick.action_date = lDate;
	sTick.action_time = lTime * 100000 + lSecs;

	if (isToday)
	{
		OrdQueBlockPair* tPair = getRTOrdQueBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdQueBlock* rtBlock = tPair->_block;
		// Decision K36=A (2026-11-04): every published record inside this mapping is
		// visible (card 13 visibleRecords, the same rule as WtDataReader); no slot is
		// writer-owned.
		const std::size_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTOrdQueBlock), sizeof(WTSOrdQueStruct));
		if (visible == 0)
			return NULL;

		std::size_t eIdx = 0;
		if (!find_rt_hft_end_index(rtBlock->_queues, visible, eTick, eIdx))
			return NULL;

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues, eIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSOrdQueStruct* pItem = std::lower_bound(rtBlock->_queues, rtBlock->_queues + eIdx + 1, sTick, hft_time_less<WTSOrdQueStruct>);
			if (pItem == rtBlock->_queues + eIdx + 1)
				return NULL;

			std::size_t sIdx = pItem - rtBlock->_queues;
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues + sIdx, eIdx - sIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
	}
	else
	{
		std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_ordque_map.find(key);
		if (it == _his_ordque_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/queue/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisOrdQueBlockPair& hisBlkPair = his_slot(_his_ordque_map, key);
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdQueBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderqueue data file {} failed", filename.c_str());
				_his_ordque_map.erase(key);
				return NULL;
			}

			HisOrdQueBlockV2* tBlockV2 = (HisOrdQueBlockV2*)hisBlkPair._buffer.data();

			const std::size_t queue_header_size = sizeof(HisOrdQueBlockV2);
			if (hisBlkPair._buffer.size() < queue_header_size
				|| tBlockV2->_size != hisBlkPair._buffer.size() - queue_header_size)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderqueue data file {} failed", filename.c_str());
				_his_ordque_map.erase(key);
				return NULL;
			}

			//需要解压；畸形 zstd frame 可能在 helper 中抛异常，不能越过 reader 接口。
			std::string buf;
			try
			{
				buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressing orderqueue data file {} failed: {}", filename.c_str(), e.what());
				_his_ordque_map.erase(key);
				return NULL;
			}
			if (buf.size() % sizeof(WTSOrdQueStruct) != 0)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressed orderqueue data file {} has an incomplete record", filename.c_str());
				_his_ordque_map.erase(key);
				return NULL;
			}

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			//先改头版本，后续 resize/append 可能重分配并使 tBlockV2 失效。
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			const uintptr_t oldDataAddress = (uintptr_t)hisBlkPair._buffer.data();
			const std::size_t oldCapacity = hisBlkPair._buffer.capacity();
#endif
			try
			{
				tBlockV2->_version = BLOCK_VERSION_RAW;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_OrdQueue,
					wtp::RdmDtReaderTransformFailureStage::BeforeResize);
#endif
				hisBlkPair._buffer.resize(sizeof(HisOrdQueBlock));
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_OrdQueue,
					wtp::RdmDtReaderTransformFailureStage::BeforeAppend);
#endif
				hisBlkPair._buffer.append(buf);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Rebuilding orderqueue data file {} failed: {}", filename.c_str(), e.what());
				_his_ordque_map.erase(key);
				return NULL;
			}
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			notify_rdm_dt_reader_buffer_rebuilt(BT_HIS_OrdQueue, oldDataAddress,
				(uintptr_t)hisBlkPair._buffer.data(), oldCapacity, hisBlkPair._buffer.capacity(),
				hisBlkPair._buffer.size(), ((const BlockHeader*)hisBlkPair._buffer.data())->_version);
#endif

			hisBlkPair._block = (HisOrdQueBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdQueBlockPair& tBlkPair = his_slot(_his_ordque_map, key);
		if (tBlkPair._block == NULL)
			return NULL;

		// R132-001: records are served from the pair's aligned copy, never from
		// the packed history buffer (12-byte header leaves them misaligned).
		if (!align_his_items<WTSOrdQueStruct>(tBlkPair, sizeof(HisOrdQueBlock)))
			return NULL;
		WTSOrdQueStruct* const hisRecs = tBlkPair._items.data();

		uint32_t tcnt = (uint32_t)tBlkPair._items.size();
		if (tcnt <= 0)
			return NULL;

		// Historical HFT ranges are end-exclusive. lower_bound also avoids
		// decrementing begin() when the requested end precedes the first item.
		WTSOrdQueStruct* pEnd = std::lower_bound(hisRecs, hisRecs + tcnt, eTick, hft_time_less<WTSOrdQueStruct>);
		std::size_t eCount = pEnd - hisRecs;
		if (eCount == 0)
			return NULL;


		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, hisRecs, eCount);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_ordque_map, key));
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSOrdQueStruct* pStart = std::lower_bound(hisRecs, pEnd, sTick, hft_time_less<WTSOrdQueStruct>);
			std::size_t sIdx = pStart - hisRecs;
			if (sIdx >= eCount)
				return NULL;
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, pStart, eCount - sIdx);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_ordque_map, key));
			return slice;
		}
	}
}

WTSOrdDtlSlice* WtRdmDtReader::readOrdDtlSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	if (etime != 0 && stime > etime)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	uint32_t lDate, lTime, lSecs;
	//20190807124533900
	lDate = (uint32_t)(stime / 1000000000);
	lTime = (uint32_t)(stime % 1000000000) / 100000;
	lSecs = (uint32_t)(stime % 100000);

	// One client: etime=0 and the current date share one clock snapshot.
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, lDate, lTime, beginTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, endTDate);
	}

	//比较时间的对象
	WTSOrdDtlStruct eTick;
	eTick.action_date = rDate;
	eTick.action_time = rTime * 100000 + rSecs;

	WTSOrdDtlStruct sTick;
	sTick.action_date = lDate;
	sTick.action_time = lTime * 100000 + lSecs;

	if (isToday)
	{
		OrdDtlBlockPair* tPair = getRTOrdDtlBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdDtlBlock* rtBlock = tPair->_block;
		// Decision K36=A (2026-11-04): every published record inside this mapping is
		// visible (card 13 visibleRecords, the same rule as WtDataReader); no slot is
		// writer-owned.
		const std::size_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTOrdDtlBlock), sizeof(WTSOrdDtlStruct));
		if (visible == 0)
			return NULL;

		std::size_t eIdx = 0;
		if (!find_rt_hft_end_index(rtBlock->_details, visible, eTick, eIdx))
			return NULL;

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details, eIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSOrdDtlStruct* pItem = std::lower_bound(rtBlock->_details, rtBlock->_details + eIdx + 1, sTick, hft_time_less<WTSOrdDtlStruct>);
			if (pItem == rtBlock->_details + eIdx + 1)
				return NULL;

			std::size_t sIdx = pItem - rtBlock->_details;
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details + sIdx, eIdx - sIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
	}
	else
	{
		std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_orddtl_map.find(key);
		if (it == _his_orddtl_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/orders/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisOrdDtlBlockPair& hisBlkPair = his_slot(_his_orddtl_map, key);
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdDtlBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderdetail data file {} failed", filename.c_str());
				_his_orddtl_map.erase(key);
				return NULL;
			}

			HisOrdDtlBlockV2* tBlockV2 = (HisOrdDtlBlockV2*)hisBlkPair._buffer.data();

			const std::size_t detail_header_size = sizeof(HisOrdDtlBlockV2);
			if (hisBlkPair._buffer.size() < detail_header_size
				|| tBlockV2->_size != hisBlkPair._buffer.size() - detail_header_size)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderdetail data file {} failed", filename.c_str());
				_his_orddtl_map.erase(key);
				return NULL;
			}

			//需要解压；畸形 zstd frame 可能在 helper 中抛异常，不能越过 reader 接口。
			std::string buf;
			try
			{
				buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressing orderdetail data file {} failed: {}", filename.c_str(), e.what());
				_his_orddtl_map.erase(key);
				return NULL;
			}
			if (buf.size() % sizeof(WTSOrdDtlStruct) != 0)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressed orderdetail data file {} has an incomplete record", filename.c_str());
				_his_orddtl_map.erase(key);
				return NULL;
			}

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			//先改头版本，后续 resize/append 可能重分配并使 tBlockV2 失效。
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			const uintptr_t oldDataAddress = (uintptr_t)hisBlkPair._buffer.data();
			const std::size_t oldCapacity = hisBlkPair._buffer.capacity();
#endif
			try
			{
				tBlockV2->_version = BLOCK_VERSION_RAW;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_OrdDetail,
					wtp::RdmDtReaderTransformFailureStage::BeforeResize);
#endif
				hisBlkPair._buffer.resize(sizeof(HisOrdDtlBlock));
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_OrdDetail,
					wtp::RdmDtReaderTransformFailureStage::BeforeAppend);
#endif
				hisBlkPair._buffer.append(buf);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Rebuilding orderdetail data file {} failed: {}", filename.c_str(), e.what());
				_his_orddtl_map.erase(key);
				return NULL;
			}
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			notify_rdm_dt_reader_buffer_rebuilt(BT_HIS_OrdDetail, oldDataAddress,
				(uintptr_t)hisBlkPair._buffer.data(), oldCapacity, hisBlkPair._buffer.capacity(),
				hisBlkPair._buffer.size(), ((const BlockHeader*)hisBlkPair._buffer.data())->_version);
#endif

			hisBlkPair._block = (HisOrdDtlBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdDtlBlockPair& tBlkPair = his_slot(_his_orddtl_map, key);
		if (tBlkPair._block == NULL)
			return NULL;

		// R132-001: records are served from the pair's aligned copy, never from
		// the packed history buffer (12-byte header leaves them misaligned).
		if (!align_his_items<WTSOrdDtlStruct>(tBlkPair, sizeof(HisOrdDtlBlock)))
			return NULL;
		WTSOrdDtlStruct* const hisRecs = tBlkPair._items.data();

		uint32_t tcnt = (uint32_t)tBlkPair._items.size();
		if (tcnt <= 0)
			return NULL;

		// Historical HFT ranges are end-exclusive. lower_bound also avoids
		// decrementing begin() when the requested end precedes the first item.
		WTSOrdDtlStruct* pEnd = std::lower_bound(hisRecs, hisRecs + tcnt, eTick, hft_time_less<WTSOrdDtlStruct>);
		std::size_t eCount = pEnd - hisRecs;
		if (eCount == 0)
			return NULL;

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, hisRecs, eCount);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_orddtl_map, key));
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSOrdDtlStruct* pStart = std::lower_bound(hisRecs, pEnd, sTick, hft_time_less<WTSOrdDtlStruct>);
			std::size_t sIdx = pStart - hisRecs;
			if (sIdx >= eCount)
				return NULL;
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, pStart, eCount - sIdx);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_orddtl_map, key));
			return slice;
		}
	}
}

WTSTransSlice* WtRdmDtReader::readTransSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	if (etime != 0 && stime > etime)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	uint32_t lDate, lTime, lSecs;
	//20190807124533900
	lDate = (uint32_t)(stime / 1000000000);
	lTime = (uint32_t)(stime % 1000000000) / 100000;
	lSecs = (uint32_t)(stime % 100000);

	// One client: etime=0 and the current date share one clock snapshot.
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, beginTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, lDate, lTime, beginTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	bool isToday = (endTDate == curTDate);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, endTDate);
	}

	//比较时间的对象
	WTSTransStruct eTick;
	eTick.action_date = rDate;
	eTick.action_time = rTime * 100000 + rSecs;

	WTSTransStruct sTick;
	sTick.action_date = lDate;
	sTick.action_time = lTime * 100000 + lSecs;

	if (isToday)
	{
		TransBlockPair* tPair = getRTTransBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTTransBlock* rtBlock = tPair->_block;
		// Decision K36=A (2026-11-04): every published record inside this mapping is
		// visible (card 13 visibleRecords, the same rule as WtDataReader); no slot is
		// writer-owned.
		const std::size_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTTransBlock), sizeof(WTSTransStruct));
		if (visible == 0)
			return NULL;

		std::size_t eIdx = 0;
		if (!find_rt_hft_end_index(rtBlock->_trans, visible, eTick, eIdx))
			return NULL;

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans, eIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSTransStruct* pItem = std::lower_bound(rtBlock->_trans, rtBlock->_trans + eIdx + 1, sTick, hft_time_less<WTSTransStruct>);
			if (pItem == rtBlock->_trans + eIdx + 1)
				return NULL;

			std::size_t sIdx = pItem - rtBlock->_trans;
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans + sIdx, eIdx - sIdx + 1);
			if (slice != NULL)
				slice->addOwner(tPair->_file);
			return slice;
		}
	}
	else
	{
		std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_trans_map.find(key);
		if (it == _his_trans_map.end())
		{
			std::stringstream ss;
			ss << _base_dir << "his/trans/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisTransBlockPair& hisBlkPair = his_slot(_his_trans_map, key);
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisTransBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of transaction data file {} failed", filename.c_str());
				_his_trans_map.erase(key);
				return NULL;
			}

			HisTransBlockV2* tBlockV2 = (HisTransBlockV2*)hisBlkPair._buffer.data();

			const std::size_t trans_header_size = sizeof(HisTransBlockV2);
			if (hisBlkPair._buffer.size() < trans_header_size
				|| tBlockV2->_size != hisBlkPair._buffer.size() - trans_header_size)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of transaction data file {} failed", filename.c_str());
				_his_trans_map.erase(key);
				return NULL;
			}

			//需要解压；畸形 zstd frame 可能在 helper 中抛异常，不能越过 reader 接口。
			std::string buf;
			try
			{
				buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressing transaction data file {} failed: {}", filename.c_str(), e.what());
				_his_trans_map.erase(key);
				return NULL;
			}
			if (buf.size() % sizeof(WTSTransStruct) != 0)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Uncompressed transaction data file {} has an incomplete record", filename.c_str());
				_his_trans_map.erase(key);
				return NULL;
			}

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			//先改头版本，后续 resize/append 可能重分配并使 tBlockV2 失效。
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			const uintptr_t oldDataAddress = (uintptr_t)hisBlkPair._buffer.data();
			const std::size_t oldCapacity = hisBlkPair._buffer.capacity();
#endif
			try
			{
				tBlockV2->_version = BLOCK_VERSION_RAW;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_Trnsctn,
					wtp::RdmDtReaderTransformFailureStage::BeforeResize);
#endif
				hisBlkPair._buffer.resize(sizeof(HisTransBlock));
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				maybe_fail_rdm_dt_reader_transform(BT_HIS_Trnsctn,
					wtp::RdmDtReaderTransformFailureStage::BeforeAppend);
#endif
				hisBlkPair._buffer.append(buf);
			}
			catch (const std::exception& e)
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Rebuilding transaction data file {} failed: {}", filename.c_str(), e.what());
				_his_trans_map.erase(key);
				return NULL;
			}
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			notify_rdm_dt_reader_buffer_rebuilt(BT_HIS_Trnsctn, oldDataAddress,
				(uintptr_t)hisBlkPair._buffer.data(), oldCapacity, hisBlkPair._buffer.capacity(),
				hisBlkPair._buffer.size(), ((const BlockHeader*)hisBlkPair._buffer.data())->_version);
#endif

			hisBlkPair._block = (HisTransBlock*)hisBlkPair._buffer.c_str();
		}

		HisTransBlockPair& tBlkPair = his_slot(_his_trans_map, key);
		if (tBlkPair._block == NULL)
			return NULL;

		// R132-001: records are served from the pair's aligned copy, never from
		// the packed history buffer (12-byte header leaves them misaligned).
		if (!align_his_items<WTSTransStruct>(tBlkPair, sizeof(HisTransBlock)))
			return NULL;
		WTSTransStruct* const hisRecs = tBlkPair._items.data();

		uint32_t tcnt = (uint32_t)tBlkPair._items.size();
		if (tcnt <= 0)
			return NULL;

		// Historical HFT ranges are end-exclusive. lower_bound also avoids
		// decrementing begin() when the requested end precedes the first item.
		WTSTransStruct* pEnd = std::lower_bound(hisRecs, hisRecs + tcnt, eTick, hft_time_less<WTSTransStruct>);
		std::size_t eCount = pEnd - hisRecs;
		if (eCount == 0)
			return NULL;

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, hisRecs, eCount);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_trans_map, key));
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			WTSTransStruct* pStart = std::lower_bound(hisRecs, pEnd, sTick, hft_time_less<WTSTransStruct>);
			std::size_t sIdx = pStart - hisRecs;
			if (sIdx >= eCount)
				return NULL;
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, pStart, eCount - sIdx);
			if (slice != NULL)
				slice->addOwner(his_owner(_his_trans_map, key));
			return slice;
		}
	}
}

bool WtRdmDtReader::cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo->_exchg, cInfo->_product);
	if (commInfo == NULL)
	{
		// I03-NEW-052
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, history bars not loaded", stdCode == NULL ? "" : stdCode);
		return false;
	}
	const char* stdPID = cInfo->stdCommID();

	// One checked clock snapshot for the current trading date and every later
	// date-zero request of this load (decision card 1, I03-NEW-006).
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, endTDate))
		return false;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	const bool hadBarsCacheEntry = _bars_cache.find(key) != _bars_cache.end();
	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo->_exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;
	auto releaseBarsSections = [&barsSections]()
	{
		for (std::vector<WTSBarStruct>* section : barsSections)
			delete section;
		barsSections.clear();
	};

	uint32_t realCnt = 0;
	bool shortAdjustedFile = false;
	const char* ruleTag = cInfo->_ruletag;
	if (strlen(ruleTag) > 0)//如果是读取期货主力连续数据
	{
		//先按照HOT代码进行读取, 如rb.HOT
		std::unique_ptr<std::vector<WTSBarStruct>> hotAy;
		uint64_t lastHotTime = 0;
		for (;;)
		{
			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo->_exchg << "/" << cInfo->_exchg << "." << cInfo->_product << "_" << ruleTag;
			if (cInfo->isExright())
				ss << (cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
			ss << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				break;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (!proc_his_kline_block(content))
			{
				// A corrupt aggregate HOT file must not prevent recovery from the
				// valid per-contract sections below. No data from this file has
				// been published to hotAy or barsSections at this point.
				pipe_rdmreader_log(_sink, LL_ERROR, "Processing HOT {} bars from {} failed; falling back to contract sections", pname.c_str(), filename.c_str());
				break;
			}

			uint32_t barcnt = (uint32_t)(content.size() / sizeof(WTSBarStruct));
			hotAy.reset(new std::vector<WTSBarStruct>(barcnt));
			memcpy(hotAy->data(), content.data(), content.size());

			if (period != KP_DAY)
				lastHotTime = hotAy->back().time;
			else
				lastHotTime = hotAy->back().date;

			pipe_rdmreader_log(_sink, LL_INFO, "{} items of back {} data of hot contract {} directly loaded", barcnt, pname.c_str(), stdCode);
			break;
		}

		HotSections secs;
		if (strlen(ruleTag))
		{
			if (!_hot_mgr->splitCustomSections(ruleTag, stdPID, 19900102, endTDate, secs))
				return false;
		}

		if (secs.empty())
			return false;

		//根据复权类型确定基础因子
		//如果是前复权，则历史数据会变小，以最后一个复权因子为基础因子
		//如果是后复权，则新数据会变大，基础因子为1
		double baseFactor = 1.0;
		if (cInfo->_exright == 1)
			baseFactor = secs.back()._factor;
		else if (cInfo->_exright == 2)
			barList._factor = secs.back()._factor;

		bool bAllCovered = false;
		for (auto it = secs.rbegin(); it != secs.rend(); it++)
		{
			const HotSection& hotSec = *it;
			const char* curCode = hotSec._code.c_str();
			uint32_t rightDt = hotSec._e_date;
			uint32_t leftDt = hotSec._s_date;

			//要先将日期转换为边界时间
			WTSBarStruct sBar, eBar;
			if (period != KP_DAY)
			{
				const wtp::DateMappingResult64 sBound = dateMapping.getBoundaryTime(stdPID, leftDt, false, true);
				const wtp::DateMappingResult64 eBound = dateMapping.getBoundaryTime(stdPID, rightDt, false, false);
				if (sBound.status != wtp::DateMappingStatus::Ok || eBound.status != wtp::DateMappingStatus::Ok)
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Mapping boundaries of {} section {}-{} failed: {}/{}", stdCode, leftDt, rightDt,
						wtp::dateMappingStatusName(sBound.status), wtp::dateMappingStatusName(eBound.status));
					releaseBarsSections();
					_bars_cache.erase(key);
					return false;
				}
				const uint64_t sTime = sBound.value;
				const uint64_t eTime = eBound.value;

				sBar.date = leftDt;
				sBar.time = ((uint32_t)(sTime / 10000) - 19900000) * 10000 + (uint32_t)(sTime % 10000);

				if(sBar.time < lastHotTime)	//如果边界时间小于主力的最后一根Bar的时间, 说明已经有交叉了, 则不需要再处理了
				{
					bAllCovered = true;
					sBar.time = lastHotTime + 1;
				}

				eBar.date = rightDt;
				eBar.time = ((uint32_t)(eTime / 10000) - 19900000) * 10000 + (uint32_t)(eTime % 10000);

				if (eBar.time <= lastHotTime)	//右边界时间小于最后一条Hot时间, 说明全部交叉了, 没有再找的必要了
					break;
			}
			else
			{
				sBar.date = leftDt;
				if (sBar.date < lastHotTime)	//如果边界时间小于主力的最后一根Bar的时间, 说明已经有交叉了, 则不需要再处理了
				{
					bAllCovered = true;
					sBar.date = (uint32_t)lastHotTime + 1;
				}

				eBar.date = rightDt;

				if (eBar.date <= lastHotTime)
					break;
			}

			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo->_exchg << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				continue;

			{
				std::string content;
				StdFile::read_file_content(filename.c_str(), content);
				if (!proc_his_kline_block(content, true))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Processing {} bars from {} failed", pname.c_str(), filename.c_str());
					releaseBarsSections();
					_bars_cache.erase(key);
					return false;
				}

				if(content.empty())
					break;

				uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
				WTSBarStruct* firstBar = (WTSBarStruct*)content.data();

				WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b){
					if (period == KP_DAY)
					{
						return a.date < b.date;
					}
					else
					{
						return a.time < b.time;
					}
				});

				std::size_t sIdx = pBar - firstBar;
				if ((period == KP_DAY && pBar->date < sBar.date) || (period != KP_DAY && pBar->time < sBar.time))	//早于边界时间
				{
					//早于边界时间, 说明没有数据了, 因为lower_bound会返回大于等于目标位置的数据
					continue;
				}

				pBar = NULL;
				pBar = std::lower_bound(firstBar + sIdx, firstBar + (barcnt - 1), eBar, [period](const WTSBarStruct& a, const WTSBarStruct& b){
					if (period == KP_DAY)
					{
						return a.date < b.date;
					}
					else
					{
						return a.time < b.time;
					}
				});

				std::size_t eIdx = pBar - firstBar;
				if ((period == KP_DAY && pBar->date > eBar.date) || (period != KP_DAY && pBar->time > eBar.time))
				{
					if(eIdx == 0)
						break;

					pBar--;
					eIdx--;
				}

				if (eIdx < sIdx)
					continue;

				uint32_t curCnt = eIdx - sIdx + 1;

				if (cInfo->isExright())
				{
					double factor = hotSec._factor / baseFactor;
					for (uint32_t idx = sIdx; idx <= eIdx; idx++)
					{
						firstBar[idx].open *= factor;
						firstBar[idx].high *= factor;
						firstBar[idx].low *= factor;
						firstBar[idx].close *= factor;
					}
				}

				std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
				tempAy->resize(curCnt);
				memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
				realCnt += curCnt;

				barsSections.emplace_back(tempAy);

				if(bAllCovered)
					break;
			}
		}

		if (hotAy)
		{
			barsSections.emplace_back(hotAy.get());
			realCnt += hotAy->size();
			hotAy.release();
		}
	}
	else if(cInfo->isExright() && commInfo->isStock())//如果是读取股票复权数据
	{
		std::vector<WTSBarStruct>* hotAy = NULL;
		uint64_t lastQTime = 0;
		
		do
		{
			//先直接读取复权过的历史数据,路径如/his/day/sse/SH600000Q.dsb
			char flag = cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ;
			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo->_exchg << "/" << cInfo->_code << flag << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				break;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
				shortAdjustedFile = true;
				break;
			}
			// Legacy adjusted files may contain a header-only CMP block to mark
			// an empty layer. Preserve this sentinel only on this direct-read path;
			// all other compressed blocks must pass normal payload validation.
			if (content.size() == sizeof(HisKlineBlockV2))
			{
				const HisKlineBlockV2* header = (const HisKlineBlockV2*)content.data();
				const uint16_t expectedType = period == KP_Minute1 ? BT_HIS_Minute1 :
					(period == KP_Minute5 ? BT_HIS_Minute5 : BT_HIS_Day);
				if (memcmp(header->_blk_flag, BLK_FLAG, FLAG_SIZE) == 0 &&
					header->_type == expectedType &&
					header->_version == BLOCK_VERSION_CMP && header->_size == 0)
					break;
			}

			if (!proc_his_kline_block(content, true))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
				delete hotAy;
				hotAy = NULL;
				releaseBarsSections();
				_bars_cache.erase(key);
				return false;
			}
			if (content.empty())
				break;

			uint32_t barcnt = (uint32_t)(content.size() / sizeof(WTSBarStruct));

			hotAy = new std::vector<WTSBarStruct>();
			hotAy->resize(barcnt);
			memcpy(hotAy->data(), content.data(), content.size());

			if (period != KP_DAY)
				lastQTime = hotAy->at(barcnt - 1).time;
			else
				lastQTime = hotAy->at(barcnt - 1).date;

			pipe_rdmreader_log(_sink, LL_INFO, "{} history exrighted {} data of {} directly cached", barcnt, pname.c_str(), stdCode);
			break;
		} while (false);

		[[maybe_unused]] bool bAllCovered = false;
		do
		{
			//const char* curCode = it->first.c_str();
			//uint32_t rightDt = it->second.second;
			//uint32_t leftDt = it->second.first;
			const char* curCode = cInfo->_code;

			//要先将日期转换为边界时间
			WTSBarStruct sBar;
			if (period != KP_DAY)
			{
				sBar.date = TimeUtils::minBarToDate(lastQTime);

				sBar.time = lastQTime + 1;
			}
			else
			{
				sBar.date = (uint32_t)lastQTime + 1;
			}

			std::stringstream ss;
			ss << _base_dir << "his/" << pname << "/" << cInfo->_exchg << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				continue;

			{
				std::string content;
				StdFile::read_file_content(filename.c_str(), content);
				if (!proc_his_kline_block(content, true))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Processing adjusted {} bars from {} failed", pname.c_str(), filename.c_str());
					delete hotAy;
					hotAy = NULL;
					releaseBarsSections();
					_bars_cache.erase(key);
					return false;
				}
				if(content.empty())
					break;

				uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
				WTSBarStruct* firstBar = (WTSBarStruct*)content.data();

				WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b){
					if (period == KP_DAY)
					{
						return a.date < b.date;
					}
					else
					{
						return a.time < b.time;
					}
				});

				if(pBar != NULL)
				{
					std::size_t sIdx = pBar - firstBar;
					uint32_t curCnt = barcnt - sIdx;
					std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
					tempAy->resize(curCnt);
					memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
					realCnt += curCnt;

					auto& ayFactors = getAdjFactors(cInfo->_code, cInfo->_exchg, cInfo->_product);
					if(!ayFactors.empty())
					{
						double baseFactor = 1.0;
						if (cInfo->_exright == 1)
							baseFactor = ayFactors.back()._factor;
						else if (cInfo->_exright == 2)
							barList._factor = ayFactors.back()._factor;

						//做前复权处理
						std::size_t lastIdx = curCnt;
						WTSBarStruct bar;
						firstBar = tempAy->data();
						for (auto it = ayFactors.rbegin(); it != ayFactors.rend(); it++)
						{
							const AdjFactor& adjFact = *it;
							bar.date = adjFact._date;

							//调整因子
							double factor = adjFact._factor / baseFactor;

							WTSBarStruct* pBar = NULL;
							pBar = std::lower_bound(firstBar, firstBar + lastIdx - 1, bar, [](const WTSBarStruct& a, const WTSBarStruct& b) {
								return a.date < b.date;
							});

							if (pBar->date < bar.date)
								continue;

							WTSBarStruct* endBar = pBar;
							if (pBar != NULL)
							{
								std::size_t curIdx = pBar - firstBar;
								while (pBar && curIdx < lastIdx)
								{
									pBar->open *= factor;
									pBar->high *= factor;
									pBar->low *= factor;
									pBar->close *= factor;

									pBar++;
									curIdx++;
								}
								lastIdx = endBar - firstBar;
							}

							if (lastIdx == 0)
								break;
						}
					}

					barsSections.emplace_back(tempAy);
				}
			}
		} while (false);

		if (hotAy)
		{
			barsSections.emplace_back(hotAy);
			realCnt += hotAy->size();
		}
	}
	else
	{
		//读取历史的
		std::stringstream ss;
		ss << _base_dir << "his/" << pname << "/" << cInfo->_exchg << "/" << cInfo->_code << ".dsb";
		std::string filename = ss.str();
		pipe_rdmreader_log(_sink, LL_DEBUG, "Target file is {}", filename);
		if (StdFile::exists(filename.c_str()))
		{
			//如果有格式化的历史数据文件, 则直接读取
			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (!proc_his_kline_block(content, true))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Processing historical {} bars from {} failed", pname.c_str(), filename.c_str());
				releaseBarsSections();
				_bars_cache.erase(key);
				return false;
			}

			if (content.empty())
				return false;

			uint32_t barcnt = content.size() / sizeof(WTSBarStruct);
			WTSBarStruct* firstBar = (WTSBarStruct*)content.data();

			if (barcnt > 0)
			{
				uint32_t sIdx = 0;
				uint32_t idx = barcnt - 1;
				uint32_t curCnt = (idx - sIdx + 1);

				std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
				tempAy->resize(curCnt);
				memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
				realCnt += curCnt;

				barsSections.emplace_back(tempAy);
			}
		}
	}

	if (shortAdjustedFile && !hadBarsCacheEntry && realCnt == 0)
	{
		releaseBarsSections();
		_bars_cache.erase(key);
		return false;
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size()*sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	pipe_rdmreader_log(_sink, LL_INFO, "{} history {} data of {} cached", realCnt, pname.c_str(), stdCode);
	return true;
}

WTSBarStruct* WtRdmDtReader::indexBarFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, uint32_t& count, bool isDay /* = false */)
{
	count = 0;
	if (stime > etime)
		return NULL;

	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	BarsList& barsList = _bars_cache[key];
	if (barsList._bars.empty())
		return NULL;
	
	std::size_t eIdx,sIdx;
	{
		//光标尚未初始化, 需要重新定位
		[[maybe_unused]] uint64_t nowTime = (uint64_t)rDate * 10000 + rTime;

		WTSBarStruct eBar;
		eBar.date = rDate;
		eBar.time = (rDate - 19900000) * 10000 + rTime;

		WTSBarStruct sBar;
		sBar.date = lDate;
		sBar.time = (lDate - 19900000) * 10000 + lTime;

		auto eit = std::lower_bound(barsList._bars.begin(), barsList._bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b){
			if (isDay)
				return a.date < b.date;
			else
				return a.time < b.time;
		});


		if (eit == barsList._bars.end())
			eIdx = barsList._bars.size() - 1;
		else
		{
			if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
			{
				if (eit == barsList._bars.begin())
					return NULL;

				eit--;
			}

			eIdx = eit - barsList._bars.begin();
		}

		auto sit = std::lower_bound(barsList._bars.begin(), eit, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (isDay)
				return a.date < b.date;
			else
				return a.time < b.time;
		});
		sIdx = sit - barsList._bars.begin();
	}

	if (sIdx >= barsList._bars.size() || sIdx > eIdx)
		return NULL;

	uint32_t curCnt = eIdx - sIdx + 1;
	count = curCnt;
	return &barsList._bars[sIdx];
}

WTSBarStruct* WtRdmDtReader::indexBarFromCacheByCount(const std::string& key, uint64_t etime, uint32_t& count, bool isDay /* = false */)
{
	const uint32_t requestedCount = count;
	count = 0;
	if (requestedCount == 0)
		return NULL;

	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	BarsList& barsList = _bars_cache[key];
	if (barsList._bars.empty())
	{
		count = 0;
		return NULL;
	}

	std::size_t eIdx, sIdx;
	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	auto eit = std::lower_bound(barsList._bars.begin(), barsList._bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
		if (isDay)
			return a.date < b.date;
		else
			return a.time < b.time;
	});


	if (eit == barsList._bars.end())
		eIdx = barsList._bars.size() - 1;
	else
	{
		if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
		{
			if (eit == barsList._bars.begin())
				return NULL;

			eit--;
		}

		eIdx = eit - barsList._bars.begin();
	}

	uint32_t curCnt = min((uint32_t)eIdx + 1, requestedCount);
	sIdx = eIdx + 1 - curCnt;
	if (curCnt == 0)
		return NULL;

	count = curCnt;
	return &barsList._bars[sIdx];
}

uint32_t WtRdmDtReader::readBarsFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, std::vector<WTSBarStruct>& ayBars, bool isDay /* = false */)
{
	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	BarsList& barsList = _bars_cache[key];
	std::size_t eIdx,sIdx;
	{
		WTSBarStruct eBar;
		eBar.date = rDate;
		eBar.time = (rDate - 19900000) * 10000 + rTime;

		WTSBarStruct sBar;
		sBar.date = lDate;
		sBar.time = (lDate - 19900000) * 10000 + lTime;

		auto eit = std::lower_bound(barsList._bars.begin(), barsList._bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b){
			if (isDay)
				return a.date < b.date;
			else
				return a.time < b.time;
		});
		

		if(eit == barsList._bars.end())
			eIdx = barsList._bars.size() - 1;
		else
		{
			if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time))
			{
				if (eit == barsList._bars.begin())
					return 0;
				
				eit--;
			}

			eIdx = eit - barsList._bars.begin();
		}

		auto sit = std::lower_bound(barsList._bars.begin(), eit, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (isDay)
				return a.date < b.date;
			else
				return a.time < b.time;
		});
		sIdx = sit - barsList._bars.begin();
	}

	uint32_t curCnt = eIdx - sIdx + 1;
	if(curCnt > 0)
	{
		ayBars.resize(curCnt);
		memcpy(ayBars.data(), &barsList._bars[sIdx], sizeof(WTSBarStruct)*curCnt);
	}
	return curCnt;
}

WTSKlineSlice* WtRdmDtReader::readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	std::string key = fmt::format("{}#{}", stdCode, (uint32_t)period);
	auto it = _bars_cache.find(key);
	if (it == _bars_cache.end())
	{
		bool bHasHisData = cacheHisBarsFromFile(&cInfo, key, stdCode, period);
		if (!bHasHisData && _bars_cache.find(key) == _bars_cache.end())
			return NULL;
	}

	if (etime == 0)
		etime = 203012312359;

	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;
	
	WTSBarStruct* hisHead = NULL;
	WTSBarStruct* rtHead = NULL;
	WTSSliceOwnerPtr hisOwner;
	WTSSliceOwnerPtr rtOwner;
	uint32_t hisCnt = 0;
	uint32_t rtCnt = 0;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	bool isDay = period == KP_DAY;

	//是否包含当天的
	bool bHasToday = (endTDate >= curTDate);
	std::string raw_code = cInfo._code;

	const char* ruleTag = cInfo._ruletag;
	if (strlen(ruleTag) > 0)
	{
		raw_code = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), curTDate);

		pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, raw_code);
	}
	else
	{
		raw_code = cInfo._code;
	}

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;

	WTSBarStruct sBar;
	sBar.date = lDate;
	sBar.time = (lDate - 19900000) * 10000 + lTime;

	bool bNeedHisData = true;

	if (bHasToday)
	{
		//读取实时的

		const char* curCode = raw_code.c_str();

		if(cInfo._exright != 2)
		{
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL)
			{
				StdUniqueLock lock(*kPair->_mtx);
				rtOwner = kPair->_file;
				if (kPair->_block != NULL && kPair->_block->_size > 0)
				{
					WTSBarStruct* pFirst = &kPair->_block->_bars[0];
					if ((isDay && pFirst->date <= eBar.date) || (!isDay && pFirst->time <= eBar.time))
					{
						//读取当日的数据
						WTSBarStruct* pBar = std::lower_bound(kPair->_block->_bars, kPair->_block->_bars + (kPair->_block->_size - 1), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
							if (isDay)
								return a.date < b.date;
							else
								return a.time < b.time;
						});
						std::size_t idx = pBar - kPair->_block->_bars;
						if ((isDay && pBar->date > eBar.date) || (!isDay && pBar->time > eBar.time))
						{
							pBar--;
							idx--;
						}

						pBar = &kPair->_block->_bars[0];
						//如果第一条实时K线的时间大于开始日期，则实时K线要全部包含进去
						if ((isDay && pBar->date > sBar.date) || (!isDay && pBar->time > sBar.time))
						{
							rtHead = &kPair->_block->_bars[0];
							rtCnt = idx + 1;
						}
						else
						{
							pBar = std::lower_bound(kPair->_block->_bars, kPair->_block->_bars + idx, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
								if (isDay)
									return a.date < b.date;
								else
									return a.time < b.time;
							});

							std::size_t sIdx = pBar - kPair->_block->_bars;
							rtHead = pBar;
							rtCnt = idx - sIdx + 1;
							bNeedHisData = false;
						}
					}
				}
			}
		}
		else
		{
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL && kPair->_block != NULL && kPair->_block->_size > 0)
			{
				//如果是后复权，实时数据是需要单独缓存的，所以这里处理会很复杂
				BarsList& barsList = _bars_cache[key];

				//1、先检查缓存中有多少实时数据
				std::size_t oldSize = barsList._rt_bars.size();
				std::size_t newSize = kPair->_block->_size;

				//2、再看看原始实时数据有多少，如果不够，就要补充进来
				if (newSize > oldSize)
				{
					barsList._rt_bars.resize(newSize);
					auto idx = oldSize;
					if (oldSize != 0)
						idx--;

					//因为每次拷贝，最后一条K线都有可能是未闭合的，所以需要把最后一条K线覆盖
					memcpy(&barsList._rt_bars[idx], &kPair->_block->_bars[idx], sizeof(WTSBarStruct)*(newSize - idx));

					//最后做复权处理
					double factor = barsList._factor;
					for (; idx < newSize; idx++)
					{
						WTSBarStruct* pBar = &barsList._rt_bars[idx];
						pBar->open *= factor;
						pBar->high *= factor;
						pBar->low *= factor;
						pBar->close *= factor;
					}
				}

				//最后做一个定位。etime 是包含式上界，转换成 endCount 后再做索引，
				//避免 lower_bound 返回 begin/end 时进行指针回退或越界解引用。
				rtOwner = barsList._rt_bars.owner();
				auto bar_less = [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
					if (isDay)
						return a.date < b.date;
					else
						return a.time < b.time;
				};
				if (!barsList._rt_bars.empty())
				{
					auto first = barsList._rt_bars.begin();
					auto last = barsList._rt_bars.end();
					auto endIt = std::lower_bound(first, last, eBar, bar_less);
					std::size_t endCount = (std::size_t)(endIt - first);
					if (endIt != last && !bar_less(eBar, *endIt))
						++endCount;

					if (endCount > 0)
					{
						// stime 早于首根 RT bar 时仍让历史路径补齐此前数据。
						if (bar_less(sBar, *first))
						{
							rtHead = &*first;
							rtCnt = (uint32_t)endCount;
						}
						else
						{
							auto startIt = std::lower_bound(first, first + endCount, sBar, bar_less);
							std::size_t startIdx = (std::size_t)(startIt - first);
							if (startIdx < endCount)
							{
								rtHead = &*startIt;
								rtCnt = (uint32_t)(endCount - startIdx);
								bNeedHisData = false;
							}
						}
					}
				}
			}
		}	
		
	}

	if (bNeedHisData)
	{
		hisHead = indexBarFromCacheByRange(key, stime, etime, hisCnt, period == KP_DAY);
		auto hisIt = _bars_cache.find(key);
		if (hisIt != _bars_cache.end())
			hisOwner = hisIt->second._bars.owner();
	}

	if (hisCnt + rtCnt > 0)
	{
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, hisHead, hisCnt);
		if (slice == NULL)
			return NULL;
		if (hisCnt > 0)
			slice->addOwner(hisOwner);
		if (rtCnt > 0)
			slice->addOwner(rtOwner);
		if (rtCnt > 0 && !slice->appendBlock(rtHead, rtCnt))
		{
			slice->release();
			return NULL;
		}
		return slice;
	}

	return NULL;
}


WtRdmDtReader::TickBlockPair* WtRdmDtReader::getRTTickBlock(const char* exchg, const char* code)
{
	std::string key = fmt::format("{}.{}", exchg, code);

	std::string path = fmt::format("{}rt/ticks/{}/{}.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	TickBlockPair& block = _rt_tick_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTickBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//说明文件大小已变, 需要重新映射
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTickBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	block._last_time = TimeUtils::getLocalTimeNow();
	return &block;
}

WtRdmDtReader::OrdDtlBlockPair* WtRdmDtReader::getRTOrdDtlBlock(const char* exchg, const char* code)
{
	std::string key = fmt::format("{}.{}", exchg, code);

	std::string path = fmt::format("{}rt/orders/{}/{}.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	OrdDtlBlockPair& block = _rt_orddtl_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdDtlBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//说明文件大小已变, 需要重新映射
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdDtlBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	block._last_time = TimeUtils::getLocalTimeNow();
	return &block;
}

WtRdmDtReader::OrdQueBlockPair* WtRdmDtReader::getRTOrdQueBlock(const char* exchg, const char* code)
{
	std::string key = fmt::format("{}.{}", exchg, code);

	std::string path = fmt::format("{}rt/queue/{}/{}.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	OrdQueBlockPair& block = _rt_ordque_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdQueBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//说明文件大小已变, 需要重新映射
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTOrdQueBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	block._last_time = TimeUtils::getLocalTimeNow();
	return &block;
}

WtRdmDtReader::TransBlockPair* WtRdmDtReader::getRTTransBlock(const char* exchg, const char* code)
{
	std::string key = fmt::format("{}.{}", exchg, code);

	std::string path = fmt::format("{}rt/trans/{}/{}.dmb", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	TransBlockPair& block = _rt_trans_map[key];
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTransBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//说明文件大小已变, 需要重新映射
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTTransBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	block._last_time = TimeUtils::getLocalTimeNow();
	return &block;
}

WtRdmDtReader::RTKlineBlockPair* WtRdmDtReader::getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period)
{
	if (period != KP_Minute1 && period != KP_Minute5)
		return NULL;

	char key[64] = { 0 }; 
	fmtutil::format_to(key, "{}.{}", exchg, code);

	std::string subdir = "";
	switch (period)
	{
	case KP_Minute1:
		subdir = "min1";
		break;
	case KP_Minute5:
		subdir = "min5";
		break;
	default: 
		return NULL;
	}

	std::string path = fmtutil::format("{}rt/{}/{}/{}.dmb", _base_dir.c_str(), subdir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return NULL;

	RTKlineBlockPair& block = (period == KP_Minute1 ? _rt_min1_map[key] : _rt_min5_map[key]);
	if (block._file == NULL || block._block == NULL)
	{
		if (block._file == NULL)
		{
			block._file.reset(new BoostMappingFile());
		}

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTKlineBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}
	else if (block._last_cap != block._block->_capacity)
	{
		//说明文件大小已变, 需要重新映射
		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTKlineBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}

	block._last_time = TimeUtils::getLocalTimeNow();
	return &block;
}

WTSKlineSlice* WtRdmDtReader::readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	pipe_rdmreader_log(_sink, LL_INFO, "CodeInfo of {}: {},{},{}", stdCode, cInfo._exchg, cInfo._product, cInfo._code);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	std::string key = fmtutil::format("{}#{}", stdCode, (uint32_t)period);
	auto it = _bars_cache.find(key);
	if (it == _bars_cache.end())
	{
		bool bHasHisData = cacheHisBarsFromFile(&cInfo, key, stdCode, period);
		if (!bHasHisData && _bars_cache.find(key) == _bars_cache.end())
			return NULL;
	}

	if (etime == 0)
		etime = 203012312359;

	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	WTSBarStruct* hisHead = NULL;
	WTSBarStruct* rtHead = NULL;
	WTSSliceOwnerPtr hisOwner;
	WTSSliceOwnerPtr rtOwner;
	uint32_t hisCnt = 0;
	uint32_t rtCnt = 0;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	bool isDay = period == KP_DAY;

	//是否包含当天的
	bool bHasToday = (endTDate >= curTDate);
	std::string raw_code = cInfo._code;

	const char* ruleTag = cInfo._ruletag;
	if (strlen(ruleTag) > 0)
	{
		raw_code = _hot_mgr->getCustomRawCode(ruleTag, stdPID, curTDate);
		pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, raw_code.c_str());
	}
	else
	{
		raw_code = cInfo._code;
	}

	WTSBarStruct eBar;
	eBar.date = rDate;
	eBar.time = (rDate - 19900000) * 10000 + rTime;


	bool bNeedHisData = true;

	if (bHasToday)
	{
		const char* curCode = raw_code.c_str();
		if(cInfo._exright != 2)
		{
			//读取实时的
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL)
			{
				StdUniqueLock lock(*(kPair->_mtx));
				rtOwner = kPair->_file;
				if (kPair->_block != NULL && kPair->_block->_size > 0)
				{
					WTSBarStruct* pFirst = &kPair->_block->_bars[0];
					if ((isDay && pFirst->date <= eBar.date) || (!isDay && pFirst->time <= eBar.time))
					{
						//读取当日的数据
						WTSBarStruct* pBar = std::lower_bound(kPair->_block->_bars, kPair->_block->_bars + (kPair->_block->_size - 1), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
							if (isDay)
								return a.date < b.date;
							else
								return a.time < b.time;
						});
						std::size_t idx = pBar - kPair->_block->_bars;
						if ((isDay && pBar->date > eBar.date) || (!isDay && pBar->time > eBar.time))
						{
							pBar--;
							idx--;
						}

						//请求在首根 RT bar 之后时再计算 RT 数量；早于首根时保留历史回退路径。
						rtCnt = min((uint32_t)idx + 1, count);
						std::size_t sIdx = idx + 1 - rtCnt;
						rtHead = kPair->_block->_bars + sIdx;
						bNeedHisData = (rtCnt < count);
					}
				}
			}
		}
		else
		{
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL && kPair->_block != NULL && kPair->_block->_size > 0)
			{
				//如果是后复权，实时数据是需要单独缓存的，所以这里处理会很复杂
				BarsList& barsList = _bars_cache[key];

				//1、先检查缓存中有多少实时数据
				std::size_t oldSize = barsList._rt_bars.size();
				std::size_t newSize = kPair->_block->_size;

				//2、再看看原始实时数据有多少，如果不够，就要补充进来
				if(newSize > oldSize)
				{
					barsList._rt_bars.resize(newSize);
					auto idx = oldSize;
					if (oldSize != 0)
						idx--;

					//因为每次拷贝，最后一条K线都有可能是未闭合的，所以需要把最后一条K线覆盖
					memcpy(&barsList._rt_bars[idx], &kPair->_block->_bars[idx], sizeof(WTSBarStruct)*(newSize - idx));

					//最后做复权处理
					double factor = barsList._factor;
					for(; idx < newSize; idx++)
					{
						WTSBarStruct* pBar = &barsList._rt_bars[idx];
						pBar->open *= factor;
						pBar->high *= factor;
						pBar->low *= factor;
						pBar->close *= factor;
					}
				}

				//最后做一个定位。etime 是包含式上界；begin 之前没有 RT 数据，
				//end 之后则将可用数量限制在缓存实际长度内。
				rtOwner = barsList._rt_bars.owner();
				auto bar_less = [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
					if (isDay)
						return a.date < b.date;
					else
						return a.time < b.time;
				};
				if (!barsList._rt_bars.empty() && count > 0)
				{
					auto first = barsList._rt_bars.begin();
					auto last = barsList._rt_bars.end();
					auto endIt = std::lower_bound(first, last, eBar, bar_less);
					std::size_t endCount = (std::size_t)(endIt - first);
					if (endIt != last && !bar_less(eBar, *endIt))
						++endCount;

					if (endCount > 0)
					{
						rtCnt = min((uint32_t)endCount, count);
						std::size_t startIdx = endCount - rtCnt;
						rtHead = &barsList._rt_bars[startIdx];
						bNeedHisData = (rtCnt < count);
					}
				}
			}
		}
	}
	

	if (bNeedHisData)
	{
		hisCnt = count - rtCnt;
		hisHead = indexBarFromCacheByCount(key, etime, hisCnt, period == KP_DAY);
		auto hisIt = _bars_cache.find(key);
		if (hisIt != _bars_cache.end())
			hisOwner = hisIt->second._bars.owner();
		if (hisHead == NULL)
			hisCnt = 0;
	}

	pipe_rdmreader_log(_sink, LL_DEBUG, "His {} bars of {} loaded, {} from history, {} from realtime", PERIOD_NAME[period], stdCode, hisCnt, rtCnt);

	if (hisCnt + rtCnt > 0)
	{
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, hisHead, hisCnt);
		if (slice == NULL)
			return NULL;
		if (hisCnt > 0)
			slice->addOwner(hisOwner);
		if (rtCnt > 0)
			slice->addOwner(rtOwner);
		if (rtCnt > 0 && !slice->appendBlock(rtHead, rtCnt))
		{
			slice->release();
			return NULL;
		}
		return slice;
	}

	return NULL;
}

WTSTickSlice* WtRdmDtReader::readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Commodity {}.{} for {} is unavailable", cInfo._exchg, cInfo._product, stdCode);
		return NULL;
	}

	const char* stdPID = commInfo->getFullPid();
	const char* sessionId = commInfo->getSession();
	WTSSessionInfo* sInfo = sessionId == NULL ? NULL : _base_data_mgr->getSession(sessionId);
	if (sInfo == NULL)
	{
		pipe_rdmreader_log(_sink, LL_ERROR, "Session for commodity {}.{} is unavailable", cInfo._exchg, cInfo._product);
		return NULL;
	}

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!rdm_trading_date(_sink, dateMapping, stdPID, rDate, rTime, endTDate) ||
		!rdm_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	bool hasToday = (endTDate >= curTDate);

	WTSTickSlice* slice = WTSTickSlice::create(stdCode);
	if (slice == NULL)	// I03-NEW-083: rejected (e.g. overlong) code
		return NULL;

	uint32_t left = count;
	while (hasToday)
	{
		std::string curCode = cInfo._code;
		if(commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if (strlen(ruleTag) > 0)
			{
				curCode = _hot_mgr->getCustomRawCode(ruleTag, stdPID, curTDate);

				pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, curCode.c_str());
			}
		}		

		TickBlockPair* tPair = getRTTickBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL || tPair->_block->_size == 0)
			break;

		StdUniqueLock lock(*tPair->_mtx);
		RTTickBlock* tBlock = tPair->_block;
		WTSTickStruct eTick;
		if (curTDate == endTDate)
		{
			eTick.action_date = rDate;
			eTick.action_time = rTime * 100000 + rSecs;
		}
		else
		{
			eTick.action_date = curTDate;
			eTick.action_time = sInfo->getCloseTime() * 100000 + 59999;
		}

		// Decision K36=A: published records inside this mapping (card 13 visibleRecords).
		const uint32_t visible = wt_rtpub::visibleRecords(tBlock, tPair->_file->size(), sizeof(RTTickBlock), sizeof(WTSTickStruct));
		if (visible == 0)
			break;
		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (visible - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pTick - tBlock->_ticks;

		//如果光标定位的tick时间比目标时间大, 则全部回退一个
		if (pTick->action_date > eTick.action_date ||
			(pTick->action_date == eTick.action_date && pTick->action_time > eTick.action_time))
		{
			if (eIdx == 0)
				break;

			pTick--;
			eIdx--;
		}

		uint32_t thisCnt = min((uint32_t)eIdx + 1, left);
		uint32_t sIdx = eIdx + 1 - thisCnt;
		slice->insertBlock(0, tBlock->_ticks + sIdx, thisCnt);
		slice->addOwner(tPair->_file);
		left -= thisCnt;
		break;
	}

	uint32_t nowTDate = min(endTDate, curTDate);
	if (nowTDate == curTDate)
		nowTDate = TimeUtils::getNextDate(nowTDate, -1);
	uint32_t missingCnt = 0;
	while (left > 0)
	{
		if(missingCnt >= 30)
			break;

		std::string curCode = cInfo._code;
		std::string hotCode;
		if(commInfo->isFuture())
		{
			const char* ruleTag = cInfo._ruletag;
			if (strlen(ruleTag) > 0)
			{
				curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), nowTDate);

				hotCode = cInfo._product;
				hotCode += "_";
				hotCode += ruleTag;
				pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, curCode.c_str());
			}
		}
		

		std::string key = fmt::format("{}-{}", stdCode, nowTDate);

		auto it = _his_tick_map.find(key);
		bool bHasHisTick = (it != _his_tick_map.end());
		if (!bHasHisTick)
		{
			for (;;)
			{
				std::string filename;
				bool bHitHot = false;
				if(!hotCode.empty())
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << nowTDate << "/" << hotCode << ".dsb";
					filename = ss.str();
					if (StdFile::exists(filename.c_str()))
					{
						bHitHot = true;
					}
				}

				if(!bHitHot)
				{
					std::stringstream ss;
					ss << _base_dir << "his/ticks/" << cInfo._exchg << "/" << nowTDate << "/" << curCode << ".dsb";
					filename = ss.str();
					if (!StdFile::exists(filename.c_str()))
					{
						missingCnt++;
						break;
					}
				}

				missingCnt = 0;

				HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					slice->release();
					return NULL;
				}

				if (!proc_block_data(tBlkPair._buffer, false, true))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Processing his tick data file {} failed", filename.c_str());
					_his_tick_map.erase(key);
					slice->release();
					return NULL;
				}

				tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
				bHasHisTick = true;
				break;
			}
		}

		while (bHasHisTick)
		{
			//比较时间的对象
			WTSTickStruct eTick;
			if (nowTDate == endTDate)
			{
				eTick.action_date = rDate;
				eTick.action_time = rTime * 100000 + rSecs;
			}
			else
			{
				eTick.action_date = nowTDate;
				eTick.action_time = sInfo->getCloseTime() * 100000 + 59999;
			}

			HisTBlockPair& tBlkPair = his_slot(_his_tick_map, key);
			if (tBlkPair._block == NULL)
				break;

			// R132-001: records are served from the pair's aligned copy, never from
			// the packed history buffer (12-byte header leaves them misaligned).
			if (!align_his_items<WTSTickStruct>(tBlkPair, sizeof(HisTickBlock)))
				break;
			WTSTickStruct* const hisRecs = tBlkPair._items.data();

			uint32_t tcnt = (uint32_t)tBlkPair._items.size();
			if (tcnt <= 0)
				break;

			WTSTickStruct* pTick = std::lower_bound(hisRecs, hisRecs + (tcnt - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t eIdx = pTick - hisRecs;
			if (pTick->action_date > eTick.action_date ||
				(pTick->action_date == eTick.action_date && pTick->action_time > eTick.action_time))
			{
				if (eIdx == 0)
					break;

				pTick--;
				eIdx--;
			}

			uint32_t thisCnt = min((uint32_t)eIdx + 1, left);
			uint32_t sIdx = eIdx + 1 - thisCnt;
			slice->insertBlock(0, hisRecs + sIdx, thisCnt);
			slice->addOwner(his_owner(_his_tick_map, key));
			left -= thisCnt;
			break;
		}

		nowTDate = TimeUtils::getNextDate(nowTDate, -1);
	}

	return slice;
}

double WtRdmDtReader::getAdjFactorByDate(const char* stdCode, uint32_t date /* = 0 */)
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL || !commInfo->isStock())	// I03-NEW-052: unknown commodity has no adjustment
		return 1.0;

	AdjFactor factor = { date, 1.0 };

	std::string key = stdCode;
	if (cInfo.isExright())
		key = key.substr(0, key.size() - 1);
	const AdjFactorList& factList = _adj_factors[key];
	if (factList.empty())
		return 1.0;

	auto it = std::lower_bound(factList.begin(), factList.end(), factor, [](const AdjFactor& a, const AdjFactor&b) {
		return a._date < b._date;
	});

	if (it == factList.end())
	{
		//找不到，则说明目标日期大于最后一条的日期，直接返回最后一条除权因子
		return factList.back()._factor;
	}
	else
	{
		//如果找到了，但是命中的日期大于目标日期，则用上一条
		//如果等于目标日期，则用命中这一条
		if ((*it)._date > date)
			it--;

		return (*it)._factor;
	}
}

void WtRdmDtReader::clearCache()
{
	std::lock_guard<StdRecurMutex> cacheGuard(_cache_mtx);
	_bars_cache.clear();

	_rt_min1_map.clear();
	_rt_min5_map.clear();

	_rt_tick_map.clear();
	_rt_trans_map.clear();
	_rt_orddtl_map.clear();
	_rt_ordque_map.clear();

	//卡11=B：历史 tick/委托队列/委托明细/成交四类缓存同样清理。
	//缓存只释放自己持有的引用，已返回切片通过 owner 保活，释放后底层缓冲区才回收。
	_his_tick_map.clear();
	_his_ordque_map.clear();
	_his_orddtl_map.clear();
	_his_trans_map.clear();
}
