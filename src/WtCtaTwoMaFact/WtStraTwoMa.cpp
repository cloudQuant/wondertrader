/*!
 * \file WtStraDualThrust.cpp
 * \brief DualThrust策略实现文件
 * 
 * DualThrust是一种经典的突破交易策略，通过计算上下轨道来确定入场和出场信号
 * 本文件实现了DualThrust策略的各个接口函数
 * 
 * \author Wesley
 */
#include <iostream>
#include "WtStraTwoMa.h"

#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"

extern const char* FACT_NAME;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"

/**
 * \brief WtStraTwoMa策略类构造函数
 * 
 * 初始化策略对象，并将ID传递给父类
 * 
 * \param id 策略ID，用于在策略工厂中标识该策略实例
 */
WtStraTwoMa::WtStraTwoMa(const char* id)
	: CtaStrategy(id)
{
}


/**
 * \brief WtStraTwoMa策略类析构函数
 * 
 * 清理策略对象资源
 */
WtStraTwoMa::~WtStraTwoMa()
{
}

/**
 * \brief 获取策略工厂名称
 * 
 * 返回全局定义的策略工厂名称
 * 
 * \return 策略工厂名称
 */
const char* WtStraTwoMa::getFactName()
{
	return FACT_NAME;
}

/**
 * \brief 获取策略名称
 * 
 * 返回策略的名称，用于标识该策略
 * 
 * \return 策略名称，固定为"TwoMa"
 */
const char* WtStraTwoMa::getName()
{
	return "TwoMa";
}

/**
 * \brief 策略初始化
 * 
 * 根据配置初始化策略参数，包括指标参数、数据周期、合约代码等
 * 
 * \param cfg 策略配置对象，包含各种参数
 * \return 初始化是否成功，true表示成功，false表示失败
 */
bool WtStraTwoMa::init(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;
	std::cout << "WtStraTwoMa::init  begin to init" << std::endl;
	// 读取指标参数
	_short_days = cfg->getUInt32("short_days");
	_long_days = cfg->getUInt32("long_days");

	// 读取数据周期和合约信息
	_period = cfg->getCString("period");
	_count = cfg->getUInt32("count");
	_code = cfg->getCString("code");

	// 读取是否为股票标的
	_isstk = cfg->getBoolean("stock");
	std::cout << "_short_days: " << _short_days << std::endl;
	std::cout << "_long_days: " << _long_days << std::endl;
	std::cout << "_period: " << _period << std::endl;
	std::cout << "_count: " << _count << std::endl;
	std::cout << "_code: " << _code << std::endl;
	return true;
}

/**
 * \brief 交易日开始回调
 * 
 * 在每个交易日开始时调用，主要用于处理主力合约换月问题
 * 当检测到主力合约变化时，将持仓从旧主力合约转移到新主力合约
 * 
 * \param ctx 策略上下文，用于调用策略接口
 * \param uTDate 交易日期，格式为YYYYMMDD
 */
void WtStraTwoMa::on_session_begin(ICtaStraCtx* ctx, uint32_t uTDate)
{
	// 获取当前的主力合约代码
	std::string newMonCode = ctx->stra_get_rawcode(_code.c_str());
	// 如果主力合约发生变化
	if(newMonCode!=_moncode)
	{
		// 如果已有之前的主力合约
		if(!_moncode.empty())
		{
			// 获取当前持仓
			double curPos = ctx->stra_get_position(_moncode.c_str());
			// 如果有持仓，需要转移到新主力合约
			if (!decimal::eq(curPos, 0))
			{
				ctx->stra_log_info(fmt::format("主力换月,  老主力{}[{}]将会被清理", _moncode, curPos).c_str());
				// 将旧主力合约持仓清零
				ctx->stra_set_position(_moncode.c_str(), 0, "switchout");
				// 将相同持仓转移到新主力合约
				ctx->stra_set_position(newMonCode.c_str(), curPos, "switchin");
			}
		}

		// 更新当前主力合约代码
		_moncode = newMonCode;
	}
}

/**
 * \brief 定时调度回调
 * 
 * 在策略定时器触发时调用，执行TwoMa策略的交易信号计算和交易操作
 * 
 * \param ctx 策略上下文，用于调用策略接口
 * \param curDate 当前日期，格式为YYYYMMDD
 * \param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
void WtStraTwoMa::on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime)
{
	// 获取交易的合约代码
	std::string code = _code;

	// 获取K线数据
	WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), _count, true);
	if(kline == NULL)
	{
		//这里可以输出一些日志
		return;
	}

	// 检查K线数据是否为空
	if (kline->size() == 0)
	{
		kline->release();
		return;
	}

	// 设置交易单位，股票为100股，其他为1手
	uint32_t trdUnit = 1;
	if (_isstk)
		trdUnit = 100;


	// 转换回看天数为有符号整数
	//int32_t days = (int32_t)_days;
	int32_t short_days = (int32_t)_short_days;
	int32_t long_days = (int32_t)_long_days;

	// 计算指定天数内的最高价和最低价
	//double hh = kline->maxprice(-days, -2);
	//double ll = kline->minprice(-days, -2);

	// 计算指定天数内的价格
	WTSValueArray* closes = kline->extractData(KFT_CLOSE);
	double pre_short_ma = 0;
	double pre_long_ma = 0;
	double now_short_ma = 0;
	double now_long_ma = 0;
	for (uint32_t i = short_days+1; i > 1 ; i--){
		pre_short_ma += closes->at(-1*i);
	}
	for (uint32_t i = long_days+1; i > 1 ; i--){
		pre_long_ma += closes->at(-1*i);
	}
    now_short_ma = pre_short_ma - closes->at(-1*(short_days+1))+closes->at(-1);
	now_long_ma = pre_long_ma - closes->at(-1*(long_days+1)) + closes->at(-1);
	pre_short_ma = pre_short_ma/short_days;
	pre_long_ma = pre_long_ma/long_days;
	now_short_ma = now_short_ma/short_days;
	now_long_ma = now_long_ma/long_days;
	//double hc = closes->maxvalue(-days, -2);
	//double lc = closes->minvalue(-days, -2);
	double curPx = closes->at(-1); // 当前收盘价
	closes->release();///!!!这个释放一定要做

	// 获取当前周期K线的开盘价
	// double openPx = kline->at(-1)->open;

	// 计算上下轨道
	// double upper_bound = openPx + _k1 * (std::max(hh - lc, hc - ll));
	// double lower_bound = openPx - _k2 * std::max(hh - lc, hc - ll);

	//设置指标值，用于图表显示
	// ctx->set_index_value("DualThrust", "upper_bound", upper_bound);
	// ctx->set_index_value("DualThrust", "lower_bound", lower_bound);

	// 获取合约信息
	WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(_code.c_str());

	// 获取当前持仓并转换为手数
	double curPos = ctx->stra_get_position(_moncode.c_str()) / trdUnit;
	
	// 无持仓时的入场逻辑
	if(decimal::eq(curPos,0))
	{
		// 做多
		if (pre_short_ma < pre_long_ma and now_short_ma > now_long_ma)
		{
			ctx->stra_enter_long(_moncode.c_str(), 2 * trdUnit, "DT_EnterLong");
			//向上突破
			ctx->stra_log_info(fmt::format("金叉{}>={},多仓进场", pre_short_ma, now_short_ma).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-buy", "DT_EnterLong");
		}
		// 死叉 不是股票，做空
		else if ( (pre_short_ma > pre_long_ma and now_short_ma < now_long_ma) && !_isstk)
		{
			ctx->stra_enter_short(_moncode.c_str(), 2 * trdUnit, "DT_EnterShort");
			//向下突破
			ctx->stra_log_info(fmt::format("死叉{}<={},空仓进场", pre_short_ma, now_short_ma).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-sell", "DT_EnterShort");
		}
	}
	// 持有多头仓位时的出场逻辑
	else if (decimal::gt(curPos, 0))
	{
		// 如果当前死叉，多头出场
		if(pre_short_ma > pre_long_ma and now_short_ma < now_long_ma)
		{
			//多仓出场
			ctx->stra_exit_long(_moncode.c_str(), 2 * trdUnit, "DT_ExitLong");
			ctx->stra_log_info(fmt::format("死叉{}<={},多仓出场", pre_short_ma, now_short_ma).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-sell", "DT_ExitLong");
		}
	}
	// 持有空头仓位时的出场逻辑
	else if (decimal::lt(curPos, 0))
	{
		// 如果当前价格突破上轨且不是股票，空头出场
		if ((pre_short_ma < pre_long_ma and now_short_ma > now_long_ma) && !_isstk)
		{
			//空仓出场
			ctx->stra_exit_short(_moncode.c_str(), 2 * trdUnit, "DT_ExitShort");
			ctx->stra_log_info(fmt::format("金叉{}>={},空仓出场", pre_short_ma, now_short_ma).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-buy", "DT_ExitShort");
		}
	}

	//这个释放一定要做
	kline->release();
}

/**
 * \brief 策略初始化回调
 * 
 * 在策略实例创建后立即调用，用于订阅行情和初始化指标
 * 主要完成以下工作：
 * 1. 订阅Tick数据
 * 2. 初始化图表K线
 * 3. 注册指标和指标线
 * 
 * \param ctx 策略上下文，用于调用策略接口
 */
void WtStraTwoMa::on_init(ICtaStraCtx* ctx)
{
	// 获取交易的合约代码
	std::string code = _code;
	// 订阅合约的Tick数据
	ctx->stra_sub_ticks(_code.c_str());
	
	// 获取K线数据，检查数据是否可用
	WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), _count, true);
	if (kline == NULL)
	{
		//这里可以输出一些日志
		return;
	}

	// 释放K线数据
	kline->release();

	//注册指标和图表K线，用于在图表上显示
	ctx->set_chart_kline(_code.c_str(), _period.c_str());

	//注册指标，参数0表示不限制指标值的范围
	ctx->register_index("TwoMa", 0);

	//注册指标线，分别是上轨和下轨
	ctx->register_index_line("TwoMa", "upper_bound", 0);
	ctx->register_index_line("TwoMa", "lower_bound", 0);
}

/**
 * \brief Tick数据回调
 * 
 * 当有新的Tick数据到来时调用
 * 在当前TwoMa策略实现中，不需要在Tick级别进行操作，因此该函数为空
 * 
 * \param ctx 策略上下文，用于调用策略接口
 * \param stdCode 合约代码
 * \param newTick 新的Tick数据
 */
void WtStraTwoMa::on_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick)
{
	//没有什么要处理
}
