#include "WtStraTwoMaStrategy.h"

#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
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

	_period = cfg->getCString("period");
	_count = cfg->getUInt32("count");
	_code = cfg->getCString("code");

	_isstk = cfg->getBoolean("stock");

	return true;
}

void WtStraTwoMaStrategy::on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime)
{
	std::string code = _code;
	if (_isstk)
		code += "-";
	WTSKlineSlice *kline = ctx->stra_get_bars(code.c_str(), _period.c_str(), _count, true);
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

	int32_t _short_days = static_cast<int32_t>(short_days);
    int32_t _long_days = static_cast<int32_t>(long_days);
    double pre_short_sum = 0;
    // 计算前一期短期均线
    for (uint32_t i=2; i<=_short_days+1; i++){
        pre_short_sum+=kline->at(-1*i)->close;
    }
    double pre_short_ma = pre_short_sum / _short_days;
    // 计算当期短期均线
    double now_short_sum = pre_short_sum - kline->at(-1*(_short_days+1))->close + kline->at(-1)->close;
    double now_short_ma = now_short_sum / _short_days;
    // 计算前一期长期均线
    double pre_long_sum = pre_short_sum;
    for (uint32_t j=_short_days+2; j<=_long_days+1; j++){
        pre_long_sum+=kline->at(-1*j)->close;
    }
	double pre_long_ma = pre_long_sum / _long_days;
    // 计算当期的长期均线
    double now_long_sum = pre_long_sum - kline->at(-1*(_long_days+1))->close + kline->at(-1)->close;
    double now_long_ma = now_long_sum / _long_days;

	WTSCommodityInfo* commInfo = ctx->stra_get_comminfo(_code.c_str());

	double curPos = ctx->stra_get_position(_code.c_str()) / trdUnit;
	if(decimal::eq(curPos,0))
	{
		if(pre_short_ma < pre_long_ma && now_short_ma >= now_long_ma)
		{
			ctx->stra_enter_long(_code.c_str(), 2 * trdUnit, "DT_EnterLong");
			//向上突破
			ctx->stra_log_info(fmt::format("金叉{}>={},多仓进场", now_short_ma, now_long_ma).c_str());
		}
		else if (pre_short_ma > pre_long_ma && now_short_ma <= now_long_ma)
		{
			ctx->stra_enter_short(_code.c_str(), 2 * trdUnit, "DT_EnterShort");
			//向下突破
			ctx->stra_log_info(fmt::format("死叉{}<={},空仓进场", now_short_ma, now_long_ma).c_str());
		}
	}
	//else if(curPos > 0)
	else if (decimal::gt(curPos, 0))
	{
		if(pre_short_ma > pre_long_ma && now_short_ma <= now_long_ma)
		{
			//多仓出场
			ctx->stra_exit_long(_code.c_str(), 2 * trdUnit, "DT_ExitLong");
            ctx->stra_log_info(fmt::format("死叉{}<={},平多", now_short_ma, now_long_ma).c_str());
		}
	}
	//else if(curPos < 0)
	else if (decimal::lt(curPos, 0))
	{
		if (pre_short_ma < pre_long_ma && now_short_ma >= now_long_ma)
		{
			//空仓出场
			ctx->stra_exit_short(_code.c_str(), 2 * trdUnit, "DT_ExitShort");
            ctx->stra_log_info(fmt::format("金叉{}>={},平空", now_short_ma, now_long_ma).c_str());
		}
	}

	ctx->stra_save_user_data("test", "waht");

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
