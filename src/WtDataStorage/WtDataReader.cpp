#include "WtDataReader.h"
#include "RTBlockPublish.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/DateMappingClient.hpp"

#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSUtils/WTSCmpBudgetConfig.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

#include <rapidjson/document.h>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <type_traits>
namespace rj = rapidjson;

namespace
{
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
	thread_local int r132_allocation_failure_stage = 0;
#endif
	bool decode_his_compressed_items(std::string& content, std::size_t itemSize)
	{
		if (content.size() < sizeof(BlockHeaderV2))
			return false;

		const BlockHeaderV2* compressedHeader = (const BlockHeaderV2*)content.data();
		if (compressedHeader->_size != content.size() - sizeof(BlockHeaderV2))
			return false;

		const void* compressedData = content.data() + sizeof(BlockHeaderV2);
		const std::size_t compressedSize = (std::size_t)compressedHeader->_size;
		const unsigned long long expandedSize = ZSTD_getFrameContentSize(compressedData, compressedSize);
		if (expandedSize == ZSTD_CONTENTSIZE_ERROR ||
			expandedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
			expandedSize > content.max_size() - sizeof(BlockHeader) ||
			expandedSize % itemSize != 0 ||
			expandedSize / itemSize > (std::numeric_limits<uint32_t>::max)())
			return false;

		try
		{
			std::string expanded = WTSCmpHelper::uncompress_data(compressedData, compressedSize);
			if (expanded.size() != expandedSize)
				return false;

			// Build a new raw block before replacing the compressed bytes. No pointer into
			// the old string survives this operation, even if the allocation moves.
			std::string raw;
			raw.reserve(sizeof(BlockHeader) + expanded.size());
			raw.append(content.data(), sizeof(BlockHeader));
			raw.append(expanded);
			((BlockHeader*)raw.data())->_version = BLOCK_VERSION_RAW_V2;
			content.swap(raw);
		}
		catch (const std::exception&)
		{
			return false;
		}

		return true;
	}

	// R132-001: move a history tick block's payload out of the packed buffer
	// (12-byte header -> records at a 4-byte offset) into the pair's aligned
	// vector once; `_buffer` keeps only the header afterwards.
	template <typename Pair>
	bool align_his_tick_items(Pair& pair)
	{
		const std::size_t headerSize = sizeof(HisTickBlock);
		if (pair._buffer.size() <= headerSize)
			return true;
		const std::size_t count = (pair._buffer.size() - headerSize) / sizeof(WTSTickStruct);
		try
		{
			std::vector<WTSTickStruct> items(count);
			if (count != 0)
				std::memcpy(items.data(), pair._buffer.data() + headerSize, count * sizeof(WTSTickStruct));
			pair._items.swap(items);
			pair._buffer.resize(headerSize);
			pair._buffer.shrink_to_fit();
		}
		catch (const std::exception&)
		{
			return false;
		}
		pair._block = reinterpret_cast<HisTickBlock*>(&pair._buffer[0]);
		return true;
	}

	template <typename T>
	bool copy_his_items_aligned(const std::string& content, std::vector<T>& output)
	{
		static_assert(std::is_trivially_copyable<T>::value, "historical records must be byte-copyable");
		if (content.size() < sizeof(BlockHeader))
			return false;
		const std::size_t payloadSize = content.size() - sizeof(BlockHeader);
		if (payloadSize % sizeof(T) != 0)
			return false;
		const std::size_t count = payloadSize / sizeof(T);
		if (count > (std::numeric_limits<uint32_t>::max)() || count > output.max_size())
			return false;
		try
		{
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
			if (r132_allocation_failure_stage == 1)
			{
				r132_allocation_failure_stage = 0;
				throw std::bad_alloc();
			}
#endif
			std::vector<T> items(count);
			if (payloadSize != 0)
				std::memcpy(items.data(), content.data() + sizeof(BlockHeader), payloadSize);
			output.swap(items);
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}
}

#if defined(WT_RDM_DT_READER_TEST_HOOKS)
// Test-only, per-thread failure injection; 1 = aligned vector, 2 = publication, 3 = slice creation.
void wt_r132_fail_next_allocation_for_test(int stage)
{
	r132_allocation_failure_stage = stage;
}
#endif

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void pipe_reader_log(IDataReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	// Keep the existing per-thread message lifetime while bounding formatter writes.
	thread_local static char buffer[512];
	try
	{
		const std::size_t max_output_size = sizeof(buffer) - 1;
		const auto formatted = fmt::format_to_n(buffer, max_output_size, format, args...);
		if (formatted.size > max_output_size)
		{
			constexpr char truncation_marker[] = "...[truncated]";
			constexpr std::size_t marker_size = sizeof(truncation_marker) - 1;
			const std::size_t marker_offset = max_output_size - marker_size;
			for (std::size_t i = 0; i < marker_size; ++i)
				buffer[marker_offset + i] = truncation_marker[i];
			buffer[max_output_size] = '\0';
		}
		else
		{
			buffer[formatted.size] = '\0';
		}
	}
	catch (...)
	{
		constexpr char fallback[] = "WtDataReader log formatting failed";
		for (std::size_t i = 0; i < sizeof(fallback); ++i)
			buffer[i] = fallback[i];
	}

	sink->reader_log(ll, buffer);
}

// Decision card 1 (A+C): trading dates come from the typed client over the
// guarded legacy IBaseDataMgr virtuals. A failure is logged and the caller
// stops before creating a slice or touching a cache.
static bool reader_trading_date(IDataReaderSink* sink, wtp::DateMappingClient& mapping, const char* stdPID,
	uint32_t uDate, uint32_t uTime, uint32_t& tradingDate)
{
	const wtp::DateMappingResult32 result = mapping.calcTradingDate(stdPID, uDate, uTime, false);
	if (result.status != wtp::DateMappingStatus::Ok)
	{
		pipe_reader_log(sink, LL_ERROR, "Mapping trading date of {} at {}.{} failed: {}",
			stdPID == NULL ? "" : stdPID, uDate, uTime, wtp::dateMappingStatusName(result.status));
		return false;
	}
	tradingDate = result.value;
	return true;
}

static void report_historical_slice_exception(IDataReaderSink* sink, const char* kind, const char* stdCode) noexcept
{
	try
	{
		pipe_reader_log(sink, LL_ERROR, "Unexpected failure reading historical {} slice for {}", kind, stdCode ? stdCode : "<null>");
	}
	catch (...)
	{
		// A failing log sink must not turn a guarded read into an escaping exception.
	}
}

extern "C"
{
	EXPORT_FLAG IDataReader* createDataReader()
	{
		IDataReader* ret = new WtDataReader();
		return ret;
	}

	EXPORT_FLAG void deleteDataReader(IDataReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}

	// K31=A: optional DateMappingV1 injection (see Includes/DateMappingBinding.h).
	EXPORT_FLAG int bindDataReaderDateMappingV1(IDataReader* reader, const wtp::DateMappingV1* table)
	{
		if (reader == NULL)
			return 0;
		return static_cast<WtDataReader*>(reader)->bindDateMapping(table) ? 1 : 0;
	}
};

/*
 *	处理块数据
 */
bool proc_block_data(std::string& content, bool isBar, bool bKeepHead /* = true */)
{
	if (content.size() < BLOCK_HEADER_SIZE)
		return false;

	BlockHeader* header = (BlockHeader*)content.data();

	bool bCmped = header->is_compressed();
	bool bOldVer = header->is_old_version();

	//如果既没有压缩，也不是老版本结构体，则直接返回
	if (!bCmped && !bOldVer)
	{
		if (isBar && header->_version == BLOCK_VERSION_RAW_V2 &&
			(content.size() - BLOCK_HEADER_SIZE) % sizeof(WTSBarStruct) != 0)
			return false;

		if (!bKeepHead)
			content.erase(0, BLOCK_HEADER_SIZE);
		return true;
	}

	std::string buffer;
	if (bCmped)
	{
		if (content.size() < BLOCK_HEADERV2_SIZE)
			return false;

		BlockHeaderV2* blkV2 = (BlockHeaderV2*)content.c_str();

		if (blkV2->_size != (uint64_t)(content.size() - BLOCK_HEADERV2_SIZE))
		{
			return false;
		}

		const void* compressedData = content.data() + BLOCK_HEADERV2_SIZE;
		const std::size_t compressedSize = (std::size_t)blkV2->_size;
		const unsigned long long uncompressedSize = ZSTD_getFrameContentSize(compressedData, compressedSize);
		std::string sizeProbe;
		if (uncompressedSize == ZSTD_CONTENTSIZE_ERROR ||
			uncompressedSize == ZSTD_CONTENTSIZE_UNKNOWN ||
			uncompressedSize > sizeProbe.max_size())
			return false;

		if (isBar)
		{
			const std::size_t barSize = bOldVer ? sizeof(WTSBarStructOld) : sizeof(WTSBarStruct);
			if (uncompressedSize % barSize != 0)
				return false;
		}

		// Zstd reports malformed frames through an error code, which the helper
		// converts to an exception. Leave content untouched and report failure.
		try
		{
			buffer = WTSCmpHelper::uncompress_data(compressedData, compressedSize);
		}
		catch (const std::exception&)
		{
			return false;
		}
	}
	else
	{
		if (!bOldVer)
		{
			//如果不是老版本，直接返回
			if (!bKeepHead)
				content.erase(0, BLOCK_HEADER_SIZE);
			return true;
		}
		else
		{
			if (isBar && (content.size() - BLOCK_HEADER_SIZE) % sizeof(WTSBarStructOld) != 0)
				return false;

			buffer.append(content.data() + BLOCK_HEADER_SIZE, content.size() - BLOCK_HEADER_SIZE);
		}
	}

	if (bOldVer)
	{
		if (isBar)
		{
			std::string bufV2;
			uint32_t barcnt = buffer.size() / sizeof(WTSBarStructOld);
			bufV2.resize(barcnt * sizeof(WTSBarStruct));
			WTSBarStruct* newBar = (WTSBarStruct*)bufV2.data();
			WTSBarStructOld* oldBar = (WTSBarStructOld*)buffer.data();
			for (uint32_t idx = 0; idx < barcnt; idx++)
			{
				newBar[idx] = oldBar[idx];
			}
			buffer.swap(bufV2);
		}
		else
		{
			uint32_t tick_cnt = buffer.size() / sizeof(WTSTickStructOld);
			std::string bufv2;
			bufv2.resize(sizeof(WTSTickStruct)*tick_cnt);
			WTSTickStruct* newTick = (WTSTickStruct*)bufv2.data();
			WTSTickStructOld* oldTick = (WTSTickStructOld*)buffer.data();
			for (uint32_t i = 0; i < tick_cnt; i++)
			{
				newTick[i] = oldTick[i];
			}
			buffer.swap(bufv2);
		}
	}

	if (bKeepHead)
	{
		content.resize(BLOCK_HEADER_SIZE);
		content.append(buffer);
		header = (BlockHeader*)content.data();
		header->_version = BLOCK_VERSION_RAW_V2;
	}
	else
	{
		content.swap(buffer);
	}

	return true;
}


WtDataReader::WtDataReader()
	: _base_data_mgr(NULL)
	, _hot_mgr(NULL)
	, _adjust_flag(0)
	, _last_time(0)
	{
}


WtDataReader::~WtDataReader()
{
}

void WtDataReader::init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader /* = NULL */)
{
	IDataReader::init(cfg, sink, loader);
	_rt_dir.clear();
	_his_dir.clear();

	_base_data_mgr = sink == NULL ? NULL : sink->get_basedata_mgr();
	_hot_mgr = sink == NULL ? NULL : sink->get_hot_mgr();
	if (sink == NULL)
		return;
	if (_base_data_mgr == NULL)
	{
		pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: base data manager is unavailable");
		return;
	}

	if (cfg == NULL)
		return ;

	// BUG-R132-001 (K37=B): this module's decompression budget comes from its own store
	// config (keys next to path); an invalid value fails closed for compressed blocks.
	std::string budgetError;
	if (wt_cmp_budget::apply(cfg, budgetError) == wt_cmp_budget::ApplyResult::Invalid)
		pipe_reader_log(sink, LL_ERROR, "{}; decompression in this storage module is refused", budgetError);

	WTSVariant* cfgPath = cfg->get("path");
	if (cfgPath == NULL)
	{
		pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: required path is missing");
		return;
	}

	std::vector<std::string> rt_dirs;
	std::vector<std::string> his_dirs;
	if (cfgPath->isArray())
	{
		std::size_t cnt = cfgPath->size();
		if (cnt == 0)
		{
			pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: path array is empty");
			return;
		}

		for (std::size_t i = 0; i < cnt; i++)
		{
			WTSVariant* pathItem = cfgPath->get(i);
			if (pathItem == NULL || pathItem->type() != WTSVariant::VT_String || pathItem->asString().empty())
			{
				pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: path array contains an empty or non-string entry at index {}", i);
				return;
			}

			std::string root_dir = StrUtil::standardisePath(pathItem->asString());
			rt_dirs.emplace_back(root_dir + "rt/");
			his_dirs.emplace_back(root_dir + "his/");
		}
	}
	else
	{
		if (cfgPath->type() != WTSVariant::VT_String || cfgPath->asString().empty())
		{
			pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: path must be a non-empty string or non-empty array of strings");
			return;
		}

		std::string root_dir = StrUtil::standardisePath(cfgPath->asString());
		rt_dirs.emplace_back(root_dir + "rt/");
		his_dirs.emplace_back(root_dir + "his/");
	}

	// The configured base history directories have precedence over supplemental his_path entries.
	std::vector<std::string> ordered_his_dirs;
	ordered_his_dirs.insert(ordered_his_dirs.end(), his_dirs.begin(), his_dirs.end());
	WTSVariant* cfgHisPath = cfg->get("his_path");
	if(cfgHisPath)
	{
		if (cfgHisPath->isArray())
		{
			std::size_t cnt = cfgHisPath->size();
			for (std::size_t i = 0; i < cnt; i++)
			{
				WTSVariant* hisPathItem = cfgHisPath->get(i);
				if (hisPathItem == NULL || hisPathItem->type() != WTSVariant::VT_String || hisPathItem->asString().empty())
				{
					pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: his_path array contains an empty or non-string entry at index {}", i);
					return;
				}

				std::string his_dir = hisPathItem->asString();
				ordered_his_dirs.emplace_back(StrUtil::standardisePath(his_dir));
			}
		}
		else
		{
			if (cfgHisPath->type() != WTSVariant::VT_String || cfgHisPath->asString().empty())
			{
				pipe_reader_log(sink, LL_ERROR, "WtDataReader initialization failed: his_path must be a non-empty string or array of strings");
				return;
			}

			std::string his_dir = cfgHisPath->asString();
			ordered_his_dirs.emplace_back(StrUtil::standardisePath(his_dir));
		}
	}

	_adjust_flag = cfg->getUInt32("adjust_flag");
	_rt_dir.swap(rt_dirs);
	_his_dir.swap(ordered_his_dirs);

	if (sink != NULL)
	{
		std::string log_message;
		bool log_message_ready = false;
		try
		{
			log_message.append("WtDataReader initialized, rt dir is ")
				.append(_rt_dir[0])
				.append(", hist dir is ")
				.append(_his_dir[0])
				.append(", adjust_flag is ")
				.append(std::to_string(_adjust_flag));
			log_message_ready = true;
		}
		catch (...)
		{
			// Leave reader initialization intact when the detailed diagnostic cannot be built.
		}

		if (log_message_ready)
			sink->reader_log(LL_INFO, log_message.c_str());
		else
			sink->reader_log(LL_INFO, "WtDataReader initialized; detailed path log unavailable");
	}

	/*
	 *	By Wesley @ 2021.12.20
	 *	先从extloader加载除权因子
	 *	如果加载失败，并且配置了除权因子文件，再加载除权因子文件
	 */
	bool bLoaded = loadStkAdjFactorsFromLoader();

	if (!bLoaded && cfg->has("adjfactor"))
		loadStkAdjFactorsFromFile(cfg->getCString("adjfactor"));
	else
		pipe_reader_log(sink, LL_INFO, "No adjusting factor file configured, loading skipped");
}

bool WtDataReader::loadStkAdjFactorsFromLoader()
{
	if (NULL == _loader)
		return false;

	bool ret = _loader->loadAllAdjFactors(&_adj_factors, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
		AdjFactorMap* fact_map = (AdjFactorMap*)obj;
		AdjFactorList& fctrLst = (*fact_map)[stdCode];

		for(uint32_t i = 0; i < count; i++)
		{
			AdjFactor adjFact;
			adjFact._date = dates[i];
			adjFact._factor = factors[i];

			fctrLst.emplace_back(adjFact);
		}

		//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
		AdjFactor adjFact;
		adjFact._date = 19900101;
		adjFact._factor = 1;
		fctrLst.emplace_back(adjFact);

		std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
			return left._date < right._date;
		});
	});

	if (ret && _sink) pipe_reader_log(_sink,LL_INFO, "Adjusting factors of {} contracts loaded via extended loader", _adj_factors.size());
	return ret;
}

bool WtDataReader::loadStkAdjFactorsFromFile(const char* adjfile)
{
	if(!StdFile::exists(adjfile))
	{
		pipe_reader_log(_sink,LL_ERROR, "Adjusting factors file {} not exists", adjfile);
		return false;
	}

	WTSVariant* doc = WTSCfgLoader::load_from_file(adjfile);
	if(doc == NULL)
	{
		pipe_reader_log(_sink, LL_ERROR, "Loading adjusting factors file {} failed", adjfile);
		return false;
	}

	uint32_t stk_cnt = 0;
	uint32_t fct_cnt = 0;
	for (const std::string& exchg : doc->memberNames())
	{
		WTSVariant* itemExchg = doc->get(exchg);
		for(const std::string& code : itemExchg->memberNames())
		{
			WTSVariant* ayFacts = itemExchg->get(code);
			if(!ayFacts->isArray() )
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

	pipe_reader_log(_sink,LL_INFO, "{} adjusting factors of {} tickers loaded", fct_cnt, stk_cnt);
	doc->release();
	return true;
}

WTSTickSlice* WtDataReader::readTickSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, stdPID, curDate, curTime, endTDate) ||
		!reader_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
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
	WTSTickStruct eTick;
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		TickBlockPair* tPair = getRTTickBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTTickBlock* tBlock = tPair->_block;
		// 卡13：只读取已发布且落在本进程映射内的记录
		const uint32_t visible = wt_rtpub::visibleRecords(tBlock, tPair->_file->size(), sizeof(RTTickBlock), sizeof(WTSTickStruct));
		if (visible == 0)
			return NULL;

		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (visible - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlock->_ticks;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pTick->action_date > eTick.action_date || pTick->action_time>eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, cnt);
		//卡11=B：实时切片持有映射文件 owner，重映射/巡检释放后切片仍有效
		if (slice != NULL)
			slice->addOwner(tPair->_file);
		return slice;
	}
	else
	{
		// I03-NEW-039: the cache key embeds the caller's stdCode, which may be longer
		// than a fixed 64-byte buffer; build it as a string.
		const std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_tick_map.find(key);
		if(it == _his_tick_map.end())
		{
			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append("ticks/").append(cInfo._exchg).append("/")
					.append(std::to_string(endTDate)).append("/").append(curCode).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
				return NULL;

			HisTBlockPair& tBlkPair = _his_tick_map[key];
			StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
			if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
			{
				pipe_reader_log(_sink,LL_ERROR, "Sizechecking of his tick data file {} failed", filename);
				tBlkPair._buffer.clear();
				return NULL;
			}

			if (!proc_block_data(tBlkPair._buffer, false, true))
			{
				pipe_reader_log(_sink, LL_ERROR, "Processing his tick data file {} failed", filename.c_str());
				_his_tick_map.erase(key);
				return NULL;
			}
			tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
		}
		
		HisTBlockPair& tBlkPair = _his_tick_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		// R132-001: records are served from the pair's aligned copy, never from
		// the packed history buffer (12-byte header leaves them misaligned).
		if (!align_his_tick_items(tBlkPair))
			return NULL;
		WTSTickStruct* const hisTicks = tBlkPair._items.data();

		uint32_t tcnt = (uint32_t)tBlkPair._items.size();
		if (tcnt <= 0)
			return NULL;

		WTSTickStruct* pTick = std::lower_bound(hisTicks, hisTicks + (tcnt - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - hisTicks;
		if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, hisTicks + sIdx, cnt);
		return slice;
	}
}

WTSOrdQueSlice* WtDataReader::readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, stdPID, curDate, curTime, endTDate) ||
		!reader_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
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
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		OrdQueBlockPair* tPair = getRTOrdQueBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdQueBlock* rtBlock = tPair->_block;
		// 卡13：只读取已发布且落在本进程映射内的记录
		const uint32_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTOrdQueBlock), sizeof(WTSOrdQueStruct));
		if (visible == 0)
			return NULL;

		WTSOrdQueStruct* pItem = std::lower_bound(rtBlock->_queues, rtBlock->_queues + (visible - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_queues;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues + sIdx, cnt);
		//卡11=B：实时切片持有映射文件 owner，重映射/巡检释放后切片仍有效
		if (slice != NULL)
			slice->addOwner(tPair->_file);
		return slice;
	}
	else
	{
		try
		{
		// I03-NEW-039: the cache key embeds the caller's stdCode, which may be longer
		// than a fixed 64-byte buffer; build it as a string.
		const std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_ordque_map.find(key);
		if (it == _his_ordque_map.end())
		{
			//std::stringstream ss;
			//ss << _his_dir << "queue/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			//std::string filename = ss.str();
			//if (!StdFile::exists(filename.c_str()))
			//	return NULL;

			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append("queue/").append(cInfo._exchg).append("/")
					.append(std::to_string(endTDate)).append("/").append(curCode).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
				return NULL;

			HisOrdQueBlockPair candidate;
			std::string buffer;
			try
			{
				StdFile::read_file_content(filename.c_str(), buffer);
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: read failed", filename.c_str());
				return NULL;
			}
			if (buffer.size() < sizeof(HisOrdQueBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史委托队列数据文件{}大小校验失败", filename);
				buffer.clear();
				return NULL;
			}

			HisOrdQueBlockV2* tBlockV2 = (HisOrdQueBlockV2*)buffer.c_str();

			if (tBlockV2->_size != buffer.size() - sizeof(HisOrdQueBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史委托队列数据文件{}大小校验失败", filename);
				return NULL;
			}

			if (!decode_his_compressed_items(buffer, sizeof(WTSOrdQueStruct)))
			{
				pipe_reader_log(_sink, LL_ERROR, "历史委托队列数据文件{}解压或记录大小校验失败", filename);
				buffer.clear();
				return NULL;
			}

			if (!copy_his_items_aligned(buffer, candidate._items))
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: aligned record allocation or validation failed", filename.c_str());
				return NULL;
			}
			candidate._loaded = true;
			try
			{
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				if (r132_allocation_failure_stage == 2)
				{
					r132_allocation_failure_stage = 0;
					throw std::bad_alloc();
				}
#endif
				it = _his_ordque_map.try_emplace(key, std::move(candidate)).first;
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: cache publication failed", filename.c_str());
				return NULL;
			}
		}

		HisOrdQueBlockPair& tBlkPair = it->second;
		if (!tBlkPair._loaded)
			return NULL;

		const uint32_t tcnt = static_cast<uint32_t>(tBlkPair._items.size());
		if (tcnt == 0)
			return NULL;

		WTSOrdQueStruct* pTick = std::lower_bound(tBlkPair._items.data(), tBlkPair._items.data() + (tcnt - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlkPair._items.data();
		if (pTick->action_date > eTick.action_date ||
			(pTick->action_date == eTick.action_date && pTick->action_time >= eTick.action_time))
		{
			// Keep the historical exclusive end-time contract, but do not
			// decrement begin() when the requested end is at/before the first item.
			if (eIdx == 0)
				return NULL;
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
		if (r132_allocation_failure_stage == 3)
		{
			r132_allocation_failure_stage = 0;
			throw std::bad_alloc();
		}
#endif
		WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, tBlkPair._items.data() + sIdx, cnt);
		return slice;
		}
		catch (...)
		{
			report_historical_slice_exception(_sink, "queue", stdCode);
			return NULL;
		}
	}
}

WTSOrdDtlSlice* WtDataReader::readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, stdPID, curDate, curTime, endTDate) ||
		!reader_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
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
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		OrdDtlBlockPair* tPair = getRTOrdDtlBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTOrdDtlBlock* rtBlock = tPair->_block;
		// 卡13：只读取已发布且落在本进程映射内的记录
		const uint32_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTOrdDtlBlock), sizeof(WTSOrdDtlStruct));
		if (visible == 0)
			return NULL;

		WTSOrdDtlStruct* pItem = std::lower_bound(rtBlock->_details, rtBlock->_details + (visible - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_details;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details + sIdx, cnt);
		//卡11=B：实时切片持有映射文件 owner，重映射/巡检释放后切片仍有效
		if (slice != NULL)
			slice->addOwner(tPair->_file);
		return slice;
	}
	else
	{
		try
		{
		// I03-NEW-039: the cache key embeds the caller's stdCode, which may be longer
		// than a fixed 64-byte buffer; build it as a string.
		const std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_orddtl_map.find(key);
		if (it == _his_orddtl_map.end())
		{
			//std::stringstream ss;
			//ss << _his_dir << "orders/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			//std::string filename = ss.str();
			//if (!StdFile::exists(filename.c_str()))
			//	return NULL;

			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append("orders/").append(cInfo._exchg).append("/")
					.append(std::to_string(endTDate)).append("/").append(curCode).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
				return NULL;

			HisOrdDtlBlockPair candidate;
			std::string buffer;
			try
			{
				StdFile::read_file_content(filename.c_str(), buffer);
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: read failed", filename.c_str());
				return NULL;
			}
			if (buffer.size() < sizeof(HisOrdDtlBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史逐笔委托数据文件{}大小校验失败", filename.c_str());
				buffer.clear();
				return NULL;
			}

			HisOrdDtlBlockV2* tBlockV2 = (HisOrdDtlBlockV2*)buffer.c_str();

			if (tBlockV2->_size != buffer.size() - sizeof(HisOrdDtlBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史逐笔委托数据文件{}大小校验失败", filename.c_str());
				return NULL;
			}

			if (!decode_his_compressed_items(buffer, sizeof(WTSOrdDtlStruct)))
			{
				pipe_reader_log(_sink, LL_ERROR, "历史逐笔委托数据文件{}解压或记录大小校验失败", filename.c_str());
				buffer.clear();
				return NULL;
			}

			if (!copy_his_items_aligned(buffer, candidate._items))
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: aligned record allocation or validation failed", filename.c_str());
				return NULL;
			}
			candidate._loaded = true;
			try
			{
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				if (r132_allocation_failure_stage == 2)
				{
					r132_allocation_failure_stage = 0;
					throw std::bad_alloc();
				}
#endif
				it = _his_orddtl_map.try_emplace(key, std::move(candidate)).first;
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: cache publication failed", filename.c_str());
				return NULL;
			}
		}

		HisOrdDtlBlockPair& tBlkPair = it->second;
		if (!tBlkPair._loaded)
			return NULL;

		const uint32_t tcnt = static_cast<uint32_t>(tBlkPair._items.size());
		if (tcnt == 0)
			return NULL;

		WTSOrdDtlStruct* pTick = std::lower_bound(tBlkPair._items.data(), tBlkPair._items.data() + (tcnt - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlkPair._items.data();
		if (pTick->action_date > eTick.action_date ||
			(pTick->action_date == eTick.action_date && pTick->action_time >= eTick.action_time))
		{
			// Keep the historical exclusive end-time contract, but do not
			// decrement begin() when the requested end is at/before the first item.
			if (eIdx == 0)
				return NULL;
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
		if (r132_allocation_failure_stage == 3)
		{
			r132_allocation_failure_stage = 0;
			throw std::bad_alloc();
		}
#endif
		WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, tBlkPair._items.data() + sIdx, cnt);
		return slice;
		}
		catch (...)
		{
			report_historical_slice_exception(_sink, "detail", stdCode);
			return NULL;
		}
	}
}

WTSTransSlice* WtDataReader::readTransSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (commInfo == NULL)
	{
		// I03-NEW-052: an unknown commodity is reported before any mapping or session use.
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, request refused", stdCode == NULL ? "" : stdCode);
		return NULL;
	}
	const char* stdPID = commInfo->getFullPid();

	uint32_t curDate, curTime, curSecs;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		curSecs = _sink->get_secs();

		etime = (uint64_t)curDate * 1000000000 + curTime * 100000 + curSecs;
	}
	else
	{
		//20190807124533900
		curDate = (uint32_t)(etime / 1000000000);
		curTime = (uint32_t)(etime % 1000000000) / 100000;
		curSecs = (uint32_t)(etime % 100000);
	}

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, stdPID, curDate, curTime, endTDate) ||
		!reader_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
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
	eTick.action_date = curDate;
	eTick.action_time = curTime * 100000 + curSecs;

	if (isToday)
	{
		TransBlockPair* tPair = getRTTransBlock(cInfo._exchg, curCode.c_str());
		if (tPair == NULL)
			return NULL;

		RTTransBlock* rtBlock = tPair->_block;
		// 卡13：只读取已发布且落在本进程映射内的记录
		const uint32_t visible = wt_rtpub::visibleRecords(rtBlock, tPair->_file->size(), sizeof(RTTransBlock), sizeof(WTSTransStruct));
		if (visible == 0)
			return NULL;

		WTSTransStruct* pItem = std::lower_bound(rtBlock->_trans, rtBlock->_trans + (visible - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pItem - rtBlock->_trans;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
		WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans + sIdx, cnt);
		//卡11=B：实时切片持有映射文件 owner，重映射/巡检释放后切片仍有效
		if (slice != NULL)
			slice->addOwner(tPair->_file);
		return slice;
	}
	else
	{
		try
		{
		// I03-NEW-039: the cache key embeds the caller's stdCode, which may be longer
		// than a fixed 64-byte buffer; build it as a string.
		const std::string key = fmt::format("{}-{}", stdCode, endTDate);

		auto it = _his_trans_map.find(key);
		if (it == _his_trans_map.end())
		{
			//std::stringstream ss;
			//ss << _his_dir << "trans/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			//std::string filename = ss.str();
			//if (!StdFile::exists(filename.c_str()))
			//	return NULL;

			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append("trans/").append(cInfo._exchg).append("/")
					.append(std::to_string(endTDate)).append("/").append(curCode).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
				return NULL;

			HisTransBlockPair candidate;
			std::string buffer;
			try
			{
				StdFile::read_file_content(filename.c_str(), buffer);
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: read failed", filename.c_str());
				return NULL;
			}
			if (buffer.size() < sizeof(HisTransBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史逐笔成交数据文件{}大小校验失败", filename.c_str());
				buffer.clear();
				return NULL;
			}

			HisTransBlockV2* tBlockV2 = (HisTransBlockV2*)buffer.c_str();

			if (tBlockV2->_size != buffer.size() - sizeof(HisTransBlockV2))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史逐笔成交数据文件{}大小校验失败", filename.c_str());
				return NULL;
			}

			if (!decode_his_compressed_items(buffer, sizeof(WTSTransStruct)))
			{
				pipe_reader_log(_sink, LL_ERROR, "历史逐笔成交数据文件{}解压或记录大小校验失败", filename.c_str());
				buffer.clear();
				return NULL;
			}

			if (!copy_his_items_aligned(buffer, candidate._items))
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: aligned record allocation or validation failed", filename.c_str());
				return NULL;
			}
			candidate._loaded = true;
			try
			{
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
				if (r132_allocation_failure_stage == 2)
				{
					r132_allocation_failure_stage = 0;
					throw std::bad_alloc();
				}
#endif
				it = _his_trans_map.try_emplace(key, std::move(candidate)).first;
			}
			catch (const std::exception&)
			{
				pipe_reader_log(_sink, LL_ERROR, "Historical data file {}: cache publication failed", filename.c_str());
				return NULL;
			}
		}

		HisTransBlockPair& tBlkPair = it->second;
		if (!tBlkPair._loaded)
			return NULL;

		const uint32_t tcnt = static_cast<uint32_t>(tBlkPair._items.size());
		if (tcnt == 0)
			return NULL;

		WTSTransStruct* pTick = std::lower_bound(tBlkPair._items.data(), tBlkPair._items.data() + (tcnt - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		uint32_t eIdx = pTick - tBlkPair._items.data();
		if (pTick->action_date > eTick.action_date ||
			(pTick->action_date == eTick.action_date && pTick->action_time >= eTick.action_time))
		{
			// Keep the historical exclusive end-time contract, but do not
			// decrement begin() when the requested end is at/before the first item.
			if (eIdx == 0)
				return NULL;
			pTick--;
			eIdx--;
		}

		uint32_t cnt = min(eIdx + 1, count);
		uint32_t sIdx = eIdx + 1 - cnt;
#if defined(WT_RDM_DT_READER_TEST_HOOKS)
		if (r132_allocation_failure_stage == 3)
		{
			r132_allocation_failure_stage = 0;
			throw std::bad_alloc();
		}
#endif
		WTSTransSlice* slice = WTSTransSlice::create(stdCode, tBlkPair._items.data() + sIdx, cnt);
		return slice;
		}
		catch (...)
		{
			report_historical_slice_exception(_sink, "transaction", stdCode);
			return NULL;
		}
	}
}


bool WtDataReader::cacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	if (NULL == _loader)
		return false;

	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo->_exchg;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "m1"; break;
	case KP_Minute5: pname = "m5"; break;
	case KP_DAY: pname = "d"; break;
	default: pname = ""; break;
	}

	pipe_reader_log(_sink,LL_INFO, "Reading final bars of {} via extended loader...", stdCode);

	bool ret = _loader->loadFinalHisBars(&barList, stdCode, period, [](void* obj, WTSBarStruct* firstBar, uint32_t count) {
		BarsList* bars = (BarsList*)obj;
		bars->_factor = 1.0;
		bars->_bars.resize(count);
		memcpy(bars->_bars.data(), firstBar, sizeof(WTSBarStruct)*count);
	});

	if(ret)
		pipe_reader_log(_sink,LL_INFO, "{} items of back {} data of {} loaded via extended loader", barList._bars.size(), pname.c_str(), stdCode);
	else
		pipe_reader_log(_sink, LL_INFO, "no back {} data of {} loaded via extended loader", pname.c_str(), stdCode);

	return ret;
}


bool WtDataReader::cacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;

	// One checked clock snapshot for the current trading date and every later
	// date-zero request of this load (decision card 1, I03-NEW-006).
	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, cInfo->stdCommID(), 0, 0, endTDate))
		return false;

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo->_exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;

	//const char* hot_flag = cInfo->isHot() ? FILE_SUF_HOT : FILE_SUF_2ND;
	const char* ruleTag = cInfo->_ruletag;

	//先按照HOT代码进行读取, 如rb.HOT
	std::vector<WTSBarStruct>* hotAy = NULL;
	uint64_t lastHotTime = 0;

	do
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	本来这里是要先调用_loader->loadRawHisBars从外部加载器读取主力合约数据的
		 *	但是上层会调用一次loadFinalHisBars，这里再调用loadRawHisBars就冗余了，所以直接跳过
		 */

		//std::stringstream ss;
		//ss << _his_dir << pname << "/" << cInfo->_exchg << "/" << cInfo->_exchg << "." << cInfo->_product << "_" << ruleTag;
		//if (cInfo->isExright())
		//	ss << (cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
		//ss << ".dsb";
		//std::string filename = ss.str();
		//if (!StdFile::exists(filename.c_str()))
		//	break;

		std::string filename;
		bool bHit = false;
		for (const std::string& hisdir : _his_dir)
		{
			filename = hisdir;
			filename.append(pname).append("/").append(cInfo->_exchg).append(".")
				.append(cInfo->_product).append("_").append(ruleTag)
				.append(cInfo->isExright() ? (cInfo->_exright == 1 ? SUFFIX_QFQ_S : SUFFIX_HFQ_S) : "")
				.append(".dsb");
			if (StdFile::exists(filename.c_str()))
			{
				bHit = true;
				break;
			}
		}

		if (!bHit)
			break;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlock))
		{
			pipe_reader_log(_sink, LL_ERROR, u8"历史K线数据文件{}大小校验失败", filename);
			break;
		}
		if (!proc_block_data(content, true, false) ||
			content.size() % sizeof(WTSBarStruct) != 0)
		{
			pipe_reader_log(_sink, LL_ERROR, "Processing or validating wrapped {} bars from {} failed", pname.c_str(), filename.c_str());
			_bars_cache.erase(key);
			return false;
		}

		if (content.empty())
			break;

		uint32_t barcnt = content.size() / sizeof(WTSBarStruct);

		hotAy = new std::vector<WTSBarStruct>();
		hotAy->resize(barcnt);
		memcpy(hotAy->data(), content.data(), content.size());

		if (period != KP_DAY)
			lastHotTime = hotAy->at(barcnt - 1).time;
		else
			lastHotTime = hotAy->at(barcnt - 1).date;

		pipe_reader_log(_sink, LL_INFO, "{} items of back {} data of wrapped contract {} directly loaded", barcnt, pname.c_str(), stdCode);
	} while (false);
	

	HotSections secs;
	if (strlen(ruleTag) > 0)
	{
		if (!_hot_mgr->splitCustomSections(ruleTag, cInfo->stdCommID(), 19900102, endTDate, secs))
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
			const wtp::DateMappingResult64 sBound = dateMapping.getBoundaryTime(cInfo->stdCommID(), leftDt, false, true);
			const wtp::DateMappingResult64 eBound = dateMapping.getBoundaryTime(cInfo->stdCommID(), rightDt, false, false);
			if (sBound.status != wtp::DateMappingStatus::Ok || eBound.status != wtp::DateMappingStatus::Ok)
			{
				pipe_reader_log(_sink, LL_ERROR, "Mapping boundaries of {} section {}-{} failed: {}/{}", stdCode, leftDt, rightDt,
					wtp::dateMappingStatusName(sBound.status), wtp::dateMappingStatusName(eBound.status));
				for (std::vector<WTSBarStruct>* section : barsSections)
					delete section;
				barsSections.clear();
				if (hotAy != NULL)
					delete hotAy;
				_bars_cache.erase(key);
				return false;
			}
			const uint64_t sTime = sBound.value;
			const uint64_t eTime = eBound.value;

			sBar.date = leftDt;
			sBar.time = ((uint32_t)(sTime / 10000) - 19900000) * 10000 + (uint32_t)(sTime % 10000);

			if (sBar.time < lastHotTime)	//如果边界时间小于主力的最后一根Bar的时间, 说明已经有交叉了, 则不需要再处理了
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

		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader读取分月合约的K线数据
		 *	如果没有读到，再从文件读取
		 */
		bool bLoaded = false;
		std::string buffer;
		if (NULL != _loader)
		{
			std::string wCode = fmt::format("{}.{}.{}", cInfo->_exchg, cInfo->_product, (char*)curCode + strlen(cInfo->_product));
			bLoaded = _loader->loadRawHisBars(&buffer, wCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});
		}

		if (!bLoaded)
		{
			//std::stringstream ss;
			//ss << _his_dir << pname << "/" << cInfo->_exchg << "/" << curCode << ".dsb";
			//std::string filename = ss.str();
			//if (!StdFile::exists(filename.c_str()))
			//	continue;

			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append(pname).append("/").append(cInfo->_exchg).append("/").append(curCode).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}

			if (!bHit)
				continue;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_reader_log(_sink, LL_ERROR, "Sizechecking of his dta file {} failed", filename.c_str());
				for (std::vector<WTSBarStruct>* section : barsSections)
					delete section;
				barsSections.clear();
				if (hotAy != NULL)
					delete hotAy;
				_bars_cache.erase(key);
				return false;
			}
			if (!proc_block_data(content, true, false) ||
				content.size() % sizeof(WTSBarStruct) != 0)
			{
				pipe_reader_log(_sink, LL_ERROR, "Processing or validating {} bars from {} failed", pname.c_str(), filename.c_str());
				for (std::vector<WTSBarStruct>* section : barsSections)
					delete section;
				barsSections.clear();
				if (hotAy != NULL)
					delete hotAy;
				_bars_cache.erase(key);
				return false;
			}
			buffer.swap(content);
		}

		if (buffer.size() % sizeof(WTSBarStruct) != 0)
		{
			pipe_reader_log(_sink, LL_ERROR, "Validating {} bars for component {} failed", pname.c_str(), curCode);
			for (std::vector<WTSBarStruct>* section : barsSections)
				delete section;
			barsSections.clear();
			if (hotAy != NULL)
				delete hotAy;
			_bars_cache.erase(key);
			return false;
		}

		if(buffer.empty())
			break;

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		uint32_t sIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date < sBar.date) || (period != KP_DAY && pBar->time < sBar.time))	//早于边界时间
		{
			//早于边界时间, 说明没有数据了, 因为lower_bound会返回大于等于目标位置的数据
			continue;
		}

		pBar = std::lower_bound(firstBar + sIdx, firstBar + (barcnt - 1), eBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});
		uint32_t eIdx = pBar - firstBar;
		if ((period == KP_DAY && pBar->date > eBar.date) || (period != KP_DAY && pBar->time > eBar.time))
		{
			if (eIdx == 0)
				break;

			pBar--;
			eIdx--;
		}

		if (eIdx < sIdx)
			continue;

		uint32_t curCnt = eIdx - sIdx + 1;

		if(cInfo->isExright())
		{	
			double factor = hotSec._factor / baseFactor;
			for (uint32_t idx = sIdx; idx <= eIdx; idx++)
			{
				firstBar[idx].open *= factor;
				firstBar[idx].high *= factor;
				firstBar[idx].low *= factor;
				firstBar[idx].close *= factor;

				if (_adjust_flag & 1)
					firstBar[idx].vol /= factor;

				if (_adjust_flag & 2)
					firstBar[idx].money *= factor;

				if (_adjust_flag & 4)
				{
					firstBar[idx].hold /= factor;
					firstBar[idx].add /= factor;
				}
			}
		}		

		std::vector<WTSBarStruct>* tempAy = new std::vector<WTSBarStruct>();
		tempAy->resize(curCnt);
		memcpy(tempAy->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
		realCnt += curCnt;

		barsSections.emplace_back(tempAy);

		if (bAllCovered)
			break;
	}

	if (hotAy)
	{
		barsSections.emplace_back(hotAy);
		realCnt += hotAy->size();
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	pipe_reader_log(_sink,LL_INFO, "{} items of back {} data of {} cached", realCnt, pname.c_str(), stdCode);

	return true;
}

bool WtDataReader::cacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;

	// The former current-trading-date mapping here was never used; it is
	// removed so an unchecked clock/mapping cannot affect this load.

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo->_exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;

	std::vector<WTSBarStruct>* ayAdjusted = NULL;
	uint64_t lastQTime = 0;

	do
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	本来这里是要先调用_loader->loadRawHisBars从外部加载器读取复权数据的
		 *	但是上层会调用一次loadFinalHisBars，这里再调用loadRawHisBars就冗余了，所以直接跳过
		 */
		char flag = cInfo->_exright == 1 ? SUFFIX_QFQ : SUFFIX_HFQ;
		//std::stringstream ss;
		//ss << _his_dir << pname << "/" << cInfo->_exchg << "/" << cInfo->_code << flag << ".dsb";
		//std::string filename = ss.str();
		//if (!StdFile::exists(filename.c_str()))
		//	break;

		std::string filename;
		bool bHit = false;
		for (const std::string& hisdir : _his_dir)
		{
			filename = hisdir;
			filename.append(pname).append("/").append(cInfo->_exchg).append("/").append(cInfo->_code)
				.append(1, flag).append(".dsb");
			if (StdFile::exists(filename.c_str()))
			{
				bHit = true;
				break;
			}
		}
		if (!bHit)
			break;

		std::string content;
		StdFile::read_file_content(filename.c_str(), content);
		if (content.size() < sizeof(HisKlineBlock))
		{
			pipe_reader_log(_sink,LL_ERROR, "历史K线数据文件{}大小校验失败", filename.c_str());
			break;
		}

		if (!proc_block_data(content, true, false) ||
			content.size() % sizeof(WTSBarStruct) != 0)
		{
			pipe_reader_log(_sink, LL_ERROR, "Processing or validating adjusted {} bars from {} failed", pname.c_str(), filename.c_str());
			_bars_cache.erase(key);
			return false;
		}

		if (content.empty())
		{
			pipe_reader_log(_sink, LL_ERROR, u8"缓存{}的复权数据异常：复权历史数据为空", stdCode);
			break;
		}

		uint32_t barcnt = content.size() / sizeof(WTSBarStruct);

		ayAdjusted = new std::vector<WTSBarStruct>();
		ayAdjusted->resize(barcnt);
		memcpy(ayAdjusted->data(), content.data(), content.size());

		if (period != KP_DAY)
			lastQTime = ayAdjusted->at(barcnt - 1).time;
		else
			lastQTime = ayAdjusted->at(barcnt - 1).date;

		pipe_reader_log(_sink,LL_INFO, "{} items of adjusted back {} data of stock {} directly loaded", barcnt, pname.c_str(), stdCode);
	} while (false);


	[[maybe_unused]] bool bAllCovered = false;
	do
	{
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

		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader读取
		 *	如果没有读到，再从文件读取
		 */
		bool bLoaded = false;
		std::string buffer;
		std::string rawCode = fmt::format("{}.{}.{}", cInfo->_exchg, cInfo->_product, curCode);
		if (NULL != _loader)
		{
			bLoaded = _loader->loadRawHisBars(&buffer, rawCode.c_str(), period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
				std::string* buff = (std::string*)obj;
				buff->resize(sizeof(WTSBarStruct)*count);
				memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
			});
		}

		[[maybe_unused]] bool bOldVer = false;
		if (!bLoaded)
		{
			//std::stringstream ss;
			//ss << _his_dir << pname << "/" << cInfo->_exchg << "/" << curCode << ".dsb";
			//std::string filename = ss.str();
			//if (!StdFile::exists(filename.c_str()))
			//	continue;

			std::string filename;
			bool bHit = false;
			for (const std::string& hisdir : _his_dir)
			{
				filename = hisdir;
				filename.append(pname).append("/").append(cInfo->_exchg).append("/").append(cInfo->_code).append(".dsb");
				if (StdFile::exists(filename.c_str()))
				{
					bHit = true;
					break;
				}
			}
			if (!bHit)
				continue;

			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_reader_log(_sink,LL_ERROR, "历史K线数据文件{}大小校验失败", filename.c_str());
				for (std::vector<WTSBarStruct>* section : barsSections)
					delete section;
				barsSections.clear();
				if (ayAdjusted != NULL)
					delete ayAdjusted;
				_bars_cache.erase(key);
				return false;
			}

			if (!proc_block_data(content, true, false) ||
				content.size() % sizeof(WTSBarStruct) != 0)
			{
				pipe_reader_log(_sink, LL_ERROR, "Processing or validating {} bars from {} failed", pname.c_str(), filename.c_str());
				for (std::vector<WTSBarStruct>* section : barsSections)
					delete section;
				barsSections.clear();
				if (ayAdjusted != NULL)
					delete ayAdjusted;
				_bars_cache.erase(key);
				return false;
			}
			buffer.swap(content);
		}

		if (buffer.size() % sizeof(WTSBarStruct) != 0)
		{
			pipe_reader_log(_sink, LL_ERROR, "Validating {} bars for adjusted contract {} failed", pname.c_str(), rawCode.c_str());
			for (std::vector<WTSBarStruct>* section : barsSections)
				delete section;
			barsSections.clear();
			if (ayAdjusted != NULL)
				delete ayAdjusted;
			_bars_cache.erase(key);
			return false;
		}

		if(buffer.empty())
			break;

		uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);

		WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

		WTSBarStruct* pBar = std::lower_bound(firstBar, firstBar + (barcnt - 1), sBar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
			if (period == KP_DAY)
			{
				return a.date < b.date;
			}
			else
			{
				return a.time < b.time;
			}
		});

		if (pBar != NULL)
		{
			uint32_t sIdx = pBar - firstBar;
			uint32_t curCnt = barcnt - sIdx;

			std::vector<WTSBarStruct>* ayRaw = new std::vector<WTSBarStruct>();
			ayRaw->resize(curCnt);
			memcpy(ayRaw->data(), &firstBar[sIdx], sizeof(WTSBarStruct)*curCnt);
			realCnt += curCnt;

			auto& ayFactors = getAdjFactors(cInfo->_code, cInfo->_exchg, cInfo->_product);
			if (!ayFactors.empty())
			{
				//做复权处理
				int32_t lastIdx = curCnt;
				WTSBarStruct bar;
				firstBar = ayRaw->data();

				//根据复权类型确定基础因子
				//如果是前复权，则历史数据会变小，以最后一个复权因子为基础因子
				//如果是后复权，则新数据会变大，基础因子为1
				double baseFactor = 1.0;
				if (cInfo->_exright == 1)
					baseFactor = ayFactors.back()._factor;
				else if (cInfo->_exright == 2)
					barList._factor = ayFactors.back()._factor;

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
						int32_t curIdx = pBar - firstBar;
						while (pBar && curIdx < lastIdx)
						{
							pBar->open *= factor;
							pBar->high *= factor;
							pBar->low *= factor;
							pBar->close *= factor;

							if (_adjust_flag & 1)
								pBar->vol /= factor;

							if (_adjust_flag & 2)
								pBar->money *= factor;

							if (_adjust_flag & 4)
							{
								pBar->hold /= factor;
								pBar->add /= factor;
							}

							pBar++;
							curIdx++;
						}
						lastIdx = endBar - firstBar;
					}

					if (lastIdx == 0)
						break;
				}
			}

			barsSections.emplace_back(ayRaw);
		}
	} while (false);

	if (ayAdjusted)
	{
		barsSections.emplace_back(ayAdjusted);
		realCnt += ayAdjusted->size();
	}

	if (realCnt > 0)
	{
		barList._bars.resize(realCnt);

		uint32_t curIdx = 0;
		for (auto it = barsSections.rbegin(); it != barsSections.rend(); it++)
		{
			std::vector<WTSBarStruct>* tempAy = *it;
			memcpy(barList._bars.data() + curIdx, tempAy->data(), tempAy->size() * sizeof(WTSBarStruct));
			curIdx += tempAy->size();
			delete tempAy;
		}
		barsSections.clear();
	}

	pipe_reader_log(_sink,LL_INFO, "{} items of back {} data of {} cached", realCnt, pname.c_str(), stdCode);

	return true;
}

bool WtDataReader::cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo->_exchg, cInfo->_product);
	if (commInfo == NULL)
	{
		// I03-NEW-052
		pipe_reader_log(_sink, LL_ERROR, "Commodity of {} not found, history bars not loaded", stdCode == NULL ? "" : stdCode);
		return false;
	}

	// The former current-trading-date mapping here was never used; it is
	// removed so an unchecked clock/mapping cannot affect this load.

	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo->_exchg;

	std::vector<std::vector<WTSBarStruct>*> barsSections;

	uint32_t realCnt = 0;
	const char* ruleTag = cInfo->_ruletag;
	if (strlen(ruleTag) > 0)
	{
		//如果是读取期货主力连续数据
		try
		{
			return cacheIntegratedBars(cInfo, key, stdCode, period);
		}
		catch (const std::exception& ex)
		{
			_bars_cache.erase(key);
			pipe_reader_log(_sink, LL_ERROR, u8"缓存{}的连续{}数据异常：{}", stdCode, ruleTag, ex.what());
		}
		catch(...)
		{
			_bars_cache.erase(key);
			pipe_reader_log(_sink, LL_ERROR, u8"缓存{}的连续{}数据异常", stdCode, ruleTag);
		}
		return false;
	}
	else if(cInfo->isExright() && commInfo->isStock())
	{
		//如果是读取股票复权数据
		try
		{
			return cacheAdjustedStkBars(cInfo, key, stdCode, period);
		}
		catch (const std::exception& ex)
		{
			pipe_reader_log(_sink, LL_ERROR, u8"缓存{}的复权数据异常：{}", stdCode, ex.what());
		}
		catch (...)
		{
			pipe_reader_log(_sink, LL_ERROR, u8"缓存{}的复权{}数据异常", stdCode, ruleTag);
		}
		return false;
	}

	
	//直接原始数据直接加载

	/*
	 *	By Wesley @ 2021.12.20
	 *	先从extloader读取
	 *	如果没有读到，再从文件读取
	 */
	bool bLoaded = false;
	std::string buffer;
	if (NULL != _loader)
	{
		bLoaded = _loader->loadRawHisBars(&buffer, stdCode, period, [](void* obj, WTSBarStruct* bars, uint32_t count) {
			std::string* buff = (std::string*)obj;
			buff->resize(sizeof(WTSBarStruct)*count);
			memcpy((void*)buff->c_str(), bars, sizeof(WTSBarStruct)*count);
		});
	}

	if (!bLoaded)
	{
		//读取历史的
		//std::stringstream ss;
		//ss << _his_dir << pname << "/" << cInfo->_exchg << "/" << cInfo->_code << ".dsb";

		std::string filename;
		bool bHit = false;
		for (const std::string& hisdir : _his_dir)
		{
			filename = hisdir;
			filename.append(pname).append("/").append(cInfo->_exchg).append("/").append(cInfo->_code).append(".dsb");
			if (StdFile::exists(filename.c_str()))
			{
				bHit = true;
				break;
			}
		}

		if (bHit)
		{
			//如果有格式化的历史数据文件, 则直接读取
			std::string content;
			StdFile::read_file_content(filename.c_str(), content);
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_reader_log(_sink,LL_ERROR, u8"历史K线数据文件{}大小校验失败", filename.c_str());
				_bars_cache.erase(key);
				return false;
			}

			if (!proc_block_data(content, true, false) ||
				content.size() % sizeof(WTSBarStruct) != 0)
			{
				pipe_reader_log(_sink, LL_ERROR, "Processing or validating historical {} bars from {} failed", pname.c_str(), filename.c_str());
				_bars_cache.erase(key);
				return false;
			}
			buffer.swap(content);
		}
	}

	if (buffer.size() % sizeof(WTSBarStruct) != 0)
	{
		pipe_reader_log(_sink, LL_ERROR, "Validating historical {} bars failed", pname.c_str());
		_bars_cache.erase(key);
		return false;
	}

	if (buffer.empty())
		return false;

	uint32_t barcnt = buffer.size() / sizeof(WTSBarStruct);

	WTSBarStruct* firstBar = (WTSBarStruct*)buffer.data();

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

	pipe_reader_log(_sink,LL_INFO, "{} items of back {} data of {} cached", realCnt, pname.c_str(), stdCode);
	return true;
}

WTSKlineSlice* WtDataReader::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	// K61 (BUG-I02-NEW-060, user decision 2026-11-06): storage and external loaders only hold
	// the base periods; other periods are resampled upstream and are rejected here.
	if (period != KP_Minute1 && period != KP_Minute5 && period != KP_DAY)
	{
		pipe_reader_log(_sink, LL_ERROR, "Unsupported kline period {} of {} requested, only m1/m5/d are stored", (uint32_t)period, stdCode == NULL ? "" : stdCode);
		return NULL;
	}

	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return NULL;

	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	const char* stdPID = cInfo.stdCommID();

	// I03-NEW-039: caller stdCode may exceed a fixed 64-byte key buffer.
	const std::string key = fmt::format("{}#{}", stdCode, (uint32_t)period);
	auto it = _bars_cache.find(key);
	bool bHasHisData = false;
	if (it == _bars_cache.end())
	{
		/*
		 *	By Wesley @ 2021.12.20
		 *	先从extloader加载最终的K线数据（如果是复权）
		 *	如果加载失败，则再从文件加载K线数据
		 */
		bHasHisData = cacheFinalBarsFromLoader(&cInfo, key, stdCode, period);

		if(!bHasHisData)
		{
			bHasHisData = cacheHisBarsFromFile(&cInfo, key, stdCode, period);
			const bool retryAfterHistoryFailure = !bHasHisData && _bars_cache.find(key) == _bars_cache.end();
			if (retryAfterHistoryFailure)
				return NULL;
		}
	}
	else
	{
		bHasHisData = true;
	}

	if(!bHasHisData)
	{
		pipe_reader_log(_sink, LL_DEBUG, "No {} bars of {} loaded", PERIOD_NAME[period], stdCode);
	}

	uint32_t curDate, curTime;
	if (etime == 0)
	{
		curDate = _sink->get_date();
		curTime = _sink->get_min_time();
		etime = (uint64_t)curDate * 10000 + curTime;
	}
	else
	{
		curDate = (uint32_t)(etime / 10000);
		curTime = (uint32_t)(etime % 10000);
	}

	wtp::DateMappingClient dateMapping = _date_binding.client(_base_data_mgr);
	uint32_t endTDate = 0, curTDate = 0;
	if (!reader_trading_date(_sink, dateMapping, stdPID, curDate, curTime, endTDate) ||
		!reader_trading_date(_sink, dateMapping, stdPID, 0, 0, curTDate))
		return NULL;

	BarsList& barsList = _bars_cache[key];
	WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, NULL, 0);
	if (slice == NULL)	// I03-NEW-083: rejected (e.g. overlong) code
		return NULL;
	WTSBarStruct* head = NULL;
	uint32_t hisCnt = 0;
	uint32_t rtCnt = 0;
	uint32_t totalCnt = 0;
	std::string pname;
	switch (period)
	{
	case KP_Minute1: pname = "min1"; break;
	case KP_Minute5: pname = "min5"; break;
	default: pname = "day"; break;
	}

	uint32_t left = count;

	//是否包含当天的
	bool bHasToday = (endTDate == curTDate);

	//By Wesley @ 2022.05.28
	//不需要区分是否是期货了
	const char* ruleTag = cInfo._ruletag;
	if (strlen(ruleTag) > 0)
	{
		barsList._raw_code = _hot_mgr->getCustomRawCode(ruleTag, stdPID, curTDate);
		pipe_reader_log(_sink, LL_INFO, "{} contract on {} confirmed: {} -> {}", ruleTag, curTDate, stdCode, barsList._raw_code.c_str());
	}
	else
	{
		barsList._raw_code = cInfo._code;
	}

	if (bHasToday)
	{
		WTSBarStruct bar;
		bar.date = curDate;
		bar.time = (curDate - 19900000) * 10000 + curTime;

		const char* curCode = barsList._raw_code.c_str();

		//读取实时的
		RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
		// 卡13：只读取已发布且落在本进程映射内的记录
		const uint32_t visibleBars = (kPair != NULL && kPair->_block && kPair->_file)
			? wt_rtpub::visibleRecords(kPair->_block, kPair->_file->size(), sizeof(RTKlineBlock), sizeof(WTSBarStruct)) : 0;
		if (visibleBars > 0)
		{
			//读取当日的数据
			WTSBarStruct* pBar = NULL;
			pBar = std::lower_bound(kPair->_block->_bars, kPair->_block->_bars + (visibleBars - 1), bar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
				if (period == KP_DAY)
					return a.date < b.date;
				else
					return a.time < b.time;
			});

			uint32_t idx = 0;
			if (pBar != NULL)
				idx = pBar - kPair->_block->_bars;
			else
				idx = visibleBars;

			if ((period == KP_DAY && pBar->date > bar.date) || (period != KP_DAY && pBar->time > bar.time))
			{
				pBar--;
				idx--;
			}

			uint32_t sIdx = 0;
			if (left <= idx + 1)
			{
				sIdx = idx - left + 1;
			}

			uint32_t curCnt = (idx - sIdx + 1);
			left -= (idx - sIdx + 1);
			hisCnt = bHasHisData ? left : 0;
			rtCnt = curCnt;
			//By Wesley @ 2022.05.28
			//连续合约也要支持复权
			if(cInfo._exright == 2/* && commInfo->isStock()*/)
			{
				//后复权数据要把最新的数据进行复权处理，所以要作为历史数据追加到尾部
				//虽然后复权数据要进行复权处理，但是实时数据的位置标记也要更新到最新，不然OnMinuteEnd会从开盘开始回放的
				//复权数据是创建副本后修改
				if (barsList._rt_cursor == UINT_MAX || idx > barsList._rt_cursor)
				{
					uint32_t copyStart = barsList._rt_cursor == UINT_MAX ? 0 : barsList._rt_cursor + 1;
					uint32_t copyCnt = idx - copyStart + 1;					
					barsList._rt_cursor = idx;
					double factor = barsList._factor;
					uint32_t oldSize = barsList._bars.size();
					uint32_t newSize = oldSize + copyCnt;
					barsList._bars.resize(newSize);
					memcpy(&barsList._bars[oldSize], &kPair->_block->_bars[copyStart], sizeof(WTSBarStruct)* copyCnt);
					for(uint32_t thisIdx = oldSize; thisIdx < newSize; thisIdx++)
					{
						WTSBarStruct* pBar = &barsList._bars[thisIdx];
						pBar->open *= factor;
						pBar->high *= factor;
						pBar->low *= factor;
						pBar->close *= factor;
					}
				}
				totalCnt = hisCnt + rtCnt;
				totalCnt = min(totalCnt, (uint32_t)barsList._bars.size());
				// 复权后的数据直接从barlist中截取
				if (totalCnt > 0)
				{
					head = &barsList._bars[barsList._bars.size() - totalCnt];
					slice->appendBlock(head, totalCnt);
				}
			}
			else
			{
				// 普通数据由历史和rt拼接，其中rt直接引用
				barsList._rt_cursor = idx;
				hisCnt = min(hisCnt, (uint32_t)barsList._bars.size());
				if (hisCnt > 0)
				{
					head = &barsList._bars[barsList._bars.size() - hisCnt];
					slice->appendBlock(head, hisCnt);
				}
				// 添加rt
				if (rtCnt > 0)
				{
					head = &kPair->_block->_bars[sIdx];
					slice->appendBlock(head, rtCnt);
					slice->addOwner(kPair->_file);
				}
			}
		}
		else
		{
			rtCnt = 0;
			hisCnt = count;
			hisCnt = min(hisCnt, (uint32_t)barsList._bars.size());
			if(hisCnt != 0)
			{
				head = &barsList._bars[barsList._bars.size() - hisCnt];
				slice->appendBlock(head, hisCnt);
			}
		}
	}
	else
	{
		rtCnt = 0;
		const std::vector<WTSBarStruct>& historicalBars = barsList._bars;
		if (period == KP_DAY)
		{
			// Daily bars are keyed by trading date, which may differ from the
			// calendar date for contracts with overnight sessions.
			auto end = std::upper_bound(historicalBars.begin(), historicalBars.end(), endTDate,
				[](uint32_t tradingDate, const WTSBarStruct& bar) {
					return tradingDate < bar.date;
				});
			const size_t available = static_cast<size_t>(end - historicalBars.begin());
			hisCnt = static_cast<uint32_t>(min(static_cast<size_t>(count), available));
			if (hisCnt > 0)
			{
				head = &barsList._bars[available - hisCnt];
				slice->appendBlock(head, hisCnt);
			}
		}
		else
		{
			// Minute bar time is encoded as (calendarDate - 19900000) * 10000
			// plus HHMM. Compare on that timeline so overnight bars after
			// midnight are ordered correctly, regardless of their trading date.
			const uint64_t endTime = TimeUtils::timeToMinBar(curDate, curTime);
			auto end = std::upper_bound(historicalBars.begin(), historicalBars.end(), endTime,
				[](uint64_t minuteTime, const WTSBarStruct& bar) {
					return minuteTime < bar.time;
				});
			const size_t available = static_cast<size_t>(end - historicalBars.begin());
			hisCnt = static_cast<uint32_t>(min(static_cast<size_t>(count), available));
			if (hisCnt > 0)
			{
				head = &barsList._bars[available - hisCnt];
				slice->appendBlock(head, hisCnt);
			}
		}
	}

	pipe_reader_log(_sink, LL_DEBUG, "His {} bars of {} loaded, {} from history, {} from realtime", PERIOD_NAME[period], stdCode, hisCnt, rtCnt);
	return slice;
}

WtDataReader::TickBlockPair* WtDataReader::getRTTickBlock(const char* exchg, const char* code)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", exchg, code);

	std::string path;

	bool bHit = false;
	for (const std::string& rtdir : _rt_dir)
	{
		path = rtdir;
		path.append("ticks/").append(exchg).append("/").append(code).append(".dmb");
		bHit = StdFile::exists(path.c_str());
		if(bHit)
			break;
	}

	if (!bHit)
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
	// 卡13：除容量变化外，若已发布容量超过本进程映射覆盖的记录数（映射早于扩容）也要重新映射
	else if (block._last_cap != block._block->_capacity
		|| wt_rtpub::publishedCapacity(block._block) > wt_rtpub::mappedRecords(block._file->size(), sizeof(RTTickBlock), sizeof(WTSTickStruct)))
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

	return &block;
}

WtDataReader::OrdDtlBlockPair* WtDataReader::getRTOrdDtlBlock(const char* exchg, const char* code)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", exchg, code);

	std::string path;
	//fmtutil::format_to(path, "{}orders/{}/{}.dmb", _rt_dir.c_str(), exchg, code);

	//if (!StdFile::exists(path))
	//	return NULL;

	bool bHit = false;
	for (const std::string& rtdir : _rt_dir)
	{
		path = rtdir;
		path.append("orders/").append(exchg).append("/").append(code).append(".dmb");
		bHit = StdFile::exists(path.c_str());
		if (bHit)
			break;
	}

	if (!bHit)
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
	// 卡13：除容量变化外，若已发布容量超过本进程映射覆盖的记录数（映射早于扩容）也要重新映射
	else if (block._last_cap != block._block->_capacity
		|| wt_rtpub::publishedCapacity(block._block) > wt_rtpub::mappedRecords(block._file->size(), sizeof(RTOrdDtlBlock), sizeof(WTSOrdDtlStruct)))
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

	return &block;
}

WtDataReader::OrdQueBlockPair* WtDataReader::getRTOrdQueBlock(const char* exchg, const char* code)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", exchg, code);

	std::string path;
	//fmtutil::format_to(path, "{}queue/{}/{}.dmb", _rt_dir.c_str(), exchg, code);

	//if (!StdFile::exists(path))
	//	return NULL;

	bool bHit = false;
	for (const std::string& rtdir : _rt_dir)
	{
		path = rtdir;
		path.append("queue/").append(exchg).append("/").append(code).append(".dmb");
		bHit = StdFile::exists(path.c_str());
		if (bHit)
			break;
	}

	if (!bHit)
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
	// 卡13：除容量变化外，若已发布容量超过本进程映射覆盖的记录数（映射早于扩容）也要重新映射
	else if (block._last_cap != block._block->_capacity
		|| wt_rtpub::publishedCapacity(block._block) > wt_rtpub::mappedRecords(block._file->size(), sizeof(RTOrdQueBlock), sizeof(WTSOrdQueStruct)))
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

	return &block;
}

WtDataReader::TransBlockPair* WtDataReader::getRTTransBlock(const char* exchg, const char* code)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", exchg, code);

	std::string path;
	//fmtutil::format_to(path, "{}trans/{}/{}.dmb", _rt_dir.c_str(), exchg, code);

	//if (!StdFile::exists(path))
	//	return NULL;

	bool bHit = false;
	for (const std::string& rtdir : _rt_dir)
	{
		path = rtdir;
		path.append("trans/").append(exchg).append("/").append(code).append(".dmb");
		bHit = StdFile::exists(path.c_str());
		if (bHit)
			break;
	}

	if (!bHit)
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
	// 卡13：除容量变化外，若已发布容量超过本进程映射覆盖的记录数（映射早于扩容）也要重新映射
	else if (block._last_cap != block._block->_capacity
		|| wt_rtpub::publishedCapacity(block._block) > wt_rtpub::mappedRecords(block._file->size(), sizeof(RTTransBlock), sizeof(WTSTransStruct)))
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

	return &block;
}

WtDataReader::RTKlineBlockPair* WtDataReader::getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period)
{
	if (period != KP_Minute1 && period != KP_Minute5)
		return NULL;

	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}.{}", exchg, code);

	RTKBlockFilesMap* cache_map = NULL;
	std::string subdir = "";
	[[maybe_unused]] BlockType bType;
	switch (period)
	{
	case KP_Minute1:
		cache_map = &_rt_min1_map;
		subdir = "min1";
		bType = BT_RT_Minute1;
		break;
	case KP_Minute5:
		cache_map = &_rt_min5_map;
		subdir = "min5";
		bType = BT_RT_Minute5;
		break;
	default: break;
	}

	std::string path;
	//fmtutil::format_to(path, "{}{}/{}/{}.dmb", _rt_dir, subdir, exchg, code);

	//if (!StdFile::exists(path))
	//	return NULL;

	bool bHit = false;
	for (const std::string& rtdir : _rt_dir)
	{
		path = rtdir;
		path.append(subdir).append("/").append(exchg).append("/").append(code).append(".dmb");
		bHit = StdFile::exists(path.c_str());
		if (bHit)
			break;
	}

	if (!bHit)
		return NULL;

	RTKlineBlockPair& block = (*cache_map)[key];
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
		pipe_reader_log(_sink, LL_DEBUG, "RT {} block of {}.{} loaded", subdir.c_str(), exchg, code);
	}
	// 卡13：除容量变化外，若已发布容量超过本进程映射覆盖的记录数（映射早于扩容）也要重新映射
	else if (block._last_cap != block._block->_capacity
		|| wt_rtpub::publishedCapacity(block._block) > wt_rtpub::mappedRecords(block._file->size(), sizeof(RTKlineBlock), sizeof(WTSBarStruct)))
	{
		//说明文件大小已变, 需要重新映射
		pipe_reader_log(_sink, LL_DEBUG, "RT {} block of {}.{} expanded to {}, remapping...", subdir.c_str(), exchg, code, block._block->_capacity);

		block._file.reset(new BoostMappingFile());
		block._last_cap = 0;
		block._block = NULL;

		if (!block._file->map(path.c_str(), boost::interprocess::read_only, boost::interprocess::read_only))
			return NULL;

		block._block = (RTKlineBlock*)block._file->addr();
		block._last_cap = block._block->_capacity;
	}	

	return &block;
}

void WtDataReader::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t /*endTDate*/ /* = 0 */)
{
	if (_rt_dir.empty() || _sink == NULL || _base_data_mgr == NULL)
		return;

	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	if (nowTime <= _last_time)
		return;

	for (auto it = _bars_cache.begin(); it != _bars_cache.end(); it++)
	{
		BarsList& barsList = (BarsList&)it->second;
		if (barsList._period != KP_DAY)
		{
			if (!barsList._raw_code.empty())
			{
				RTKlineBlockPair* kBlk = getRTKilneBlock(barsList._exchg.c_str(), barsList._raw_code.c_str(), barsList._period);
				if (kBlk == NULL)
					continue;

				//确定上一次的读取过的实时K线条数
				uint32_t preCnt = 0;
				//如果实时K线没有初始化过，则已读取的条数为0
				//如果已经初始化过，则已读取的条数为光标+1
				if (barsList._rt_cursor == UINT_MAX)
					preCnt = 0;
				else
					preCnt = barsList._rt_cursor + 1;

				for (;;)
				{
					if (kBlk->_block == NULL || kBlk->_file == NULL
						|| wt_rtpub::visibleRecords(kBlk->_block, kBlk->_file->size(), sizeof(RTKlineBlock), sizeof(WTSBarStruct)) <= preCnt)
						break;

					WTSBarStruct& nextBar = kBlk->_block->_bars[preCnt];

					uint64_t barTime = 199000000000 + nextBar.time;
					if (barTime <= nowTime)
					{
						//如果不是后复权，则直接回调onbar
						//如果是后复权，则将最新bar复权处理以后，添加到cache中，再回调onbar
						if(barsList._factor == DBL_MAX)
						{
							_sink->on_bar(barsList._code.c_str(), barsList._period, &nextBar);
						}
						else
						{
							WTSBarStruct cpBar = nextBar;
							cpBar.open *= barsList._factor;
							cpBar.high *= barsList._factor;
							cpBar.low *= barsList._factor;
							cpBar.close *= barsList._factor;

							barsList._bars.emplace_back(cpBar);

							_sink->on_bar(barsList._code.c_str(), barsList._period, &barsList._bars[barsList._bars.size()-1]);
						}
					}
					else
					{
						break;
					}

					preCnt++;
				}

				//如果已处理的K线条数不为0，则修改光标位置
				if (preCnt > 0)
					barsList._rt_cursor = preCnt - 1;
			}
		}
		//这一段逻辑没有用了，在实盘中日线是不会闭合的，所以也不存在当日K线闭合的情况
		//实盘中都通过ontick处理当日实时数据
		//else if (barsList._period == KP_DAY)
		//{
		//	if (barsList._his_cursor != UINT_MAX && barsList._bars.size() - 1 > barsList._his_cursor)
		//	{
		//		for (;;)
		//		{
		//			WTSBarStruct& nextBar = barsList._bars[barsList._his_cursor + 1];

		//			if (nextBar.date <= endTDate)
		//			{
		//				_sink->on_bar(barsList._code.c_str(), barsList._period, &nextBar);
		//			}
		//			else
		//			{
		//				break;
		//			}

		//			barsList._his_cursor++;

		//			if (barsList._his_cursor == barsList._bars.size() - 1)
		//				break;
		//		}
		//	}
		//}
	}

	if (_sink)
		_sink->on_all_bar_updated(uTime);

	_last_time = nowTime;
}

double WtDataReader::getAdjFactorByDate(const char* stdCode, uint32_t date /* = 0 */)
{
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

	if(it == factList.end())
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

const char* WtDataReader::getStableAdjFactorLoaderKey(const std::string& key)
{
	std::lock_guard<std::mutex> lock(_adj_factor_loader_keys_mutex);
	auto stableKey = _adj_factor_loader_keys.emplace(key).first;
	return stableKey->c_str();
}

const WtDataReader::AdjFactorList& WtDataReader::getAdjFactors(const char* code, const char* exchg, const char* pid)
{
	const std::string key = fmt::format("{}.{}.{}", exchg, pid, code);

	auto it = _adj_factors.find(key);
	if (it == _adj_factors.end())
	{
		//By Wesley @ 2021.12.21
		//如果没有复权因子，就从extloader按需读一次
		if (_loader)
		{
			const char* loaderKey = getStableAdjFactorLoaderKey(key);
			if(_sink) pipe_reader_log(_sink,LL_INFO, "No adjusting factors of {} cached, searching via extented loader...", key);
			_loader->loadAdjFactors(this, loaderKey, [](void* obj, const char* stdCode, uint32_t* dates, double* factors, uint32_t count) {
				WtDataReader* self = (WtDataReader*)obj;
				AdjFactorList& fctrLst = self->_adj_factors[stdCode];

				for (uint32_t i = 0; i < count; i++)
				{
					AdjFactor adjFact;
					adjFact._date = dates[i];
					adjFact._factor = factors[i];

					fctrLst.emplace_back(adjFact);
				}

				//一定要把第一条加进去，不然如果是前复权的话，可能会漏处理最早的数据
				AdjFactor adjFact;
				adjFact._date = 19900101;
				adjFact._factor = 1;
				fctrLst.emplace_back(adjFact);

				std::sort(fctrLst.begin(), fctrLst.end(), [](const AdjFactor& left, const AdjFactor& right) {
					return left._date < right._date;
				});

				pipe_reader_log(self->_sink, LL_INFO, "{} items of adjusting factors of {} loaded via extended loader", count, stdCode);
			});
		}
	}

	return _adj_factors[key];
}
