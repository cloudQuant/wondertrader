/*!
 * \file WtBtDtReaderAD.cpp
 * \brief 基于LMDB的回测数据读取器实现文件
 * 
 * 该文件实现了基于LMDB数据库的回测数据读取器，用于从LMDB数据库中读取历史行情数据供回测使用
 * 
 * \author Wesley
 * \date 2022/01/05
 */

#include "WtBtDtReaderAD.h"
#include "LMDBKeys.h"

#include "../Includes/WTSStruct.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

USING_NS_WTP;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
/**
 * \brief 日志输出函数
 * 
 * 将格式化的日志信息输出到指定的接收器
 * 
 * \param sink 日志接收器
 * \param ll 日志级别
 * \param format 格式化字符串
 * \param args 可变参数列表
 */
template<typename... Args>
inline void pipe_btreader_log(IBtDtReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->reader_log(ll, buffer);
}

/**
 * \brief 导出函数声明
 */
extern "C"
{
	/**
	 * \brief 创建回测数据读取器实例
	 * 
	 * \return 返回新创建的WtBtDtReaderAD实例
	 */
	EXPORT_FLAG IBtDtReader* createBtDtReader()
	{
		IBtDtReader* ret = new WtBtDtReaderAD();
		return ret;
	}

	/**
	 * \brief 删除回测数据读取器实例
	 * 
	 * \param reader 要删除的读取器实例指针
	 */
	EXPORT_FLAG void deleteBtDtReader(IBtDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};

/**
 * \brief WtBtDtReaderAD类的构造函数
 */
WtBtDtReaderAD::WtBtDtReaderAD()
{
}


/**
 * \brief WtBtDtReaderAD类的析构函数
 * 
 * 清理资源并释放内存
 */
WtBtDtReaderAD::~WtBtDtReaderAD()
{

}

/**
 * \brief 初始化回测数据读取器
 * 
 * 根据配置初始化数据读取器，设置数据存储路径和日志接收器
 * 
 * \param cfg 配置项，包含数据存储路径等信息
 * \param sink 数据接收器，用于接收读取器返回的数据和日志
 */
void WtBtDtReaderAD::init(WTSVariant* cfg, IBtDtReaderSink* sink)
{
	_sink = sink;

	if (cfg == NULL)
		return;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	pipe_btreader_log(_sink, LL_INFO, "WtBtDtReaderAD initialized, root data dir is {}", _base_dir);
}

/**
 * \brief 读取原始K线数据
 * 
 * 从LMDB数据库中读取指定交易所、合约和周期的K线数据
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \param period K线周期
 * \param buffer 输出参数，用于存储读取的数据
 * \return 返回true表示读取成功，false表示读取失败
 */
bool WtBtDtReaderAD::read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer)
{
	//直接从LMDB读取
	WtLMDBPtr db = get_k_db(exchg, period);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back {} bars of {}.{}...", PERIOD_NAME[period], exchg, code);
	WtLMDBQuery query(*db);
	LMDBBarKey rKey(exchg, code, 0xffffffff);
	LMDBBarKey lKey(exchg, code, 0);
	int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[this, &buffer, &lKey](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		std::size_t cnt = ayVals.size();
		auto szUnit = sizeof(WTSBarStruct);
		buffer.resize(szUnit*cnt);
		char* cursor = (char*)buffer.data();
		for(const std::string& item : ayVals)
		{
			memcpy(cursor, item.data(), szUnit);
			cursor += szUnit;
		}
	});

	return true;
}

/**
 * \brief 读取原始Tick数据
 * 
 * 从LMDB数据库中读取指定交易所、合约和日期的Tick数据
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \param uDate 交易日期，格式为YYYYMMDD
 * \param buffer 输出参数，用于存储读取的数据
 * \return 返回true表示读取成功，false表示读取失败
 */
bool WtBtDtReaderAD::read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	//直接从LMDB读取
	WtLMDBPtr db = get_t_db(exchg, code);
	if (db == NULL)
		return false;

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back ticks on {} of {}.{}...", uDate, exchg, code);
	WtLMDBQuery query(*db);
	LMDBHftKey rKey(exchg, code, uDate, 240000000);
	LMDBHftKey lKey(exchg, code, uDate, 0);
	int cnt = query.get_range(std::string((const char*)&lKey, sizeof(lKey)), std::string((const char*)&rKey, sizeof(rKey)),
		[this, &buffer, &lKey](const ValueArray& ayKeys, const ValueArray& ayVals) {
		if (ayVals.empty())
			return;

		std::size_t cnt = ayVals.size();
		auto szUnit = sizeof(WTSTickStruct);
		buffer.resize(szUnit*cnt);
		char* cursor = (char*)buffer.data();
		for (const std::string& item : ayVals)
		{
			memcpy(cursor, item.data(), szUnit);
			cursor += szUnit;
		}
	});

	return true;
}

/**
 * \brief 获取指定交易所和周期的K线数据库
 * 
 * 根据交易所和K线周期获取对应的LMDB数据库实例。
 * 如果数据库已经打开，则直接返回缓存的实例；
 * 否则尝试打开数据库并缓存实例以便后续使用。
 * 
 * \param exchg 交易所代码
 * \param period K线周期，支持1分钟、5分钟和日线
 * \return 返回LMDB数据库指针，如果不存在或打开失败则返回空指针
 */
WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_k_db(const char* exchg, WTSKlinePeriod period)
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
	if (!StdFile::exists(path.c_str()))
		return std::move(WtLMDBPtr());

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening {} db if {} failed: {}", subdir, exchg, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "{} db of {} opened", subdir, exchg);
	}

	(*the_map)[exchg] = dbPtr;
	return std::move(dbPtr);
}

/**
 * \brief 获取指定交易所和合约的Tick数据库
 * 
 * 根据交易所和合约代码获取对应的Tick数据库实例。
 * 如果数据库已经打开，则直接返回缓存的实例；
 * 否则尝试打开数据库并缓存实例以便后续使用。
 * 
 * \param exchg 交易所代码
 * \param code 合约代码
 * \return 返回LMDB数据库指针，如果不存在或打开失败则返回空指针
 */
WtBtDtReaderAD::WtLMDBPtr WtBtDtReaderAD::get_t_db(const char* exchg, const char* code)
{
	std::string key = StrUtil::printf("%s.%s", exchg, code);
	auto it = _tick_dbs.find(key);
	if (it != _tick_dbs.end())
		return std::move(it->second);

	WtLMDBPtr dbPtr(new WtLMDB(true));
	std::string path = StrUtil::printf("%sticks/%s/%s", _base_dir.c_str(), exchg, code);
	if (!StdFile::exists(path.c_str()))
		return std::move(WtLMDBPtr());

	if (!dbPtr->open(path.c_str()))
	{
		pipe_btreader_log(_sink, LL_ERROR, "Opening tick db of {}.{} failed: {}", exchg, code, dbPtr->errmsg());
		return std::move(WtLMDBPtr());
	}
	else
	{
		pipe_btreader_log(_sink, LL_DEBUG, "Tick db of {}.{} opened", exchg, code);
	}

	_tick_dbs[exchg] = dbPtr;
	return std::move(dbPtr);
}