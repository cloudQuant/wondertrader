/*!
 * \file WtBtDtReader.cpp
 * \project WonderTrader
 *
 * \author Wesley
 * 
 * \brief WonderTrader回测数据读取器实现
 * \details 该文件实现了WtBtDtReader类，用于读取回测所需的各类原始行情数据
 *          主要包含对K线、Tick、逐笔成交/委托等不同类型数据的读取实现
 */

#include "WtBtDtReader.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/StrUtil.hpp"
#include "../WTSUtils/WTSCmpHelper.hpp"

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"

/**
 * @brief 日志输出辅助函数
 * @tparam Args 可变参数模板类型
 * @param sink 日志接收器指针
 * @param ll 日志级别
 * @param format 格式化字符串
 * @param args 格式化参数
 * @details 该函数使用fmt库将日志消息格式化并输出到指定的接收器
 *          主要用于内部调试信息和错误警告的输出
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
 * @brief 外部函数声明，用于处理数据块
 * @param content 内容缓冲区，存放要处理的原始数据
 * @param isBar 是否为K线数据
 * @param bKeepHead 是否保留头部信息
 * @return 处理成功返回true，失败返回false
 */
extern bool proc_block_data(std::string& content, bool isBar, bool bKeepHead = true);

/**
 * @brief 导出函数定义，供外部调用
 */
extern "C"
{
	/**
	 * @brief 创建回测数据读取器实例
	 * @return 返回创建的IBtDtReader接口指针
	 */
	EXPORT_FLAG IBtDtReader* createBtDtReader()
	{
		IBtDtReader* ret = new WtBtDtReader();
		return ret;
	}

	/**
	 * @brief 删除回测数据读取器实例
	 * @param reader 要删除的读取器指针
	 */
	EXPORT_FLAG void deleteBtDtReader(IBtDtReader* reader)
	{
		if (reader != NULL)
			delete reader;
	}
};

/**
 * @brief 处理数据块函数
 * @details 处理从文件中读取的原始数据块，包括完整性检查、数据解包、头部处理等
 *          该函数在外部实现，在WtDataReader.cpp文件中
 * @param content 内容缓冲区，存放要处理的原始数据
 * @param isBar 是否为K线数据
 * @param bKeepHead 是否保留头部信息
 * @return 处理成功返回true，失败返回false
 */
extern bool proc_block_data(std::string& content, bool isBar, bool bKeepHead);

/**
 * @brief 构造函数
 * @details 初始化WtBtDtReader实例
 */
WtBtDtReader::WtBtDtReader()
{
}


/**
 * @brief 析构函数
 * @details 清理资源并释放WtBtDtReader实例
 */
WtBtDtReader::~WtBtDtReader()
{

}

/**
 * @brief 初始化回测数据读取器
 * @param cfg 配置项，包含数据路径等配置
 * @param sink 数据接收器，用于接收日志和读取的数据
 * @details 根据外部配置初始化回测数据读取器
 *          主要设置数据存储的根路径和日志接收器
 */
void WtBtDtReader::init(WTSVariant* cfg, IBtDtReaderSink* sink)
{
	_sink = sink;

	if (cfg == NULL)
		return;

	_base_dir = cfg->getCString("path");
	_base_dir = StrUtil::standardisePath(_base_dir);

	pipe_btreader_log(_sink, LL_INFO, "WtBtDtReader initialized, root data dir is {}", _base_dir);
}

/**
 * @brief 读取原始K线数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param period K线周期
 * @param buffer 输出参数，用于存储读取的原始数据
 * @return 读取成功返回true，失败返回false
 * @details 从存储介质中读取指定合约、指定周期的K线数据
 *          文件路径格式为: [base_dir]/his/[period]/[exchg]/[code].dsb
 *          读取后通过proc_block_data函数处理原始数据
 */
bool WtBtDtReader::read_raw_bars(const char* exchg, const char* code, WTSKlinePeriod period, std::string& buffer)
{
	std::stringstream ss;
	ss << _base_dir << "his/" << PERIOD_NAME[period] << "/" << exchg << "/" << code << ".dsb";
	std::string filename = ss.str();
	if (!StdFile::exists(filename.c_str()))
	{
		pipe_btreader_log(_sink, LL_WARN, "Back {} data file {} not exists", PERIOD_NAME[period], filename);
		return false;
	}

	pipe_btreader_log(_sink, LL_DEBUG, "Reading back {} bars from file {}...", PERIOD_NAME[period], filename);
	StdFile::read_file_content(filename.c_str(), buffer);
	bool bSucc = proc_block_data(buffer, true, false);
	if(!bSucc)
		pipe_btreader_log(_sink, LL_ERROR, "Processing back {} data from file {} failed", PERIOD_NAME[period], filename);

	return bSucc;
}

/**
 * @brief 读取原始Tick数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param buffer 输出参数，用于存储读取的原始数据
 * @return 读取成功返回true，失败返回false
 * @details 从存储介质中读取指定合约、指定日期的Tick数据
 *          文件路径格式为: [base_dir]/his/ticks/[exchg]/[date]/[code].dsb
 *          读取后通过proc_block_data函数处理原始数据
 */
bool WtBtDtReader::read_raw_ticks(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	std::stringstream ss;
	ss << _base_dir << "his/ticks/" << exchg << "/" << uDate << "/" << code << ".dsb";
	std::string filename = ss.str();
	if (!StdFile::exists(filename.c_str()))
	{
		pipe_btreader_log(_sink, LL_WARN, "Back tick data file {} not exists", filename);
		return false;
	}

	StdFile::read_file_content(filename.c_str(), buffer);
	bool bSucc = proc_block_data(buffer, false, false);
	if (!bSucc)
		pipe_btreader_log(_sink, LL_ERROR, "Processing back tick data from file {} failed", filename);

	return bSucc;
}

/**
 * @brief 读取原始逐笔委托数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param buffer 输出参数，用于存储读取的原始数据
 * @return 读取成功返回true，失败返回false
 * @details 从存储介质中读取指定合约、指定日期的逐笔委托数据
 *          文件路径格式为: [base_dir]/his/orders/[exchg]/[date]/[code].dsb
 *          这些数据可用于分析市场深度和委托流
 */
bool WtBtDtReader::read_raw_order_details(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	std::stringstream ss;
	ss << _base_dir << "his/orders/" << exchg << "/" << uDate << "/" << code << ".dsb";
	std::string filename = ss.str();
	if (!StdFile::exists(filename.c_str()))
	{
		pipe_btreader_log(_sink, LL_WARN, "Back order detail data file {} not exists", filename);
		return false;
	}

	StdFile::read_file_content(filename.c_str(), buffer);
	bool bSucc = proc_block_data(buffer, false, false);
	if (!bSucc)
		pipe_btreader_log(_sink, LL_ERROR, "Processing back order detail data from file {} failed", filename);

	return bSucc;
}

/**
 * @brief 读取原始委托队列数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param buffer 输出参数，用于存储读取的原始数据
 * @return 读取成功返回true，失败返回false
 * @details 从存储介质中读取指定合约、指定日期的委托队列数据
 *          文件路径格式为: [base_dir]/his/queue/[exchg]/[date]/[code].dsb
 *          这些数据提供了市场深度信息，基于委托的排队数据
 */
bool WtBtDtReader::read_raw_order_queues(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	std::stringstream ss;
	ss << _base_dir << "his/queue/" << exchg << "/" << uDate << "/" << code << ".dsb";
	std::string filename = ss.str();
	if (!StdFile::exists(filename.c_str()))
	{
		pipe_btreader_log(_sink, LL_WARN, "Back order queue data file {} not exists", filename);
		return false;
	}

	StdFile::read_file_content(filename.c_str(), buffer);
	bool bSucc = proc_block_data(buffer, false, false);
	if (!bSucc)
		pipe_btreader_log(_sink, LL_ERROR, "Processing back order queue data from file {} failed", filename);

	return bSucc;
}

/**
 * @brief 读取原始逐笔成交数据
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param uDate 交易日期(YYYYMMDD格式)
 * @param buffer 输出参数，用于存储读取的原始数据
 * @return 读取成功返回true，失败返回false
 * @details 从存储介质中读取指定合约、指定日期的逐笔成交数据
 *          文件路径格式为: [base_dir]/his/trans/[exchg]/[date]/[code].dsb
 *          这些数据可用于分析市场活跃度和成交流
 */
bool WtBtDtReader::read_raw_transactions(const char* exchg, const char* code, uint32_t uDate, std::string& buffer)
{
	std::stringstream ss;
	ss << _base_dir << "his/trans/" << exchg << "/" << uDate << "/" << code << ".dsb";
	std::string filename = ss.str();
	if (!StdFile::exists(filename.c_str()))
	{
		pipe_btreader_log(_sink, LL_WARN, "Back transaction data file {} not exists", filename);
		return false;
	}

	StdFile::read_file_content(filename.c_str(), buffer);
	bool bSucc = proc_block_data(buffer, false, false);
	if (!bSucc)
		pipe_btreader_log(_sink, LL_ERROR, "Processing back transaction data from file {} failed", filename);

	return bSucc;
}