/*!
 * \file ExecMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 执行单元模拟器实现文件
 * \details 该文件实现了ExecMocker类，用于回测环境下的交易指令模拟执行、撮合及仓位管理
 */
#include "ExecMocker.h"
#include "WtHelper.h"

#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../WTSTools/WTSLogger.h"

#include <boost/filesystem.hpp>

#define PRICE_DOUBLE_TO_INT_P(x) ((int32_t)((x)*10000.0 + 0.5))
#define PRICE_DOUBLE_TO_INT_N(x) ((int32_t)((x)*10000.0 - 0.5))
#define PRICE_DOUBLE_TO_INT(x) (((x)==DBL_MAX)?0:((x)>0?PRICE_DOUBLE_TO_INT_P(x):PRICE_DOUBLE_TO_INT_N(x)))

/**
 * @brief 生成本地订单ID
 * @return uint32_t 新生成的本地订单ID
 * @details 外部全局函数，用于生成唯一的本地订单ID
 */
extern uint32_t makeLocalOrderID();

/**
 * @brief 构造函数
 * @param replayer 历史数据回放器指针
 * @details 初始化执行单元模拟器的各个成员变量，包括持仓量、未完成数量、统计计数器等
 */
ExecMocker::ExecMocker(HisDataReplayer* replayer)
	: _replayer(replayer)
	, _position(0)       // 初始持仓量为0
	, _undone(0)         // 初始未完成数量为0
	, _ord_qty(0)        // 初始委托总量为0
	, _ord_cnt(0)        // 初始委托计数为0
	, _cacl_cnt(0)       // 初始撤单计数为0
	, _cacl_qty(0)       // 初始撤单总量为0
	, _sig_cnt(0)        // 初始信号计数为0
	, _sig_px(DBL_MAX)   // 初始信号价格设为最大值
	, _last_tick(NULL)   // 初始最新Tick指针为空
{
}


/**
 * @brief 析构函数
 * @details 释放模拟器持有的资源，主要是最后Tick数据对象
 */
ExecMocker::~ExecMocker()
{
	// 释放最新Tick数据对象，如果存在
	if (_last_tick)
		_last_tick->release();
}

/**
 * @brief 初始化执行单元模拟器
 * @param cfg 配置参数对象
 * @return bool 初始化是否成功
 * @details 从配置中加载各种参数，初始化撮合引擎，并加载执行器工厂动态库
 *          创建实际的执行单元实例并完成初始化
 */
bool ExecMocker::init(WTSVariant* cfg)
{
	// 从配置中读取基本参数
	const char* module = cfg->getCString("module");  // 动态库模块路径
	_code = cfg->getCString("code");                // 合约代码
	_period = cfg->getCString("period");            // K线周期
	_volunit = cfg->getDouble("volunit");           // 数量单位
	_volmode = cfg->getInt32("volmode");            // 数量模式：0-反复正负，-1-一直卖，+1-一直买

	// 初始化撮合引擎
	_matcher.regisSink(this);                       // 注册当前对象为撮合引擎的回调接收者
	_matcher.init(cfg->get("matcher"));              // 使用配置初始化撮合引擎

	// 加载执行器工厂动态库
	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
		return false;  // 加载失败返回失败

	// 获取创建执行器工厂的函数指针
	FuncCreateExeFact creator = (FuncCreateExeFact)DLLHelper::get_symbol(hInst, "createExecFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);  // 获取失败时释放动态库句柄
		return false;
	}

	// 设置执行器工厂相关信息
	_factory._module_inst = hInst;
	_factory._module_path = module;
	_factory._creator = creator;
	_factory._remover = (FuncDeleteExeFact)DLLHelper::get_symbol(hInst, "deleteExecFact");
	_factory._fact = _factory._creator();  // 创建执行器工厂实例

	// 读取执行器配置并创建执行单元
	WTSVariant* cfgExec = cfg->get("executer");
	if (cfgExec)
	{
		// 创建并初始化执行单元
		_exec_unit = _factory._fact->createExeUnit(cfgExec->getCString("name"));
		_exec_unit->init(this, _code.c_str(), cfgExec->get("params"));
		_id = cfgExec->getCString("id");  // 设置执行单元ID
	}

	return true;
}

/**
 * @brief 获取商品信息
 * @param stdCode 标准化合约代码
 * @return WTSCommodityInfo* 商品信息对象指针
 * @details 从回放器中获取特定合约对应的商品信息
 */
WTSCommodityInfo* ExecMocker::getCommodityInfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

/**
 * @brief 获取交易时段信息
 * @param stdCode 标准化合约代码
 * @return WTSSessionInfo* 交易时段信息对象指针
 * @details 从回放器中获取特定合约对应的交易时段信息，用于确定合约的交易时间
 */
WTSSessionInfo* ExecMocker::getSessionInfo(const char* stdCode)
{
	return _replayer->get_session_info(stdCode, true);
}

/**
 * @brief 获取当前时间
 * @return uint64_t 当前时间戳，格式为YYYYMMDDHHMMSSsss
 * @details 基于回放器当前的日期和时间生成时间戳，用于计算和记录交易时间
 */
uint64_t ExecMocker::getCurTime()
{
	return TimeUtils::makeTime(_replayer->get_date(), _replayer->get_raw_time() * 100000 + _replayer->get_secs());
}

/**
 * @brief 处理K线周期结束事件
 * @param stdCode 标准化合约代码
 * @param period K线周期代码（如m, d）
 * @param times 周期倍数
 * @param newBar 新建成的K线数据
 * @details 当一个K线周期结束时被回测引擎调用，当前实现为空
 */
void ExecMocker::handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	//throw std::logic_error("The method or operation is not implemented.");
}

/**
 * @brief 处理交易日开始事件
 * @param curTDate 当前交易日（YYYYMMDD格式）
 * @details 在每个交易日开始时被回测引擎调用，当前实现为空
 */
void ExecMocker::handle_session_begin(uint32_t curTDate)
{
	//throw std::logic_error("The method or operation is not implemented.");
}

/**
 * @brief 处理交易日结束事件
 * @param curTDate 当前交易日（YYYYMMDD格式）
 * @details 在每个交易日结束时被回测引擎调用，清理撮合引擎中的未完成订单
 *          并将当天交易统计信息输出到日志
 */
void ExecMocker::handle_session_end(uint32_t curTDate)
{
	// 清理撮合引擎中的未完成订单
	_matcher.clear();
	// 重置未完成委托数量
	_undone = 0;

	// 输出交易统计信息到日志
	WTSLogger::info("Total entrust:{}, total quantity:{}, total cancels:{}, total cancel quantity:{}, total signals:{}", 
		_ord_cnt, _ord_qty, _cacl_cnt, _cacl_qty, _sig_cnt);
}

/**
 * @brief 处理tick行情数据
 * @param stdCode 标准化合约代码
 * @param curTick 当前的tick数据
 * @param pxType 价格类型
 * @details 接收并处理最新的tick数据，保存当前的tick，并转发给撮合引擎和执行单元
 */
void ExecMocker::handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType)
{
	// 释放之前的tick数据，防止内存泄漏
 	if (_last_tick)
	{
		_last_tick->release();
		_last_tick = NULL;
	}

	// 保存新的tick数据并增加引用计数
	_last_tick = curTick;
	_last_tick->retain();
	
	// 将tick数据转发给撮合引擎处理
	_matcher.handle_tick(stdCode, curTick);

	// 如果执行单元存在，将tick数据转发给执行单元
	if (_exec_unit)
		_exec_unit->on_tick(curTick);
}

/**
 * @brief 初始化处理
 * @details 在回测开始前进行初始化操作，包括订阅数据、初始化执行单元、设置初始仓位等
 */
void ExecMocker::handle_init()
{
	// 解析周期参数，提取基础周期和倍数
	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = _period[0];    // 获取基础周期（如m、d等）
	uint32_t times = 1;             // 默认倍数为1
	if (_period.size() > 1)         // 如果有倍数设置（如m5中的5）
		times = strtoul(_period.c_str() + 1, NULL, 10);

	// 获取K线分片数据，用于预热
	WTSKlineSlice* kline = _replayer->get_kline_slice(_code.c_str(), basePeriod,  10, times, true);
	if (kline)
		kline->release();  // 释放分片数据，防止内存泄漏

	// 订阅合约的Tick数据
	_replayer->sub_tick(0, _code.c_str());

	// 初始化交易日志头部
	_trade_logs << "localid,signaltime,ordertime,bs,sigprice,ordprice,lmtprice,tradetime,trdprice,qty,sigtimespan,exectime,cancel" << std::endl;

	// 通知执行单元通道已就绪
	_exec_unit->on_channel_ready();

	// 设置初始信号时间
	_sig_time = (uint64_t)_replayer->get_date() * 10000 + _replayer->get_raw_time();

	// 设置初始目标仓位
	_exec_unit->set_position(_code.c_str(), _volunit);
	WTSLogger::info("Target position updated at the beginning: {}", _volunit);
}

/**
 * @brief 处理定时任务
 * @param uDate 当前日期（YYYYMMDD格式）
 * @param uTime 当前时间（HHMMSS格式）
 * @details 根据设定的定时产生交易信号，并根据仓位模式计算目标仓位
 */
void ExecMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	// 如果时间是15:00，则不处理
	if (uTime == 1500)
		return;

	// 获取当前信号价格，使用最新价格或昨收价
	_sig_px = _last_tick->price();
	if (_sig_px == DBL_MAX || _sig_px == FLT_MAX)  // 如果价格无效，使用昨收价
		_sig_px = _last_tick->preclose();

	// 记录信号生成时间
	_sig_time = (uint64_t)uDate * 10000 + uTime;
	
	// 根据仓位模式计算目标仓位
	if(_volmode == 0)  // 反复正负模式，持仓为负时则做多，持仓为正时则做空
	{
		if (_position <= 0)
			_target = _volunit;     // 如果当前空仓或无仓，则做多
		else
			_target = -_volunit;    // 如果当前多仓，则做空
	}
	else if (_volmode == -1)  // 一直卖模式，目标仓位不断减少
	{
		_target -= _volunit;
	}
	else if (_volmode == 1)   // 一直买模式，目标仓位不断增加
	{
		_target += _volunit;
	}

	// 设置执行单元的目标仓位
	_exec_unit->set_position(_code.c_str(), _target);
	WTSLogger::info("Target position updated @{}.{}: {}", uDate, uTime, _volunit);
	// 信号计数增加
	_sig_cnt++;
}

/**
 * @brief 获取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 需要获取的Tick数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return WTSTickSlice* Tick数据切片指针
 * @details 从回放器中获取指定合约的历史Tick数据切片
 */
WTSTickSlice* ExecMocker::getTicks(const char* stdCode, uint32_t count, uint64_t etime /*= 0*/)
{
	return _replayer->get_tick_slice(stdCode, count, etime);
}

/**
 * @brief 获取最新的Tick数据
 * @param stdCode 标准化合约代码
 * @return WTSTickData* 最新的Tick数据指针
 * @details 从回放器中获取指定合约的最新Tick数据
 */
WTSTickData* ExecMocker::grabLastTick(const char* stdCode)
{
	return _replayer->get_last_tick(stdCode);
}

/**
 * @brief 获取当前持仓量
 * @param stdCode 标准化合约代码
 * @param validOnly 是否只考虑有效持仓，默认为true
 * @param flag 持仓方向标志，默认为3（全部）
 * @return double 当前持仓量，正数为多头持仓，负数为空头持仓
 * @details 返回当前模拟器中记录的持仓量，忽略参数设置
 */
double ExecMocker::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	return _position;  // 直接返回内部记录的持仓量，不考虑参数
}

/**
 * @brief 获取当前所有订单
 * @param stdCode 标准化合约代码
 * @return OrderMap* 订单映射对象指针
 * @details 应该从撮合引擎中获取指定合约的所有未完成订单，当前实现返回NULL
 */
OrderMap* ExecMocker::getOrders(const char* stdCode)
{
	return NULL;  // 当前实现返回NULL，可以改进为返回_matcher.getOrders(stdCode)
}

/**
 * @brief 获取未完成数量
 * @param stdCode 标准化合约代码
 * @return double 未完成数量，正数表示多头未完成，负数表示空头未完成
 * @details 返回当前模拟器中记录的未完成指令数量
 */
double ExecMocker::getUndoneQty(const char* stdCode)
{
	return _undone;  // 返回内部记录的未完成数量
}

/**
 * @brief 发送买入委托
 * @param stdCode 标准化合约代码
 * @param price 买入价格
 * @param qty 买入数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 * @details 发送买入委托到撮合引擎，并更新相关统计信息
 */
OrderIDs ExecMocker::buy(const char* stdCode, double price, double qty, bool bForceClose /* = false */)
{
	// 计算当前时间戳，格式为YYYYMMDDHHMMSSsss
	uint64_t curTime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	// 调用撮合引擎的买入方法
	OrderIDs ret = _matcher.buy(stdCode, price, qty, curTime);

	// 如果订单创建成功，更新统计信息
	if(!ret.empty())
	{
		_ord_cnt++;  // 增加委托计数
		_ord_qty += qty;  // 增加委托总量

		_undone += (int32_t)qty;  // 增加未完成多头数量
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	return ret;
}

/**
 * @brief 发送卖出委托
 * @param stdCode 标准化合约代码
 * @param price 卖出价格
 * @param qty 卖出数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 * @details 发送卖出委托到撮合引擎，并更新相关统计信息
 */
OrderIDs ExecMocker::sell(const char* stdCode, double price, double qty, bool bForceClose /* = false */)
{
	// 计算当前时间戳，格式为YYYYMMDDHHMMSSsss
	uint64_t curTime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	// 调用撮合引擎的卖出方法
	OrderIDs ret = _matcher.sell(stdCode, price, qty, curTime);

	// 如果订单创建成功，更新统计信息
	if(!ret.empty())
	{
		_ord_cnt++;  // 增加委托计数
		_ord_qty += qty;  // 增加委托总量
	
		_undone -= (int32_t)qty;  // 增加未完成空头数量（负值表示空头）
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	return ret;
}

/**
 * @brief 取消指定订单
 * @param localid 本地订单ID
 * @return bool 是否成功取消
 * @details 取消指定的订单，并更新相关统计信息
 */
bool ExecMocker::cancel(uint32_t localid)
{
	// 调用撮合引擎的取消方法，返回取消的数量
	double change = _matcher.cancel(localid);
	// 如果没有取消任何数量，返回false
	if (decimal::eq(change, 0))
		return false;

	// 更新未完成数量
	_undone -= change;
	// 更新取消统计信息
	_cacl_cnt++;  // 增加取消计数
	_cacl_qty += abs(change);  // 增加取消总量
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);

	return true;
}

/**
 * @brief 批量取消指定合约的订单
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param qty 要取消的数量，默认为0表示取消全部
 * @return OrderIDs 被取消的订单ID列表
 * @details 批量取消指定合约和方向的订单，并更新相关统计信息
 */
OrderIDs ExecMocker::cancel(const char* stdCode, bool isBuy, double qty /*= 0*/)
{
	// 调用撮合引擎的批量取消方法，并提供回调函数处理每个被取消的订单
	OrderIDs ret = _matcher.cancel(stdCode, isBuy, qty, [this](double change) {
		// 更新未完成数量
		_undone -= change;

		// 更新取消统计信息
		_cacl_cnt++;  // 增加取消计数
		_cacl_qty += abs(change);  // 增加取消总量
	});
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);

	return ret;
}

/**
 * @brief 写入日志
 * @param message 日志消息
 * @details 将指定的日志消息写入到日志系统中，使用执行器的ID作为标识
 */
void ExecMocker::writeLog(const char* message)
{
	WTSLogger::log_dyn_raw("executer", _id.c_str(), LL_INFO, message);
}

/**
 * @brief 处理委托回报
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 是否委托成功
 * @param message 委托回报消息
 * @param ordTime 委托时间
 * @details 处理委托回报事件，并将其转发给执行单元
 */
void ExecMocker::handle_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, uint64_t ordTime)
{
	// 将委托回报转发给执行单元
	_exec_unit->on_entrust(localid, stdCode, bSuccess, message);
}

/**
 * @brief 处理订单状态变化
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 委托价格
 * @param isCanceled 是否已取消
 * @param ordTime 委托时间
 * @details 处理订单状态变化事件，包括订单取消和其他状态更新，并记录交易日志
 */
void ExecMocker::handle_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled, uint64_t ordTime)
{
	// 获取当前时间
	uint64_t curTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	uint64_t curUnixTime = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());

	// 计算信号时间和委托时间的Unix时间戳
	uint64_t sigUnixTime = TimeUtils::makeTime((uint32_t)(_sig_time / 10000), _sig_time % 10000 * 100000);
	uint64_t ordUnixTime = TimeUtils::makeTime((uint32_t)(ordTime / 1000000000), ordTime % 1000000000);

	// 如果订单被取消，记录取消日志
	if(isCanceled)
	{
		// 如果信号价格无效，使用昨收价
		if (_sig_px == DBL_MAX)
			_sig_px = _last_tick->preclose();

		// 记录取消订单的交易日志
		_trade_logs << localid << ","
			<< _sig_time << ","
			<< ordTime << ","
			<< (isBuy ? "B" : "S") << ","
			<< _sig_px << ","
			<< 0 << ","
			<< price << ","
			<< curTime << ","
			<< price << ","
			<< 0 << ","
			<< curUnixTime - sigUnixTime << ","
			<< curUnixTime - ordUnixTime << ","
			<< "true" << std::endl;

		// 更新未完成数量
		_undone -= leftover * (isBuy ? 1 : -1);
		WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	}

	// 将订单状态变化转发给执行单元
	_exec_unit->on_order(localid, stdCode, isBuy, leftover, price, isCanceled);
}

/**
 * @brief 处理成交事件
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param vol 成交数量
 * @param fireprice 委托价格
 * @param price 成交价格
 * @param ordTime 委托时间
 * @details 处理订单成交事件，更新持仓和未完成数量，记录交易日志
 */
void ExecMocker::handle_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double fireprice, double price, uint64_t ordTime)
{
	// 获取当前时间
	uint64_t curTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	uint64_t curUnixTime = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());

	// 计算信号时间和委托时间的Unix时间戳
	uint64_t sigUnixTime = TimeUtils::makeTime((uint32_t)(_sig_time / 10000), _sig_time % 10000 * 100000);
	uint64_t ordUnixTime = TimeUtils::makeTime((uint32_t)(ordTime / 1000000000), ordTime % 1000000000);

	// 如果信号价格无效，使用昨收价
	if (_sig_px == DBL_MAX)
		_sig_px = _last_tick->preclose();

	// 记录成交的交易日志
	_trade_logs << localid << ","
		<< _sig_time << ","
		<< ordTime << ","
		<< (isBuy?"B":"S") << ","
		<< _sig_px << ","
		<< fireprice << ","
		<< price << ","
		<< curTime << ","
		<< price << ","
		<< vol << ","
		<< curUnixTime - sigUnixTime << ","
		<< curUnixTime - ordUnixTime << ","
		<< "false" << std::endl;

	// 更新持仓量，买入增加，卖出减少
	_position += vol* (isBuy?1:-1);
	// 更新未完成数量
	_undone -= vol * (isBuy ? 1 : -1);
	WTSLogger::info("{}, undone orders updated: {}", __FUNCTION__, _undone);
	WTSLogger::info("Position updated: {}", _position);

	// 将成交事件转发给执行单元
	_exec_unit->on_trade(localid, stdCode, isBuy, vol, price);
}

/**
 * @brief 处理回放结束事件
 * @details 当回测完成时调用此函数，将交易日志保存到文件
 */
void ExecMocker::handle_replay_done()
{
	// 创建输出目录
	std::string folder = WtHelper::getOutputDir();
	folder += "exec/";
	boost::filesystem::create_directories(folder.c_str());

	// 生成交易日志文件名
	std::stringstream ss;
	ss << folder << "trades_" << _id << ".csv";
	std::string filename = ss.str();
	
	// 将交易日志内容写入文件
	StdFile::write_file_content(filename.c_str(), _trade_logs.str());
}