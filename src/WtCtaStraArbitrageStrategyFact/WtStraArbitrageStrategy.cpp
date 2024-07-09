#include "WtStraArbitrageStrategy.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <numeric>
#include <limits>

#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"

extern const char* FACT_NAME;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"


// 计算向量的均值
double mean(const std::vector<double>& vec) {
	return std::accumulate(vec.begin(), vec.end(), 0.0) / vec.size();
}

// 计算向量的标准差
double std_dev(const std::vector<double>& vec, double mean) {
	double sum = 0.0;
	for (double val : vec) {
		sum += (val - mean) * (val - mean);
	}
	return std::sqrt(sum / (vec.size() - 1));
}

// 计算ADF统计量
double adf_statistic(const std::vector<double>& series, int max_lag) {
	size_t n = series.size();
	std::vector<double> diff_series(n - 1);
	for (size_t i = 1; i < n; ++i) {
		diff_series[i - 1] = series[i] - series[i - 1];
	}

	double mean_diff = mean(diff_series);
	double std_diff = std_dev(diff_series, mean_diff);

	double adf_stat = (series[0] - mean_diff) / std_diff;

	for (int lag = 1; lag <= max_lag; ++lag) {
		double sum = 0.0;
		for (size_t i = lag; i < n - 1; ++i) {
			sum += (diff_series[i] - mean_diff) * (diff_series[i - lag] - mean_diff);
		}
		double cov = sum / (n - lag - 1);
		adf_stat -= cov / std_diff;
	}

	return adf_stat;
}

// 选择最佳滞后阶数（简单的信息准则）
int select_lag(const std::vector<double>& series, int max_lag) {
	double aic = std::numeric_limits<double>::max();
	int best_lag = 0;

	for (int lag = 0; lag <= max_lag; ++lag) {
		double adf_stat = adf_statistic(series, lag);
		double n = series.size() - lag;
		double current_aic = -2 * adf_stat + 2 * (lag + 1);

		if (current_aic < aic) {
			aic = current_aic;
			best_lag = lag;
		}
	}

	return best_lag;
}

// 简单的ADF检验实现
double adfuller(const std::vector<double>& series, int max_lag) {
	//int best_lag = select_lag(series, max_lag);
	double adf_stat = adf_statistic(series, 1);
	std::cout << " adf_stat = " << adf_stat << std::endl;
	// 这里我们需要使用ADF统计量的临界值来计算p值
	// 这里只是一个示例，实际的ADF检验需要更复杂的统计处理
	// 我们可以使用一些已知的临界值表或库来计算p值
	// 这里假设adf_stat的临界值为-2.86（1%显著性水平）
	double critical_value_1pct = 2.86;
	double critical_value_5pct = 1.95;
	double critical_value_10pct = 1.62;

	double p_value = 1.0;
	adf_stat = std::abs(adf_stat);
	if (adf_stat > critical_value_1pct) {
		p_value = 0.01;
		return p_value;
	}
	else if (adf_stat > critical_value_5pct) {
		p_value = 0.05;
		return p_value;
	}
	else if (adf_stat > critical_value_10pct) {
		p_value = 0.10;
		return p_value;

	}

	return p_value;
}

// OLS回归函数
std::pair<double, double> ols(const std::vector<double>& y, const std::vector<double>& x) {
	double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
	size_t n = y.size();

	for (size_t i = 0; i < n; ++i) {
		sum_x += x[i];
		sum_y += y[i];
		sum_xy += x[i] * y[i];
		sum_x2 += x[i] * x[i];
	}

	double beta = (n * sum_xy - sum_x * sum_y) / (n * sum_x2 - sum_x * sum_x);
	double c = (sum_y - beta * sum_x) / n;

	return { beta, c };
}

std::tuple<double, double, std::vector<double>, bool> cointegration_check(const std::vector<double>& series01, const std::vector<double>& series02) {
	double urt_1 = adfuller(series01, 1);
	double urt_2 = adfuller(series02, 1);
	std::cout << "urt_1 = " << urt_1 << " urt_2 = " << urt_2 << std::endl;
	// 同时平稳或不平稳则差分再次检验
	if ((urt_1 > 0.1 && urt_2 > 0.1) || (urt_1 < 0.1 && urt_2 < 0.1)) {
		std::vector<double> series01_diff(series01.size() - 1);
		std::vector<double> series02_diff(series02.size() - 1);

		for (size_t i = 1; i < series01.size(); ++i) {
			series01_diff[i - 1] = series01[i] - series01[i - 1];
		}
		for (size_t i = 1; i < series02.size(); ++i) {
			series02_diff[i - 1] = series02[i] - series02[i - 1];
		}

		double urt_diff_1 = adfuller(series01_diff, 1);
		double urt_diff_2 = adfuller(series02_diff, 1);

		// 同时差分平稳进行OLS回归的残差平稳检验
		if (urt_diff_1 < 0.1 && urt_diff_2 < 0.1) {
			auto[beta, c] = ols(series01, series02);
			std::vector<double> resid(series01.size());
			for (size_t i = 0; i < series01.size(); ++i) {
				resid[i] = series01[i] - beta * series02[i] - c;
			}
			if (adfuller(resid, 1) > 0.1) {
				return { beta, c, resid, false };
			}
			else {
				return { beta, c, resid, true };
			}
		}
		else {
			return { 0.0, 0.0, std::vector<double>(), false };
		}
	}
	else {
		return { 0.0, 0.0, std::vector<double>(), false };
	}
}

WtStraArbitrageStrategy::WtStraArbitrageStrategy(const char* id)
	: CtaStrategy(id)
{
}


WtStraArbitrageStrategy::~WtStraArbitrageStrategy()
{
}

const char* WtStraArbitrageStrategy::getFactName()
{
	return FACT_NAME;
}

const char* WtStraArbitrageStrategy::getName()
{
	return "ArbitrageStrategy";
}

bool WtStraArbitrageStrategy::init(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	_look_back_bars = cfg->getUInt32("look_back_bars");
	_threshold = cfg->getDouble("threshold");

	_period = cfg->getCString("period");
	_count = cfg->getUInt32("count");
	_left_code = cfg->getCString("left_code");
	_right_code = cfg->getCString("right_code");

	_isstk = cfg->getBoolean("stock");
	_can_trade = false;

	std::cout << "left_code = " << _left_code << 
		" right_code = " << _right_code << 
		" look_back_bars = " << _look_back_bars << 
		" threshold = " << _threshold << std::endl;
	std::cout << "init params success" << std::endl;


	return true;
}

void WtStraArbitrageStrategy::on_schedule(ICtaStraCtx* ctx, uint32_t curDate, uint32_t curTime)
{
	//ctx->stra_log_info(fmt::format("on_schedule==>current_date ={}, current_time = {}", curDate, curTime).c_str());
	std::string left_code = _left_code;
	std::string right_code = _right_code;
	if (_isstk) {
		left_code += "-";
		right_code += "-";
	}
	// get kline data 	
	WTSKlineSlice *left_kline = ctx->stra_get_bars(left_code.c_str(), _period.c_str(), _count, true);
	if(left_kline == NULL)
	{
		return;
	}

	if (left_kline->size() == 0)
	{
		left_kline->release();
		return;
	}
	WTSKlineSlice *right_kline = ctx->stra_get_bars(right_code.c_str(), _period.c_str(), _count, false);
	if (right_kline == NULL)
	{
		return;
	}

	if (right_kline->size() == 0)
	{
		right_kline->release();
		return;
	}

	uint32_t trdUnit = 1;
	if (_isstk)
	{
		trdUnit = 100;
	}

	

	// 尝试获取当前的时间，价格
	auto left_now_date = left_kline->at(-1)->date;
	auto left_now_time = left_kline->at(-1)->time;
	double left_now_open = left_kline->at(-1)->open;
	double left_now_close = left_kline->at(-1)->close;

	auto right_now_date = right_kline->at(-1)->date;
	auto right_now_time = right_kline->at(-1)->time;
	double right_now_open = right_kline->at(-1)->open;
	double right_now_close = right_kline->at(-1)->close;

	auto current_date = ctx->stra_get_date();
	auto current_time = ctx->stra_get_time();

	if (current_time == 2105) {
		std::tuple<double, double, std::vector<double>, bool> content;
		std::vector<double> series01(_look_back_bars);
		std::vector<double> series02(_look_back_bars);
		for (uint32_t j = 1; j <= _look_back_bars; j++) {
			double left_close_price = left_kline->at(-1 * j)->close;
			double right_close_price = right_kline->at(-1 * j)->close;
			series01[_look_back_bars - j] = left_close_price;
			series02[_look_back_bars - j] = right_close_price;
		}
		content = cointegration_check(series01, series02);
		// 使用结构化绑定来提取tuple中的元素
		auto[_beta, _c, _resid, result] = content;
		_can_trade = result;
		// 计算残差的标准差上下轨
		_mean_price = mean(_resid);
		double std_price = std_dev(_resid, _mean_price);
		_up_price = _mean_price + _threshold * std_price;
		_down_price = _mean_price - _threshold * std_price;
		ctx->stra_log_info(fmt::format("left_close ={}, right_close = {},beta = {}, c = {}, mean = {}, up = {}, down={}", left_now_close, right_now_close, _beta, _c, _up_price, _mean_price, _down_price).c_str());
	}
	double current_left_position = ctx->stra_get_position(left_code.c_str()) / trdUnit;
	double current_right_position = ctx->stra_get_position(right_code.c_str()) / trdUnit;

	if (current_time < 1430) {
		// 计算新残差
		double resid_new = left_now_close - _beta * right_now_close - _c;
		if (_can_trade && current_left_position > 0 && resid_new < _mean_price) {
			// 多头止损
			ctx->stra_log_info(fmt::format("[{}.{}] 多头止损" , current_date, current_time).c_str());
			ctx->stra_set_position(left_code.c_str(), 0, "left_long_exit");
			ctx->stra_set_position(right_code.c_str(), 0, "right_long_exit");
			current_left_position = 0;
			current_right_position = 0;
		}
		else if(_can_trade && current_left_position < 0 && resid_new > _mean_price) {
			// 空头止损
			ctx->stra_log_info(fmt::format("[{}.{}] 空头止损", current_date, current_time).c_str());
			ctx->stra_set_position(left_code.c_str(), 0, "left_short_exit");
			ctx->stra_set_position(right_code.c_str(), 0, "right_short_exit");
			current_left_position = 0;
			current_right_position = 0;
		}
		else if (_can_trade && current_left_position == 0 && resid_new > _up_price) {
			// 做多价差
			ctx->stra_log_info(fmt::format("[{}.{}] 做多价差", current_date, current_time).c_str());
			ctx->stra_enter_long(left_code.c_str(), 1, "left_long_entry");
			ctx->stra_enter_short(right_code.c_str(), 1, "right_long_entry");

		}
		else if (_can_trade && current_left_position == 0 && resid_new< _down_price) {
			// 做多价差
			ctx->stra_log_info(fmt::format("[{}.{}] 做空价差", current_date, current_time).c_str());
			ctx->stra_enter_short(left_code.c_str(), 1, "left_short_entry");
			ctx->stra_enter_long(right_code.c_str(), 1, "right_short_entry");

		}	
	}

	if (current_time == 1455 && current_left_position!=0) {
		// ctx->stra_log_info(fmt::format("[{}.{}] 多头止损", current_date, current_time).c_str());
		ctx->stra_set_position(left_code.c_str(), 0, "收盘平仓");
		ctx->stra_set_position(right_code.c_str(), 0, "收盘平仓");
		_mean_price = 0;
		_can_trade = false;
	}
	
		
		




	// WTSValueArray* closes = kline->extractData(KFT_CLOSE);
	//double hc = closes->maxvalue(-days, -2);
	//double lc = closes->minvalue(-days, -2);
	//double curPx = closes->at(-1);
	//closes->release();///

	//double openPx = kline->at(-1)->open;
	//double highPx = kline->at(-1)->high;
	//double lowPx = kline->at(-1)->low;

	//double upper_bound = openPx + _k1 * (std::max(hh - lc, hc - ll));
	//double lower_bound = openPx - _k2 * std::max(hh - lc, hc - ll);

	

	ctx->stra_save_user_data("test", "waht");

	left_kline->release();
	right_kline->release();
}

void WtStraArbitrageStrategy::on_init(ICtaStraCtx* ctx)
{
	std::string left_code = _left_code;
	std::string right_code = _right_code;
	auto current_date = ctx->stra_get_date();
	auto current_time = ctx->stra_get_time();
	ctx->stra_log_info(fmt::format("0==>current_date ={}, current_time = {}, left_code = {}, right_code = {}", current_date, current_time, left_code, right_code).c_str());
	
	if (_isstk) {
		left_code += "-";
		right_code += "-";
	}
	// get kline data 	
	WTSKlineSlice *left_kline = ctx->stra_get_bars(_left_code.c_str(), _period.c_str(), _count, true);
	if (left_kline == NULL)
	{
		std::cout << _left_code << " kline is NULL" << std::endl;
		return;
	}

	WTSKlineSlice *right_kline = ctx->stra_get_bars(_right_code.c_str(), _period.c_str(), _count, false);
	if (right_kline == NULL)
	{
		std::cout << _right_code << " right is NULL" << std::endl;
		return;
	}

	ctx->stra_log_info(fmt::format("1==>current_date ={}, current_time = {}, left_code = {}, right_code = {}", current_date, current_time, left_code, right_code).c_str());

	left_kline->release();
	right_kline->release();
}

void WtStraArbitrageStrategy::on_tick(ICtaStraCtx* ctx, const char* stdCode, WTSTickData* newTick)
{
}
