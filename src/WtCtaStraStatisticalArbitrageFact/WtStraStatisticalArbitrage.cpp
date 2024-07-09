#include "WtStraStatisticalArbitrage.h"
#include <iostream>
#include <vector>
#include <tuple>
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>


#include "../Includes/ICtaStraCtx.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Share/decimal.h"

extern const char* FACT_NAME;

//By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"



double mean(const std::vector<double>& vec) {
	return std::accumulate(vec.begin(), vec.end(), 0.0) / vec.size();
}

double std_dev(const std::vector<double>& vec, double mean) {
	double sum = 0.0;
	for (double val : vec) {
		sum += (val - mean) * (val - mean);
	}
	return std::sqrt(sum / (vec.size() - 1));
}



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

std::vector<double> pyListToVector(PyObject* pyList) {
	std::vector<double> vec;
	if (!PyList_Check(pyList)) {
		std::cerr << "Not a Python list" << std::endl;
		return vec;
	}

	Py_ssize_t size = PyList_Size(pyList);
	for (Py_ssize_t i = 0; i < size; ++i) {
		PyObject* item = PyList_GetItem(pyList, i);
		if (!PyFloat_Check(item) && !PyLong_Check(item)) {
			std::cerr << "List item is not a float or long" << std::endl;
			continue;
		}
		double value = PyFloat_AsDouble(item);
		vec.push_back(value);
	}

	return vec;
}

std::tuple<double, double, double, double, bool> WtStraArbitrageStrategy::my_cointegration(const std::vector<double>& series01, const std::vector<double>& series02) {
	
	std::tuple<double, double, double, double, bool> result;
	// 创建参数列表
	PyObject* pList1 = PyList_New(0);
	for (int i = 0; i < series01.size(); ++i) {
		PyList_Append(pList1, PyFloat_FromDouble(series01[i]));
	}

	PyObject* pList2 = PyList_New(0);
	for (int i = 0; i < series02.size(); ++i) {
		PyList_Append(pList2, PyFloat_FromDouble(series02[i]));
	}

	// 创建参数元组
	PyObject* pArgs = PyTuple_New(2);
	PyTuple_SetItem(pArgs, 0, pList1); // 示例参数
	PyTuple_SetItem(pArgs, 1, pList2); // 示例参数

	// 调用函数
	PyObject* pResult = PyObject_CallObject(pFunc, pArgs);

	if (pResult != NULL) {
		Py_ssize_t size = PyTuple_Size(pResult);
		double r1 = PyFloat_AsDouble(PyTuple_GetItem(pResult, 0));
		double r2 = PyFloat_AsDouble(PyTuple_GetItem(pResult, 1));
		double r3 = PyFloat_AsDouble(PyTuple_GetItem(pResult, 2));
		double r4 = PyFloat_AsDouble(PyTuple_GetItem(pResult, 3));
		bool r5 = PyObject_IsTrue(PyTuple_GetItem(pResult, 4));
		result = std::make_tuple(r1, r2, r3, r4, r5);
		Py_DECREF(pResult);
	}
	else {
		PyErr_Print();
	}
	return result;
}



WtStraArbitrageStrategy::WtStraArbitrageStrategy(const char* id)
	: CtaStrategy(id)
{
	// 初始化Python解释器
	Py_Initialize();
	if (!Py_IsInitialized()) {
		std::cerr << "Python initialization failed!" << std::endl;
	}

	// 添加Python模块路径
	PyRun_SimpleString("import sys");
	PyRun_SimpleString("sys.path.append('.')");
	// 导入Cython生成的模块
	pModule = PyImport_ImportModule("my_adf");
	if (!pModule) {
		std::cerr << "Failed to load module 'my_adf'" << std::endl;
		PyErr_Print();
		Py_Finalize();
	}

	// 获取模块中的函数
	pFunc = PyObject_GetAttrString(pModule, "cointegration_check");
	if (!pFunc || !PyCallable_Check(pFunc)) {
		std::cerr << "Cannot find function 'cointegration_check'" << std::endl;
		throw std::runtime_error("Cannot find function 'cointegration_check'"); // 抛出异常
		Py_XDECREF(pFunc);
		Py_DECREF(pModule);
		Py_Finalize();
	}
}


WtStraArbitrageStrategy::~WtStraArbitrageStrategy()
{
	// 清理
	Py_XDECREF(pFunc);
	Py_DECREF(pModule);

	// 关闭Python解释器
	Py_Finalize();
}



const char* WtStraArbitrageStrategy::getFactName()
{
	return FACT_NAME;
}

const char* WtStraArbitrageStrategy::getName()
{
	return "StatisticalArbitrage";
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
		std::tuple<double, double, double, double, bool> content;
		std::vector<double> series01(_look_back_bars);
		std::vector<double> series02(_look_back_bars);
		for (uint32_t j = 1; j <= _look_back_bars; j++) {
			double left_close_price = left_kline->at(-1 * j)->close;
			double right_close_price = right_kline->at(-1 * j)->close;
			series01[_look_back_bars - j] = left_close_price;
			series02[_look_back_bars - j] = right_close_price;
		}
		content = my_cointegration(series01, series02);
		_beta = std::get<0>(content);
		_c = std::get<1>(content);
		_mean_price = std::get<2>(content);
		double std_price = std::get<3>(content);
		_can_trade = std::get<4>(content);

		_up_price = _mean_price + _threshold * std_price;
		_down_price = _mean_price - _threshold * std_price;
		//ctx->stra_log_info(fmt::format("left_close ={}, right_close = {},beta = {}, c = {}, up_price = {}, mean_price = {}, down_price={}", left_now_close, right_now_close, _beta, _c, _up_price, _mean_price, _down_price).c_str());
	}
	double current_left_position = ctx->stra_get_position(left_code.c_str()) / trdUnit;
	double current_right_position = ctx->stra_get_position(right_code.c_str()) / trdUnit;

	if (current_time < 1430) {

		double resid_new = left_now_close - _beta * right_now_close - _c;
		//ctx->stra_log_info(fmt::format("left_position = {}, new_resid = {}, up_price = {}, mean_price = {}, down_price={}", current_left_position, resid_new, _up_price, _mean_price, _down_price).c_str());
		if (_can_trade && current_left_position > 0 && resid_new < _mean_price) {

			ctx->stra_log_info(fmt::format("[{}.{}] long stop loss" , current_date, current_time).c_str());
			ctx->stra_set_position(left_code.c_str(), 0, "left_long_exit");
			ctx->stra_set_position(right_code.c_str(), 0, "right_long_exit");
			current_left_position = 0;
			current_right_position = 0;
		}
		else if(_can_trade && current_left_position < 0 && resid_new > _mean_price) {
			ctx->stra_log_info(fmt::format("[{}.{}] short stop loss", current_date, current_time).c_str());
			ctx->stra_set_position(left_code.c_str(), 0, "left_short_exit");
			ctx->stra_set_position(right_code.c_str(), 0, "right_short_exit");
			current_left_position = 0;
			current_right_position = 0;
		}
		else if (_can_trade && current_left_position == 0 && resid_new > _up_price) {

			ctx->stra_log_info(fmt::format("[{}.{}] long opened", current_date, current_time).c_str());
			ctx->stra_enter_long(left_code.c_str(), 1, "left_long_entry");
			ctx->stra_enter_short(right_code.c_str(), 1, "right_long_entry");

		}
		else if (_can_trade && current_left_position == 0 && resid_new< _down_price) {

			ctx->stra_log_info(fmt::format("[{}.{}] short opened", current_date, current_time).c_str());
			ctx->stra_enter_short(left_code.c_str(), 1, "left_short_entry");
			ctx->stra_enter_long(right_code.c_str(), 1, "right_short_entry");

		}	
	}

	if (current_time == 1455 && current_left_position!=0) {
		ctx->stra_log_info(fmt::format("[{}.{}] closed before sleep", current_date, current_time).c_str());
		ctx->stra_set_position(left_code.c_str(), 0, "closed before sleep");
		ctx->stra_set_position(right_code.c_str(), 0, "closed before sleep");
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
