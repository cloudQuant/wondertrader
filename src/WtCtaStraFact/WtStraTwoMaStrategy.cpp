#include "WtStraTwoMaStrategy.h"

#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"

#include <iostream>

extern const char* FACT_NAME;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"

WtStraTwoMaStrategy::WtStraTwoMaStrategy(const char* id)
	: CtaStrategy(id)
{
}


WtStraTwoMaStrategy::~WtStraTwoMaStrategy()
{
}

const char* WtStraTwoMaStrategy::getFactName()
{
	return FACT_NAME;
}

const char* WtStraTwoMaStrategy::getName()
{
	return "TwoMaStrategy";
}

bool WtStraTwoMaStrategy::init(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	short_days = cfg->getUInt32("short_days");
	long_days = cfg->getDouble("long_days");
	pre_long_ma = NAN;
	pre_short_ma = NAN;

	_period = cfg->getCString("period");
	_count = cfg->getUInt32("count");
	_code = cfg->getCString("code");

	_isstk = cfg->getBoolean("stock");
	std::cout << "参数初始化情况" << std::endl;
	std::cout << "short_days = " << short_days << std::endl;
	std::cout << "long_days = " << long_days << std::endl;
	std::cout << "_period = " << _period << std::endl;
	std::cout << "_count = " << _count << std::endl;
	std::cout << "_code = " << _code << std::endl;
	return true;
}

void WtStraTwoMaStrategy::on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime)
{
	std::string code = _code;
	if (_isstk)
		code += "-";
	WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), long_days, true);
	if(kline == NULL)
	{
		//这里可以输出一些日志
		std::cout << "kline is NULL" << std::endl;
		return;
	}

	if (kline->size() == 0)
	{
		std::cout << "kline is length 0" << std::endl;
		kline->release();
		return;
	}

	uint32_t trdUnit = 1;
	if (_isstk)
		trdUnit = 100;

	//std::cout << "short_days = " << short_days << std::endl;
	//std::cout << "long_days = " << long_days << std::endl;

	// 尝试获取当前的时间，价格
	auto now_date = kline->at(-1)->date;
	auto now_time = kline->at(-1)->time;
	double now_open = kline->at(-1)->open;
	double now_close = kline->at(-1)->close;

	//int32_t _short_days = static_cast<int32_t>(short_days);
    //int32_t _long_days = static_cast<int32_t>(long_days);
    double short_sum = 0;
    // 计算短周期均线
    for (uint32_t i=1; i<=short_days; i++){
        short_sum+=kline->at(-1*i)->close;
    }
    double now_short_ma = short_sum / short_days;
 
    // 计算前一期长期均线
    double long_sum = 0;
	for (uint32_t j = 1; j <= long_days; j++) {
		double close_price = kline->at(-1 * j)->close;
		//std::cout <<"j = " << j <<  " now_time = " << kline->at(-1*j)->time << " now_close = " << close_price << std::endl;
        long_sum+=close_price;
    }
	double now_long_ma = long_sum / long_days;
   

	//ctx->stra_log_info(fmt::format("now_datetime = {}:{}, now_close = {}, short_ma = {},long_ma = {}", now_date, now_time, now_close, now_short_ma, now_long_ma).c_str());

	WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(_code.c_str());

	double curPos = ctx->stra_get_position(_code.c_str()) / trdUnit;
	
	//if(curPos > 0)
	if (decimal::gt(curPos, 0))
	{
		if(pre_short_ma > pre_long_ma && now_short_ma <= now_long_ma)
		{
			//多仓出场
			ctx->stra_exit_long(_code.c_str(), trdUnit, "DT_ExitLong");
			//ctx->stra_log_info(fmt::format("now_datetime = {}:{}, now_close = {}", now_date, now_time, now_close).c_str());
            ctx->stra_log_info(fmt::format("死叉{}<={},平多", now_short_ma, now_long_ma).c_str());
			curPos = 0;
		}
	}
	//else if(curPos < 0)
	if (decimal::lt(curPos, 0))
	{
		if (pre_short_ma < pre_long_ma && now_short_ma >= now_long_ma)
		{
			//空仓出场
			ctx->stra_exit_short(_code.c_str(), trdUnit, "DT_ExitShort");
			//ctx->stra_log_info(fmt::format("now_datetime = {}:{}, now_close = {}", now_date, now_time, now_close).c_str());
            ctx->stra_log_info(fmt::format("金叉{}>={},平空", now_short_ma, now_long_ma).c_str());
			curPos = 0;
		}
	}
	if (decimal::eq(curPos, 0))
	{
		if (pre_short_ma < pre_long_ma && now_short_ma >= now_long_ma)
		{
			ctx->stra_enter_long(_code.c_str(), trdUnit, "DT_EnterLong");
			//向上突破
			//ctx->stra_log_info(fmt::format("now_datetime = {}:{}, now_close = {}", now_date, now_time, now_close).c_str());
			ctx->stra_log_info(fmt::format("金叉{}>={},多仓进场", now_short_ma, now_long_ma).c_str());
			//std::cout << "中断程序进行调试代码" << std::endl;
			//exit(0);
		}
		else if (pre_short_ma > pre_long_ma && now_short_ma <= now_long_ma)
		{
			ctx->stra_enter_short(_code.c_str(), trdUnit, "DT_EnterShort");
			//向下突破
			//ctx->stra_log_info(fmt::format("now_datetime = {}:{}, now_close = {}", now_date, now_time, now_close).c_str());
			ctx->stra_log_info(fmt::format("死叉{}<={},空仓进场", now_short_ma, now_long_ma).c_str());
			//std::cout << "中断程序进行调试代码" << std::endl;
			//exit(0);
		}
	}

	pre_short_ma =now_short_ma;
	pre_long_ma = now_long_ma;

	

	//这个释放一定要做
	kline->release();
}

void WtStraTwoMaStrategy::on_init(ICtaStraCtx* ctx)
{
	std::string code = _code;
	if (_isstk)
		code += "-";
	WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), _count, true);
	if (kline == NULL)
	{
		//这里可以输出一些日志
		return;
	}

	kline->release();
}

void WtStraTwoMaStrategy::on_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick)
{
	//没有什么要处理
}
