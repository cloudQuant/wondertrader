/**
 * @file WtStraDtSel.cpp
 * @brief 双突破(Dual Thrust)选股策略实现
 * 
 * @details 实现了基于双突破算法的选股策略，该算法基于上下边界进行交易决策
 */

#include "WtStraDtSel.h"

#include "../Includes/ISelStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"

/** @brief 工厂名称常量，外部变量，在WtSelStraFact中定义 */
extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @param id 策略ID
 * @details 初始化双突破选股策略对象
 */
WtStraDtSel::WtStraDtSel(const char* id)
	:SelStrategy(id)
{
	// 应用父类构造函数初始化策略ID
}


/**
 * @brief 析构函数
 * @details 清理双突破选股策略对象资源
 */
WtStraDtSel::~WtStraDtSel()
{
	// 没有额外资源需要释放
}

/**
 * @brief 获取策略名称
 * @return 策略名称
 * @details 返回双突破选股策略的唯一标识名称
 */
const char* WtStraDtSel::getName()
{
	return "DualThrustSelection";
}

/**
 * @brief 获取策略所属工厂名称
 * @return 工厂名称
 * @details 返回创建该策略的工厂名称，用于策略与工厂的关联
 */
const char* WtStraDtSel::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 初始化策略
 * @param cfg 策略配置
 * @return 初始化是否成功
 * @details 从配置中读取双突破策略所需的参数，包括计算天数、系数、周期等
 */
bool WtStraDtSel::init(WTSVariant* cfg)
{
	// 配置为空则返回失败
	if (cfg == NULL)
		return false;

	// 读取双突破算法相关参数
	_days = cfg->getUInt32("days");   // 计算区间天数
	_k1 = cfg->getDouble("k1");       // 上边界系数
	_k2 = cfg->getDouble("k2");       // 下边界系数

	// 读取K线数据相关参数
	_period = cfg->getCString("period"); // 周期标识
	_count = cfg->getUInt32("count");    // K线条数

	// 读取是否为股票的标记
	_isstk = cfg->getBoolean("stock");

	// 通过参数读取并初始化交易代码列表
	std::string codes = cfg->getCString("codes");
	// 将逗号分隔的代码字符串分割为列表
	auto ayCodes = StrUtil::split(codes, ",");
	// 将每个代码加入代码集合
	for (auto& code : ayCodes)
		_codes.insert(code);

	return true;
}

/**
 * @brief 策略初始化回调
 * @param ctx 策略上下文
 * @details 在策略引擎启动时调用，用于订阅各交易品种的行情数据
 */
void WtStraDtSel::on_init(ISelStraCtx* ctx)
{
	// 遍历所有代码并订阅它们的Tick数据
	for(auto& code : _codes)
	{
		// 订阅每个代码的实时Tick数据
		ctx->stra_sub_ticks(code.c_str());
	}
}

/**
 * @brief 定时调度回调方法，实现双突破策略的核心交易逻辑
 * @param ctx 策略上下文
 * @param uDate 当前日期，YYYYMMDD格式
 * @param uTime 当前时间，HHMMSS格式
 * @details 该方法实现了双突破算法的核心逻辑，计算上下边界并根据价格突破情况进行交易
 */
void WtStraDtSel::on_schedule(ISelStraCtx* ctx, uint32_t uDate, uint32_t uTime)
{
	// 遍历所有监控的交易代码
	for (auto& curCode : _codes)
	{
		// 获取交易品种的交易时间信息
		WTSSessionInfo* sInfo = ctx->stra_get_sessinfo(curCode.c_str());
		// 如果当前不在交易时间内，则跳过处理
		if(!sInfo->isInTradingTime(uTime))
			continue;

		// 构建标准化代码，股票需要添加"-"
		std::string code = curCode;
		if (_isstk)
			code += "-";

		// 获取K线数据
		WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), _count);
		if (kline == NULL)
		{
			//没有获取到K线数据，这里可以输出一些日志
			return;
		}

		// 如果K线数据为空，释放资源并返回
		if (kline->size() == 0)
		{
			kline->release();
			return;
		}

		// 设置交易单位，股票为100股，其他为1
		int32_t trdUnit = 1;
		if (_isstk)
			trdUnit = 100;

		// 转换天数参数为int32_t类型
		int32_t days = (int32_t)_days;

		// 计算历史最高价与最低价
		double hh = kline->maxprice(-days, -2); // 区间内最高价
		double ll = kline->minprice(-days, -2); // 区间内最低价

		// 提取收盘价数据
		WTSValueArray* closes = kline->extractData(KFT_CLOSE);
		double hc = closes->maxvalue(-days, -2); // 区间内最高收盘价
		double lc = closes->minvalue(-days, -2); // 区间内最低收盘价
		double curPx = closes->at(-1);           // 当前收盘价
		closes->release(); // 释放收盘价数据数组资源

		// 获取当前交易日的开盘、最高和最低价
		double openPx = kline->at(-1)->open;
		double highPx = kline->at(-1)->high;
		double lowPx = kline->at(-1)->low;

		// 计算双突破上下边界
		// 上边界 = 开盘价 + K1 * max(最高价-最低收盘价, 最高收盘价-最低价)
		double upper_bound = openPx + _k1 * (std::max(hh - lc, hc - ll));
		// 下边界 = 开盘价 - K2 * max(最高价-最低收盘价, 最高收盘价-最低价)
		double lower_bound = openPx - _k2 * std::max(hh - lc, hc - ll);

		// 获取品种信息
		WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(curCode.c_str());

		// 获取当前持仓并转换为持仓手数
		double curPos = ctx->stra_get_position(curCode.c_str()) / trdUnit;
		
		// 无持仓状态下的入场逻辑
		if (decimal::eq(curPos, 0))
		{
			// 价格突破上边界，进场做多
			if (highPx >= upper_bound)
			{
				ctx->stra_set_position(curCode.c_str(), 1 * trdUnit, "DT_EnterLong");
				// 记录多仓进场日志
				ctx->stra_log_info(fmt::format("{} 向上突破{}>={},多仓进场", curCode.c_str(), highPx, upper_bound).c_str());
			}
			// 价格突破下边界且非股票，进场做空
			else if (lowPx <= lower_bound && !_isstk)
			{
				ctx->stra_set_position(curCode.c_str(), -1 * trdUnit, "DT_EnterShort");
				// 记录空仓进场日志
				ctx->stra_log_info(fmt::format("{} 向下突破{}<={},空仓进场", curCode.c_str(), lowPx, lower_bound).c_str());
			}
		}
		// 多头持仓状态下的出场逻辑
		else if (decimal::gt(curPos, 0))
		{
			// 价格突破下边界，多头出场
			if (lowPx <= lower_bound)
			{
				// 将多头持仓平仓至零
				ctx->stra_set_position(curCode.c_str(), 0, "DT_ExitLong");
				// 记录多仓出场日志
				ctx->stra_log_info(fmt::format("{} 向下突破{}<={},多仓出场", curCode.c_str(), lowPx, lower_bound).c_str());
			}
		}
		// 空头持仓状态下的出场逻辑
		else if (decimal::lt(curPos, 0))
		{
			// 价格突破上边界且非股票，空头出场
			if (highPx >= upper_bound && !_isstk)
			{
				// 将空头持仓平仓至零
				ctx->stra_set_position(curCode.c_str(), 0, "DT_ExitShort");
				// 记录空仓出场日志
				ctx->stra_log_info(fmt::format("{} 向上突破{}>={},空仓出场", curCode.c_str(), highPx, upper_bound).c_str());
			}
		}

		// 释放K线数据资源，这个释放一定要做
		kline->release();
	}
}

/**
 * @brief 行情数据Tick回调
 * @param ctx 策略上下文
 * @param stdCode 标准化合约代码
 * @param newTick 最新的Tick数据
 * @details 当收到新的Tick数据时调用，当前策略中未实现具体处理逻辑，交易信号的生成在on_schedule中实现
 */
void WtStraDtSel::on_tick(ISelStraCtx* ctx, const char* stdCode, WTSTickData* newTick)
{
	// 当前版本中未实现tick数据处理逻辑
}

/**
 * @brief K线数据回调
 * @param ctx 策略上下文
 * @param stdCode 标准化合约代码
 * @param period 周期标识
 * @param newBar 最新的K线数据
 * @details 当收到新的K线数据时调用，当前策略中未实现具体处理逻辑，交易信号的生成在on_schedule中实现
 */
void WtStraDtSel::on_bar(ISelStraCtx* ctx, const char* stdCode, const char* period, WTSBarStruct* newBar)
{
	// 当前版本中未实现新K线数据处理逻辑
}
