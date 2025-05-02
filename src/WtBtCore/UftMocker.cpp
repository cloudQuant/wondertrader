/*!
 * \file UftMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略回测模拟器实现文件
 * 
 * UFT策略回测模拟器(UftMocker)是WonderTrader回测框架的核心组件之一，
 * 负责在回测过程中模拟策略的运行环境、处理行情数据、执行交易指令、
 * 模拟订单撮合、计算绩效指标等功能。该模块实现了IDataSink和IUftStraCtx接口，
 * 能够无缝对接策略和行情数据，提供与实盘环境一致的接口体验。
 */
#include "UftMocker.h"
#include "WtHelper.h"

#include <stdarg.h>

#include <boost/filesystem.hpp>

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/TimeUtils.hpp"
#include "../Share/StrUtil.hpp"

#include "../WTSTools/WTSLogger.h"

extern uint32_t makeLocalOrderID();

const char* OFFSET_NAMES[] =
{
	"OPEN",
	"CLOSE",
	"CLOSET"
};

extern std::vector<uint32_t> splitVolume(uint32_t vol);
extern std::vector<double> splitVolume(double vol, double minQty = 1.0, double maxQty = 100.0, double qtyTick = 1.0);

extern uint32_t genRand(uint32_t maxVal = 10000);

/**
 * @brief 生成UFT策略上下文ID
 * @return 新生成的唯一的上下文ID
 * @details 使用原子操作生成唯一的UFT策略上下文ID，起始值为7000，每次调用自增1
 */
inline uint32_t makeUftCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 7000 };
	return _auto_context_id.fetch_add(1);
}

/**
 * @brief UFT策略回测模拟器构造函数
 * @param replayer 历史数据回放器指针
 * @param name 策略名称
 * @details 初始化UFT策略回测模拟器，设置基本参数并生成唯一的上下文ID
 */
UftMocker::UftMocker(HisDataReplayer* replayer, const char* name)
	: IUftStraCtx(name)
	, _replayer(replayer)
	, _strategy(NULL)
	, _use_newpx(false)
	, _error_rate(0)
	, _match_this_tick(false)
{
	_context_id = makeUftCtxId();
}


/**
 * @brief UFT策略回测模拟器析构函数
 * @details 清理模拟器资源，释放策略实例
 */
UftMocker::~UftMocker()
{
	if(_strategy)
	{
		_factory._fact->deleteStrategy(_strategy);
	}
}

/**
 * @brief 处理任务队列中的任务
 * @details 从任务队列中取出并执行所有待处理的任务，如果队列为空则直接返回。
 *          使用递归锁保护整个任务处理过程，并在取出单个任务时使用互斥锁确保线程安全。
 */
void UftMocker::procTask()
{
	if (_tasks.empty())
	{
		return;
	}

	_mtx_control.lock();

	while (!_tasks.empty())
	{
		Task& task = _tasks.front();

		task();

		{
			std::unique_lock<std::mutex> lck(_mtx);
			_tasks.pop();
		}
	}

	_mtx_control.unlock();
}

/**
 * @brief 提交任务到任务队列
 * @param task 要提交的任务
 * @details 将任务添加到任务队列中，任务队列是线程安全的。
 */
void UftMocker::postTask(Task task)
{
	{
		std::unique_lock<std::mutex> lck(_mtx);
		_tasks.push(task);
		return;
	}

	//if(_thrd == NULL)
	//{
	//	_thrd.reset(new std::thread([this](){
	//		while (!_stopped)
	//		{
	//			if(_tasks.empty())
	//			{
	//				std::this_thread::sleep_for(std::chrono::milliseconds(1));
	//				continue;
	//			}

	//			_mtx_control.lock();

	//			while(!_tasks.empty())
	//			{
	//				Task& task = _tasks.front();

	//				task();

	//				{
	//					std::unique_lock<std::mutex> lck(_mtx);
	//					_tasks.pop();
	//				}
	//			}

	//			_mtx_control.unlock();
	//		}
	//	}));
	//}
}

/**
 * @brief 初始化UFT策略工厂
 * @param cfg 配置参数，包含策略工厂的加载路径、撮合参数等
 * @return 初始化是否成功
 * @details 根据配置参数加载策略工厂动态链接库，初始化撮合参数，并创建UFT策略实例
 */
bool UftMocker::init_uft_factory(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	const char* module = cfg->getCString("module");
	
	_use_newpx = cfg->getBoolean("use_newpx");
	_error_rate = cfg->getUInt32("error_rate");
	_match_this_tick = cfg->getBoolean("match_this_tick");

	log_info("UFT match params: use_newpx-{}, error_rate-{}, match_this_tick-{}", _use_newpx, _error_rate, _match_this_tick);

	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
		return false;

	FuncCreateUftStraFact creator = (FuncCreateUftStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);
		return false;
	}

	_factory._module_inst = hInst;
	_factory._module_path = module;
	_factory._creator = creator;
	_factory._remover = (FuncDeleteUftStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
	_factory._fact = _factory._creator();

	WTSVariant* cfgStra = cfg->get("strategy");
	if(cfgStra)
	{
		_strategy = _factory._fact->createStrategy(cfgStra->getCString("name"), "uft");
		_strategy->init(cfgStra->get("params"));
	}
	return true;
}

/**
 * @brief 处理Tick数据
 * @param stdCode 标准合约代码
 * @param curTick 当前的Tick数据
 * @param pxType 价格类型
 * @details 实现IDataSink接口的方法，用于接收并处理行情数据源发送的Tick数据
 */
void UftMocker::handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType)
{
	on_tick(stdCode, curTick);
}

/**
 * @brief 处理委托明细数据
 * @param stdCode 标准合约代码
 * @param curOrdDtl 当前的委托明细数据
 * @details 实现IDataSink接口的方法，用于接收并处理行情数据源发送的委托明细数据
 */
void UftMocker::handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl)
{
	on_order_detail(stdCode, curOrdDtl);
}

/**
 * @brief 处理委托队列数据
 * @param stdCode 标准合约代码
 * @param curOrdQue 当前的委托队列数据
 * @details 实现IDataSink接口的方法，用于接收并处理行情数据源发送的委托队列数据
 */
void UftMocker::handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue)
{
	on_order_queue(stdCode, curOrdQue);
}

/**
 * @brief 处理逐笔成交数据
 * @param stdCode 标准合约代码
 * @param curTrans 当前的逐笔成交数据
 * @details 实现IDataSink接口的方法，用于接收并处理行情数据源发送的逐笔成交数据
 */
void UftMocker::handle_transaction(const char* stdCode, WTSTransData* curTrans)
{
	on_transaction(stdCode, curTrans);
}

/**
 * @brief 处理K线周期结束事件
 * @param stdCode 标准合约代码
 * @param period 周期标识符
 * @param times 周期倍数
 * @param newBar 新生成的K线数据
 * @details 实现IDataSink接口的方法，当K线周期结束时被调用，将最新的K线数据传递给策略
 */
void UftMocker::handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	on_bar(stdCode, period, times, newBar);
}

/**
 * @brief 处理初始化事件
 * @details 实现IDataSink接口的方法，在回测开始时调用，初始化策略并通知交易通道就绪
 *          依次触发策略的on_init和on_channel_ready回调
 */
void UftMocker::handle_init()
{
	on_init();
	on_channel_ready();
}

/**
 * @brief 处理定时任务事件
 * @param uDate 当前交易日期，格式YYYYMMDD
 * @param uTime 当前时间，格式HHMMSS
 * @details 实现IDataSink接口的方法，在回测器的定时任务触发时被调用，当前实现中未使用
 */
void UftMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	//on_schedule(uDate, uTime);
}

/**
 * @brief 处理交易日开始事件
 * @param curTDate 当前交易日期，格式YYYYMMDD
 * @details 实现IDataSink接口的方法，在每个交易日开始时被调用，触发策略的on_session_begin回调
 */
void UftMocker::handle_session_begin(uint32_t curTDate)
{
	on_session_begin(curTDate);
}

/**
 * @brief 处理交易日结束事件
 * @param curTDate 当前交易日期，格式YYYYMMDD
 * @details 实现IDataSink接口的方法，在每个交易日结束时被调用，触发策略的on_session_end回调
 */
void UftMocker::handle_session_end(uint32_t curTDate)
{
	on_session_end(curTDate);
}

/**
 * @brief 处理回放完成事件
 * @details 实现IDataSink接口的方法，在历史数据回放完成时被调用
 *          首先调用dump_outputs()输出回测结果，然后触发on_bactest_end()回调通知策略回测结束
 */
void UftMocker::handle_replay_done()
{
	dump_outputs();

	this->on_bactest_end();
}

/**
 * @brief 处理K线数据并调用策略的on_bar回调
 * @param stdCode 标准合约代码
 * @param period 周期标识符
 * @param times 周期倍数
 * @param newBar 新生成的K线数据
 * @details 当收到新的K线数据时，将其传递给策略实例的on_bar回调函数处理
 */
void UftMocker::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (_strategy)
		_strategy->on_bar(this, stdCode, period, times, newBar);
}

/**
 * @brief 处理Tick行情数据
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @details 这是回测模拟器的核心方法之一，负责处理新到的Tick数据，更新合约价格缓存，
 *          计算动态收益，并根据配置决定是先处理订单还是先触发策略回调。
 *          如果_match_this_tick为true，则先触发策略的on_tick回调，再处理订单；
 *          否则先处理订单，再触发策略的on_tick回调。
 */
void UftMocker::on_tick(const char* stdCode, WTSTickData* newTick)
{
	_price_map[stdCode] = newTick->price();
	{
		std::unique_lock<std::recursive_mutex> lck(_mtx_control);
	}

	update_dyn_profit(stdCode, newTick);
	
	//如果开启了同tick撮合，则先触发策略的ontick，再处理订单
	//如果没开启同tick撮合，则先处理订单，再触发策略的ontick
	if(_match_this_tick)
	{
		on_tick_updated(stdCode, newTick);

		procTask();

		if (!_orders.empty())
		{
			OrderIDs ids;
			for (auto it = _orders.begin(); it != _orders.end(); it++)
			{
				uint32_t localid = it->first;
				ids.emplace_back(localid);
			}

			OrderIDs to_erase;
			for (uint32_t localid : ids)
			{
				bool bNeedErase = procOrder(localid);
				if (bNeedErase)
					to_erase.emplace_back(localid);
			}

			for (uint32_t localid : to_erase)
			{
				auto it = _orders.find(localid);
				_orders.erase(it);
			}
		}
	}
	else
	{
		if (!_orders.empty())
		{
			OrderIDs ids;
			for (auto it = _orders.begin(); it != _orders.end(); it++)
			{
				uint32_t localid = it->first;
				bool bNeedErase = procOrder(localid);
				if (bNeedErase)
					ids.emplace_back(localid);
			}

			for (uint32_t localid : ids)
			{
				auto it = _orders.find(localid);
				_orders.erase(it);
			}
		}

		on_tick_updated(stdCode, newTick);

		procTask();
	}
}

/**
 * @brief 处理Tick数据更新并触发策略回调
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @details 当收到新的Tick数据时，首先检查该合约是否在订阅列表中，
 *          如果在订阅列表中，则调用策略的on_tick回调函数处理该Tick数据
 */
void UftMocker::on_tick_updated(const char* stdCode, WTSTickData* newTick)
{
	auto it = _tick_subs.find(stdCode);
	if (it == _tick_subs.end())
		return;

	if (_strategy)
		_strategy->on_tick(this, stdCode, newTick);
}

/**
 * @brief 处理委托队列数据
 * @param stdCode 标准合约代码
 * @param newOrdQue 新的委托队列数据
 * @details 当收到新的委托队列数据时，调用on_ordque_updated方法处理
 */
void UftMocker::on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	on_ordque_updated(stdCode, newOrdQue);
}

/**
 * @brief 处理委托队列数据更新并触发策略回调
 * @param stdCode 标准合约代码
 * @param newOrdQue 新的委托队列数据
 * @details 当收到新的委托队列数据时，调用策略的on_order_queue回调函数处理
 */
void UftMocker::on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_strategy)
		_strategy->on_order_queue(this, stdCode, newOrdQue);
}

/**
 * @brief 处理委托明细数据
 * @param stdCode 标准合约代码
 * @param newOrdDtl 新的委托明细数据
 * @details 当收到新的委托明细数据时，调用on_orddtl_updated方法处理
 */
void UftMocker::on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	on_orddtl_updated(stdCode, newOrdDtl);
}

/**
 * @brief 处理委托明细数据更新并触发策略回调
 * @param stdCode 标准合约代码
 * @param newOrdDtl 新的委托明细数据
 * @details 当收到新的委托明细数据时，调用策略的on_order_detail回调函数处理
 */
void UftMocker::on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_strategy)
		_strategy->on_order_detail(this, stdCode, newOrdDtl);
}

/**
 * @brief 处理逐笔成交数据
 * @param stdCode 标准合约代码
 * @param newTrans 新的逐笔成交数据
 * @details 当收到新的逐笔成交数据时，调用on_trans_updated方法处理
 */
void UftMocker::on_transaction(const char* stdCode, WTSTransData* newTrans)
{
	on_trans_updated(stdCode, newTrans);
}

/**
 * @brief 处理逐笔成交数据更新并触发策略回调
 * @param stdCode 标准合约代码
 * @param newTrans 新的逐笔成交数据
 * @details 当收到新的逐笔成交数据时，调用策略的on_transaction回调函数处理
 */
void UftMocker::on_trans_updated(const char* stdCode, WTSTransData* newTrans)
{
	if (_strategy)
		_strategy->on_transaction(this, stdCode, newTrans);
}

/**
 * @brief 获取策略上下文ID
 * @return 返回策略上下文的唯一ID
 * @details 实现IUftStraCtx接口的方法，用于获取策略上下文的唯一标识
 */
uint32_t UftMocker::id()
{
	return _context_id;
}

/**
 * @brief 初始化策略
 * @details 在回测开始时调用，触发策略的on_init回调函数，实现策略的初始化
 */
void UftMocker::on_init()
{
	if (_strategy)
		_strategy->on_init(this);
}

/**
 * @brief 处理交易日开始事件
 * @param curTDate 当前交易日期，格式YYYYMMDD
 * @details 在每个交易日开始时调用，首先处理持仓状态，将冻结持仓释放，
 *          将新建持仓转为旧持仓，然后触发策略的on_session_begin回调
 */
void UftMocker::on_session_begin(uint32_t curTDate)
{
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosItem& pInfo = (PosItem&)it.second;
		if (!decimal::eq(pInfo.frozen(), 0))
		{
			log_debug("{} frozen of {} released on {}", pInfo.frozen(), stdCode, curTDate);
			pInfo._prevol += pInfo._newvol;
			pInfo._preavail = pInfo._prevol;

			pInfo._newvol = 0;
			pInfo._newavail = 0;
		}
	}

	_strategy->on_session_begin(this, curTDate);
}

/**
 * @brief 处理交易日结束事件
 * @param curTDate 当前交易日期，格式YYYYMMDD
 * @details 在每个交易日结束时调用，首先触发策略的on_session_end回调，
 *          然后计算当日的盈亏情况，包括平仓盈亏和浮动盈亏，
 *          并将持仓和资金信息记录到日志中
 */
void UftMocker::on_session_end(uint32_t curTDate)
{
	_strategy->on_session_end(this, curTDate);

	uint32_t curDate = curTDate;// _replayer->get_trading_date();

	double total_profit = 0;
	double total_dynprofit = 0;

	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		const PosInfo& pInfo = it->second;
		total_profit += pInfo.closeprofit();
		total_dynprofit += pInfo.dynprofit();

		if (!decimal::eq(pInfo._long.volume(), 0.0))
			_pos_logs << fmt::format("{},{},LONG,{},{:.2f},{:.2f}\n", curTDate, stdCode, pInfo._long.volume(), pInfo._long._closeprofit, pInfo._long._dynprofit);

		if (!decimal::eq(pInfo._short.volume(), 0.0))
			_pos_logs << fmt::format("{},{},LONG,{},{:.2f},{:.2f}\n", curTDate, stdCode, pInfo._short.volume(), pInfo._short._closeprofit, pInfo._short._dynprofit);
	}

	_fund_logs << fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curDate,
		_fund_info._total_profit, _fund_info._total_dynprofit,
		_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees);
}

/**
 * @brief 获取未完成委托的数量
 * @param stdCode 标准合约代码
 * @return 未完成委托的数量，多头为正，空头为负
 * @details 实现IUftStraCtx接口的方法，用于获取指定合约的未完成委托数量
 *          遍历所有未完成订单，累加计算指定合约的未成交数量
 */
double UftMocker::stra_get_undone(const char* stdCode)
{
	double ret = 0;
	for (auto it = _orders.begin(); it != _orders.end(); it++)
	{
		const OrderInfo& ordInfo = it->second;
		if (strcmp(ordInfo._code, stdCode) == 0)
		{
			ret += ordInfo._left * ordInfo._isLong ? 1 : -1;
		}
	}

	return ret;
}

/**
 * @brief 取消委托
 * @param localid 本地委托ID
 * @return 取消是否成功，始终返回true
 * @details 实现IUftStraCtx接口的方法，用于取消指定的委托
 *          将取消操作添加到任务队列中异步执行，包括以下步骤：
 *          1. 查找并锁定订单
 *          2. 如果是平仓订单，释放冻结的持仓
 *          3. 记录日志并触发on_order回调
 *          4. 从订单列表中移除该订单
 */
bool UftMocker::stra_cancel(uint32_t localid)
{
	postTask([this, localid](){
		auto it = _orders.find(localid);
		if (it == _orders.end())
			return;

		StdLocker<StdRecurMutex> lock(_mtx_ords);
		OrderInfo& ordInfo = (OrderInfo&)it->second;
		
		if (ordInfo._offset != 0)
		{
			PosInfo& pInfo = _pos_map[ordInfo._code];
			PosItem& pItem = ordInfo._isLong ? pInfo._long : pInfo._short;
			WTSCommodityInfo* commInfo = _replayer->get_commodity_info(ordInfo._code);
			if(commInfo->getCoverMode() == CM_CoverToday)
			{
				if (ordInfo._offset == 2)
					pItem._newavail += ordInfo._left;
				else
					pItem._preavail += ordInfo._left;
			}
			else
			{
				//如果不分平昨平今，则先释放今仓
				double maxQty = std::min(ordInfo._left, pItem._newvol - pItem._newavail);
				pItem._newavail += maxQty;
				pItem._preavail += ordInfo._left - maxQty;
			}
		}

		log_debug("Order {} canceled, action: {} {} @ {}({})", ordInfo._localid, OFFSET_NAMES[ordInfo._offset], ordInfo._isLong?"long":"short", ordInfo._total, ordInfo._left);
		ordInfo._left = 0;
		on_order(localid, ordInfo._code, ordInfo._isLong, ordInfo._offset, ordInfo._total, ordInfo._left, ordInfo._price, true);
		_orders.erase(it);
	});

	return true;
}

/**
 * @brief 取消指定合约的所有委托
 * @param stdCode 标准合约代码
 * @return 被取消的委托ID列表（当前实现中返回空列表）
 * @details 实现IUftStraCtx接口的方法，用于取消指定合约的所有未完成委托
 *          遍历所有未完成订单，如果合约代码匹配，则调用stra_cancel方法取消该订单
 */
OrderIDs UftMocker::stra_cancel_all(const char* stdCode)
{
	OrderIDs ret;
	uint32_t cnt = 0;
	for (auto it = _orders.begin(); it != _orders.end(); it++)
	{
		const OrderInfo& ordInfo = it->second;
		if(strcmp(ordInfo._code, stdCode) == 0)
		{
			stra_cancel(it->first);
		}
	}

	return ret;
}

/**
 * @brief 买入操作（开多或平空）
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param flag 标记，默认为0
 * @return 委托ID列表
 * @details 实现IUftStraCtx接口的方法，用于执行买入操作
 *          首先检查合约信息和数量有效性，然后根据以下规则处理：
 *          1. 如果有空头持仓，先平仓（根据平仓模式决定平仓方式）
 *          2. 剩余数量再开多仓
 *          该方法会自动处理平仓和开仓的逻辑
 */
OrderIDs UftMocker::stra_buy(const char* stdCode, double price, double qty, int flag /* = 0 */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return OrderIDs();
	}

	if (decimal::le(qty, 0))
	{
		log_error("Entrust error: qty {} <= 0", qty);
		return OrderIDs();
	}

	OrderIDs ids;
	const PosInfo& pInfo = _pos_map[stdCode];

	double left = qty;
	//先检查空头
	const PosItem& pItem = pInfo._short;
	if(decimal::gt(pItem.valid(), 0.0))
	{
		if(commInfo->getCoverMode() != CM_CoverToday)
		{
			double maxQty = std::min(left, pItem.valid());
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_short(stdCode, price, maxQty, false, 0);
				if (localid != 0) ids.emplace_back(localid);
			}
			left -= maxQty;
		}
		else
		{
			double maxQty = std::min(left, pItem._preavail);
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_short(stdCode, price, maxQty, false, 0);
				if (localid != 0) ids.emplace_back(localid);
			}
			left -= maxQty;

			maxQty = std::min(left, pItem._newavail);
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_short(stdCode, price, maxQty, true, 0);
				if (localid != 0) ids.emplace_back(localid);
			}
			left -= maxQty;
		}
	}

	//还有剩余则开仓
	if(decimal::gt(left, 0.0))
	{
		ids.emplace_back(stra_enter_long(stdCode, price, left));
	}

	return ids;
}

OrderIDs UftMocker::stra_sell(const char* stdCode, double price, double qty, int flag /* = 0 */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return OrderIDs();
	}

	if (decimal::le(qty, 0))
	{
		log_error("Entrust error: qty {} <= 0", qty);
		return OrderIDs();
	}

	OrderIDs ids;
	const PosInfo& pInfo = _pos_map[stdCode];

	double left = qty;
	//先检查空头
	const PosItem& pItem = pInfo._long;
	if (decimal::gt(pItem.valid(), 0.0))
	{
		if (commInfo->getCoverMode() != CM_CoverToday)
		{
			double maxQty = std::min(left, pItem.valid());
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_long(stdCode, price, maxQty, false, 0);
				if (localid != 0) ids.emplace_back(localid);
			}

			left -= maxQty;
		}
		else
		{
			double maxQty = std::min(left, pItem._preavail);
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_long(stdCode, price, maxQty, false, 0);
				if (localid != 0) ids.emplace_back(localid);
			}
			left -= maxQty;

			maxQty = std::min(left, pItem._newavail);
			if (decimal::gt(maxQty, 0.0))
			{
				uint32_t localid = stra_exit_long(stdCode, price, maxQty, true, 0);
				if (localid != 0) ids.emplace_back(localid);
			}
			left -= maxQty;
		}
	}

	//还有剩余则开仓
	if (decimal::gt(left, 0.0))
	{
		ids.emplace_back(stra_enter_short(stdCode, price, left));
	}

	return ids;
}

/**
 * @brief 开多仓操作
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param flag 标记，默认为0
 * @return 委托ID，如果委托失败返回0
 * @details 实现IUftStraCtx接口的方法，用于执行开多仓操作
 *          首先检查合约信息和数量有效性，然后创建并提交开多仓订单
 *          该方法会异步触发on_entrust回调通知策略委托已提交
 */
uint32_t UftMocker::stra_enter_long(const char* stdCode, double price, double qty, int flag /* = 0 */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return 0;
	}

	if (decimal::le(qty, 0))
	{
		log_error("Entrust error: qty {} <= 0", qty);
		return 0;
	}

	uint32_t localid = makeLocalOrderID();

	OrderInfo order;
	order._localid = localid;
	strcpy(order._code, stdCode);
	order._isLong = true;
	order._offset = 0;
	order._price = price;
	order._total = qty;
	order._left = qty;

	{
		_mtx_ords.lock();
		_orders[localid] = order;
		_mtx_ords.unlock();
	}

	postTask([this, localid]() {
		const OrderInfo& ordInfo = _orders[localid];
		log_debug("order placed: open long of {} @ {} by {}", ordInfo._code, ordInfo._price, ordInfo._total);
		on_entrust(localid, ordInfo._code, true, "entrust success");
	});

	return localid;
}

/**
 * @brief 开空仓操作
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param flag 标记，默认为0
 * @return 委托ID，如果委托失败返回0
 * @details 实现IUftStraCtx接口的方法，用于执行开空仓操作
 *          首先检查合约信息和数量有效性，然后创建并提交开空仓订单
 *          该方法会异步触发on_entrust回调通知策略委托已提交
 */
uint32_t UftMocker::stra_enter_short(const char* stdCode, double price, double qty, int flag /* = 0 */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return 0;
	}

	if (decimal::le(qty, 0))
	{
		log_error("Entrust error: qty {} <= 0", qty);
		return 0;
	}

	uint32_t localid = makeLocalOrderID();

	OrderInfo order;
	order._localid = localid;
	strcpy(order._code, stdCode);
	order._isLong = false;
	order._offset = 0;
	order._price = price;
	order._total = qty;
	order._left = qty;

	{
		_mtx_ords.lock();
		_orders[localid] = order;
		_mtx_ords.unlock();
	}

	postTask([this, localid]() {
		const OrderInfo& ordInfo = _orders[localid];
		log_debug("order placed: open short of {} @ {} by {}", ordInfo._code, ordInfo._price, ordInfo._total);
		on_entrust(localid, ordInfo._code, true, "entrust success");
	});

	return localid;
}

uint32_t UftMocker::stra_exit_long(const char* stdCode, double price, double qty, bool isToday /* = false */, int flag /* = 0 */)
{
	PosInfo& pInfo = _pos_map[stdCode];
	PosItem& pItem = pInfo._long;
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	uint32_t offset = 1;
	if(commInfo->getCoverMode() != CM_CoverToday)
	{
		if(decimal::lt(pItem.valid(), qty))
		{
			log_error("Entrust error: no enough available position");
			return 0;
		}

		double maxQty = std::min(qty, pItem._preavail);
		pItem._preavail -= maxQty;
		pItem._newavail -= qty - maxQty;
	}
	else
	{
		if (isToday) offset = 2;

		double valid = isToday ? pItem._newavail : pItem._preavail;
		if (decimal::lt(valid, qty))
		{
			log_error("Entrust error: no enough available {} position", isToday?"new":"old");
			return 0;
		}

		if (isToday)
			pItem._newavail -= qty;
		else
			pItem._preavail -= qty;
	}

	uint32_t localid = makeLocalOrderID();

	OrderInfo order;
	order._localid = localid;
	strcpy(order._code, stdCode);
	order._isLong = true;
	order._offset = offset;
	order._price = price;
	order._total = qty;
	order._left = qty;

	{
		_mtx_ords.lock();
		_orders[localid] = order;
		_mtx_ords.unlock();
	}

	postTask([this, localid]() {
		const OrderInfo& ordInfo = _orders[localid];
		log_debug("order placed: {} long of {} @ {} by {}", OFFSET_NAMES[ordInfo._offset], ordInfo._code, ordInfo._price, ordInfo._total);
		on_entrust(localid, ordInfo._code, true, "entrust success");
	});

	return localid;
}

uint32_t UftMocker::stra_exit_short(const char* stdCode, double price, double qty, bool isToday /* = false */, int flag /* = 0 */)
{
	PosInfo& pInfo = _pos_map[stdCode];
	PosItem& pItem = pInfo._short;
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	uint32_t offset = 1;
	if (commInfo->getCoverMode() != CM_CoverToday)
	{
		if (decimal::lt(pItem.valid(), qty))
		{
			log_error("Entrust error: no enough available position");
			return 0;
		}

		double maxQty = std::min(qty, pItem._preavail);
		pItem._preavail -= maxQty;
		pItem._newavail -= qty - maxQty;
	}
	else
	{
		if (isToday) offset = 2;

		double valid = isToday ? pItem._newavail : pItem._preavail;
		if (decimal::lt(valid, qty))
		{
			log_error("Entrust error: no enough available {} position", isToday ? "new" : "old");
			return 0;
		}

		if (isToday)
			pItem._newavail -= qty;
		else
			pItem._preavail -= qty;
	}

	uint32_t localid = makeLocalOrderID();

	OrderInfo order;
	order._localid = localid;
	strcpy(order._code, stdCode);
	order._isLong = false;
	order._offset = offset;
	order._price = price;
	order._total = qty;
	order._left = qty;

	{
		_mtx_ords.lock();
		_orders[localid] = order;
		_mtx_ords.unlock();
	}

	postTask([this, localid]() {
		const OrderInfo& ordInfo = _orders[localid];
		log_debug("order placed: {} short of {} @ {} by {}", OFFSET_NAMES[ordInfo._offset], ordInfo._code, ordInfo._price, ordInfo._total);
		on_entrust(localid, ordInfo._code, true, "entrust success");
	});

	return localid;
}

/**
 * @brief 处理订单状态变化
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平标记（0为开仓，1为平仓，2为平今）
 * @param totalQty 总委托量
 * @param leftQty 剩余未成交量
 * @param price 委托价格
 * @param isCanceled 是否已撤单
 * @details 当订单状态发生变化时调用，将订单状态变化信息传递给策略
 *          通过策略的on_order回调函数处理
 */
void UftMocker::on_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double totalQty, double leftQty, double price, bool isCanceled)
{
	if(_strategy)
		_strategy->on_order(this, localid, stdCode, isLong, offset, totalQty, leftQty, price, isCanceled);
}

/**
 * @brief 处理成交回报
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平标记（0为开仓，1为平仓，2为平今）
 * @param vol 成交数量
 * @param price 成交价格
 * @details 当订单成交时调用，首先更新持仓信息，然后将成交信息传递给策略
 *          通过update_position更新持仓状态，并通过策略的on_trade回调函数处理
 */
void UftMocker::on_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset, double vol, double price)
{
	//PosInfo& posInfo = _pos_map[stdCode];
	//PosItem& posItem = isLong ? posInfo._long : posInfo._short;
	update_position(stdCode, isLong, offset, vol, price);
	if (_strategy)
		_strategy->on_trade(this, localid, stdCode, isLong, offset, vol, price);
}

void UftMocker::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	if (_strategy)
		_strategy->on_entrust(localid, bSuccess, message);
}

void UftMocker::on_channel_ready()
{
	if (_strategy)
		_strategy->on_channel_ready(this);
}

void UftMocker::update_dyn_profit(const char* stdCode, WTSTickData* newTick)
{
	auto it = _pos_map.find(stdCode);
	if (it != _pos_map.end())
	{
		WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
		PosInfo& pInfo = (PosInfo&)it->second;
		{
			bool isLong = true;
			PosItem& pItem = pInfo._long;
			if (pItem.volume() == 0)
				pItem._dynprofit = 0;
			else
			{
				double price = isLong ? newTick->bidprice(0) : newTick->askprice(0);
				double dynprofit = 0;
				for (auto pit = pItem._details.begin(); pit != pItem._details.end(); pit++)
				{

					DetailInfo& dInfo = *pit;
					dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale();
					if (dInfo._profit > 0)
						dInfo._max_profit = max(dInfo._profit, dInfo._max_profit);
					else if (dInfo._profit < 0)
						dInfo._max_loss = min(dInfo._profit, dInfo._max_loss);

					dynprofit += dInfo._profit;
				}

				pItem._dynprofit = dynprofit;
			}
		}

		{
			bool isLong = false;
			PosItem& pItem = pInfo._short;
			if (pItem.volume() == 0)
				pItem._dynprofit = 0;
			else
			{
				double price = isLong ? newTick->bidprice(0) : newTick->askprice(0);
				double dynprofit = 0;
				for (auto pit = pItem._details.begin(); pit != pItem._details.end(); pit++)
				{

					DetailInfo& dInfo = *pit;
					dInfo._profit = dInfo._volume*(dInfo._price - price)*commInfo->getVolScale();
					if (dInfo._profit > 0)
						dInfo._max_profit = max(dInfo._profit, dInfo._max_profit);
					else if (dInfo._profit < 0)
						dInfo._max_loss = min(dInfo._profit, dInfo._max_loss);

					dynprofit += dInfo._profit;
				}

				pItem._dynprofit = dynprofit;
			}
		}
	}
}

bool UftMocker::procOrder(uint32_t localid)
{
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfo ordInfo = (OrderInfo&)it->second;

	//第一步,如果在撤单概率中,则执行撤单
	if(_error_rate>0 && genRand(10000)<=_error_rate)
	{
		on_order(localid, ordInfo._code, ordInfo._isLong, ordInfo._offset, ordInfo._total, ordInfo._left, ordInfo._price, true);
		log_info("Random error order: {}", localid);
		return true;
	}
	else
	{
		on_order(localid, ordInfo._code, ordInfo._isLong, ordInfo._offset, ordInfo._total, ordInfo._left, ordInfo._price, false);
	}

	WTSTickData* curTick = stra_get_last_tick(ordInfo._code);
	if (curTick == NULL)
		return false;

	double curPx = curTick->price();
	double orderQty = ordInfo._isLong ? curTick->askqty(0) : curTick->bidqty(0);	//看对手盘的数量
	if (decimal::eq(orderQty, 0.0))
		return false;

	if (!_use_newpx)
	{
		curPx = ordInfo._isLong ? curTick->askprice(0) : curTick->bidprice(0);
		//if (curPx == 0.0)
		if(decimal::eq(curPx, 0.0))
		{
			curTick->release();
			return false;
		}
	}
	curTick->release();

	//如果没有成交条件,则退出逻辑
	if(!decimal::eq(ordInfo._price, 0.0))
	{
		if(ordInfo._isLong && decimal::gt(curPx, ordInfo._price))
		{
			//买单,但是当前价大于限价,不成交
			return false;
		}

		if (!ordInfo._isLong && decimal::lt(curPx, ordInfo._price))
		{
			//卖单,但是当前价小于限价,不成交
			return false;
		}
	}

	/*
	 *	下面就要模拟成交了
	 */
	double maxQty = min(orderQty, ordInfo._left);
	auto vols = splitVolume((uint32_t)maxQty);
	for(uint32_t curQty : vols)
	{
		on_trade(ordInfo._localid, ordInfo._code, ordInfo._isLong, ordInfo._offset, curQty, curPx);

		ordInfo._left -= curQty;
		on_order(localid, ordInfo._code, ordInfo._isLong, ordInfo._offset, ordInfo._total, ordInfo._left, ordInfo._price, false);
	}

	//if(ordInfo._left == 0)
	if(decimal::eq(ordInfo._left, 0.0))
	{
		return true;
	}

	return false;
}

/**
 * @brief 获取合约信息
 * @param stdCode 标准合约代码
 * @return 合约信息对象指针
 * @details 实现IUftStraCtx接口的方法，用于获取指定合约的详细信息
 *          直接调用回放器的get_commodity_info方法获取合约信息
 *          返回的对象包含合约的代码、类型、交易单位、手续费率、交易模式等信息
 */
WTSCommodityInfo* UftMocker::stra_get_comminfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

WTSKlineSlice* UftMocker::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	return _replayer->get_kline_slice(stdCode, basePeriod, count, times);
}

WTSTickSlice* UftMocker::stra_get_ticks(const char* stdCode, uint32_t count)
{
	return _replayer->get_tick_slice(stdCode, count);
}

WTSOrdQueSlice* UftMocker::stra_get_order_queue(const char* stdCode, uint32_t count)
{
	return _replayer->get_order_queue_slice(stdCode, count);
}

WTSOrdDtlSlice* UftMocker::stra_get_order_detail(const char* stdCode, uint32_t count)
{
	return _replayer->get_order_detail_slice(stdCode, count);
}

WTSTransSlice* UftMocker::stra_get_transaction(const char* stdCode, uint32_t count)
{
	return _replayer->get_transaction_slice(stdCode, count);
}

WTSTickData* UftMocker::stra_get_last_tick(const char* stdCode)
{
	return _replayer->get_last_tick(stdCode);
}

/**
 * @brief 获取合约的持仓量
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只返回可用仓位，默认为false表示返回总仓位
 * @param iFlag 持仓方向标记，1为单返回多头仓位，2为单返回空头仓位，3为多空差额，默认为3
 * @return 持仓量
 * @details 实现IUftStraCtx接口的方法，用于返回策略当前持有的指定合约的仓位
 *          根据iFlag参数决定返回的是多头、空头还是多空差额
 *          根据bOnlyValid参数决定返回的是总仓位还是可用仓位（总仓位减去冻结仓位）
 */
double UftMocker::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, int32_t iFlag /* = 3 */)
{
	const PosInfo& posInfo = _pos_map[stdCode];
	if (iFlag == 1)
		return bOnlyValid ? posInfo._long.valid() : posInfo._long.volume();
	else if (iFlag == 2)
		return bOnlyValid ? posInfo._short.valid() : posInfo._short.volume();
	else
		return bOnlyValid ? (posInfo._long.valid() - posInfo._short.valid()) : (posInfo._long.volume() - posInfo._short.volume());
}

/**
 * @brief 获取合约的本地净持仓量
 * @param stdCode 标准合约代码
 * @return 净持仓量（多头减去空头）
 * @details 实现IUftStraCtx接口的方法，用于返回策略当前本地的净持仓量
 *          计算方式为多头总仓位减去空头总仓位
 *          与 stra_get_position(stdCode, false, 3) 结果相同
 */
double UftMocker::stra_get_local_position(const char* stdCode)
{
	const PosInfo& posInfo = _pos_map[stdCode];
	return posInfo._long.volume() - posInfo._short.volume();
}

/**
 * @brief 枚举并回调所有持仓信息
 * @param stdCode 标准合约代码，如果为空字符串则枚举所有合约
 * @return 所有枚举合约的总持仓量
 * @details 实现IUftStraCtx接口的方法，用于枚举当前所有或指定合约的持仓信息
 *          对于每个合约，会通过策略的on_position回调将持仓详情传递给策略
 *          同时累加计算总持仓量并返回
 *          回调信息包括收盘持仓、可用持仓、新开持仓和新开可用持仓
 */
double UftMocker::stra_enum_position(const char* stdCode)
{
	uint32_t tdate = _replayer->get_trading_date();
	double ret = 0;
	bool bAll = (strlen(stdCode) == 0);
	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		if (!bAll && strcmp(it->first.c_str(), stdCode) != 0)
			continue;

		const PosInfo& pInfo = it->second;
		_strategy->on_position(this, stdCode, true, pInfo._long._prevol, pInfo._long._preavail, pInfo._long._newvol, pInfo._long._newavail);
		_strategy->on_position(this, stdCode, false, pInfo._short._prevol, pInfo._short._preavail, pInfo._short._newvol, pInfo._short._newavail);
		ret += pInfo._long.volume() + pInfo._short.volume();
	}

	return ret;
}

/**
 * @brief 获取合约当前价格
 * @param stdCode 标准合约代码
 * @return 当前价格
 * @details 实现IUftStraCtx接口的方法，用于获取合约当前最新价格
 *          通过调用回放器的get_cur_price方法获取当前价格
 */
double UftMocker::stra_get_price(const char* stdCode)
{
	return _replayer->get_cur_price(stdCode);
}

/**
 * @brief 获取当前日期
 * @return 当前日期，格式为YYYYMMDD
 * @details 实现IUftStraCtx接口的方法，用于获取回测当前的计算日期
 *          返回四位数字格式的日期，如20220305表示2022年3月5日
 */
uint32_t UftMocker::stra_get_date()
{
	return _replayer->get_date();
}

/**
 * @brief 获取当前时间
 * @return 当前时间，格式为HHMMSS或HHMM
 * @details 实现IUftStraCtx接口的方法，用于获取回测当前的原始时间
 *          返回数字格式的时间，如092530表示9点25分30秒或ह点25分
 */
uint32_t UftMocker::stra_get_time()
{
	return _replayer->get_raw_time();
}

/**
 * @brief 获取当前秒数
 * @return 当前秒数
 * @details 实现IUftStraCtx接口的方法，用于获取回测当前的秒数信息
 *          返回0-59的秒数值，与当前时间的秒数部分相对应
 */
uint32_t UftMocker::stra_get_secs()
{
	return _replayer->get_secs();
}

/**
 * @brief 订阅tick数据
 * @param stdCode 标准合约代码
 * @details 实现IUftStraCtx接口的方法，用于订阅指定合约的tick数据
 *          首先将合约代码添加到本地订阅列表中，然后通知回放器订阅该合约的tick数据
 *          自从2022.03.01起，增加了本地订阅列表，以便在tick数据回调前进行检查
 */
void UftMocker::stra_sub_ticks(const char* stdCode)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	_tick_subs.insert(stdCode);

	_replayer->sub_tick(_context_id, stdCode);
}

/**
 * @brief 订阅订单队列数据
 * @param stdCode 标准合约代码
 * @details 实现IUftStraCtx接口的方法，用于订阅指定合约的订单队列数据
 *          直接调用回放器的sub_order_queue方法进行订阅
 *          订阅后将通过handle_order_queue方法接收订单队列数据
 */
void UftMocker::stra_sub_order_queues(const char* stdCode)
{
	_replayer->sub_order_queue(_context_id, stdCode);
}

/**
 * @brief 订阅订单详情数据
 * @param stdCode 标准合约代码
 * @details 实现IUftStraCtx接口的方法，用于订阅指定合约的订单详情数据
 *          直接调用回放器的sub_order_detail方法进行订阅
 *          订阅后将通过handle_order_detail方法接收订单详情数据
 */
void UftMocker::stra_sub_order_details(const char* stdCode)
{
	_replayer->sub_order_detail(_context_id, stdCode);
}

/**
 * @brief 订阅逆回成交数据
 * @param stdCode 标准合约代码
 * @details 实现IUftStraCtx接口的方法，用于订阅指定合约的逆回成交数据
 *          直接调用回放器的sub_transaction方法进行订阅
 *          订阅后将通过handle_transaction方法接收成交数据
 */
void UftMocker::stra_sub_transactions(const char* stdCode)
{
	_replayer->sub_transaction(_context_id, stdCode);
}

/**
 * @brief 输出信息级别日志
 * @param message 日志消息
 * @details 实现IUftStraCtx接口的方法，用于输出信息级别的日志
 *          调用WTSLogger的log_dyn_raw方法，将日志写入到strategy模块下当前策略的日志文件中
 *          日志级别为INFO，用于记录普通信息
 */
void UftMocker::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

/**
 * @brief 输出调试级别日志
 * @param message 日志消息
 * @details 实现IUftStraCtx接口的方法，用于输出调试级别的日志
 *          调用WTSLogger的log_dyn_raw方法，将日志写入到strategy模块下当前策略的日志文件中
 *          日志级别为DEBUG，用于记录详细的调试信息
 */
void UftMocker::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

/**
 * @brief 输出错误级别日志
 * @param message 日志消息
 * @details 实现IUftStraCtx接口的方法，用于输出错误级别的日志
 *          调用WTSLogger的log_dyn_raw方法，将日志写入到strategy模块下当前策略的日志文件中
 *          日志级别为ERROR，用于记录错误信息和异常情况
 */
void UftMocker::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}


/**
 * @brief 输出回测结果到CSV文件
 * @details 将回测过程中收集的交易、平仓、资金和持仓日志导出到CSV文件
 *          创建以策略名命名的文件夹，并将以下文件写入该文件夹：
 *          1. trades.csv - 交易记录，包含合约、时间、方向、动作、价格、数量、手续费等
 *          2. closes.csv - 平仓记录，包含合约、方向、开仓时间、开仓价格、平仓时间、平仓价格、数量、利润等
 *          3. funds.csv - 资金记录，包含日期、平仓盈亏、持仓盈亏、动态余额和手续费
 *          4. positions.csv - 持仓记录，包含日期、合约、方向、数量、平仓盈亏和动态盈亏
 *          这些文件可用于后续的策略分析和绩效评估
 */
void UftMocker::dump_outputs()
{
	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "/";
	boost::filesystem::create_directories(folder.c_str());

	std::string filename = folder + "trades.csv";
	std::string content = "code,time,direct,action,price,qty,fee,usertag\n";
	content += _trade_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "closes.csv";
	content = "code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag\n";
	content += _close_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());


	filename = folder + "funds.csv";
	content = "date,closeprofit,positionprofit,dynbalance,fee\n";
	content += _fund_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "positions.csv";
	content = "date,code,direct,volume,closeprofit,dynprofit\n";
	if (!_pos_logs.str().empty()) content += _pos_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());
}

/**
 * @brief 记录交易日志
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平标记（0为开仓，1为平仓，2为平今）
 * @param curTime 当前时间
 * @param price 交易价格
 * @param qty 交易数量
 * @param fee 交易手续费
 * @details 将交易信息格式化并追加到内部交易日志流中
 *          记录的信息包括合约代码、时间、交易方向、开平动作、价格、数量和手续费
 *          这些日志最终会写入trades.csv文件中
 */
void UftMocker::log_trade(const char* stdCode, bool isLong, uint32_t offset, uint64_t curTime, double price, double qty, double fee)
{
	_trade_logs << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << OFFSET_NAMES[offset]
		<< "," << price << "," << qty << "," << fee  << "\n";
}

/**
 * @brief 记录平仓日志
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头方向
 * @param openTime 开仓时间
 * @param openpx 开仓价格
 * @param closeTime 平仓时间
 * @param closepx 平仓价格
 * @param qty 平仓数量
 * @param profit 平仓盈亏
 * @param maxprofit 最大盈利
 * @param maxloss 最大亏损
 * @param totalprofit 总盈亏，默认为0
 * @details 将平仓信息格式化并追加到内部平仓日志流中
 *          记录的信息包括合约代码、交易方向、开仓时间、开仓价格、平仓时间、
 *          平仓价格、数量、平仓盈亏、最大盈利、最大亏损和总盈亏
 *          这些日志最终会写入closes.csv文件中
 */
void UftMocker::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double maxprofit, double maxloss,
	double totalprofit /* = 0 */)
{
	_close_logs << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
		<< "," << closeTime << "," << closepx << "," << qty << "," << profit << "," << maxprofit << "," << maxloss << ","
		<< totalprofit << "\n";
}

/**
 * @brief 更新持仓信息
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头方向
 * @param offset 开平标记（0为开仓，1为平仓，2为平今）
 * @param qty 交易数量
 * @param price 交易价格，默认为0.0会使用当前的市场价格
 * @details 根据交易信息更新账户持仓状态
 *          不同的offset值对应不同的处理逻辑：
 *          - 开仓(0): 添加新的持仓明细，更新新开仓量
 *          - 平仓(1): 优先平收盘前持仓，然后再平新开仓仓，计算平仓盈亏
 *          - 平今(2): 只平当日新开的仓位，计算平仓盈亏
 *          同时记录交易日志和平仓日志，更新资金信息
 */
void UftMocker::update_position(const char* stdCode, bool isLong, uint32_t offset, double qty, double price /* = 0.0 */)
{
	PosItem& pItem = isLong ? _pos_map[stdCode]._long : _pos_map[stdCode]._short;

	//先确定成交价格
	double curPx = price;
	if (decimal::eq(price, 0.0))
		curPx = _price_map[stdCode];

	const char* pos_dir = isLong ? "long" : "short";

	//获取时间
	uint64_t curTm = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_min_time()*100000 + _replayer->get_secs();
	uint32_t curTDate = _replayer->get_trading_date();

	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
		return;

	//成交价
	double trdPx = curPx;

	if (offset == 0)
	{
		//如果是开仓，则直接增加明细即可
		pItem._newvol += qty;
		//如果T+1，则冻结仓位要增加
		if (commInfo->isT1())
		{
			//ASSERT(diff>0);
			log_debug("{} position of {} frozen up to {}", pos_dir, stdCode, pItem.frozen());
		}
		else
		{
			pItem._newavail += qty;
		}

		DetailInfo dInfo;
		dInfo._price = trdPx;
		dInfo._volume = qty;
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		pItem._details.emplace_back(dInfo);

		double fee = _replayer->calc_fee(stdCode, trdPx, qty, 0);
		_fund_info._total_fees += fee;

		log_trade(stdCode, isLong, 0, curTm, trdPx, qty, fee);
	}
	else if(offset == 1)
	{
		//如果是平仓（平昨也是这个），则根据明细的时间先后处理平仓
		double maxQty = min(pItem._prevol, qty);
		pItem._prevol -= maxQty;
		pItem._newvol -= qty - maxQty;

		std::vector<DetailInfo>::iterator eit = pItem._details.end();
		double left = qty;
		for (auto it = pItem._details.begin(); it != pItem._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			double maxQty = min(dInfo._volume, left);
			if (decimal::eq(maxQty, 0))
				continue;

			double maxProf = dInfo._max_profit * maxQty / dInfo._volume;
			double maxLoss = dInfo._max_loss * maxQty / dInfo._volume;

			dInfo._volume -= maxQty;
			left -= maxQty;

			if (decimal::eq(dInfo._volume, 0))
				eit = it;

			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (isLong)
				profit *= -1;
			pItem._closeprofit += profit;

			//等比缩放明细的相关浮盈
			dInfo._profit = dInfo._profit*dInfo._volume / (dInfo._volume + maxQty);
			dInfo._max_profit = dInfo._max_profit*dInfo._volume / (dInfo._volume + maxQty);
			dInfo._max_loss = dInfo._max_loss*dInfo._volume / (dInfo._volume + maxQty);
			_fund_info._total_profit += profit;
			double fee = _replayer->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, isLong, offset, curTm, trdPx, maxQty, fee);
			//这里写平仓记录
			log_close(stdCode, isLong, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, maxProf, maxLoss, pItem._closeprofit);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		if (eit != pItem._details.end())
			pItem._details.erase(pItem._details.begin(), eit);

	}
	else if (offset == 2)
	{
		//如果是平今，只更新今仓，先找到今仓起始的位置，再开始处理
		pItem._newvol -= qty;
		std::vector<DetailInfo>::iterator sit = pItem._details.end();
		std::vector<DetailInfo>::iterator eit = pItem._details.end();

		uint32_t count = 0;
		double left = qty;
		for (auto it = pItem._details.begin(); it != pItem._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			//如果不是今仓，就直接跳过
			if(dInfo._opentdate != curTDate)
				continue;

			double maxQty = min(dInfo._volume, left);
			if (decimal::eq(maxQty, 0))
				continue;

			if (sit == pItem._details.end())
				sit = it;

			eit = it;

			double maxProf = dInfo._max_profit * maxQty / dInfo._volume;
			double maxLoss = dInfo._max_loss * maxQty / dInfo._volume;

			dInfo._volume -= maxQty;
			left -= maxQty;

			if (decimal::eq(dInfo._volume, 0))
				count++;

			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!isLong)
				profit *= -1;
			pItem._closeprofit += profit;
			pItem._dynprofit = pItem._dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放
			_fund_info._total_profit += profit;

			uint32_t offset = dInfo._opentdate == curTDate ? 2 : 1;
			double fee = _replayer->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, isLong, offset, curTm, trdPx, maxQty, fee);
			//这里写平仓记录
			log_close(stdCode, isLong, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, maxProf, maxLoss, pItem._closeprofit);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		if (sit != pItem._details.end())
			pItem._details.erase(sit, eit);
	}

	log_info("[{:04d}.{:05d}] {} position of {} updated: {} {} to {}", _replayer->get_min_time(), _replayer->get_secs(), pos_dir, stdCode, OFFSET_NAMES[offset], qty, pItem.volume());

	double dynprofit = 0;
	for (const DetailInfo& dInfo : pItem._details)
	{
		dynprofit += dInfo._profit;
	}
	pItem._dynprofit = dynprofit;
}
