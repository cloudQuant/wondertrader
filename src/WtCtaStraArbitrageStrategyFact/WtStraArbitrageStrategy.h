#pragma once
#include "../Includes/CtaStrategyDefs.h"
#include <vector>
class WtStraArbitrageStrategy : public CtaStrategy
{
public:
	WtStraArbitrageStrategy(const char* id);
	virtual ~WtStraArbitrageStrategy();

public:
	virtual const char* getFactName() override;

	virtual const char* getName() override;

	virtual bool init(WTSVariant* cfg) override;

	virtual void on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime) override;

	virtual void on_init(ICtaStraCtx* ctx) override;

	virtual void on_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick) override;

private:
	//指标参数
	double		_threshold;
	uint32_t	_look_back_bars;

	//数据周期
	std::string _period;
	//K线条数
	uint32_t	_count;

	//合约代码
	std::string _left_code;
	std::string _right_code;

	// 策略中使用到的变量
	double _up_price;
	double _mean_price;
	double _down_price;
	double _beta;
	double _c;
	std::vector<double> _resid;

	bool		_isstk;
	bool        _can_trade;
};

