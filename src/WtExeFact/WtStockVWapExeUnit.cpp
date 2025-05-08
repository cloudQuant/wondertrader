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

/**
 * @brief 处理交易通道就绪回调
 * @details 当交易通道准备就绪时调用，主要处理未完成订单的状态同步
 *          包含三种情况的处理：
 *          1. 有未完成订单但本地无订单记录：清除所有未被管理的订单
 *          2. 无未完成订单但本地有订单记录：清除本地错误订单
 *          3. 其他异常情况处理
 */
void WtStockVWapExeUnit::on_channel_ready()
{
	// 标记交易通道为就绪状态
	_channel_ready = true;
	
	// 获取未完成的订单数量
	double undone = _ctx->getUndoneQty(_code.c_str());
	
	// 情况1: 如果有未完成订单但本地没有订单记录，需要清除未被管理的订单
	if (!decimal::eq(undone, 0) && !_orders_mon.has_order())
	{
		/*
		 *	如果未完成单不为0，而OMS没有订单
		 *	这说明有未完成单不在监控之中,全部撤销掉
		 *	因为这些订单没有本地订单号，无法直接进行管理
		 *	这种情况，就是刚启动的时候，上次的未完成单或者外部的挂单
		 */
		_ctx->writeLog(fmt::format("{} unmanaged orders of {},cancel all", undone, _code).c_str());

		// 根据未完成订单的正负判断是买入还是卖出订单
		bool isBuy = (undone > 0);
		
		// 撤销对应方向的所有未完成订单
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		
		// 将撤销的订单添加到监控器中进行跟踪
		_orders_mon.push_order(ids.data(), ids.size(), _ctx->getCurTime());
		
		// 更新撤单计数
		_cancel_cnt += ids.size();

		// 记录撤单日志
		_ctx->writeLog(fmtutil::format("[{}@{}]cancelcnt -> {}", __FILE__, __LINE__, _cancel_cnt));
	}
	// 情况2: 如果没有未完成订单但本地有订单记录，需要清除本地错误订单
	else if (decimal::eq(undone, 0) && _orders_mon.has_order())
	{	/*
		 *	By Wesey @ 2021.12.13
		 *	如果未完成单为0，但是OMS中是有订单的
		 *	说明OMS中是错单，需要清理掉，不然超时撤单就会出错
		 *	这种情况，一般是断线重连以后，之前下出去的订单，并没有真正发送到柜台
		 *	所以这里需要清理掉本地订单
		 */
		// 记录日志并清除所有本地订单
		_ctx->writeLog(fmtutil::format("Local orders of {} not confirmed in trading channel, clear all", _code.c_str()));
		_orders_mon.clear_orders();
	}
	// 情况3: 其他异常情况处理
	else
	{
		// 记录日志，显示当前未完成订单和本地监控状态
		_ctx->writeLog(fmtutil::format("Unrecognized condition while channle ready, {:.2f} live orders of {} exists, local orders {}exist",
			undone, _code.c_str(), _orders_mon.has_order() ? "" : "not "));
	}
	
	// 通道就绪后触发一次计算，重新调整仓位
	do_calc();
}


/**
 * @brief 处理行情数据回调
 * @details 当收到新的行情数据时被调用，更新内部状态并检查超时订单，触发交易计算
 * @param newTick 新的行情数据
 */
void WtStockVWapExeUnit::on_tick(WTSTickData * newTick)
{
	// 检查行情数据是否有效且属于当前合约
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	// 标记是否为第一笔行情
	bool isFirstTick = false;
	
	// 释放旧的行情数据
	if (_last_tick) {
		_last_tick->release();
	}
	else {
		// 如果之前没有行情数据，这是第一笔行情
		isFirstTick = true;
		// 如果行情时间不在交易时间内（如集合竞价时段），则过滤掉该行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;
	}
	// 保存新的行情数据并增加引用计数
	_last_tick = newTick;
	_last_tick->retain();

	// 如果是第一笔行情，需要检查目标仓位是否需要调整
	if (isFirstTick)
	{
		// 获取当前目标仓位、未完成订单和实际仓位
		double newVol = _target_pos;
		const char* stdCode = _code.c_str();
		double undone = _ctx->getUndoneQty(stdCode);
		double realPos = _ctx->getPosition(stdCode);
		
		// 如果目标仓位不等于未完成订单加实际仓位，需要调整
		if (!decimal::eq(newVol, undone + realPos))
		{
			// 触发计算逻辑进行仓位调整
			do_calc();
		}
	}
	else // 非第一笔行情处理
	{
		// 获取当前时间
		uint64_t now = TimeUtils::getLocalTimeNow();
		bool hasCancel = false;
		
		// 检查是否有超时订单需要撤销
		if (_ord_sticky != 0 && _orders_mon.has_order())
		{
			// 检查订单是否超时，如果超时则撤销
			_orders_mon.check_orders(_ord_sticky, now, [this, &hasCancel](uint32_t localid) {
				if (_ctx->cancel(localid))
				{
					_cancel_cnt++;
					_ctx->writeLog(fmt::format("Order expired, cancelcnt updated to {}", _cancel_cnt).c_str());
					hasCancel = true;
				}
			});
		}
		
		// 如果没有撤单操作，且距离上次发单时间超过了发单间隔，则触发新的计算
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
/**
 * @brief VWAP执行单元的核心计算逻辑
 * @details 根据当前市场状况、目标仓位和实际仓位计算下一步操作
 *          包括判断是否需要发单、发单数量、价格策略等
 *          实现了根据VWAP分布序列分批次执行大单的核心逻辑
 */
void WtStockVWapExeUnit::do_calc()
{
	// 防止重入计算
	CalcFlag flag(&_in_calc);
	if (flag)
		return;

	// 加锁保护共享数据
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

/**
 * @brief 立即发送订单
 * @details 根据指定的数量立即发送买入或卖出订单
 *          计算目标价格基于当前价格模式设置（最新价、最优价或对手价）
 *          并处理涅跌停板限制和订单监控
 * @param qty 待下单数量，正数表示买入，负数表示卖出
 */
void WtStockVWapExeUnit::fire_at_once(double qty)
{
	// 如果数量为0，直接返回
	if (decimal::eq(qty, 0))
		return;
	// 增加行情数据的引用计数
	_last_tick->retain();
	WTSTickData* curTick = _last_tick;
	
	// 获取合约代码、当前时间
	const char* code = _code.c_str();
	uint64_t now = TimeUtils::getLocalTimeNow();
	
	// 根据数量正负判断是买入还是卖出
	bool isBuy = decimal::gt(qty, 0);
	double targetPx = 0;

	// 根据价格模式设置确定委托基准价格: 
	// 0-最新价, 1-最优价, 2-对手价
	if (_price_mode == 0) {
		// 使用最新成交价格
		targetPx = curTick->price();
	}
	else if (_price_mode == 1) {
		// 使用最优价：买入时用卖一价，卖出时用买一价
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

/**
 * @brief 设置合约的目标仓位
 * @details 设置指定合约的目标仓位，并触发VWAP执行计算
 *          如果目标仓位与当前仓位相同或为负数，则不进行操作
 *          设置目标仓位时，会重置执行状态并记录当前时间和价格
 * @param stdCode 合约代码
 * @param newVol 新的目标仓位量
 */
void WtStockVWapExeUnit::set_position(const char * stdCode, double newVol)
{
	// 检查合约是否与当前管理的合约一致
	if (_code.compare(stdCode) != 0)
		return;

	// 如果目标仓位与当前设置的一样，则不需要重复设置
	if (decimal::eq(newVol, _target_pos))
		return;
	
	// 验证目标仓位是否合法（不能为负数）
	if (decimal::lt(newVol, 0))
	{
		_ctx->writeLog(fmt::format("{} is an error stock target position", newVol).c_str());
		return;
	}
	
	// 设置新的目标仓位
	_target_pos = newVol;

	// 将目标模式设置为股票数量模式
	_target_mode = TargetMode::stocks;
	
	// 重置执行状态
	_is_finish = false;
	
	// 记录当前时间作为开始时间
	_start_time = TimeUtils::getLocalTimeNow();
	
	// 获取当前合约的最新行情并记录开始价格
	WTSTickData* tick = _ctx->grabLastTick(_code.c_str());
	if (tick) {
		_start_price = tick->price();
		tick->release();
	}
	
	// 重置已执行次数
	_fired_times = 0;

	// 触发计算逻辑
	do_calc();
}

/**
 * @brief 处理交易通道断开回调
 * @details 当交易通道断开时被调用，当前为空实现
 *          可以在此处添加必要的断开后处理逻辑，如标记订单状态、清除本地订单等
 */
void WtStockVWapExeUnit::on_channel_lost()
{
	// 在交易通道断开时执行的处理逻辑
	// 当前为空实现
}
/**
 * @brief 清除指定合约的所有仓位
 * @details 将指定合约的目标仓位和目标数量设置为0，并标记为清仓状态
 *          如果输入的合约代码与执行单元当前管理的合约不符，则不进行操作
 * @param stdCode 合约代码
 */
void WtStockVWapExeUnit::clear_all_position(const char* stdCode) {
	// 检查传入的合约代码是否与当前管理的合约一致
	if (_code.compare(stdCode) != 0)
		return;
	
	// 标记为清仓状态
	_is_clear = true;
	
	// 将目标仓位和目标数量设为0
	_target_pos = 0;
	_target_amount = 0;
	
	// 触发计算逻辑执行清仓
	do_calc();
}
