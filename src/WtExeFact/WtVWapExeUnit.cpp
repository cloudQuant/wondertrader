/**
 * @file WtVWapExeUnit.cpp
 * @brief VWAP(成交量加权平均价格)执行单元实现
 * @details 实现基于VWAP算法的智能交易执行单元，通过预测成交量分布来优化订单执行
 * @author zhaoyk
 * @date 2023-05-23
 */
#include "WtVWapExeUnit.h"

#include "../Share/TimeUtils.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"
#include "../Includes/WTSSessionInfo.hpp"


extern const char* FACT_NAME;

/**
 * @brief VWAP执行单元的构造函数
 * @details 初始化VWAP执行单元对象，设置所有成员变量的默认值
 *          包括行情数据、品种信息、交易参数和状态标记等
 */
WtVWapExeUnit::WtVWapExeUnit()
	:_last_tick(NULL)
	,_comm_info(NULL)//品种信息
	,_ord_sticky(0)
	,_cancel_cnt(0)
	,_channel_ready(false)
	, _last_fire_time(0)
	, _fired_times(0)
	, _total_times(0)
	, _total_secs(0)
	, _price_mode(0)
	, _price_offset(0)
	, _target_pos(0)
	, _cancel_times(0)
	, _begin_time(0)
	, _end_time(0)
	,isCanCancel(true)
{
}

/**
 * @brief VWAP执行单元的析构函数
 * @details 清理VWAP执行单元对象的资源，释放行情数据和品种信息等内存
 *          释放内存前会检查指针是否为空，避免释放空指针
 */
WtVWapExeUnit::~WtVWapExeUnit()
{
	if (_last_tick)
		_last_tick->release();

	if (_comm_info)
		_comm_info->release();
}
/**
 * @brief 获取真实目标仓位
 * @details 如果目标仓位是DBL_MAX（表示清仓标记），则返回0，否则返回原始目标值
 * @param _target 原始目标仓位值
 * @return 处理后的目标仓位值
 */
inline double get_real_target(double _target) {
	if (_target == DBL_MAX)
		return 0;

	return _target;
}
/**
 * @brief 检查是否为清仓指令
 * @details 判断目标仓位是否为DBL_MAX，该值在系统中用于表示清仓指令
 * @param target 目标仓位值
 * @return 如果是清仓指令返回true，否则返回false
 */
inline bool is_clear(double target)
{
	return (target == DBL_MAX);
}
/**
 * @brief 计算两个时间点之间的秒数
 * @details 将HHMM格式的时间转换为秒数，并计算它们之间的差值
 *          例如：1030和1130之间的3600秒
 * @param begintime 开始时间，格式为HHMM，如1030表示10:30
 * @param endtime 结束时间，格式为HHMM，如1130表示11:30
 * @return 两个时间点之间的秒数
 */
inline uint32_t calTmSecs(uint32_t begintime, uint32_t endtime) //计算执行时间：s
{
	return   ((endtime / 100) * 3600 + (endtime % 100) * 60) - ((begintime / 100) * 3600 + (begintime % 100) * 60);

}
/**
 * @brief 计算时间戳在交易时间中的分钟位置
 * @details 将HHMMSS格式的时间转换为交易日内的分钟数，考虑了交易时段和中午休市
 *          主要用于将当前时间映射到VWAP成交量分布数组中的位置
 *          交易时间分为上午和下午两段：9:30-11:30和13:30-15:00
 * @param actiontime 时间戳，格式为HHMMSS或HHMMSSMMM
 * @return 交易日内的分钟数，范围0-240，其中0表示开盘前，240表示收盘时
 */
inline double calTmStamp(uint32_t actiontime) //计算tick时间属于哪个时间单元
{
	string timestamp = to_string(actiontime);
	int hour = stoi(timestamp.substr(0, 2));
	int minute = stoi(timestamp.substr(2, 2));
	double total_minute = 0;
	if (hour < 9 || (hour == 9 && minute < 30)) {
		total_minute = 0;
	}
	else if (hour < 11 || (hour == 11 && minute <= 30)) {
		total_minute = (hour - 9) * 60 + minute - 30;
	}
	else if (hour < 13 || (hour == 13 && minute < 30)) {
		total_minute = 120 + (hour - 11) * 60 + minute;
	}
	else if (hour < 15 || (hour == 15 && minute <= 0)) {
		total_minute = 240 + (hour - 13) * 60 + minute - 30;
	}
	else {
		total_minute = 240;
	}
	if (timestamp >= "113000000" && timestamp < "130000000") {
		total_minute = 120;
	}
	total_minute += stoi(timestamp.substr(4, 2)) / 60;
	total_minute += stoi(timestamp.substr(6, 3)) / 60000;
	return total_minute ;//这里应该+1，对应vector 所以再-1
}
/**
 * @brief 获取所属执行器工厂名称
 * @details 返回该执行单元所属的工厂名称，用于执行单元的注册和管理
 * @return 工厂名称字符串
 */
const char * WtVWapExeUnit::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 获取执行单元名称
 * @details 返回该执行单元的名称，用于标识不同类型的执行单元
 * @return 执行单元名称字符串
 */
const char * WtVWapExeUnit::getName()
{
	return "WtVWapExeUnit";
}

/**
 * @brief 初始化VWAP执行单元
 * @details 根据提供的执行上下文、合约代码和配置参数初始化VWAP执行单元
 *          设置各种交易参数如执行时间、价格模式、发单间隔等
 *          并加载历史VWAP成交量分布数据用于预测
 * @param ctx 执行单元运行环境，提供交易接口和市场数据
 * @param stdCode 标准化合约代码，指定要交易的合约
 * @param cfg 配置参数，包含执行单元的各种设置
 */
void WtVWapExeUnit::init(ExecuteContext * ctx, const char * stdCode, WTSVariant * cfg)
{
	ExecuteUnit::init(ctx, stdCode, cfg);

	_comm_info = ctx->getCommodityInfo(stdCode);//获取品种参数
	if (_comm_info)
		_comm_info->retain();
	
	_sess_info = ctx->getSessionInfo(stdCode);//获取交易时间模板信息
	if (_sess_info)
		_sess_info->retain();
	_begin_time = cfg->getUInt32("begin_time");
	_end_time = cfg->getUInt32("end_time");
	_ord_sticky = cfg->getUInt32("ord_sticky");//挂单时限
	_tail_secs = cfg->getUInt32("tail_secs");//执行尾部时间
	_total_times = cfg->getUInt32("total_times");//总执行次数
	_price_mode = cfg->getUInt32("price_mode");
	_price_offset = cfg->getUInt32("price_offset");
	_order_lots = cfg->getDouble("lots");		//单次发单手数
	if (cfg->has("minopenlots"))
	_min_open_lots = cfg->getDouble("minopenlots");	//最小开仓数量
	_fire_span = (_total_secs - _tail_secs) / _total_times;		//单次发单时间间隔,要去掉尾部时间计算,这样的话,最后剩余的数量就有一个兵底发单的机制了

	ctx->writeLog(fmt::format("执行单元WtVWapExeUnit[{}] 初始化完成,订单超时 {} 秒,执行时限 {} 秒,收尾时间 {} 秒", stdCode, _ord_sticky, _total_secs, _tail_secs).c_str());
	_total_secs = calTmSecs(_begin_time, _end_time);//执行总时间：秒

	// 加载VWAP成交量分布数据
	std::string filename = "Vwap_";
	filename += _comm_info->getName();
	filename += ".txt";
	if (!StdFile::exists(filename.c_str()))
	{
		_ctx->writeLog(fmtutil::format("Vwap file {} not exists ,check and return.", filename.c_str()));
		return;
	}

	ifstream file(filename.c_str());
	if (file.is_open()) {
		string line;
		while (getline(file, line)) {
			stringstream s(line);
			string prz;
			while (getline(s, prz, ',')) {
				VwapAim.push_back(stod(prz));
			}
		}
		file.close();
	}
} 

/**
 * @brief 处理订单回报
 * @details 处理订单状态变化，包括成交、撤销等情况，并更新内部订单管理状态
 *          如果订单被撤销且目标仓位未达到，则会重新发送订单
 *          在VWAP执行策略中，这个方法还会跟踪当前的成交进度以调整执行计划
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 委托价格
 * @param isCanceled 是否已撤销
 */
void WtVWapExeUnit::on_order(uint32_t localid, const char * stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
	if (!_orders_mon.has_order(localid))
		return;
	if (isCanceled || leftover == 0)
	{
		_orders_mon.erase_order(localid);
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->writeLog(fmtutil::format("[{}@{}] Order of {} cancelling done, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));
		}
	}
	
	if (leftover == 0 && !isCanceled) {
		_cancel_times = 0;
		_ctx->writeLog(fmtutil::format("Order {} has filled", localid));
	}
	//如果全部订单已撤销,这个时候一般是遇到要超时撤单（挂单超时） 
	if (isCanceled && _cancel_cnt == 0)
	{
		double realPos = _ctx->getPosition(stdCode);
		if (!decimal::eq(realPos, _this_target))
		{
			_ctx->writeLog(fmtutil::format("Order {} of {} canceled, re_fire will be done", localid, stdCode));
			_cancel_times++;
			//撤单以后重发,一般是加点重发;对最小下单量的校验
			fire_at_once(max(_min_open_lots, _this_target - realPos));
		}
	}

	if (!isCanceled&&_cancel_cnt != 0) {//一般出现问题，需要返回检查  触发撤单 cnt++,onorder响应处理才会--
		_ctx->writeLog(fmtutil::format("Order {} of {}  hasn't canceled, error will be return ", localid, stdCode));
		return;
	}
}

/**
 * @brief 处理交易通道就绪
 * @details 当交易通道就绪时调用，设置内部通道状态标记并检查未完成订单
 *          处理三种情况：
 *          1. 有未完成订单但本地无记录：撤销这些外部订单
 *          2. 无未完成订单但本地有记录：清理本地错误订单
 *          3. 其他情况：记录日志
 *          最后触发仓位计算和发单操作
 */
void WtVWapExeUnit::on_channel_ready()
{
	_channel_ready = true;
	double undone = _ctx->getUndoneQty(_code.c_str());
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	{//未完成单不在监控中，撤单
		/*
		 *	如果未完成单不为0，而OMS没有订单
		 *	这说明有未完成单不在监控之中,全部撤销掉
		 *	因为这些订单没有本地订单号，无法直接进行管理
		 *	这种情况，就是刚启动的时候，上次的未完成单或者外部的挂单
		 */
		_ctx->writeLog(fmt::format("{} unmanaged orders of {},cancel all", undone, _code).c_str());
		
		bool isBuy = (undone > 0);
		OrderIDs ids =_ctx->cancel(_code.c_str(), isBuy);
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		_cancel_cnt += ids.size();

		_ctx->writeLog(fmtutil::format("[{}@{}]cancelcnt -> {}", __FILE__, __LINE__, _cancel_cnt));
	}
	else if (decimal::eq(undone, 0) && _orders_mon.has_order())
	{	/*
		 *	By Wesey @ 2021.12.13
		 *	如果未完成单为0，但是OMS中是有订单的
		 *	说明OMS中是错单，需要清理掉，不然超时撤单就会出错
		 *	这种情况，一般是断线重连以后，之前下出去的订单，并没有真正发送到柜台
		 *	所以这里需要清理掉本地订单
		 */
		_ctx->writeLog(fmtutil::format("Local orders of {} not confirmed in trading channel, clear all", _code.c_str()));
		_orders_mon.clear_orders();
	}
	else
	{
		_ctx->writeLog(fmtutil::format("Unrecognized condition while channle ready, {:.2f} live orders of {} exists, local orders {}exist",
			undone, _code.c_str(), _orders_mon.has_order() ? "" : "not "));
	}
	do_calc();
}


/**
 * @brief 处理行情数据回调
 * @details 当收到新的行情数据时调用，更新内部行情缓存并触发相关的交易逻辑
 *          处理两种情况：
 *          1. 首次行情：检查目标仓位与实际仓位的差异，如有差异则触发计算和发单
 *          2. 后续行情：检查订单超时情况并撤单，或根据时间间隔触发新一轮计算
 *          这是VWAP执行单元的核心方法，负责根据当前市场状况调整执行计划
 * @param newTick 新的行情数据指针
 */
void WtVWapExeUnit::on_tick(WTSTickData * newTick)
{
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	bool isFirstTick = false;
	//原来tick不为空 则要释放掉
	if (_last_tick) {
		_last_tick->release();
	}
	else {
		isFirstTick = true;
		//如果行情时间不在交易时间,这种情况一般是集合竞价的行情进来,下单会失败,所以直接过滤掉这笔行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;
	}
	//新的tick数据需要保留
	_last_tick = newTick;
	_last_tick->retain();

	if (isFirstTick)//如果是第一笔tick,则检查目标仓位,不符合则下单
	{
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode);
		if (!decimal::eq(newVol, undone + realPos))
		{//如果是第一笔TICK，且目标量==未完成+仓位，退出 
			do_calc();
		}
	}
	else
	{
		uint64_t now = TimeUtils::getLocalTimeNow();
		bool hasCancel = false;
		if (_ord_sticky != 0 && _orders_mon.has_order())
		{
			_orders_mon.check_orders(_ord_sticky, now, [this, &hasCancel](uint32_t localid) {
				if (_ctx->cancel(localid))
				{
					_cancel_cnt++;
					_ctx->writeLog(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());//订单过期，撤单量更新
					hasCancel = true;
				}
			});
		}
		if (!hasCancel && (now - _last_fire_time >= _fire_span * 1000))
		{
			do_calc();
		}
	}
	
}

/**
 * @brief 处理成交回报
 * @details 当收到成交回报时调用，在当前实现中仅作为占位函数
 *          实际的成交处理逻辑在on_tick方法中触发
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtVWapExeUnit::on_trade(uint32_t localid, const char * stdCode, bool isBuy, double vol, double price)
{//在ontick中触发
}
/**
 * @brief 处理委托回报
 * @details 当收到委托回报时调用，主要处理委托失败的情况
 *          如果委托失败，会从订单监控器中移除该订单并重新计算执行计划
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 */
void WtVWapExeUnit::on_entrust(uint32_t localid, const char * stdCode, bool bSuccess, const char * message)
{
    if (!bSuccess)
    {
        //如果不是我发出去的订单,我就不管了
        if (!_orders_mon.has_order(localid))
            return;
        
        _orders_mon.erase_order(localid);

        do_calc();
    }
}
/**
 * @brief 执行VWAP交易计算和下单逻辑
 * @details 这是VWAP执行单元的核心方法，负责计算目标仓位与实际仓位的差异，
 *          并根据VWAP算法确定下单时机、数量和价格
 *          包含多种交易条件检查和价格调整逻辑，确保订单在最优价格执行
 *          同时处理涨跌停限制、最小下单量等交易约束
 */
void WtVWapExeUnit::do_calc()
{
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	StdUniqueLock lock(_mtx_calc);
	const char* code = _code.c_str();
	double undone = _ctx->getUndoneQty(code);
	double newVol = get_real_target(_target_pos);
	double realPos = _ctx->getPosition(code);
	double diffQty = newVol - realPos;//目标差
	if (!_channel_ready)
		return;
	//有正在撤销的订单,则不能进行下一轮计算
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog(fmt::format("{}尚有未完成的撤单指令，暂时退出本轮执行", _code).c_str());
		return;
	}
	if (decimal::eq(diffQty, 0))
		return;
	//每一次发单要保障成交,所以如果有未完成单,说明上一轮没完成
	//有未完成订单&&与实际仓位变动方向相反
	//则需要撤销现有订单
	if (decimal::lt(diffQty*undone, 0))
	{
		bool isBuy = decimal::gt(undone, 0);//undone>0,isbuy=1
		OrderIDs ids = _ctx->cancel(code, isBuy);
		if (!ids.empty())
		{
			_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
			_cancel_cnt += ids.size();
			_ctx->writeLog(fmtutil::format("[{}@{}] live opposite order of {} canceled, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));//相反的订单取消
		}
		return;
	}
	if (!decimal::eq(undone, 0))
	{
		_ctx->writeLog(fmt::format("{}上一轮有挂单未完成,暂时退出本轮执行", _code).c_str());
		return;
	}
	if (_last_tick == NULL)
	{
		_ctx->writeLog(fmt::format("{}没有最新的tick数据，退出执行逻辑", _code).c_str());
		return;
	}
	double curPos = realPos;

	if (decimal::eq(curPos, newVol))
	{
		//当前仓位和最新目标仓位匹配时，如果不是全部清仓的需求，则直接退出计算了
		if (!is_clear(_target_pos))
			return;

		//如果是清仓的需求，还要再进行对比
		//如果多头为0，说明已经全部清理掉了，则直接退出
		double lPos = _ctx->getPosition(code, true, 1);
		if (decimal::eq(lPos, 0))
			return;
		//如果还有多头仓位，则将目标仓位设置为非0，强制触发    
		newVol = -min(lPos, _order_lots);
		_ctx->writeLog(fmtutil::format("Clearing process triggered, target position of {} has been set to {}", _code.c_str(), newVol));
	}
	//如果相比上次没有更新的tick进来，则先不下单，防止开盘前集中下单导致通道被封
	uint64_t curTickTime = (uint64_t)_last_tick->actiondate() * 1000000000 + _last_tick->actiontime();
	if (curTickTime <= _last_tick_time)
	{
		_ctx->writeLog(fmtutil::format("No tick of {} updated, {} <= {}, execute later", _code, curTickTime, _last_tick_time));
		return;
	}
	_last_tick_time = curTickTime;
	double InminsTm = calTmStamp(_last_tick->actiontime());//当前tick属于vwap240分钟内的第几(-1)分钟
	double aimQty = VwapAim[InminsTm];//取到对应时刻的目标vwapaim （递增）

	uint32_t leftTimes = _total_times - _fired_times;
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times+1).c_str());
	_Vwap_vol = aimQty - curPos;//若在本分钟发单，对应的VWapVol
	bool bNeedShowHand = false;
	double curQty = 0;
	if (leftTimes == 0 && !decimal::eq(diffQty, 0))
	{
		bNeedShowHand = true;
		curQty = max(diffQty, _min_open_lots);
	}
	else {
		curQty = max(_Vwap_vol, _min_open_lots)* abs(diffQty) / diffQty;//curqty=单位预测量sum
	}
	//设定本轮目标仓位
	_this_target = realPos + curQty;

	WTSTickData* curTick = _last_tick;
	uint64_t now = TimeUtils::getLocalTimeNow();
	bool isBuy = decimal::gt(diffQty, 0);
	double targetPx = 0;
	//根据价格模式设置,确定委托基准价格: 0-最新价,1-最优价,2-对手价
	if (_price_mode == 0)
	{
		targetPx = curTick->price();
	}
	else if (_price_mode == 1)
	{
		targetPx = isBuy ? curTick->bidprice(0) : curTick->askprice(0);
	}
	else
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}
	if (bNeedShowHand) {
		targetPx += _comm_info->getPriceTick() * 5 * (isBuy ? 1 : -1);
	}
	else if (_price_offset != 0)
	{
		targetPx += _comm_info->getPriceTick() * _price_offset * (isBuy ? 1 : -1);
	}

	// 如果最后价格为0，再做一个修正
	if (decimal::eq(targetPx, 0.0))
		targetPx = decimal::eq(_last_tick->price(), 0.0) ? _last_tick->preclose() : _last_tick->price();

	//检查涨跌停价
	isCanCancel = true;
	if (isBuy && !decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(targetPx, _last_tick->upperlimit()))
	{
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", targetPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		targetPx = _last_tick->upperlimit();
		isCanCancel = false;//如果价格被修正为涨跌停价，订单不可撤销
	}
	if (isBuy != 1 && !decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(targetPx, _last_tick->lowerlimit()))
	{
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", targetPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		targetPx = _last_tick->lowerlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}
	OrderIDs ids;
	if (curQty > 0)
	{
		ids = _ctx->buy(code, targetPx, abs(curQty));
	}
	else {
		ids = _ctx->sell(code, targetPx, abs(curQty));
	}
	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);
	_last_fire_time = now;
	_fired_times += 1;

	curTick->release();
}

/**
 * @brief 立即发送交易订单
 * @details 当需要立即发送订单时调用，例如在订单被撤销后需要重新发送
 *          根据当前市场状况和价格模式设置订单价格，并处理涨跌停限制
 *          这个方法与普通的do_calc方法不同，它会立即发送订单而不考虑VWAP算法
 * @param qty 要交易的数量，正数表示买入，负数表示卖出
 */
void WtVWapExeUnit::fire_at_once(double qty)
{
	if (decimal::eq(qty, 0))
		return;
	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	const char* code = _code.c_str();
	uint64_t now = TimeUtils::getLocalTimeNow();
	bool isBuy = decimal::gt(qty, 0);
	double targetPx = 0;
	//根据价格模式设置,确定委托基准价格: 0-最新价,1-最优价,2-对手价
	if (_price_mode == 0) {
		targetPx = curTick->price();
	}
	else if (_price_mode == 1) {
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);
	}
	else // if(_price_mode == 2)
	{
		targetPx = isBuy ? curTick->askprice(0) : curTick->bidprice(0);//买入方向：卖价，卖出方向：买价
	}

	targetPx += _comm_info->getPriceTick()*_cancel_times *(isBuy ? 1 : -1);//增加价格偏移
	isCanCancel = true;
	if (isBuy && !decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(targetPx, _last_tick->upperlimit()))
	{
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", targetPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		targetPx = _last_tick->upperlimit();
		isCanCancel = false;//如果价格被修正为涨跌停价，订单不可撤销
	}
	if (isBuy != 1 && !decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(targetPx, _last_tick->lowerlimit()))
	{
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", targetPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		targetPx = _last_tick->lowerlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}

	OrderIDs ids;
	if (qty > 0)
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		ids = _ctx->sell(code, targetPx, abs(qty));

	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);

	curTick->release();
}

/**
 * @brief 设置目标仓位
 * @details 设置指定合约的目标仓位，并触发交易计算
 *          如果合约代码不匹配或目标仓位与当前目标相同，则不进行操作
 *          否则更新目标仓位并重置执行次数，然后触发交易计算
 * @param stdCode 标准化合约代码
 * @param newVol 新的目标仓位数量
 */
void WtVWapExeUnit::set_position(const char * stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	if (decimal::eq(newVol, _target_pos))
		return;
	
	_target_pos = newVol;
	
	_fired_times = 0;//已执行次数

	do_calc();
}

/**
 * @brief 处理交易通道丢失
 * @details 当交易通道断开连接或发生异常时调用
 *          当前实现中仅作为占位函数，可在此处添加通道丢失时的处理逻辑
 *          例如清理订单状态、记录日志或触发重连机制
 */
void WtVWapExeUnit::on_channel_lost()
{
}
