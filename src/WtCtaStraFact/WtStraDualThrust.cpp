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
#include <cmath>
#include <cctype>
#include <limits>
#include <memory>
#include <locale>
#include <sstream>
#include "WtStraDualThrust.h"

#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"

extern const char* FACT_NAME;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"

namespace
{
template <typename T>
struct WtsRelease
{
	void operator()(T* value) const noexcept
	{
		if (value != NULL)
			value->release();
	}
};

bool isValidPrice(double value)
{
	return value != INVALID_DOUBLE && std::isfinite(value);
}

bool readNumber(const WTSVariant* value, double& result)
{
	if (value == NULL || value->type() == WTSVariant::VT_Boolean ||
		value->type() == WTSVariant::VT_Null || value->isArray() || value->isObject())
		return false;
	std::istringstream input(value->asString());
	input.imbue(std::locale::classic());
	if (!(input >> result))
		return false;
	input >> std::ws;
	return input.eof() && isValidPrice(result);
}

bool readPositiveUInt32(const WTSVariant* value, uint32_t& result)
{
	if (value == NULL || value->type() == WTSVariant::VT_Boolean ||
		value->type() == WTSVariant::VT_Null || value->isArray() || value->isObject())
		return false;
	// Decimal/scientific notation is normalized as text, avoiding rounding a
	// fractional value or overflow into a valid integer near UINT32_MAX.
	const std::string text = value->asString();
	size_t pos = 0;
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
	if (pos < text.size() && text[pos] == '+') ++pos;
	std::string digits;
	size_t fractional = 0;
	bool dot = false;
	while (pos < text.size())
	{
		const char c = text[pos];
		if (c >= '0' && c <= '9')
		{
			digits.push_back(c);
			if (dot) ++fractional;
		}
		else if (c == '.' && !dot) dot = true;
		else break;
		++pos;
	}
	if (digits.empty()) return false;
	int exponent = 0;
	if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E'))
	{
		++pos;
		bool negative = false;
		if (pos < text.size() && (text[pos] == '+' || text[pos] == '-'))
			negative = text[pos++] == '-';
		const size_t start = pos;
		while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
		{
			if (exponent > 10000) return false;
			exponent = exponent * 10 + (text[pos++] - '0');
		}
		if (pos == start) return false;
		if (negative) exponent = -exponent;
	}
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
	if (pos != text.size()) return false;
	const size_t first = digits.find_first_not_of('0');
	if (first == std::string::npos) return false;
	digits.erase(0, first);
	if (fractional > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
	const int64_t shift = static_cast<int64_t>(exponent) - static_cast<int64_t>(fractional);
	if (shift < 0)
	{
		const uint64_t remove = static_cast<uint64_t>(-shift);
		if (remove >= digits.size() || digits.find_last_not_of('0') >= digits.size() - remove)
			return false;
		digits.resize(digits.size() - static_cast<size_t>(remove));
	}
	else if (shift > 0)
	{
		if (shift > 10 || digits.size() + static_cast<size_t>(shift) > 10) return false;
		digits.append(static_cast<size_t>(shift), '0');
	}
	if (digits.size() > 10) return false;
	uint64_t parsed = 0;
	for (char c : digits) parsed = parsed * 10 + static_cast<unsigned>(c - '0');
	if (parsed > (std::numeric_limits<uint32_t>::max)()) return false;
	result = static_cast<uint32_t>(parsed);
	return true;
}
}

/**
 * \brief WtStraDualThrust策略类构造函数
 * 
 * 初始化策略对象，并将ID传递给父类
 * 
 * \param id 策略ID，用于在策略工厂中标识该策略实例
 */
WtStraDualThrust::WtStraDualThrust(const char* id)
	: CtaStrategy(id)
{
}


/**
 * \brief WtStraDualThrust策略类析构函数
 * 
 * 清理策略对象资源
 */
WtStraDualThrust::~WtStraDualThrust()
{
}

/**
 * \brief 获取策略工厂名称
 * 
 * 返回全局定义的策略工厂名称
 * 
 * \return 策略工厂名称
 */
const char* WtStraDualThrust::getFactName()
{
	return FACT_NAME;
}

/**
 * \brief 获取策略名称
 * 
 * 返回策略的名称，用于标识该策略
 * 
 * \return 策略名称，固定为"DualThrust"
 */
const char* WtStraDualThrust::getName()
{
	return "DualThrust";
}

/**
 * \brief 策略初始化
 * 
 * 根据配置初始化策略参数，包括指标参数、数据周期、合约代码等
 * 
 * \param cfg 策略配置对象，包含各种参数
 * \return 初始化是否成功，true表示成功，false表示失败
 */
bool WtStraDualThrust::init(WTSVariant* cfg)
{
	std::cout << "WtStraDualThrust::init begins" << std::endl;
	if (cfg == NULL || !cfg->isObject())
		return false;

	// Validate every parameter before publishing any of the new configuration.
	// The days field must explicitly provide a positive lookback period.
	uint32_t days = 0, count = 0;
	double k1 = 0, k2 = 0;
	const WTSVariant* daysValue = cfg->get("days");
	const WTSVariant* k1Value = cfg->get("k1");
	const WTSVariant* k2Value = cfg->get("k2");
	const WTSVariant* periodValue = cfg->get("period");
	const WTSVariant* codeValue = cfg->get("code");
	if (!readPositiveUInt32(daysValue, days) ||
		!readPositiveUInt32(cfg->get("count"), count) ||
		(k1Value != NULL && !readNumber(k1Value, k1)) ||
		(k2Value != NULL && !readNumber(k2Value, k2)) ||
		periodValue == NULL || periodValue->type() != WTSVariant::VT_String ||
		codeValue == NULL || codeValue->type() != WTSVariant::VT_String)
		return false;
	const std::string period = periodValue->asString();
	const std::string code = codeValue->asString();
	if (period.empty() || code.empty())
		return false;

	_days = days;
	_k1 = k1;
	_k2 = k2;
	_period = period;
	_count = count;
	_code = code;
	_isstk = cfg->getBoolean("stock");
	std::cout << "WtStraDualThrust::init ends" << std::endl;
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
void WtStraDualThrust::on_session_begin(ICtaStraCtx* ctx, uint32_t /*uTDate*/)
{
	std::string newMonCode = ctx->stra_get_rawcode(_code.c_str());
	if(newMonCode!=_moncode)
	{
		if(!_moncode.empty())
		{
			double curPos = ctx->stra_get_position(_moncode.c_str());
			if (!decimal::eq(curPos, 0))
			{
				ctx->stra_log_info(fmt::format("主力换月,  老主力{}[{}]将会被清理", _moncode, curPos).c_str());
				ctx->stra_set_position(_moncode.c_str(), 0, "switchout");
				ctx->stra_set_position(newMonCode.c_str(), curPos, "switchin");
			}
		}

		_moncode = newMonCode;
	}
}

/**
 * \brief 定时调度回调
 * 
 * 在策略定时器触发时调用，执行DualThrust策略的交易信号计算和交易操作
 * 主要流程：
 * 1. 获取K线数据
 * 2. 计算上下轨道
 * 3. 根据当前价格和持仓状态决定交易操作
 * 
 * \param ctx 策略上下文，用于调用策略接口
 * \param curDate 当前日期，格式为YYYYMMDD
 * \param curTime 当前时间，格式为HHMMSS或HHMMSS000
 */
void WtStraDualThrust::on_schedule(ICtaStraCtx* ctx, uint32_t /*curDate*/, uint32_t /*curTime*/)
{
	// 获取交易的合约代码
	std::string code = _code;

	// 获取K线数据
	std::unique_ptr<WTSKlineSlice, WtsRelease<WTSKlineSlice>> kline(
		ctx->stra_get_bars(code.c_str(), _period.c_str(), _count, true));
	if(kline == NULL)
	{
		//这里可以输出一些日志
		return;
	}

	// The [-days, -2] window needs at least one completed bar plus the
	// current bar. An explicit days of 1 still waits,
	// but do not produce signals without a representable, complete window.
	const int32_t count = kline->size();
	if (count <= 0 || _days < 2 ||
		_days > static_cast<uint32_t>((std::numeric_limits<int32_t>::max)()) ||
		_days > static_cast<uint32_t>(count))
		return;

	const int32_t days = static_cast<int32_t>(_days);
	for (int32_t i = count - days; i < count - 1; ++i)
	{
		const WTSBarStruct* bar = kline->at(i);
		if (bar == NULL || !isValidPrice(bar->high) || !isValidPrice(bar->low) ||
			!isValidPrice(bar->close))
			return;
	}
	const WTSBarStruct* currentBar = kline->at(-1);
	if (currentBar == NULL || !isValidPrice(currentBar->open) ||
		!isValidPrice(currentBar->close) || !isValidPrice(_k1) || !isValidPrice(_k2))
		return;

	// 设置交易单位，股票为100股，其他为1手
	uint32_t trdUnit = 1;
	if (_isstk)
		trdUnit = 100;


	// 计算指定天数内的最高价和最低价
	double hh = kline->maxprice(-days, -2);
	double ll = kline->minprice(-days, -2);

	// 计算指定天数内的最高收盘价和最低收盘价
	std::unique_ptr<WTSValueArray, WtsRelease<WTSValueArray>> closes(kline->extractData(KFT_CLOSE));
	if (closes == NULL || closes->size() < static_cast<uint32_t>(count))
		return;
	double hc = closes->maxvalue(-days, -2);
	double lc = closes->minvalue(-days, -2);
	double curPx = closes->at(-1); // 当前收盘价
	closes.reset();

	// 获取当前周期K线的开盘价
	double openPx = currentBar->open;
	if (!isValidPrice(hh) || !isValidPrice(ll) || !isValidPrice(hc) ||
		!isValidPrice(lc) || !isValidPrice(curPx))
		return;
	const double highRange = hh - lc;
	const double lowRange = hc - ll;
	if (!isValidPrice(highRange) || !isValidPrice(lowRange))
		return;

	// 计算上下轨道
	double upper_bound = openPx + _k1 * std::max(highRange, lowRange);
	double lower_bound = openPx - _k2 * std::max(highRange, lowRange);
	if (!isValidPrice(upper_bound) || !isValidPrice(lower_bound))
		return;

	//设置指标值，用于图表显示
	ctx->set_index_value("DualThrust", "upper_bound", upper_bound);
	ctx->set_index_value("DualThrust", "lower_bound", lower_bound);

	// 获取合约信息
	[[maybe_unused]] WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(_code.c_str());

	// 获取当前持仓并转换为手数
	double curPos = ctx->stra_get_position(_moncode.c_str()) / trdUnit;
	
	// 无持仓时的入场逻辑
	if(decimal::eq(curPos,0))
	{
		// 如果当前价格突破上轨，做多
		if(curPx >= upper_bound)
		{
			ctx->stra_enter_long(_moncode.c_str(), 2 * trdUnit, "DT_EnterLong");
			//向上突破
			ctx->stra_log_info(fmt::format("向上突破{}>={},多仓进场", curPx, upper_bound).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-buy", "DT_EnterLong");
		}
		// 如果当前价格突破下轨且不是股票，做空
		else if (curPx <= lower_bound && !_isstk)
		{
			ctx->stra_enter_short(_moncode.c_str(), 2 * trdUnit, "DT_EnterShort");
			//向下突破
			ctx->stra_log_info(fmt::format("向下突破{}<={},空仓进场", curPx, lower_bound).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-sell", "DT_EnterShort");
		}
	}
	// 持有多头仓位时的出场逻辑
	else if (decimal::gt(curPos, 0))
	{
		// 如果当前价格突破下轨，多头出场
		if(curPx <= lower_bound)
		{
			//多仓出场
			ctx->stra_exit_long(_moncode.c_str(), 2 * trdUnit, "DT_ExitLong");
			ctx->stra_log_info(fmt::format("向下突破{}<={},多仓出场", curPx, lower_bound).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-sell", "DT_ExitLong");
		}
	}
	// 持有空头仓位时的出场逻辑
	else if (decimal::lt(curPos, 0))
	{
		// 如果当前价格突破上轨且不是股票，空头出场
		if (curPx >= upper_bound && !_isstk)
		{
			//空仓出场
			ctx->stra_exit_short(_moncode.c_str(), 2 * trdUnit, "DT_ExitShort");
			ctx->stra_log_info(fmt::format("向上突破{}>={},空仓出场", curPx, upper_bound).c_str());

			//添加图表标记
			ctx->add_chart_mark(curPx, "wt-mark-buy", "DT_ExitShort");
		}
	}

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
void WtStraDualThrust::on_init(ICtaStraCtx* ctx)
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
	ctx->register_index("DualThrust", 0);

	//注册指标线，分别是上轨和下轨
	ctx->register_index_line("DualThrust", "upper_bound", 0);
	ctx->register_index_line("DualThrust", "lower_bound", 0);
}

/**
 * \brief Tick数据回调
 * 
 * 当有新的Tick数据到来时调用
 * 在当前DualThrust策略实现中，不需要在Tick级别进行操作，因此该函数为空
 * 
 * \param ctx 策略上下文，用于调用策略接口
 * \param stdCode 合约代码
 * \param newTick 新的Tick数据
 */
void WtStraDualThrust::on_tick(ICtaStraCtx* /*ctx*/, const char* /*stdCode*/, WTSTickData* /*newTick*/)
{
	//没有什么要处理
}
