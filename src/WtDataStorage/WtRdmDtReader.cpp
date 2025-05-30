/**
 * @file WtRdmDtReader.cpp
 * @author Wesley
 * @brief WonderTrader随机数据访问器实现文件
 * @version 0.1
 * @date 2022-01-05
 * 
 * @copyright Copyright (c) 2022-2025
 * 
 * @details 该文件实现了WtRdmDtReader类，提供了高效的数据随机访问功能，
 * 支持K线、Tick、委托明细、委托队列和成交数据的读取。
 * 数据管理包括实时和历史数据，支持按时间范围、数量和日期等多种方式查询。
 */

#include "WtRdmDtReader.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSSessionInfo.hpp"

#include "../WTSUtils/WTSCmpHelper.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

#include <rapidjson/document.h>
namespace rj = rapidjson;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
/**
 * @brief 日志输出函数，使用fmt格式化字符串并发送到日志接收器
 * @tparam Args 可变参数类型列表
 * @param sink 日志接收器指针
 * @param ll 日志级别
 * @param format 格式化字符串模板
 * @param args 可变参数列表
 * 
 * @details 该函数将格式化后的日志信息通过IRdmDtReaderSink接口的reader_log方法输出
 */
template<typename... Args>
inline void pipe_rdmreader_log(IRdmDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmtutil::format_to(buffer, format, args...);

	sink->reader_log(ll, buffer);
}

extern "C"
{
	/**
	 * @brief 创建IRdmDtReader接口实例
	 * @return IRdmDtReader* 新创建的接口实例指针
	 * 
	 * @details 创建WtRdmDtReader对象并返回其接口指针，供外部模块使用
	 */
	EXPORT_FLAG IRdmDtReader* createRdmDtReader()
	{
		IRdmDtReader* ret = new WtRdmDtReader();
		return ret;
	}

	/**
	 * @brief 删除IRdmDtReader接口实例
	 * @param reader 要删除的接口实例指针
	 * 
	 * @details 安全地删除传入的IRdmDtReader接口实例，释放相关资源
	 */
	EXPORT_FLAG void deleteRdmDtReader(IRdmDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};

/**
 * @brief 处理数据块
 * @param content 数据内容字符串，传入传出参数
 * @param isBar 是否是K线数据块
 * @param bKeepHead 是否保留块头部，默认为true
 * @return bool 处理是否成功
 * 
 * @details 外部函数，用于处理不同类型的数据块，如Tick块和K线块等。
 * 该函数负责将读取的原始数据块内容进行解析和处理，使其可以被后续程序直接使用。
 */
extern bool proc_block_data(std::string& content, bool isBar, bool bKeepHead = true);

/**
 * @brief WtRdmDtReader类的构造函数
 * 
 * @details 初始化随机数据访问器对象，设置初始状态。
 * 将基础数据管理器、主力合约管理器设置为空，
 * 并将停止标志初始化为false。
 */
WtRdmDtReader::WtRdmDtReader()
	: _base_data_mgr(NULL)
	, _hot_mgr(NULL)
	, _stopped(false)
{
}


/**
 * @brief WtRdmDtReader类的析构函数
 * 
 * @details 清理资源，停止检查线程，并等待线程安全结束。
 * 首先设置_stopped标志为true，通知后台线程停止运行，
 * 然后等待线程结束，避免资源泄漏。
 */
WtRdmDtReader::~WtRdmDtReader()
{
	_stopped = true;
	if (_thrd_check)
		_thrd_check->join();
}

/**
 * @brief 初始化随机数据访问器
 * @param cfg 配置项参数，包含数据路径和除权因子文件等配置
 * @param sink 数据收集器接口，用于日志输出和获取其他管理器
 * 
 * @details 根据配置参数初始化随机数据访问器，设置数据路径、加载除权因子，
 * 并启动资源清理线程，定期释放长时间未访问的数据块缓存。
 * 该方法首先收集基础数据管理器和主力合约管理器，然后根据配置初始化
 * 数据路径并加载除权因子文件。最后创建内存资源清理线程，定期清理
 * 长时间不使用的数据块，以节省内存使用。
 */
void WtRdmDtReader::init(WTSVariant* cfg, IRdmDtReaderSink* sink)
{
	_sink = sink;

	_base_data_mgr = _sink->get_basedata_mgr();
	_hot_mgr = _sink->get_hot_mgr();

	if (cfg == NULL)
		return ;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	bool bAdjLoaded = false;
	
	if (!bAdjLoaded && cfg->has("adjfactor"))
		loadStkAdjFactorsFromFile(cfg->getCString("adjfactor"));

	_thrd_check.reset(new StdThread([this]() {
		while(!_stopped)
		{
			std::this_thread::sleep_for(std::chrono::seconds(5));
			uint64_t now = TimeUtils::getLocalTimeNow();

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


/**
 * @brief 从文件加载股票除权因子
 * @param adjfile 除权因子文件路径
 * @return bool 加载是否成功
 * 
 * @details 该方法从指定的JSON文件中加载股票除权因子数据，
 * 用于实现前复权或者后复权的功能。文件格式为交易所->股票代码->除权因子列表。
 * 除权因子用于调整股票历史价格，使得股票分红除权后的价格可比。
 */
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

/**
 * @brief 按日期读取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param uDate 交易日期，默认为0（表示当前日期）
 * @return WTSTickSlice* Tick数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码和日期，获取对应日期的全部Tick数据切片。
 * 如果是当日数据，会同时查询实时数据和历史数据。
 * 如果是期货合约，会根据访问规则获取真实合约的数据。
 */
WTSTickSlice* WtRdmDtReader::readTickSliceByDate(const char* stdCode, uint32_t uDate )
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	const char* stdPID = commInfo->getFullPid();

	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);
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
				pipe_rdmreader_log(_sink, LL_INFO, "{} contract on {} confirmed with rule {}: {} -> {}", ruleTag, uDate, stdCode, curCode.c_str());
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

				HisTBlockPair& tBlkPair = _his_tick_map[key];
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of tick data file {} failed", filename.c_str());
					tBlkPair._buffer.clear();
					break;
				}

				proc_block_data(tBlkPair._buffer, false, true);
				tBlkPair._block = (HisTickBlock*)tBlkPair._buffer.c_str();
				bHasHisTick = true;
				break;
			}
		}

		while (bHasHisTick)
		{
			HisTBlockPair& tBlkPair = _his_tick_map[key];
			if (tBlkPair._block == NULL)
				break;

			HisTickBlock* tBlock = tBlkPair._block;

			uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTickBlock)) / sizeof(WTSTickStruct);
			if (tcnt <= 0)
				break;

			WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, tcnt);
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
		
		WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, tBlock->_size);
		return slice;
	}

	return NULL;
}

/**
 * @brief 按时间范围读取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSTickSlice* Tick数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码和时间范围，获取对应的Tick数据切片。
 * 时间格式为年月日时分秒毫秒，例如：20190807124533900。
 * 数据包括历史Tick数据和实时Tick数据，会根据时间范围自动判断是否需要根据交易日分批处理数据。
 */
WTSTickSlice* WtRdmDtReader::readTickSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID, lDate, lTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

	bool hasToday = (endTDate >= curTDate);

	WTSTickSlice* slice = WTSTickSlice::create(stdCode, NULL, 0);

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

				HisTBlockPair& tBlkPair = _his_tick_map[key];
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of tick data file {} failed", filename.c_str());
					tBlkPair._buffer.clear();
					break;
				}

				proc_block_data(tBlkPair._buffer, false, true);
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

			HisTBlockPair& tBlkPair = _his_tick_map[key];
			if (tBlkPair._block == NULL)
				break;

			HisTickBlock* tBlock = tBlkPair._block;

			uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTickBlock)) / sizeof(WTSTickStruct);
			if (tcnt <= 0)
				break;

			WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tcnt - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t eIdx = pTick - tBlock->_ticks;
			if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
			{
				pTick--;
				eIdx--;
			}

			if (beginTDate != nowTDate)
			{
				//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
				//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, eIdx + 1);
				//ayTicks->append(slice, false);
				slice->appendBlock(tBlock->_ticks, eIdx + 1);
			}
			else
			{
				//如果交易日相同，则查找起始的位置
				pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + eIdx, sTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
					if (a.action_date != b.action_date)
						return a.action_date < b.action_date;
					else
						return a.action_time < b.action_time;
				});

				std::size_t sIdx = pTick - tBlock->_ticks;
				//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, eIdx - sIdx + 1);
				//ayTicks->append(slice, false);
				slice->appendBlock(tBlock->_ticks + sIdx, eIdx - sIdx + 1);
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

		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tBlock->_size - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pTick - tBlock->_ticks;

		//如果光标定位的tick时间比目标时间大, 则全部回退一个
		if (pTick->action_date > eTick.action_date || pTick->action_time > eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		if (beginTDate != curTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks, eIdx + 1);
			//ayTicks->append(slice, false);
			slice->appendBlock(tBlock->_ticks, eIdx + 1);
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + eIdx, sTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pTick - tBlock->_ticks;
			//WTSTickSlice* slice = WTSTickSlice::create(stdCode, tBlock->_ticks + sIdx, eIdx - sIdx + 1);
			//ayTicks->append(slice, false);
			slice->appendBlock(tBlock->_ticks + sIdx, eIdx - sIdx + 1);
		}
		break;
	}

	return slice;
}

/**
 * @brief 按时间范围读取委托队列数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSOrdQueSlice* 委托队列数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码和时间范围，获取对应的委托队列数据切片。
 * 时间格式为年月日时分秒毫秒，例如：20190807124533900。
 * 数据包括历史委托队列数据和实时委托队列数据，会根据时间范围自动判断是否需要根据交易日分批处理数据。
 */
WTSOrdQueSlice* WtRdmDtReader::readOrdQueSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID, lDate, lTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

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

		WTSOrdQueStruct* pItem = std::lower_bound(rtBlock->_queues, rtBlock->_queues + (rtBlock->_size - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - rtBlock->_queues;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(rtBlock->_queues, rtBlock->_queues + eIdx, sTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - rtBlock->_queues;
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, rtBlock->_queues + sIdx, eIdx - sIdx + 1);
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

			HisOrdQueBlockPair& hisBlkPair = _his_ordque_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdQueBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderqueue data file {} failed", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisOrdQueBlockV2* tBlockV2 = (HisOrdQueBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisOrdQueBlockV2) + tBlockV2->_size))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderqueue data file {} failed", filename.c_str());
				return NULL;
			}

			//需要解压
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			hisBlkPair._buffer.resize(sizeof(HisOrdQueBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisOrdQueBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdQueBlockPair& tBlkPair = _his_ordque_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisOrdQueBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisOrdQueBlock)) / sizeof(WTSOrdQueStruct);
		if (tcnt <= 0)
			return NULL;

		WTSOrdQueStruct* pItem = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - tBlock->_items;
		if (pItem->action_date > eTick.action_date || pItem->action_time >= eTick.action_time)
		{
			pItem--;
			eIdx--;
		}


		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, tBlock->_items, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(tBlock->_items, tBlock->_items + eIdx, sTick, [](const WTSOrdQueStruct& a, const WTSOrdQueStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - tBlock->_items;
			WTSOrdQueSlice* slice = WTSOrdQueSlice::create(stdCode, tBlock->_items + sIdx, eIdx - sIdx + 1);
			return slice;
		}
	}
}

/**
 * @brief 按时间范围读取委托明细数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSOrdDtlSlice* 委托明细数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码和时间范围，获取对应的委托明细数据切片。
 * 时间格式为年月日时分秒毫秒，例如：20190807124533900。
 * 数据包括历史委托明细数据和实时委托明细数据，会根据时间范围自动判断是否需要根据交易日分批处理数据。
 */
WTSOrdDtlSlice* WtRdmDtReader::readOrdDtlSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID, lDate, lTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

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

		WTSOrdDtlStruct* pItem = std::lower_bound(rtBlock->_details, rtBlock->_details + (rtBlock->_size - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - rtBlock->_details;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(rtBlock->_details, rtBlock->_details + eIdx, sTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - rtBlock->_details;
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, rtBlock->_details + sIdx, eIdx - sIdx + 1);
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
			ss << _base_dir << "his/orders/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisOrdDtlBlockPair& hisBlkPair = _his_orddtl_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisOrdDtlBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderdetail data file {} failed", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisOrdDtlBlockV2* tBlockV2 = (HisOrdDtlBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisOrdDtlBlockV2) + tBlockV2->_size))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of orderdetail data file {} failed", filename.c_str());
				return NULL;
			}

			//需要解压
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			hisBlkPair._buffer.resize(sizeof(HisOrdDtlBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisOrdDtlBlock*)hisBlkPair._buffer.c_str();
		}

		HisOrdDtlBlockPair& tBlkPair = _his_orddtl_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisOrdDtlBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisOrdDtlBlock)) / sizeof(WTSOrdDtlStruct);
		if (tcnt <= 0)
			return NULL;

		WTSOrdDtlStruct* pItem = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - tBlock->_items;
		if (pItem->action_date > eTick.action_date || pItem->action_time >= eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, tBlock->_items, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(tBlock->_items, tBlock->_items + eIdx, sTick, [](const WTSOrdDtlStruct& a, const WTSOrdDtlStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - tBlock->_items;
			WTSOrdDtlSlice* slice = WTSOrdDtlSlice::create(stdCode, tBlock->_items + sIdx, eIdx - sIdx + 1);
			return slice;
		}
	}
}

/**
 * @brief 按时间范围读取成交数据切片
 * @param stdCode 标准化合约代码
 * @param stime 开始时间
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSTransSlice* 成交数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码和时间范围，获取对应的成交数据切片。
 * 时间格式为年月日时分秒毫秒，例如：20190807124533900。
 * 数据包括历史成交数据和实时成交数据，会根据时间范围自动判断是否需要根据交易日分批处理数据。
 */
WTSTransSlice* WtRdmDtReader::readTransSliceByRange(const char* stdCode, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t beginTDate = _base_data_mgr->calcTradingDate(stdPID, lDate, lTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

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

		WTSTransStruct* pItem = std::lower_bound(rtBlock->_trans, rtBlock->_trans + (rtBlock->_size - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - rtBlock->_trans;

		//如果光标定位的tick时间比目标时间打, 则全部回退一个
		if (pItem->action_date > eTick.action_date || pItem->action_time > eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(rtBlock->_trans, rtBlock->_trans + eIdx, sTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - rtBlock->_trans;
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, rtBlock->_trans + sIdx, eIdx - sIdx + 1);
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
			ss << _base_dir << "his/trans/" << cInfo._exchg << "/" << endTDate << "/" << curCode << ".dsb";
			std::string filename = ss.str();
			if (!StdFile::exists(filename.c_str()))
				return NULL;

			HisTransBlockPair& hisBlkPair = _his_trans_map[key];
			StdFile::read_file_content(filename.c_str(), hisBlkPair._buffer);
			if (hisBlkPair._buffer.size() < sizeof(HisTransBlockV2))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of transaction data file {} failed", filename.c_str());
				hisBlkPair._buffer.clear();
				return NULL;
			}

			HisTransBlockV2* tBlockV2 = (HisTransBlockV2*)hisBlkPair._buffer.c_str();

			if (hisBlkPair._buffer.size() != (sizeof(HisTransBlockV2) + tBlockV2->_size))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of transaction data file {} failed", filename.c_str());
				return NULL;
			}

			//需要解压
			std::string buf = WTSCmpHelper::uncompress_data(tBlockV2->_data, (std::size_t)tBlockV2->_size);

			//将原来的buffer只保留一个头部,并将所有tick数据追加到尾部
			hisBlkPair._buffer.resize(sizeof(HisTransBlock));
			hisBlkPair._buffer.append(buf);
			tBlockV2->_version = BLOCK_VERSION_RAW;

			hisBlkPair._block = (HisTransBlock*)hisBlkPair._buffer.c_str();
		}

		HisTransBlockPair& tBlkPair = _his_trans_map[key];
		if (tBlkPair._block == NULL)
			return NULL;

		HisTransBlock* tBlock = tBlkPair._block;

		uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTransBlock)) / sizeof(WTSTransStruct);
		if (tcnt <= 0)
			return NULL;

		WTSTransStruct* pItem = std::lower_bound(tBlock->_items, tBlock->_items + (tcnt - 1), eTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pItem - tBlock->_items;
		if (pItem->action_date > eTick.action_date || pItem->action_time >= eTick.action_time)
		{
			pItem--;
			eIdx--;
		}

		if (beginTDate != endTDate)
		{
			//如果开始的交易日和当前的交易日不一致，则返回全部的tick数据
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, tBlock->_items, eIdx + 1);
			return slice;
		}
		else
		{
			//如果交易日相同，则查找起始的位置
			pItem = std::lower_bound(tBlock->_items, tBlock->_items + eIdx, sTick, [](const WTSTransStruct& a, const WTSTransStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t sIdx = pItem - tBlock->_items;
			WTSTransSlice* slice = WTSTransSlice::create(stdCode, tBlock->_items + sIdx, eIdx - sIdx + 1);
			return slice;
		}
	}
}

/**
 * @brief 从文件中缓存历史K线数据
 * @param codeInfo 合约信息指针，包含交易所、品种等信息
 * @param key 缓存映射的键名
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return bool 缓存是否成功
 * 
 * @details 根据给定的合约信息和周期，从磁盘文件中加载历史K线数据并存入内存缓存。
 * 处理多种不同的数据存储格式，包括日线、分钟线等，并支持前复权和后复权的处理。
 * 该方法会根据不同的存储路径规则查找相应的数据文件，对于股票数据还会进行复权处理。
 */
bool WtRdmDtReader::cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period)
{
	CodeHelper::CodeInfo* cInfo = (CodeHelper::CodeInfo*)codeInfo;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo->_exchg, cInfo->_product);
	const char* stdPID = cInfo->stdCommID();

	uint32_t curDate = TimeUtils::getCurDate();
	uint32_t curTime = TimeUtils::getCurMin() / 100;

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, curDate, curTime, false);

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
	if (strlen(ruleTag) > 0)//如果是读取期货主力连续数据
	{
		//先按照HOT代码进行读取, 如rb.HOT
		std::vector<WTSBarStruct>* hotAy = NULL;
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
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
				break;
			}

			proc_block_data(content, true, false);
			uint32_t barcnt = content.size() / sizeof(WTSBarStruct);

			hotAy = new std::vector<WTSBarStruct>();
			hotAy->resize(barcnt);
			memcpy(hotAy->data(), content.data(), content.size());

			if (period != KP_DAY)
				lastHotTime = hotAy->at(barcnt - 1).time;
			else
				lastHotTime = hotAy->at(barcnt - 1).date;

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
				uint64_t sTime = _base_data_mgr->getBoundaryTime(stdPID, leftDt, false, true);
				uint64_t eTime = _base_data_mgr->getBoundaryTime(stdPID, rightDt, false, false);

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
				if (content.size() < sizeof(HisKlineBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
					return false;
				}
				
				proc_block_data(content, true, false);

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
			barsSections.emplace_back(hotAy);
			realCnt += hotAy->size();
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
				break;
			}

			HisKlineBlock* kBlock = (HisKlineBlock*)content.c_str();
			uint32_t barcnt = 0;
			std::string buffer;
			bool bOldVer = kBlock->is_old_version();
			if (kBlock->_version == BLOCK_VERSION_CMP)
			{
				if (content.size() < sizeof(HisKlineBlockV2))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
					break;
				}

				HisKlineBlockV2* kBlockV2 = (HisKlineBlockV2*)content.c_str();
				if (kBlockV2->_size == 0)
					break;

				buffer = WTSCmpHelper::uncompress_data(kBlockV2->_data, (std::size_t)kBlockV2->_size);
			}
			else
			{
				content.erase(0, BLOCK_HEADER_SIZE);
				buffer.swap(content);
			}

			if(buffer.empty())
				break;

			if(bOldVer)
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

			barcnt = buffer.size() / sizeof(WTSBarStruct);

			hotAy = new std::vector<WTSBarStruct>();
			hotAy->resize(barcnt);
			memcpy(hotAy->data(), buffer.data(), buffer.size());

			if (period != KP_DAY)
				lastQTime = hotAy->at(barcnt - 1).time;
			else
				lastQTime = hotAy->at(barcnt - 1).date;

			pipe_rdmreader_log(_sink, LL_INFO, "{} history exrighted {} data of {} directly cached", barcnt, pname.c_str(), stdCode);
			break;
		} while (false);

		bool bAllCovered = false;
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
				if (content.size() < sizeof(HisKlineBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
					return false;
				}

				proc_block_data(content, true, false);
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
							pBar = std::lower_bound(firstBar, firstBar + lastIdx - 1, bar, [period](const WTSBarStruct& a, const WTSBarStruct& b) {
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
			if (content.size() < sizeof(HisKlineBlock))
			{
				pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his kline data file {} failed", filename.c_str());
				return false;
			}

			proc_block_data(content, true, false);

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

/**
 * @brief 根据时间范围从缓存中获取K线数据指针
 * @param key 缓存键名
 * @param stime 开始时间
 * @param etime 结束时间
 * @param count 输出参数，返回获取到的K线数量
 * @param isDay 是否为日线数据，默认为false
 * @return WTSBarStruct* 返回K线数据数组的开始指针
 * 
 * @details 根据给定的时间范围，从内存缓存中快速检索符合条件的K线数据。
 * 返回的是直接指向缓存中数据的指针，不进行数据复制，效率更高。
 * 根据时间格式不同（日线或分钟线），使用不同的比较方法定位数据。
 */
WTSBarStruct* WtRdmDtReader::indexBarFromCacheByRange(const std::string& key, uint64_t stime, uint64_t etime, uint32_t& count, bool isDay /* = false */)
{
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
		uint64_t nowTime = (uint64_t)rDate * 10000 + rTime;

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
	count = curCnt;
	return &barsList._bars[sIdx];
}

/**
 * @brief 根据数量从缓存中获取K线数据指针
 * @param key 缓存键名
 * @param etime 结束时间
 * @param count 输入输出参数，输入时指定要获取的数量，输出时返回实际获取到的数量
 * @param isDay 是否为日线数据，默认为false
 * @return WTSBarStruct* 返回K线数据数组的开始指针
 * 
 * @details 根据给定的结束时间和数量，向前获取指定数量的K线数据。
 * 返回的是直接指向缓存中数据的指针，不进行数据复制，效率更高。
 * 该方法会先定位到结束时间的数据，然后向前获取指定数量的数据。
 */
WTSBarStruct* WtRdmDtReader::indexBarFromCacheByCount(const std::string& key, uint64_t etime, uint32_t& count, bool isDay /* = false */)
{
	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	BarsList& barsList = _bars_cache[key];
	if (barsList._bars.empty())
		return NULL;

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
			eit--;
		}

		eIdx = eit - barsList._bars.begin();
	}

	uint32_t curCnt = min((uint32_t)eIdx + 1, count);
	sIdx = eIdx + 1 - curCnt;
	count = curCnt;
	return &barsList._bars[sIdx];
}

/**
 * @brief 根据时间范围从缓存中读取K线数据并复制到向量中
 * @param key 缓存键名
 * @param stime 开始时间
 * @param etime 结束时间
 * @param ayBars 输出参数，用于存放获取到的K线数据
 * @param isDay 是否为日线数据，默认为false
 * @return uint32_t 返回获取到的K线数量
 * 
 * @details 根据给定的时间范围，从内存缓存中获取K线数据并复制到提供的向量中。
 * 用于需要对数据进行进一步处理的场景。
 * 该方法会先从缓存中定位符合时间范围的数据，然后复制到提供的向量中返回。
 */
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

/**
 * @brief 按时间范围读取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param stime 开始时间
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSKlineSlice* K线数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码、周期和时间范围，获取对应的K线数据切片。
 * 数据包括历史数据和实时数据，并会根据需要对股票数据进行复权处理。
 * 该方法首先会查询缓存中是否已有数据，如果没有则从文件中加载。然后根据周期类型和时间范围
 * 获取历史和实时数据，并合并成完整的数据切片返回。
 */
WTSKlineSlice* WtRdmDtReader::readKlineSliceByRange(const char* stdCode, WTSKlinePeriod period, uint64_t stime, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	const char* stdPID = commInfo->getFullPid();

	std::string key = fmt::format("{}#{}", stdCode, period);
	auto it = _bars_cache.find(key);
	bool bHasHisData = false;
	if (it == _bars_cache.end())
	{
		bHasHisData = cacheHisBarsFromFile(&cInfo, key, stdCode, period);
	}
	else
	{
		bHasHisData = true;
	}

	if (etime == 0)
		etime = 203012312359;

	uint32_t rDate, rTime, lDate, lTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);
	lDate = (uint32_t)(stime / 10000);
	lTime = (uint32_t)(stime % 10000);

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);
	
	WTSBarStruct* hisHead = NULL;
	WTSBarStruct* rtHead = NULL;
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
		else
		{
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL)
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
					memcpy(&barsList._rt_bars[idx], &kPair->_block->_bars[idx], sizeof(WTSBarStruct)*(newSize - oldSize + 1));

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

				//最后做一个定位
				auto it = std::lower_bound(barsList._rt_bars.begin(), barsList._rt_bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
					if (isDay)
						return a.date < b.date;
					else
						return a.time < b.time;
				});
				std::size_t idx = it - barsList._rt_bars.begin();
				WTSBarStruct* pBar = &barsList._rt_bars[idx];
				if ((isDay && pBar->date > eBar.date) || (!isDay && pBar->time > eBar.time))
				{
					pBar--;
					idx--;
				}

				pBar = &barsList._rt_bars[0];
				//如果第一条实时K线的时间大于开始日期，则实时K线要全部包含进去
				if ((isDay && pBar->date > sBar.date) || (!isDay && pBar->time > sBar.time))
				{
					rtHead = &barsList._rt_bars[0];
					rtCnt = idx + 1;
				}
				else
				{
					it = std::lower_bound(barsList._rt_bars.begin(), barsList._rt_bars.begin() + idx, sBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
						if (isDay)
							return a.date < b.date;
						else
							return a.time < b.time;
					});

					std::size_t sIdx = it - barsList._rt_bars.begin();
					rtHead = &barsList._rt_bars[sIdx];
					rtCnt = idx - sIdx + 1;
					bNeedHisData = false;
				}
			}
		}	
		
	}

	if (bNeedHisData)
	{
		hisHead = indexBarFromCacheByRange(key, stime, etime, hisCnt, period == KP_DAY);
	}

	if (hisCnt + rtCnt > 0)
	{
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, hisHead, hisCnt);
		if (rtCnt > 0)
			slice->appendBlock(rtHead, rtCnt);
		return slice;
	}

	return NULL;
}


/**
 * @brief 获取实时Tick数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return WtRdmDtReader::TickBlockPair* Tick数据块对象指针，如果不存在则返回NULL
 * 
 * @details 根据交易所和合约代码获取实时Tick数据块。
 * 如果已经存在内存缓存中，则直接返回；否则，从磁盘文件中加载。
 * 该方法会更新数据块的最后访问时间以支持缓存过期机制。
 */
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

/**
 * @brief 获取实时委托明细数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return WtRdmDtReader::OrdDtlBlockPair* 委托明细数据块对象指针，如果不存在则返回NULL
 * 
 * @details 根据交易所和合约代码获取实时委托明细数据块。
 * 如果已经存在内存缓存中，则直接返回；否则，从磁盘文件中加载。
 * 该方法会更新数据块的最后访问时间以支持缓存过期机制。
 * 当文件大小发生变化时，会自动重新映射文件。
 */
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

/**
 * @brief 获取实时委托队列数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return WtRdmDtReader::OrdQueBlockPair* 委托队列数据块对象指针，如果不存在则返回NULL
 * 
 * @details 根据交易所和合约代码获取实时委托队列数据块。
 * 如果已经存在内存缓存中，则直接返回；否则，从磁盘文件中加载。
 * 该方法会更新数据块的最后访问时间以支持缓存过期机制。
 * 当文件大小发生变化时，会自动重新映射文件。
 */
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

/**
 * @brief 获取实时成交数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return WtRdmDtReader::TransBlockPair* 成交数据块对象指针，如果不存在则返回NULL
 * 
 * @details 根据交易所和合约代码获取实时成交数据块。
 * 如果已经存在内存缓存中，则直接返回；否则，从磁盘文件中加载。
 * 该方法会更新数据块的最后访问时间以支持缓存过期机制。
 * 当文件大小发生变化时，会自动重新映射文件。
 */
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

/**
 * @brief 获取实时K线数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param period K线周期，目前仅支持1分钟和5分钟K线
 * @return WtRdmDtReader::RTKlineBlockPair* K线数据块对象指针，如果不存在则返回NULL
 * 
 * @details 根据交易所、合约代码和K线周期获取实时K线数据块。
 * 目前仅支持1分钟和5分钟周期的K线数据。
 * 如果已经存在内存缓存中，则直接返回；否则，从磁盘文件中加载。
 * 该方法会更新数据块的最后访问时间以支持缓存过期机制。
 * 当文件大小发生变化时，会自动重新映射文件。
 */
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

/**
 * @brief 按数量读取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param count 要读取的K线数量
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSKlineSlice* K线数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码、周期、数量和结束时间，向前获取指定数量的K线数据切片。
 * 该方法会同时查询历史数据和实时数据，并将它们合并成一个完整的数据切片返回。
 * 如果是股票数据，还会根据需要对数据进行复权处理。
 */
WTSKlineSlice* WtRdmDtReader::readKlineSliceByCount(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	pipe_rdmreader_log(_sink, LL_INFO, "CodeInfo of {}: {},{},{}", stdCode, cInfo._exchg, cInfo._product, cInfo._code);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	const char* stdPID = commInfo->getFullPid();

	std::string key = fmtutil::format("{}#{}", stdCode, period);
	auto it = _bars_cache.find(key);
	bool bHasHisData = false;
	if (it == _bars_cache.end())
	{
		bHasHisData = cacheHisBarsFromFile(&cInfo, key, stdCode, period);
	}
	else
	{
		bHasHisData = true;
	}

	if (etime == 0)
		etime = 203012312359;

	uint32_t rDate, rTime;
	rDate = (uint32_t)(etime / 10000);
	rTime = (uint32_t)(etime % 10000);

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

	WTSBarStruct* hisHead = NULL;
	WTSBarStruct* rtHead = NULL;
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

				//如果第一条实时K线的时间大于开始日期，则实时K线要全部包含进去
				rtCnt = min((uint32_t)idx + 1, count);
				std::size_t sIdx = idx + 1 - rtCnt;
				rtHead = kPair->_block->_bars + sIdx;
				bNeedHisData = (rtCnt < count);
			}
		}
		else
		{
			RTKlineBlockPair* kPair = getRTKilneBlock(cInfo._exchg, curCode, period);
			if (kPair != NULL)
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

				//最后做一个定位
				auto it = std::lower_bound(barsList._rt_bars.begin(), barsList._rt_bars.end(), eBar, [isDay](const WTSBarStruct& a, const WTSBarStruct& b) {
					if (isDay)
						return a.date < b.date;
					else
						return a.time < b.time;
				});
				std::size_t idx = it - barsList._rt_bars.begin();
				WTSBarStruct* pBar = &barsList._rt_bars[idx];
				if ((isDay && pBar->date > eBar.date) || (!isDay && pBar->time > eBar.time))
				{
					pBar--;
					idx--;
				}

				//如果第一条实时K线的时间大于开始日期，则实时K线要全部包含进去
				rtCnt = min((uint32_t)idx + 1, count);
				std::size_t sIdx = idx + 1 - rtCnt;
				rtHead = &barsList._rt_bars[sIdx];
				bNeedHisData = (rtCnt < count);
			}
		}
	}
	

	if (bNeedHisData)
	{
		hisCnt = count - rtCnt;
		hisHead = indexBarFromCacheByCount(key, etime, hisCnt, period == KP_DAY);
	}

	pipe_rdmreader_log(_sink, LL_DEBUG, "His {} bars of {} loaded, {} from history, {} from realtime", PERIOD_NAME[period], stdCode, hisCnt, rtCnt);

	if (hisCnt + rtCnt > 0)
	{
		WTSKlineSlice* slice = WTSKlineSlice::create(stdCode, period, 1, hisHead, hisCnt);
		if (rtCnt > 0)
			slice->appendBlock(rtHead, rtCnt);
		return slice;
	}

	return NULL;
}

/**
 * @brief 按数量读取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 要读取的Tick数量
 * @param etime 结束时间，默认为0（表示当前时间）
 * @return WTSTickSlice* Tick数据切片指针，如果无数据则返回NULL
 * 
 * @details 根据给定的合约代码、数量和结束时间，向前获取指定数量的Tick数据切片。
 * 该方法会同时查询实时数据和历史数据，并将它们合并成一个完整的数据切片返回。
 * 对于期货合约，会根据规则标签自动处理主力合约转换。
 * 时间格式为年月日时分秒毫秒，例如：20190807124533900。
 */
WTSTickSlice* WtRdmDtReader::readTickSliceByCount(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	const char* stdPID = commInfo->getFullPid();

	WTSSessionInfo* sInfo = _base_data_mgr->getSession(_base_data_mgr->getCommodity(cInfo._exchg, cInfo._code)->getSession());

	uint32_t rDate, rTime, rSecs;
	//20190807124533900
	rDate = (uint32_t)(etime / 1000000000);
	rTime = (uint32_t)(etime % 1000000000) / 100000;
	rSecs = (uint32_t)(etime % 100000);

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID, rDate, rTime, false);
	uint32_t curTDate = _base_data_mgr->calcTradingDate(stdPID, 0, 0, false);

	bool hasToday = (endTDate >= curTDate);

	WTSTickSlice* slice = WTSTickSlice::create(stdCode);

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

		WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tBlock->_size - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});

		std::size_t eIdx = pTick - tBlock->_ticks;

		//如果光标定位的tick时间比目标时间大, 则全部回退一个
		if (pTick->action_date > eTick.action_date || pTick->action_time > eTick.action_time)
		{
			pTick--;
			eIdx--;
		}

		uint32_t thisCnt = min((uint32_t)eIdx + 1, left);
		uint32_t sIdx = eIdx + 1 - thisCnt;
		slice->insertBlock(0, tBlock->_ticks + sIdx, thisCnt);
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
			//else if (cInfo.isHot())
			//{
			//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, nowTDate);
			//	hotCode = cInfo._product;
			//	hotCode += "_HOT";
			//	pipe_rdmreader_log(_sink, LL_INFO, "Hot contract on {} confirmed: {} -> {}", curTDate, stdCode, curCode.c_str());
			//}
			//else if (cInfo.isSecond())
			//{
			//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, nowTDate);
			//	hotCode = cInfo._product;
			//	hotCode += "_2ND";
			//	pipe_rdmreader_log(_sink, LL_INFO, "Second contract on {} confirmed: {} -> {}", curTDate, stdCode, curCode.c_str());
			//}
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

				HisTBlockPair& tBlkPair = _his_tick_map[key];
				StdFile::read_file_content(filename.c_str(), tBlkPair._buffer);
				if (tBlkPair._buffer.size() < sizeof(HisTickBlock))
				{
					pipe_rdmreader_log(_sink, LL_ERROR, "Sizechecking of his tick data file {} failed", filename.c_str());
					tBlkPair._buffer.clear();
					break;
				}

				proc_block_data(tBlkPair._buffer, false, true);				
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

			HisTBlockPair& tBlkPair = _his_tick_map[key];
			if (tBlkPair._block == NULL)
				break;

			HisTickBlock* tBlock = tBlkPair._block;

			uint32_t tcnt = (tBlkPair._buffer.size() - sizeof(HisTickBlock)) / sizeof(WTSTickStruct);
			if (tcnt <= 0)
				break;

			WTSTickStruct* pTick = std::lower_bound(tBlock->_ticks, tBlock->_ticks + (tcnt - 1), eTick, [](const WTSTickStruct& a, const WTSTickStruct& b) {
				if (a.action_date != b.action_date)
					return a.action_date < b.action_date;
				else
					return a.action_time < b.action_time;
			});

			std::size_t eIdx = pTick - tBlock->_ticks;
			if (pTick->action_date > eTick.action_date || pTick->action_time >= eTick.action_time)
			{
				pTick--;
				eIdx--;
			}

			uint32_t thisCnt = min((uint32_t)eIdx + 1, left);
			uint32_t sIdx = eIdx + 1 - thisCnt;
			slice->insertBlock(0, tBlock->_ticks + sIdx, thisCnt);
			left -= thisCnt;
			break;
		}

		nowTDate = TimeUtils::getNextDate(nowTDate, -1);
	}

	return slice;
}

/**
 * @brief 根据日期获取股票除权因子
 * @param stdCode 标准化合约代码
 * @param date 日期，默认为0（表示当前日期）
 * @return double 除权因子值
 * 
 * @details 根据指定的合约代码和日期，从缓存中查找并返回适用的除权因子。
 * 如果指定日期没有对应因子，则返回小于等于当前日期的最近一个因子。
 * 非股票品种永远返回1.0。
 */
double WtRdmDtReader::getAdjFactorByDate(const char* stdCode, uint32_t date /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	if (!commInfo->isStock())
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

/**
 * @brief 清理所有数据缓存
 * 
 * @details 清空所有数据缓存，包括K线缓存、实时分钟线缓存、Tick数据缓存、
 * 成交数据缓存、委托明细缓存和委托队列缓存等。
 * 这个方法通常在系统重置或清理内存时调用。
 */
void WtRdmDtReader::clearCache()
{
	_bars_cache.clear();

	_rt_min1_map.clear();
	_rt_min5_map.clear();

	_rt_tick_map.clear();
	_rt_trans_map.clear();
	_rt_orddtl_map.clear();
	_rt_ordque_map.clear();
}