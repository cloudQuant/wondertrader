/*!
 * \file WtDataReaderAD.cpp
 * \brief WtDataReaderAD数据读取器实现文件
 * 
 * 该文件实现了基于LMDB的历史数据读取器，用于从LMDB数据库中读取K线和Tick数据
 * 并提供缓存机制以提高读取效率
 * 
 * \author Wesley
 * \date 2022/01/05
 */

#include "WtDataReaderAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSDataDef.hpp"

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"

/**
 * \brief 格式化并输出日志到数据读取器接收器
 * 
 * 该函数使用fmtlib格式化日志消息，然后将其发送到数据读取器接收器
 * 
 * \tparam Args 可变参数模板，用于格式化字符串
 * \param sink 数据读取器接收器指针
 * \param ll 日志级别
 * \param format 格式化字符串
 * \param args 格式化参数
 */
template<typename... Args>
inline void pipe_reader_log(IDataReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->reader_log(ll, buffer);
}

extern "C"
{
	/**
	 * \brief 创建数据读取器实例
	 * 
	 * 该函数用于创建WtDataReaderAD类的实例，供外部模块调用
	 * 
	 * \return 返回IDataReader接口指针
	 */
	EXPORT_FLAG IDataReader* createDataReader()
	{
		IDataReader* ret = new WtDataReaderAD();
		return ret;
	}

	/**
	 * \brief 删除数据读取器实例
	 * 
	 * 该函数用于删除通过createDataReader创建的数据读取器实例
	 * 
	 * \param reader 要删除的数据读取器指针
	 */
	EXPORT_FLAG void deleteDataReader(IDataReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};


/**
 * \brief WtDataReaderAD类构造函数
 * 
 * 初始化类成员变量，包括最后更新时间、基础数据管理器和主力合约管理器
 */
WtDataReaderAD::WtDataReaderAD()
	: _last_time(0)
	, _base_data_mgr(NULL)
	, _hot_mgr(NULL)
{
}


/**
 * \brief WtDataReaderAD类析构函数
 */
WtDataReaderAD::~WtDataReaderAD()
{
}

/**
 * \brief 初始化数据读取器
 * 
 * 该方法初始化数据读取器，包括设置基础数据目录、缓存文件名等
 * 
 * \param cfg 配置参数，包含数据存储路径等配置项
 * \param sink 数据接收器，用于回调通知和日志输出
 * \param loader 历史数据加载器，可选参数，默认为NULL
 */
void WtDataReaderAD::init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader /* = NULL */)
{
	IDataReader::init(cfg, sink, loader);

	_base_data_mgr = sink->get_basedata_mgr();
	_hot_mgr = sink->get_hot_mgr();

	if (cfg == NULL)
		return ;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	_d1_cache._filename = "cache_d1.dmb";
	_m1_cache._filename = "cache_m1.dmb";
	_m5_cache._filename = "cache_m5.dmb";

	pipe_reader_log(sink, LL_INFO, "WtDataReaderAD initialized, root data folder is {}", _base_dir);
}

/**
 * \brief 读取Tick数据切片
 * 
 * 该方法根据标准代码、数量和结束时间，从LMDB数据库中读取指定数量的Tick数据
 * 处理流程包括：
 * 1. 提取合约信息
 * 2. 处理结束时间
 * 3. 计算交易日期
 * 4. 处理主力合约转换
 * 5. 检查缓存状态并决定加载策略
 * 6. 从LMDB读取数据
 * 7. 生成数据切片
 * 
 * \param stdCode 标准合约代码
 * \param count 需要的Tick数量
 * \param etime 结束时间，格式为YYYYMMDDHHMMSSMMM，默认值0表示当前时间
 * \return 返回Tick数据切片，如果读取失败则返回NULL
 */
WTSTickSlice* WtDataReaderAD::readTickSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), endTDate);
		//else if (cInfo.isHot())
		//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
		//else if (cInfo.isSecond())
		//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);
	}

	uint32_t reload_flag = 0; //重载标记，0-不需要重载，1-加载最新，2-全部重载
	std::string key = StrUtil::printf("%s.%s", cInfo._exchg, curCode.c_str());
	//先检查缓存
	TicksList& tickList = _ticks_cache[key];
	uint64_t last_access_time = 0;
	do
	{
		if(tickList._ticks.capacity() < count)
		{
			//容量不够，全部重载
			reload_flag = 2;
			tickList._ticks.rset_capacity(count);
			tickList._ticks.clear();	//清除原有数据
		}

		if(tickList._last_req_time < etime)
		{
			//请求时间不够，只拉取最新的
			reload_flag = 1;
			last_access_time = tickList._last_req_time;
			break;
		}

	} while (false);

	//这里要改成从lmdb读取
	WtLMDBPtr db = get_t_db(cInfo._exchg, cInfo._code);
	if (db == NULL)
		return NULL;

	if(reload_flag == 1)
	{
		//增量更新，从上次的时间戳到本次的时间戳，读取最新的即可
		last_access_time += 1;
		WtLMDBQuery query(*db);
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, (uint32_t)(last_access_time / 1000000000), (uint32_t)(last_access_time % 1000000000));
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000));
		int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), 
			std::string((const char*)&rKey, sizeof(rKey)), [this, &tickList](const ValueArray& ayKeys, const ValueArray& ayVals) {
			for(const std::string& item : ayVals)
			{
				WTSTickStruct* curTick = (WTSTickStruct*)item.data();
				tickList._ticks.push_back(*curTick);
			}
		});
		if(cnt > 0)
			pipe_reader_log(_sink, LL_DEBUG, "{} ticks after {} of {} append to cache", cnt, last_access_time, stdCode);
	}
	else if(reload_flag == 2)
	{
		//全部更新，从结束时间往前读取即可
		WtLMDBQuery query(*db);
		LMDBHftKey rKey(cInfo._exchg, cInfo._code, (uint32_t)(etime / 1000000000), (uint32_t)(etime % 1000000000));
		LMDBHftKey lKey(cInfo._exchg, cInfo._code, 0, 0);
		int cnt = query.get_lowers(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
			count, [this, &tickList](const ValueArray& ayKeys, const ValueArray& ayVals) {
			tickList._ticks.resize(ayVals.size());
			for (std::size_t i = 0; i < ayVals.size(); i++)
			{
				const std::string& item = ayVals[i];
				memcpy(&tickList._ticks[i], item.data(), item.size());
			}
		});

		pipe_reader_log(_sink, LL_DEBUG, "{} ticks of {} loaded to cache for the first time", cnt, stdCode);
	}

	tickList._last_req_time = etime;

	//全部读取完成以后，再生成切片
	//这里要注意一下，因为读取tick是顺序的，但是返回是需要从后往前取指定条数的，所以应该先读取array_two
	count = min((uint32_t)tickList._ticks.size(), count);	//先对count做一个修正
	auto ayTwo = tickList._ticks.array_two();
	auto cnt_2 = ayTwo.second;
	if(cnt_2 >= count)
	{
		//如果array_two条数足够，则直接返回数据块
		return WTSTickSlice::create(stdCode, &tickList._ticks[ayTwo.second - count], count);
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = tickList._ticks.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSTickSlice::create(stdCode, &tickList._ticks[ayOne.second - diff], diff);
		if(cnt_2 > 0)
			ret->appendBlock(ayTwo.first, cnt_2);
		return ret;
	}
}

/**
 * \brief 将K线数据读入到缓冲区
 * 
 * 该方法从LMDB数据库中读取指定交易所、合约和周期的K线数据，并将其存储到字符串缓冲区中
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \param period K线周期
 * \return 包含K线数据的字符串缓冲区
 */
std::string WtDataReaderAD::read_bars_to_buffer(const char* exchg, const char* code, WTSKlinePeriod period)
{
	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return "";

	std::string buffer;
	WtLMDBQuery query(*db);
	LMDBBarKey lKey(exchg, code, 0);
	LMDBBarKey rKey(exchg, code, 0xffffffff);
	query.get_range(std::string((const char*)&lKey, sizeof(lKey)),
		std::string((const char*)&lKey, sizeof(lKey)), [this, &buffer](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		buffer.resize(sizeof(WTSBarStruct)*ayVals.size());
		memcpy((void*)buffer.data(), ayVals.data(), sizeof(WTSBarStruct)*ayVals.size());
	});
	return std::move(buffer);
}

/**
 * \brief 将历史数据放入缓存
 * 
 * 该方法从LMDB数据库中读取指定数量的K线数据并存入内存缓存
 * 
 * \param key 缓存键名，通常是标准合约代码
 * \param stdCode 标准合约代码
 * \param period K线周期
 * \param count 请求的数据条数
 * \return true 缓存成功
 * \return false 缓存失败
 */
bool WtDataReaderAD::cacheBarsFromStorage(const std::string& key, const char* stdCode, WTSKlinePeriod period, uint32_t count)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

	BarsList& barList = _bars_cache[key];
	barList._code = stdCode;
	barList._period = period;
	barList._exchg = cInfo._exchg;

	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(cInfo._exchg, period);
	if (db == NULL)
		return false;

	WtLMDBQuery query(*db);
	LMDBBarKey rKey(cInfo._exchg, cInfo._code, 0xffffffff);
	LMDBBarKey lKey(cInfo._exchg, cInfo._code, 0);
	int cnt = query.get_lowers(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		count, [this, &barList, &lKey](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		std::size_t cnt = ayVals.size();
		for (std::size_t i = 0; i < cnt; i++)
		{
			//要检查左边界
			if(memcmp(ayKeys[i].data(), (void*)&lKey, sizeof(lKey)) < 0)
				continue;

			barList._bars.push_back(*(WTSBarStruct*)ayVals[i].data());
		}
	});

	pipe_reader_log(_sink, LL_DEBUG, "{} {} bars of {} loaded to cache", cnt, PERIOD_NAME[period], stdCode);
	return true;
}

/**
 * \brief 从LMDB更新K线缓存
 * 
 * 该方法从LMDB数据库中读取指定交易所、合约和周期的K线数据，并更新到内存缓存中
 * 
 * \param barsList 要更新的K线列表缓存
 * \param exchg 交易所代码
 * \param code 合约代码
 * \param period K线周期
 * \param lastBarTime 最后K线时间，引用参数，会被更新
 */
void WtDataReaderAD::update_cache_from_lmdb(BarsList& barsList, const char* exchg, const char* code, WTSKlinePeriod period, uint32_t& lastBarTime)
{
	bool isDay = (period == KP_DAY);
	WtLMDBPtr db = get_k_db(exchg, period);
	WtLMDBQuery query(*db);
	LMDBBarKey lKey(exchg, code, lastBarTime);
	LMDBBarKey rKey(exchg, code, 0xFFFFFFFF);
	int cnt = query.get_uppers(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)), 
		9999, [this, &barsList, isDay, &lastBarTime](const ValueArray& ayKeys, const ValueArray& ayVals) {

		std::size_t cnt = ayVals.size();
		for (std::size_t idx = 0; idx < cnt; idx++)
		{
			const std::string& item = ayVals[idx];
			const std::string& key = ayKeys[idx];
			LMDBBarKey* barKey = (LMDBBarKey*)key.data();
			printf("%u\r\n", reverseEndian(barKey->_bartime));
			WTSBarStruct* curBar = (WTSBarStruct*)item.data();
			uint64_t curBarTime = isDay ? curBar->date : curBar->time;
			if (curBarTime == lastBarTime)
			{
				if (barsList._last_from_cache)
					memcpy(&barsList._bars.back(), curBar, sizeof(WTSBarStruct));
			}
			else
			{
				barsList._bars.push_back(*curBar);
				lastBarTime = (uint32_t)curBarTime;
				_sink->on_bar(barsList._code.c_str(), barsList._period, &barsList._bars.back());
			}
		}
	});

	pipe_reader_log(_sink, LL_DEBUG, "{} bars of {}.{} updated to {}",
		PERIOD_NAME[period], exchg, code, isDay?barsList._bars.back().date:barsList._bars.back().time);
}

/**
 * \brief 获取实时缓存的K线数据
 * 
 * 从内存映射文件中读取最新的实时K线数据
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \param period K线周期
 * \return 返回最新的K线数据结构指针，如果读取失败则返回NULL
 */
WTSBarStruct* WtDataReaderAD::get_rt_cache_bar(const char* exchg, const char* code, WTSKlinePeriod period)
{
	RTBarCacheWrapper* wrapper = NULL;
	if (period == KP_DAY)
		wrapper = &_d1_cache;
	else if (period == KP_Minute1)
		wrapper = &_m1_cache;
	else if (period == KP_Minute5)
		wrapper = &_m5_cache;

	bool isDay = (period == KP_DAY);

	if (wrapper != NULL)
	{
		if (wrapper->empty())
		{
			//如果缓存为空，则加载缓存
			do
			{

				//缓存为空，先自动加载
				std::string filename = _base_dir + wrapper->_filename;
				if (!StdFile::exists(filename.c_str()))
					break;

				wrapper->_file_ptr.reset(new BoostMappingFile);
				wrapper->_file_ptr->map(filename.c_str());
				wrapper->_cache_block = (RTBarCache*)wrapper->_file_ptr->addr();

				wrapper->_cache_block->_size = min(wrapper->_cache_block->_size, wrapper->_cache_block->_capacity);
				wrapper->_last_size = wrapper->_cache_block->_size;

				for (uint32_t i = 0; i < wrapper->_cache_block->_size; i++)
				{
					const BarCacheItem& item = wrapper->_cache_block->_items[i];
					wrapper->_idx[StrUtil::printf("%s.%s", item._exchg, item._code)] = i;
				}
			} while (false);
		}
		else
		{
			//如果缓存不为空，检查一下有没有新的合约进来，如果有的话，就把索引更新一下
			if (wrapper->_last_size != wrapper->_cache_block->_size)
			{
				for (uint32_t i = wrapper->_last_size; i < wrapper->_cache_block->_size; i++)
				{
					const BarCacheItem& item = wrapper->_cache_block->_items[i];
					wrapper->_idx[StrUtil::printf("%s.%s", item._exchg, item._code)] = i;
				}
			}
		}

		//最后看缓存里有没有改合约对应的K线缓存
		auto it = wrapper->_idx.find(StrUtil::printf("%s.%s", exchg, code));
		if (it != wrapper->_idx.end())
		{
			return &wrapper->_cache_block->_items[it->second]._bar;
		}
	}

	return NULL;
}

/**
 * \brief 读取K线数据切片
 * 
 * 该方法根据标准代码、周期类型、数量和结束时间，从LMDB数据库中读取指定数量的K线数据
 * 处理流程类似于readTickSlice，包括：
 * 1. 提取合约信息
 * 2. 处理结束时间
 * 3. 计算交易日期
 * 4. 处理主力合约转换
 * 5. 检查缓存状态，必要时从存储中读取数据
 * 6. 生成数据切片
 * 
 * \param stdCode 标准合约代码
 * \param period K线周期类型
 * \param count 需要的K线数量
 * \param etime 结束时间，格式为YYYYMMDDHHMMSS，默认值0表示当前时间
 * \return 返回K线数据切片，如果读取失败则返回NULL
 */
WTSKlineSlice* WtDataReaderAD::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	std::string stdPID = StrUtil::printf("%s.%s", cInfo._exchg, cInfo._product);

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

	uint32_t endTDate = _base_data_mgr->calcTradingDate(stdPID.c_str(), curDate, curTime, false);

	std::string curCode = cInfo._code;
	if (commInfo->isFuture())
	{
		const char* ruleTag = cInfo._ruletag;
		if (strlen(ruleTag) > 0)
			curCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), endTDate);
		//else if (cInfo.isHot())
		//	curCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, endTDate);
		//else if (cInfo.isSecond())
		//	curCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, endTDate);
	}

	std::string key = StrUtil::printf("%s#%u", stdCode, period);
	BarsList& barsList = _bars_cache[key];
	if (barsList._bars.capacity() < count)
	{
		//容量不够，全部重载
		barsList._bars.rset_capacity(count);
		barsList._bars.clear();	//清除原有数据
		cacheBarsFromStorage(key, stdCode, period, count);
	}

	//这里只需要检查一下RTBarCache的K线时间戳和etime是否一致
	//如果一致，说明最后一条K线还没完成，但是系统要读取，这个时候就用缓存中的最后一条bar追加到最后
	//如果不一致，说明Writer那边已经处理完成了，直接用缓存好的K线即可
	//OnMinuteEnd的时候也要做类似的检查
	if (barsList._bars.empty())
		return NULL;

	//数据条数做一个修正
	count = min((uint32_t)barsList._bars.size(), count);

	bool isDay = (period == KP_DAY);
	etime = isDay ? curDate : ((curDate - 19900000)*10000 + curTime);
	if(barsList._last_req_time < etime)
	{
		//上次请求的时间，小于当前请求的时间，则要检查最后一条K线
		WTSBarStruct& lastBar = barsList._bars.back();
		uint32_t lastBarTime = isDay ? lastBar.date : (uint32_t)lastBar.time;
		if(lastBarTime < etime)
		{
			//如果最后一条K线的时间小于当前时间，先从数LMDB更新最新的K线
			update_cache_from_lmdb(barsList, cInfo._exchg, curCode.c_str(), period, lastBarTime);

			lastBar = barsList._bars.back();
			lastBarTime = isDay ? lastBar.date : (uint32_t)lastBar.time;
		}

		//从lmdb读完了以后，再检查
		//如果时间戳仍然小于截止时间
		//则从缓存中读取
		if(lastBarTime < etime)
		{
			WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, curCode.c_str(), period);
			if(rtBar != NULL)
			{
				uint64_t cacheBarTime = isDay ? rtBar->date : rtBar->time;
				if (cacheBarTime > etime)
				{
					//缓存的K线时间戳大于截止时间，说明检查的过程中Writer已经将数据转储到lmdb中了
					//这个时候就再读一次lmdb
					update_cache_from_lmdb(barsList, cInfo._exchg, curCode.c_str(), period, lastBarTime);
					barsList._last_from_cache = false;
				}
				else
				{
					//如果缓存的K线时间没有超过etime，则将缓存中的最后一条K线追加到队列中
					barsList._bars.push_back(*rtBar);
					barsList._last_from_cache = true;
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @  {} of {} updated from cache instead of lmdb in {}", PERIOD_NAME[period], etime, stdCode, __FUNCTION__);
				}
			}
		}
	}
	
	//全部处理完了以后，再从K线缓存里读取
	barsList._last_req_time = etime;

	//这里要注意一下，因为读取数据是顺序的，但是返回是需要从后往前取指定条数的，所以应该先读取array_two
	count = min((uint32_t)barsList._bars.size(), count);	//先对count做一个修正
	auto ayTwo = barsList._bars.array_two();
	auto cnt_2 = ayTwo.second;
	if (cnt_2 >= count)
	{
		//如果array_two条数足够，则直接返回数据块
		return WTSKlineSlice::create(stdCode, period, 1, &barsList._bars[ayTwo.second - count], count);
	}
	else
	{
		//如果array_two条数不够，需要再从array_one提取
		auto ayOne = barsList._bars.array_one();
		auto diff = count - cnt_2;
		auto ret = WTSKlineSlice::create(stdCode, period, 1, &barsList._bars[ayOne.second - diff], diff);
		if (cnt_2 > 0)
			ret->appendBlock(ayTwo.first, cnt_2);
		return ret;
	}


	return NULL;
}

/**
 * \brief 分钟结束时的回调处理
 * 
 * 该方法在每分钟结束时被调用，用于更新内存中的K线缓存
 * 对于不同周期的K线数据，会从LMDB数据库和实时缓存中获取最新数据
 * 
 * \param uDate 当前日期，格式为YYYYMMDD
 * \param uTime 当前时间，格式为HHMM
 * \param endTDate 结束交易日，默认为0
 */
void WtDataReaderAD::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */)
{
	//这里应该触发检查
	uint64_t nowTime = (uint64_t)uDate * 10000 + uTime;
	if (nowTime <= _last_time)
		return;

	uint64_t endBarTime = (uDate - 19900000) * 10000 + uTime;

	for (auto it = _bars_cache.begin(); it != _bars_cache.end(); it++)
	{
		BarsList& barsList = (BarsList&)it->second;
		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(barsList._code.c_str(), _hot_mgr);
		if (barsList._period != KP_DAY)
		{
			uint32_t lastBarTime = (uint32_t)barsList._bars.back().time;
			pipe_reader_log(_sink, LL_DEBUG,
				"Updating {} bars of {} in section ({},{}]", PERIOD_NAME[barsList._period], barsList._code, lastBarTime, endBarTime);
			update_cache_from_lmdb(barsList, barsList._exchg.c_str(), cInfo._code, barsList._period, lastBarTime);
			if(lastBarTime < endBarTime)
			{
				WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, cInfo._code, barsList._period);
				if(rtBar->time > lastBarTime && rtBar->time <=endBarTime)
				{
					barsList._bars.push_back(*rtBar);
					barsList._last_from_cache = true;
					_sink->on_bar(barsList._code.c_str(), barsList._period, rtBar);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @ {} of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], endBarTime, barsList._code, __FUNCTION__);
				}
			}
		}
		else if(endTDate != 0)
		{
			uint32_t lastBarTime = barsList._bars.back().date;
			endBarTime = uDate;
			update_cache_from_lmdb(barsList, barsList._exchg.c_str(), cInfo._code, barsList._period, lastBarTime);
			if (lastBarTime < endBarTime)
			{
				WTSBarStruct* rtBar = get_rt_cache_bar(cInfo._exchg, cInfo._code, barsList._period);
				if (rtBar->date > lastBarTime && rtBar->date <= endBarTime)
				{
					barsList._bars.push_back(*rtBar);
					barsList._last_from_cache = true;
					_sink->on_bar(barsList._code.c_str(), barsList._period, rtBar);
					pipe_reader_log(_sink, LL_DEBUG,
						"{} bars @  of {} updated from cache instead of lmdb in {}", PERIOD_NAME[barsList._period], endBarTime, barsList._code, __FUNCTION__);
				}
			}
		}
	}

	if (_sink)
		_sink->on_all_bar_updated(uTime);

	_last_time = nowTime;
}

/**
 * \brief 获取K线数据库连接
 * 
 * 根据交易所代码和周期类型获取对应的LMDB数据库连接
 * 如果数据库连接不存在，则创建新的连接
 * 
 * \param exchg 交易所代码
 * \param period K线周期，支持1分钟、5分钟和日线
 * \return 返回LMDB数据库指针，如果不存在则创建
 */
WtDataReaderAD::WtLMDBPtr WtDataReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
{
	WtLMDBMap* the_map = NULL;
	std::string subdir;
	if (period == KP_Minute1)
	{
		the_map = &_exchg_m1_dbs;
		subdir = "min1";
	}
	else if (period == KP_Minute5)
	{
		the_map = &_exchg_m5_dbs;
		subdir = "min5";
	}
	else if (period == KP_DAY)
	{
		the_map = &_exchg_d1_dbs;
		subdir = "day";
	}
	else
		return std::move(WtLMDBPtr());

	auto it = the_map->find(exchg);
	if (it != the_map->end())
		return std::move(it->second);

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = StrUtil::printf("%s%s/%s/", _base_dir.c_str(), subdir.c_str(), exchg);
	boost::filesystem::create_directories(path);
	if (!dbPtr->open(path.c_str()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Opening {} db if {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_reader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return std::move(dbPtr);
}

/**
 * \brief 获取Tick数据库连接
 * 
 * 根据交易所和合约代码获取对应的LMDB数据库连接
 * 如果数据库连接不存在，则创建新的连接
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \return 返回LMDB数据库指针，如果不存在则创建
 */
WtDataReaderAD::WtLMDBPtr WtDataReaderAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return std::move(it->second);

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = StrUtil::printf("%sticks/%s/%s", _base_dir.c_str(), exchg, code);
	boost::filesystem::create_directories(path);
	if (!dbPtr->open(path.c_str()))
	{
		pipe_reader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_reader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[exchg] = dbPtr;
	return std::move(dbPtr);
}