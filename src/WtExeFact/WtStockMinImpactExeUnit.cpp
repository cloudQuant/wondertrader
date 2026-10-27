/*!
 * \file WtStockMinImpactExeUnit.cpp
 * \brief 股票最小冲击执行单元实现文件
 *
 * 本文件实现了适用于股票交易的最小冲击算法的执行单元，
 * 支持按股数、金额或比例指定目标仓位，通过智能划分订单和策略化下单
 * 来最小化对市场的冲击成本
 *
 * \author Huerjie
 * \date 2022/06/01
 */

#include "WtStockMinImpactExeUnit.h"

extern const char* FACT_NAME;

/**
 * @brief 构造函数
 * @details 初始化股票最小冲击执行单元的各成员变量，设置默认值
 */
WtStockMinImpactExeUnit::WtStockMinImpactExeUnit()
	: _last_tick(NULL)          // 最新行情数据初始化为空
	, _comm_info(NULL)          // 商品信息初始化为空
	, _price_mode(0)            // 价格模式默认为最新价
	, _price_offset(0)          // 价格偏移默认为0
	, _expire_secs(0)           // 订单超时秒数默认为0
	//, _cancel_cnt(0)
	, _target_pos(0)            // 目标仓位默认为0
	, _cancel_times(0)          // 撤单次数初始化为0
	, _last_place_time(0)       // 最后下单时间初始化为0
	, _last_tick_time(0)        // 最后收到Tick时间初始化为0
	, _is_clear{ false }        // 初始非清仓状态
	, _min_order{ 0 }           // 最小下单量初始化为0
	, _is_KC{ false }           // 初始非科创板股票
	, _is_cancel_unmanaged_order{ true }  // 默认撤销未管理订单
	, _is_finish{ true }        // 初始执行完成状态
	, _is_first_tick{ true }    // 初始为第一个Tick
	, _is_ready{ false }        // 初始通道未就绪
	, _is_total_money_ready{ false }  // 初始总资金未就绪
{
}

/**
 * @brief 析构函数
 * @details 释放内部资源，包括行情数据和商品信息
 */
WtStockMinImpactExeUnit::~WtStockMinImpactExeUnit()
{
	// 释放行情数据
	if (_last_tick)
		_last_tick->release();

	// 释放商品信息
	if (_comm_info)
		_comm_info->release();
}

/**
 * @brief 获取所属执行器工厂名称
 * @return 工厂名称字符串
 */
const char* WtStockMinImpactExeUnit::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 获取执行单元名称
 * @return 执行单元名称字符串
 */
const char* WtStockMinImpactExeUnit::getName()
{
	return "WtStockMinImpactExeUnit";
}

/**
 * @brief 初始化执行单元
 * @details 根据配置参数初始化股票最小冲击执行单元的各种执行参数
 * @param ctx 执行单元运行的上下文环境
 * @param stdCode 标准化合约代码
 * @param cfg 执行单元配置项
 */
void WtStockMinImpactExeUnit::init(ExecuteContext* ctx, const char* stdCode, WTSVariant* cfg)
{
	// 调用父类初始化方法
	ExecuteUnit::init(ctx, stdCode, cfg);
	
	// 获取并保存商品信息
	_comm_info = ctx->getCommodityInfo(stdCode);
	if (_comm_info)
		_comm_info->retain();

	// 获取并保存交易时段信息
	_sess_info = ctx->getSessionInfo(stdCode);
	if (_sess_info)
		_sess_info->retain();

	// 从配置中读取各执行参数
	_price_offset = cfg->getInt32("offset");	// 价格偏移跳数，一般和订单同方向
	_expire_secs = cfg->getUInt32("expire");	// 订单超时秒数
	_price_mode = cfg->getInt32("pricemode");	// 价格类型,0-最新价,-1-最优价,1-对手价,2-自动,默认为0
	_entrust_span = cfg->getUInt32("span");		// 发单时间间隔，单位毫秒
	_by_rate = cfg->getBoolean("byrate");		// 是否按照对手的挂单数的比例下单，如果是true，则rate字段生效，如果是false则lots字段生效
	_order_lots = cfg->getDouble("lots");		// 单次发单手数
	_qty_rate = cfg->getDouble("rate");			// 下单手数比例
	
	// 处理总资金设置，用于持仓比例计算
	if (cfg->has("total_money"))
	{
		_is_total_money_ready = true;
		_total_money = cfg->getDouble("total_money"); // 执行器使用的金额，配合目标比例使用，该字段留空或值小于0，则以账户当前余额为准
	}
	
	// 读取是否撤销未管理订单的配置
	if (cfg->has("is_cancel_unmanaged_order"))
		_is_cancel_unmanaged_order = cfg->getBoolean("is_cancel_unmanaged_order");
	
	// 读取最大撤单次数配置
	if (cfg->has("max_cancel_time"))
		_max_cancel_time = cfg->getInt32("max_cancel_time");

	// 判断是否为科创板股票
	int code = std::stoi(StrUtil::split(stdCode, ".")[2]);
	if (code >= 688000)
	{
		_is_KC = true;
	}
	
	// 获取最小交易单位
	_min_hands = get_minOrderQty(stdCode);
	
	// 读取最小下单量配置
	if (cfg->has("min_order"))					// 最小下单数
		_min_order = cfg->getDouble("min_order");

	// 根据交易品种调整最小下单量
	if (_min_order != 0)
	{
		if (_is_KC)
		{
			_min_order = max(_min_order, _min_hands);
		}
		else
		{
			//_min_order = max(_min_order, _min_hands);
			_min_order = min(_min_order, _min_hands); // 2023.6.5-zhaoyk
		}
	}

	// 确定T0交易模式（可转债等可以T+0交易）
	if (_comm_info->getTradingMode() == TradingMode::TM_Long)
		_is_t0 = true;

	// 输出初始化日志
	ctx->writeLog(fmt::format("MiniImpactExecUnit {} inited, order price: {} ± {} ticks, order expired: {} secs, order timespan:{} millisec, order qty: {} @ {:.2f} min_order: {:.2f} is_cancel_unmanaged_order: {}",
		stdCode, PriceModeNames[_price_mode + 1], _price_offset, _expire_secs, _entrust_span, _by_rate ? "byrate" : "byvol", _by_rate ? _qty_rate : _order_lots, _min_order, _is_cancel_unmanaged_order ? "true" : "false").c_str());
}

/**
 * @brief 订单状态回调处理
 * @details 处理订单状态变化的回调信息，包括订单成交或撤销等情况
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入订单
 * @param leftover 剩余未成交数量
 * @param price 订单委托价格
 * @param isCanceled 是否已撤销
 */
void WtStockMinImpactExeUnit::on_order(uint32_t localid, const char* stdCode, bool isBuy, double leftover, double price, bool isCanceled)
{
	{
		// 加锁保护共享数据
		StdUniqueLock lock(_mtx_calc);
		_ctx->writeLog(fmtutil::format("on_order localid:{} stdCode:{} isBuy:{} leftover:{} price:{} isCanceled:{}", localid, stdCode, isBuy, leftover, price, isCanceled));
		
		// 检查是否为受管理的订单
		if (!_orders_mon.has_order(localid))
		{
			// 非管理的合约订单
			_ctx->writeLog(fmtutil::format("{} {} didnt in mon", stdCode, localid));
			return;
		}

		// 处理订单完成或撤销情况
		if (isCanceled || leftover == 0)
		{
			_orders_mon.erase_order(localid);
			if (isCanceled)
				_ctx->writeLog(fmtutil::format("{} {} canceled, earse from mon", stdCode, localid));
			else
				_ctx->writeLog(fmtutil::format("{} {} done, earse from mon", stdCode, localid));
		}

		// 订单全部成交时重置撤单次数
		if (leftover == 0 && !isCanceled)
			_cancel_times = 0;
	}

	// 如果有撤单,增加撤单计数并触发重新计算
	if (isCanceled)
	{
		_ctx->writeLog(fmt::format("Order {} of {} canceled, recalc will be done", localid, stdCode).c_str());
		_cancel_times++;
		do_calc();
	}
}

/**
 * @brief 交易通道就绪回调
 * @details 当交易通道连接成功并准备就绪时触发此回调方法
 *          标记通道为就绪状态，并触发交易计算
 */
void WtStockMinImpactExeUnit::on_channel_ready()
{
	_ctx->writeLog("=================================channle ready==============================");
	_is_ready = true;
	check_unmanager_order();
	do_calc();
}

/**
 * @brief 交易通道丢失回调
 * @details 当交易通道断开连接或发生错误时触发此回调方法
 *          当前实现为空，可以在此处添加通道断开时的清理操作
 */
void WtStockMinImpactExeUnit::on_channel_lost()
{
}

/**
 * @brief 账户资金信息回调
 * @details 接收并处理账户资金更新信息，只处理人民币账户，更新内部可用资金状态
 * @param currency 货币类型代码
 * @param prebalance 前结余额
 * @param balance 静态余额
 * @param dynbalance 动态余额
 * @param avaliable 可用资金
 * @param closeprofit 平仓盈亏
 * @param dynprofit 浮动盈亏
 * @param margin 占用保证金
 * @param fee 手续费
 * @param deposit 入金
 * @param withdraw 出金
 */
void WtStockMinImpactExeUnit::on_account(const char* currency, double prebalance, double balance, double dynbalance, double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	// 只处理人民币账户信息
	if (strcmp(currency, "CNY") == 0)
	{
		_ctx->writeLog(fmtutil::format("avaliable update {}->:{}", _avaliable, avaliable));
		_avaliable = avaliable;
	}
}

/**
 * @brief 检查并清理未被管理的订单
 * @details 检测当前是否有未完成的未被管理订单，如果有且配置为撤销未管理订单，
 *          则自动撤销这些订单并将其添加到订单监控器中
 */
void WtStockMinImpactExeUnit::check_unmanager_order()
{
	// 获取当前未完成数量
	double undone = _ctx->getUndoneQty(_code.c_str());
	// 清空订单监控器
	_orders_mon.clear_orders();

	// 如果有未完成订单且配置为撤销未管理订单
	if (!decimal::eq(undone, 0) && _is_cancel_unmanaged_order)
	{
		_ctx->writeLog(fmt::format("{} Unmanaged live orders with qty {} of {} found, cancel all", _code, undone, _code.c_str()).c_str());
		// 根据数量正负判断是买入还是卖出订单
		bool isBuy = (undone > 0);
		// 撤销对应方向的所有订单
		OrderIDs ids = _ctx->cancel(_code.c_str(), isBuy);
		// 将撤销订单添加到订单监控器中
		_orders_mon.push_order(ids.data(), ids.size(), _now);
		// 记录撤单日志
		for (auto id : ids)
			_ctx->writeLog(fmt::format("{} mon push unmanager order {} enter time:{}", _code.c_str(), id, _now).c_str());
	}
}

/**
 * @brief 处理市场行情数据
 * @details 接收并处理新的市场行情数据，更新内部状态，检查超时订单，并触发交易计算
 * @param newTick 最新的市场行情数据
 */
void WtStockMinImpactExeUnit::on_tick(WTSTickData* newTick)
{
	// 获取当前时间
	_now = TimeUtils::getLocalTimeNow();
	// 检查行情数据是否有效或是否属于当前合约
	if (newTick == NULL || _code.compare(newTick->code()) != 0)
		return;

	//如果原来的tick不为空,则要释放掉
	if (_last_tick)
	{
		_last_tick->release();
	}
	else
	{
		//如果行情时间不在交易时间,这种情况一般是集合竞价的行情进来,下单会失败,所以直接过滤掉这笔行情
		if (_sess_info != NULL && !_sess_info->isInTradingTime(newTick->actiontime() / 100000))
			return;
	}

	//新的tick数据,要保留
	_last_tick = newTick;
	_last_tick->retain();

	//如果相比上次没有更新的tick进来，则先不下单，防止开盘前集中下单导致通道被封
	uint64_t curTickTime = TimeUtils::makeTime(_last_tick->actiondate(), _last_tick->actiontime());
	if (curTickTime <= _last_tick_time)
	{
		_ctx->writeLog(fmt::format("No tick of {} updated, {} <= {}, execute later",
			_code, curTickTime, _last_tick_time).c_str());
		return;
	}
	_last_tick_time = curTickTime;

	/*
	 *	这里可以考虑一下
	 *	如果写的上一次丢出去的单子不够达到目标仓位
	 *	那么在新的行情数据进来的时候可以再次触发核心逻辑
	 */

	// 输出当前管理的订单状态
	_orders_mon.enumOrder([this](uint32_t localid, uint64_t entertime, bool cancancel) {
		_ctx->writeLog(fmtutil::format("[{}]{} entertime:{} cancancel:{} now:{} last_tick_time:{} live_time:{}", _code, localid, entertime, cancancel, _now, _last_tick_time, _now - entertime));
	});

	// 检查并处理超时订单
	if (_expire_secs != 0 && _orders_mon.has_order())
	{
		_orders_mon.check_orders(_expire_secs, _now, [this](uint32_t localid) {
			if (_ctx->cancel(localid))
			{
				_ctx->writeLog(fmt::format("[{}] Expired order of {} canceled", localid, _code.c_str()).c_str());
				// 记录撤单次数
				if (_cancel_map.find(localid) == _cancel_map.end())
				{
					_cancel_map[localid] = 0;
				}
				_cancel_map[localid] += 1;
			}
		});
	}
	
	// 处理撤单次数超过限制的订单，可能是错单
	if (!_cancel_map.empty())
	{
		std::vector<uint32_t> erro_cancel_orders{};
		for (auto item : _cancel_map)
		{
			if (item.second > _max_cancel_time)
			{
				erro_cancel_orders.push_back(item.first);
			}
		}
		for (uint32_t localid : erro_cancel_orders)
		{
			_cancel_map.erase(localid);
			_orders_mon.erase_order(localid);
			_ctx->writeLog(fmtutil::format("error order:{} canceled by {} times,erase forcely", localid, _max_cancel_time));
		}
	}

	// 触发交易计算
	do_calc();
}

/**
 * @brief 处理成交回报
 * @details 接收并处理订单成交的回报信息，当前实现为空
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param isBuy 是否为买入成交
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtStockMinImpactExeUnit::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
}

/**
 * @brief 处理委托回报
 * @details 接收并处理订单委托的回报结果，如果委托失败则移除该订单并重新计算
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托结果消息
 */
void WtStockMinImpactExeUnit::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	if (!bSuccess)
	{
		StdUniqueLock lock(_mtx_calc);
		//如果不是我发出去的订单,我就不管了
		if (!_orders_mon.has_order(localid))
			return;

		_orders_mon.erase_order(localid);
		_ctx->writeLog(fmtutil::format("{} {} entrust failed erase from mon", _code.c_str(), localid));
	}
	// 委托不成功，重新处理
	do_calc();
}

/**
 * @brief 设置目标仓位
 * @details 设置股票的目标股数仓位，根据目标仓位与当前仓位的差异触发交易计算
 * @param stdCode 标准化合约代码
 * @param newVol 新的目标仓位（股数）
 */
void WtStockMinImpactExeUnit::set_position(const char* stdCode, double newVol)
{
	// 检查是否为当前管理的合约
	if (_code.compare(stdCode) != 0)
		return;

	//如果原来的目标仓位是DBL_MAX，说明已经进入清理逻辑
	//如果这个时候又设置为0，则直接跳过了
	if (is_clear() && decimal::eq(newVol, 0))
	{
		_ctx->writeLog(fmt::format("{} is in clearing processing, position can not be set to 0", stdCode).c_str());
		return;
	}
	// 获取当前实际仓位
	double cur_pos = _ctx->getPosition(stdCode);

	// 目标仓位与当前仓位相等，无需操作
	if (decimal::eq(cur_pos, newVol))
		return;

	// 目标仓位不能为负数
	if (decimal::lt(newVol, 0))
	{
		_ctx->writeLog(fmt::format("{} is an error stock target position", newVol).c_str());
		return;
	}

	// 设置新的目标仓位
	_target_pos = newVol;

	// 设置目标模式为股数模式
	_target_mode = TargetMode::stocks;
	// 输出目标仓位设置日志
	if (is_clear())
		_ctx->writeLog(fmt::format("{} is set to be in clearing processing", stdCode).c_str());
	else
		_ctx->writeLog(fmt::format("Target position of {} is set tb be {}", stdCode, _target_pos).c_str());

	// 重置执行状态
	_is_finish = false;
	_start_time = TimeUtils::getLocalTimeNow();
	// 获取当前市场价格作为开始价格
	WTSTickData* tick = _ctx->grabLastTick(_code.c_str());
	if (tick)
	{
		_start_price = tick->price();
		tick->release();
	}
	// 触发交易计算
	do_calc();
}

/**
 * @brief 清空所有仓位
 * @details 将指定合约的仓位设置为0，并进入清仓模式
 * @param stdCode 标准化合约代码
 */
void WtStockMinImpactExeUnit::clear_all_position(const char* stdCode)
{
	// 检查是否为当前管理的合约
	if (_code.compare(stdCode) != 0)
		return;

	// 设置清仓标志和目标仓位
	_is_clear = true;
	_target_pos = 0;
	_target_amount = 0;
	// 触发交易计算
	do_calc();
}

/**
 * @brief 检查是否处于清仓模式
 * @return 如果处于清仓状态返回true，否则返回false
 */
inline bool WtStockMinImpactExeUnit::is_clear()
{
	return _is_clear;
}

/**
 * @brief 执行交易计算
 * @details 根据当前市场状态和目标仓位计算并执行交易操作
 *          该方法是执行单元的核心交易逻辑，实现最小市场冲击策略
 */
void WtStockMinImpactExeUnit::do_calc()
{
	// 如果没有最新行情数据，无法计算
	if (!_last_tick)
		return;
	if (_is_finish)
		return;
	if (!_is_ready)
	{
		_ctx->writeLog(fmtutil::format("{} wait channel ready", _code));
		return;
	}

	//这里加一个锁，主要原因是实盘过程中发现
	//在修改目标仓位的时候，会触发一次do_calc
	//而ontick也会触发一次do_calc，两次调用是从两个线程分别触发的，所以会出现同时触发的情况
	//如果不加锁，就会引起问题
	//这种情况在原来的SimpleExecUnit没有出现，因为SimpleExecUnit只在set_position的时候触发
	StdUniqueLock lock(_mtx_calc);

	const char* stdCode = _code.c_str();

	double undone = _ctx->getUndoneQty(stdCode);
	// 总仓位，等于昨仓 + 今仓买入的
	double curPos = _ctx->getPosition(stdCode, false);
	// 可用仓位，即昨仓的
	double vailyPos = _ctx->getPosition(stdCode, true);
	if (_is_t0)
		vailyPos = curPos;
	double target_pos = max(curPos - vailyPos, _target_pos);

	if (!decimal::eq(target_pos, _target_pos))
	{
		_ctx->writeLog(fmtutil::format("{} can sell hold pos not enough, target adjust {}->{}", stdCode, _target_pos, target_pos));
		_target_pos = target_pos;
	}

	// 补一次
	if (decimal::ge(_start_price, 0))
	{
		_start_price = _last_tick->price();
	}

	double diffPos = target_pos - curPos;
	_ctx->writeLog(fmtutil::format("{}: target: {} hold:{} left {} wait to execute", _code.c_str(), target_pos, curPos, diffPos));
	// 在判断的时候，要两边四舍五入，防止一些碎股导致一直无法完成执行
	if (decimal::eq(round_hands(target_pos, _min_hands), round_hands(curPos, _min_hands)) && !(target_pos == 0 && curPos < _min_hands && curPos > target_pos))
	{
		_ctx->writeLog(fmtutil::format("{}: target position {} set finish", _code.c_str(), _target_pos));
		_is_finish = true;
		return;
	}

	bool isBuy = decimal::gt(diffPos, 0);
	//有未完成订单，与实际仓位变动方向相反
	//则需要撤销现有订单
	if (decimal::lt(diffPos * undone, 0))
	{
		_ctx->writeLog(fmt::format("{} undone:{} diff:{} cancel", stdCode, undone, diffPos).c_str());
		bool isBuy = decimal::gt(undone, 0);
		OrderIDs ids = _ctx->cancel(stdCode, isBuy);
		if (!ids.empty())
		{
			_orders_mon.push_order(ids.data(), ids.size(), _now);
			for (auto localid : ids)
			{
				_ctx->writeLog(fmt::format("{} mon push wait cancel order {} enter time:{}", _code.c_str(), localid, _now).c_str());
				_ctx->writeLog(fmt::format("[{}] live opposite order of {} canceled", localid, _code.c_str()).c_str());
			}
		}
		return;
	}

	//因为是逐笔发单，所以如果有不需要撤销的未完成单，则暂不发单
	if (!decimal::eq(undone, 0))
	{
		_ctx->writeLog(fmtutil::format("{} undone {} wait...", _code, undone));
		return;
	}

	if (_last_tick == NULL)
	{
		_ctx->writeLog(fmt::format("No lastest tick data of {}, execute later", _code.c_str()).c_str());
		return;
	}

	//检查下单时间间隔;
	if (_now - _last_place_time < _entrust_span)
	{
		_ctx->writeLog(fmtutil::format("entrust span {} last_place_time {} _now {}", _entrust_span, _last_place_time, _now));
		return;
	}

	double this_qty = _order_lots;
	if (_by_rate)
	{
		double book_qty = isBuy ? _last_tick->askqty(0) : _last_tick->bidqty(0);
		book_qty = book_qty * _qty_rate;
		//book_qty = round_hands(book_qty, _min_hands); 
		book_qty = round_hands(book_qty, _min_order);//2023.6.5-zhaoyk
		book_qty = max(_min_order, book_qty);
		this_qty = book_qty;
	}
	diffPos = abs(diffPos);
	this_qty = min(this_qty, diffPos);
	// 买
	if (isBuy)
	{
		//如果是买的话，要考虑取整和资金余额
		//this_qty = round_hands(this_qty, _min_hands);
		this_qty = round_hands(this_qty, _min_order);//2023.6.5-zhaoyk
		if (_avaliable)
		{
			double max_can_buy = _avaliable / _last_tick->price();
			//max_can_buy = (int)(max_can_buy / _min_hands) * _min_hands;
			max_can_buy = (int)(max_can_buy / _min_order) * _min_order;//2023.6.5-zhaoyk
			this_qty = min(max_can_buy, this_qty);
		}
	}
	// 卖要对碎股做检查
	else
	{
		//double chip_stk = vailyPos - int(vailyPos / _min_hands) * _min_hands;
		//if (decimal::lt(vailyPos, _min_hands))
			if (decimal::lt(vailyPos, _min_order))//2023.6.5-zhaoyk
		{
			this_qty = vailyPos;
		}
		else
		{
			//this_qty = round_hands(this_qty, _min_hands);
			this_qty = round_hands(this_qty, _min_order);//2023.6.5-zhaoyk

		}
		this_qty = min(vailyPos, this_qty);
	}

	if (decimal::eq(this_qty, 0))
		return;

	double buyPx, sellPx;
	if (_price_mode == AUTOPX)
	{
		double mp = (_last_tick->bidqty(0) - _last_tick->askqty(0)) * 1.0 / (_last_tick->bidqty(0) + _last_tick->askqty(0));
		bool isUp = (mp > 0);
		if (isUp)
		{
			buyPx = _last_tick->askprice(0);
			sellPx = _last_tick->askprice(0);
		}
		else
		{
			buyPx = _last_tick->bidprice(0);
			sellPx = _last_tick->bidprice(0);
		}
	}
	else
	{
		if (_price_mode == BESTPX)
		{
			buyPx = _last_tick->bidprice(0);
			sellPx = _last_tick->askprice(0);
		}
		else if (_price_mode == LASTPX)
		{
			buyPx = _last_tick->price();
			sellPx = _last_tick->price();
		}
		else if (_price_mode == MARKET)
		{
			buyPx = _last_tick->askprice(0) + _comm_info->getPriceTick() * _price_offset;
			sellPx = _last_tick->bidprice(0) - _comm_info->getPriceTick() * _price_offset;
		}
	}

	// 如果最后价格为0，再做一个修正
	if (decimal::eq(buyPx, 0.0))
		buyPx = decimal::eq(_last_tick->price(), 0.0) ? _last_tick->preclose() : _last_tick->price();

	if (decimal::eq(sellPx, 0.0))
		sellPx = decimal::eq(_last_tick->price(), 0.0) ? _last_tick->preclose() : _last_tick->price();

	buyPx += _comm_info->getPriceTick() * _cancel_times;
	sellPx -= _comm_info->getPriceTick() * _cancel_times;

	//检查涨跌停价
	bool isCanCancel = true;
	if (!decimal::eq(_last_tick->upperlimit(), 0) && decimal::gt(buyPx, _last_tick->upperlimit()))
	{
		_ctx->writeLog(fmt::format("Buy price {} of {} modified to upper limit price", buyPx, _code.c_str(), _last_tick->upperlimit()).c_str());
		buyPx = _last_tick->upperlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}

	if (!decimal::eq(_last_tick->lowerlimit(), 0) && decimal::lt(sellPx, _last_tick->lowerlimit()))
	{
		_ctx->writeLog(fmt::format("Sell price {} of {} modified to lower limit price", sellPx, _code.c_str(), _last_tick->lowerlimit()).c_str());
		sellPx = _last_tick->lowerlimit();
		isCanCancel = false;	//如果价格被修正为涨跌停价，订单不可撤销
	}

	if (isBuy)
	{
		OrderIDs ids = _ctx->buy(stdCode, buyPx, this_qty);
		_orders_mon.push_order(ids.data(), ids.size(), _now, isCanCancel);
		for (auto id : ids)
		{
			_ctx->writeLog(fmt::format("{} mon push buy order {} enter time:{}", _code.c_str(), id, _now).c_str());
		}
	}
	else
	{
		OrderIDs ids = _ctx->sell(stdCode, sellPx, this_qty);
		_orders_mon.push_order(ids.data(), ids.size(), _now, isCanCancel);
		for (auto id : ids)
		{
			_ctx->writeLog(fmt::format("{} mon push sell order {} enter time:{}", _code.c_str(), id, _now).c_str());
		}
	}

	_last_place_time = _now;
}