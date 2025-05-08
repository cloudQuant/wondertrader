/**
 * @file WtStockVWapExeUnit.cpp
 * @author zhaoyk (23.6.2)
 * @brief 股票VWAP执行单元实现文件
 * @details 实现股票的成交量加权平均价(VWAP)执行单元的核心逻辑
 *          将大单拆分为多个小单并按时间分布执行，减少市场冲击
 */
#include "WtStockVWapExeUnit.h"





extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @details 初始化所有成员变量为默认值
 */
WtStockVWapExeUnit::WtStockVWapExeUnit()
	:_last_tick(NULL)       // 行情数据指针
	, _comm_info(NULL)      // 品种信息
	, _ord_sticky(0)        // 挂单超时时间
	, _cancel_cnt(0)        // 撤单计数
	, _channel_ready(false) // 交易通道就绪标志
	, _last_fire_time(0)    // 上次发单时间
	, _fired_times(0)       // 已执行次数
	, _total_times(0)       // 总执行次数
	, _total_secs(0)        // 总执行时间
	, _price_mode(0)        // 价格模式
	, _price_offset(0)      // 价格偏移
	, _target_pos(0)        // 目标仓位
	, _cancel_times(0)      // 撤单次数
	, _begin_time(0)        // 开始时间
	, _end_time(0)          // 结束时间
	, _is_clear{ false }    // 清仓标志
	, _is_KC{ false }       // 科创板标志
	,isCanCancel{true}     // 订单是否可撤销
{
}

/**
 * @brief 析构函数
 * @details 清理分配的资源，包括行情数据和商品信息
 */
WtStockVWapExeUnit::~WtStockVWapExeUnit()
{
	// 释放行情数据
	if (_last_tick)
		_last_tick->release();

	// 释放商品信息
	if (_comm_info)
		_comm_info->release();
}
/**
 * @brief 获取实际目标仓位
 * @details 如果目标仓位是DBL_MAX（清仓标记），则返回0，否则返回原值
 * @param _target 原始目标仓位
 * @return 实际目标仓位
 */
inline double get_real_target(double _target) {
	if (_target == DBL_MAX)
		return 0;

	return _target;
}
/**
 * @brief 检查是否为清仓状态
 * @details 判断目标仓位是否等于DBL_MAX，该值被用作清仓标记
 * @param target 目标仓位
 * @return 如果为清仓状态返回true，否则返回false
 */
inline bool is_clear(double target)
{
	return (target == DBL_MAX);
}
/**
 * @brief 计算时间区间的秒数
 * @details 将开始时间和结束时间（以HHMM格式表示）转换为总秒数
 * @param begintime 开始时间，格式为HHMM，如1030表示10:30
 * @param endtime 结束时间，格式为HHMM，如1100表示11:00
 * @return 开始时间和结束时间之间的秒数
 */
inline uint32_t calTmSecs(uint32_t begintime, uint32_t endtime) //计算执行时间：s
{
	// 将HHMM格式转换为总秒数：小时*3600 + 分钟*60
	return   ((endtime / 100) * 3600 + (endtime % 100) * 60) - ((begintime / 100) * 3600 + (begintime % 100) * 60);

}
/**
 * @brief 计算交易时间点在交易日内的分钟数
 * @details 将行情的时间戳转换为分钟计数，考虑交易日内的不同时段
 *          上午交易时段：9:30-11:30，下午交易时段：13:00-15:00
 * @param actiontime 行情时间，格式为HHMMSSmmm
 * @return 该时间点在交易日内的分钟数，用于匹配目标VWAP序列
 */
inline double calTmStamp(uint32_t actiontime) //计算tick时间属于哪个时间单元
{
	string timestamp = to_string(actiontime);
	int hour = stoi(timestamp.substr(0, 2));
	int minute = stoi(timestamp.substr(2, 2));
	double total_minute = 0;
	
	// 开盘前
	if (hour < 9 || (hour == 9 && minute < 30)) {
		total_minute = 0;
	}
	// 上午交易时段：9:30-11:30
	else if (hour < 11 || (hour == 11 && minute <= 30)) {
		total_minute = (hour - 9) * 60 + minute - 30;
	}
	// 中午休市
	else if (hour < 13 || (hour == 13 && minute < 30)) {
		total_minute = 120 + (hour - 11) * 60 + minute;
	}
	// 下午交易时段：13:00-15:00
	else if (hour < 15 || (hour == 15 && minute <= 0)) {
		total_minute = 240 + (hour - 13) * 60 + minute - 30;
	}
	// 收盘后
	else {
		total_minute = 240;
	}
	
	// 中午休市时间特殊处理
	if (timestamp >= "113000000" && timestamp < "130000000") {
		total_minute = 120;
	}
	
	// 添加秒和毫秒的分数
	total_minute += stoi(timestamp.substr(4, 2)) / 60;
	total_minute += stoi(timestamp.substr(6, 3)) / 60000;
	
	return total_minute; // 这里应该+1，对应vector 所以再-1
}
/**
 * @brief 获取工厂名称
 * @return 执行器工厂名称
 */
const char * WtStockVWapExeUnit::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 获取执行单元名称
 * @return 执行单元名称
 */
const char * WtStockVWapExeUnit::getName()
{
	return "WtStockVWapExeUnit";
}

/**
 * @brief 初始化执行单元
 * @details 从配置中读取参数并设置执行单元的各项配置，加载相关资源
 * @param ctx 执行上下文对象，用于访问交易环境
 * @param stdCode 标准化合约代码
 * @param cfg 配置项对象
 */
void WtStockVWapExeUnit::init(ExecuteContext * ctx, const char * stdCode, WTSVariant * cfg)
{
	// 调用父类初始化
	ExecuteUnit::init(ctx, stdCode, cfg);

	// 获取商品信息并保持引用
	_comm_info = ctx->getCommodityInfo(stdCode);//获取品种参数
	if (_comm_info)
		_comm_info->retain();

	// 获取交易时段信息并保持引用
	_sess_info = ctx->getSessionInfo(stdCode);//获取交易时间模板信息
	if (_sess_info)
		_sess_info->retain();
	
	// 从配置中读取执行参数
	_begin_time = cfg->getUInt32("begin_time");    // 开始时间
	_end_time = cfg->getUInt32("end_time");        // 结束时间
	_ord_sticky = cfg->getUInt32("ord_sticky");    // 挂单时限
	_tail_secs = cfg->getUInt32("tail_secs");      // 执行尾部时间
	_total_times = cfg->getUInt32("total_times");  // 总执行次数
	_price_mode = cfg->getUInt32("price_mode");    // 价格模式
	_price_offset = cfg->getUInt32("offset");      // 价格偏移
	_order_lots = cfg->getDouble("lots");          // 单次发单手数
	
	// 读取最小开仓数量（可选参数）
	if (cfg->has("minopenlots"))
		_min_open_lots = cfg->getDouble("minopenlots");	//最小下单数
	
	// 计算单次发单时间间隔，考虑去除尾部时间
	_fire_span = (_total_secs - _tail_secs) / _total_times;		//单次发单时间间隔,要去掉尾部时间计算,这样的话,最后剩余的数量就有一个兜底发单的机制了

	// 输出初始化完成的日志
	ctx->writeLog(fmt::format("执行单元WtStockVWapExeUnit[{}] 初始化完成,订单超时 {} 秒,执行时限 {} 秒,收尾时间 {} 秒", stdCode, _ord_sticky, _total_secs, _tail_secs).c_str());
	
	// 根据开始和结束时间计算总秒数
	_total_secs = calTmSecs(_begin_time, _end_time);//执行总时间：秒

	// 判断是否为科创板股票
	int code = std::stoi(StrUtil::split(stdCode, ".")[2]);
	if (code >= 688000)
	{
		_is_KC = true;
	}
	
	// 获取最小下单数量
	_min_hands = get_minOrderQty(stdCode);
	
	// 根据是否为科创板调整最小开仓数量
	if (_min_open_lots != 0) { 
		if (_is_KC) {
			_min_open_lots = max(_min_open_lots, _min_hands); // 科创板取较大值
		}
		else {
			_min_open_lots = min(_min_open_lots, _min_hands); // 普通股票取较小值
		}
	}
	
	// 确定T+0交易模式（可转债等可以T+0交易）
	if (_comm_info->getTradingMode() == TradingMode::TM_Long)
		_is_t0 = true;
	
	// 检查VWAP分布文件是否存在
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
 * @brief 处理订单状态回调
 * @details 当订单状态变化时被调用，如成交、撤销等情况。更新订单监控器状态并处理相应的业务逻辑
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 订单委托价格
 * @param isCanceled 订单是否已撤销
 */
void WtStockVWapExeUnit::on_order(uint32_t localid, const char * stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
	// 检查订单是否在监控器中
	if (!_orders_mon.has_order(localid))
		return;
	
	// 处理订单完成或撤销的情况
	if (isCanceled || leftover == 0)
	{
		// 从监控器中移除订单
		_orders_mon.erase_order(localid);
		// 如果有撤单计数，减少并记录日志
		if (_cancel_cnt > 0)
		{
			_cancel_cnt--;
			_ctx->writeLog(fmtutil::format("[{}@{}] Order of {} cancelling done, cancelcnt -> {}", __FILE__, __LINE__, _code.c_str(), _cancel_cnt));
		}
	}

	// 如果订单全部成交（非撤销），重置撤单次数计数
	if (leftover == 0 && !isCanceled) {
		_cancel_times = 0;
		_ctx->writeLog(fmtutil::format("Order {} has filled", localid));
	}
	
	// 如果全部订单已撤销，这个时候一般是遇到要超时撤单（挂单超时） 
	if (isCanceled && _cancel_cnt == 0)
	{
		// 获取当前实际持仓
		double realPos = _ctx->getPosition(stdCode);
		// 如果实际持仓与目标不一致，需要重新发单
		if (!decimal::eq(realPos, _this_target))
		{
			_ctx->writeLog(fmtutil::format("Order {} of {} canceled, re_fire will be done", localid, stdCode));
			_cancel_times++;
			// 撤单以后重发，一般是加点重发；对最小下单量的校验
			fire_at_once(max(_min_open_lots, _this_target - realPos));
		}
	}

	// 处理异常情况：订单未撤销但撤单计数不为0
	if (!isCanceled && _cancel_cnt != 0) { // 一般出现问题，需要返回检查  触发撤单 cnt++,onorder响应处理才会--
		_ctx->writeLog(fmtutil::format("Order {} of {}  hasn't canceled, error will be return ", localid, stdCode));
		return;
	}
}

void WtStockVWapExeUnit::on_channel_ready()
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
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
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


void WtStockVWapExeUnit::on_tick(WTSTickData * newTick)
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
 * @brief 处理成交回调
 * @details 当有成交发生时被调用，记录成交日志
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtStockVWapExeUnit::on_trade(uint32_t localid, const char * stdCode, bool isBuy, double vol, double price)
{
	// 记录成交日志
	_ctx->writeLog(fmtutil::format("Order {} of {} traded: {} @ {}", localid, stdCode, vol, price));
}
void WtStockVWapExeUnit::do_calc()
{
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	StdUniqueLock lock(_mtx_calc);
	const char* code = _code.c_str();
	double undone = _ctx->getUndoneQty(code);
	double newVol = get_real_target(_target_pos);
	double realPos = _ctx->getPosition(code);//总仓位，昨仓 + 今仓买入的
	double vailyPos = _ctx->getPosition(code,false);//可用仓位  即昨仓
	double diffQty = newVol - realPos;//目标差
	if (!_channel_ready)
		return;
	if (_is_finish)
		return;
	if (_is_t0)
		vailyPos = realPos;
	double target_pos = max(realPos - vailyPos, _target_pos);
	if (!decimal::eq(target_pos, _target_pos))
	{
		_ctx->writeLog(fmtutil::format("{} can sell hold pos not enough, target adjust {}->{}", code, _target_pos, target_pos));
		_target_pos = target_pos;
	}
	//有正在撤销的订单,则不能进行下一轮计算
	if (_cancel_cnt != 0)
	{
		_ctx->writeLog(fmt::format("{}尚有未完成的撤单指令，暂时退出本轮执行", _code).c_str());
		return;
	}
	if (decimal::eq(diffQty, 0))
		return;
	if (decimal::ge(_start_price, 0)) {//补一次
		_start_price = _last_tick->price();
	}
	// 在判断的时候，要两边四舍五入，防止一些碎股导致一直无法完成执行
	if (decimal::eq(round_hands(target_pos, _min_hands), round_hands(realPos, _min_hands)) && !(target_pos == 0 && realPos < _min_hands && realPos>target_pos))
	{
		_ctx->writeLog(fmtutil::format("{}: target position {} set finish", _code.c_str(), _target_pos));
		_is_finish = true;
		return;
	}
	//每一次发单要保障成交,所以如果有未完成单,说明上一轮没完成
	//有未完成订单&&与实际仓位变动方向相反
	//则需要撤销现有订单
	bool isBuy = decimal::gt(undone, 0);//undone>0,isbuy=1
	if (decimal::lt(diffQty*undone, 0))
	{
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
	_ctx->writeLog(fmt::format("第 {} 次发单", _fired_times + 1).c_str());
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

	//买
	if (isBuy) {
		//如果是买的话，要考虑取整和     
		curQty = round_hands(curQty, _min_open_lots);
	}
	// 卖要对碎股做检查
	else {
		if (decimal::lt(vailyPos, _min_open_lots))
		{
			curQty = vailyPos;
		}
		else
		{
			curQty = round_hands(curQty, _min_open_lots);
		}
		curQty = min(vailyPos, _min_open_lots);
	}

	//设定本轮目标仓位
	_this_target = realPos + curQty;

	WTSTickData* curTick = _last_tick;
	uint64_t now = TimeUtils::getLocalTimeNow();
	isBuy = decimal::gt(diffQty, 0);
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

void WtStockVWapExeUnit::fire_at_once(double qty)
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
	if (qty > 0)
		ids = _ctx->buy(code, targetPx, abs(qty));
	else
		ids = _ctx->sell(code, targetPx, abs(qty));

	_orders_mon.push_order(ids.data(), ids.size(), now, isCanCancel);

	curTick->release();
}

void WtStockVWapExeUnit::set_position(const char * stdCode, double newVol)
{
	if (_code.compare(stdCode) != 0)
		return;

	if (decimal::eq(newVol, _target_pos))
		return;
	if (decimal::lt(newVol, 0))
	{
		_ctx->writeLog(fmt::format("{} is an error stock target position", newVol).c_str());
		return;
	}
	_target_pos = newVol;

	_target_mode = TargetMode::stocks;
	_is_finish = false;
	_start_time = TimeUtils::getLocalTimeNow();
	WTSTickData* tick = _ctx->grabLastTick(_code.c_str());
	if (tick) {
		_start_price = tick->price();
		tick->release();
	}
	_fired_times = 0;//已执行次数

	do_calc();
}

void WtStockVWapExeUnit::on_channel_lost()
{
}
void WtStockVWapExeUnit::clear_all_position(const char* stdCode) {
	if (_code.compare(stdCode) != 0)
		return;
	_is_clear = true;
	_target_pos = 0;
	_target_amount = 0;
	do_calc();
}
