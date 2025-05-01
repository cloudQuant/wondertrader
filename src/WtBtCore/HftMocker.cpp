/*!
 * \file HftMocker.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易策略回测模拟器实现文件
 * \details 实现了高频交易策略回测模拟器，包括订单处理、成交模拟、持仓管理等功能
 *          提供了高频策略多品种、多合约的回测功能，实现了完整的交易模拟和策略运行环境
 */
#include "HftMocker.h"
#include "WtHelper.h"

#include <stdarg.h>

#include <boost/filesystem.hpp>

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/TimeUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

/**
 * @brief 生成序列号自增的本地订单ID
 * @return 新生成的订单ID
 * @details 基于当前时间和原子操作生成唯一的序列号递增的订单ID
 *          第一次调用时会初始化基准值，之后每次调用递增返回
 */
uint32_t makeLocalOrderID()
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	if (_auto_order_id == 0)
	{
		uint32_t curYear = TimeUtils::getCurDate() / 10000 * 10000 + 101;
		_auto_order_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(curYear, 0)) / 1000 * 50);
	}

	return _auto_order_id.fetch_add(1);
}

/**
 * @brief 将交易量随机分拆为多个订单量
 * @param vol 要分拆的总交易量
 * @return 分拆后的交易量列表
 * @details 将指定的交易量随机分拆为多个小单，模拟实际交易中的拆单行为
 *          分拆量在最小交易量到最大交易量之间随机生成。如果总量小于等于最小量则不分拆
 */
std::vector<uint32_t> splitVolume(uint32_t vol)
{
	if (vol == 0) return std::move(std::vector<uint32_t>());

	uint32_t minQty = 1;
	uint32_t maxQty = 100;
	uint32_t length = maxQty - minQty + 1;
	std::vector<uint32_t> ret;
	if (vol <= minQty)
	{
		ret.emplace_back(vol);
	}
	else
	{
		uint32_t left = vol;
		srand((uint32_t)time(NULL));
		while (left > 0)
		{
			uint32_t curVol = minQty + (uint32_t)rand() % length;

			if (curVol >= left)
				curVol = left;

			if (curVol == 0)
				continue;

			ret.emplace_back(curVol);
			left -= curVol;
		}
	}

	return std::move(ret);
}

/**
 * @brief 将浮点型交易量随机分拆为多个订单量
 * @param vol 要分拆的总交易量
 * @param minQty 最小分拆量，默认为1.0
 * @param maxQty 最大分拆量，默认为100.0
 * @param qtyTick 交易量单位进位，默认为1.0
 * @return 分拆后的交易量列表
 * @details 将浮点型交易量随机分拆为多个小单，支持自定义最小最大交易量及进位数
 *          分拆量在最小交易量到最大交易量之间随机生成，并按照进位单位取整
 */
std::vector<double> splitVolume(double vol, double minQty = 1.0, double maxQty = 100.0, double qtyTick = 1.0)
{
	auto length = (std::size_t)round((maxQty - minQty)/qtyTick) + 1;
	std::vector<double> ret;
	if (vol <= minQty)
	{
		ret.emplace_back(vol);
	}
	else
	{
		double left = vol;
		srand((uint32_t)time(NULL));
		while (left > 0)
		{
			double curVol = minQty + (rand() % length)*qtyTick;

			if (curVol >= left)
				curVol = left;

			if (curVol == 0)
				continue;

			ret.emplace_back(curVol);
			left -= curVol;
		}
	}

	return std::move(ret);
}

/**
 * @brief 生成随机数
 * @param maxVal 随机数的最大值，默认为10000
 * @return 生成的随机数值
 * @details 基于当前分钟数设置随机种子，生成一个0到maxVal-1之间的随机整数
 *          用于在回测中模拟各种随机事件
 */
uint32_t genRand(uint32_t maxVal = 10000)
{
	srand(TimeUtils::getCurMin());
	return rand() % maxVal;
}

/**
 * @brief 生成高频策略上下文ID
 * @return 新生成的上下文ID
 * @details 基于原子操作生成递增的高频策略上下文ID，起始值为6000
 *          用于区分不同的高频策略实例
 */
inline uint32_t makeHftCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 6000 };
	return _auto_context_id.fetch_add(1);
}

/**
 * @brief 高频策略模拟器构造函数
 * @param replayer 历史数据回放器指针
 * @param name 策略名称
 * @details 初始化高频策略回测模拟器，设置各项必要的初始参数和内部状态
 *          包括创建商品信息和数据缓存容器，以及生成唯一的上下文ID
 */
HftMocker::HftMocker(HisDataReplayer* replayer, const char* name)
	: IHftStraCtx(name)
	, _replayer(replayer)
	, _strategy(NULL)
	, _use_newpx(false)
	, _error_rate(0)
	, _match_this_tick(false)
	, _has_hook(false)
	, _hook_valid(true)
	, _resumed(false)
{
	_commodities = CommodityMap::create();

	_context_id = makeHftCtxId();

	_ticks = TickCache::create();
}


/**
 * @brief 高频策略模拟器析构函数
 * @details 清理高频策略模拟器的资源，释放策略实例和各种容器
 *          包括从工厂中删除策略实例，释放商品信息和Tick缓存
 */
HftMocker::~HftMocker()
{
	if(_strategy)
	{
		_factory._fact->deleteStrategy(_strategy);
	}

	_commodities->release();

	_ticks->release();
	_ticks = NULL;
}

/**
 * @brief 处理任务队列中的任务
 * @details 从任务队列中按顺序取出并执行任务，直到队列为空
 *          整个处理过程由控制锁保护，确保在执行任务时不会被其他线程中断
 *          当任务队列为空时直接返回
 */
void HftMocker::procTask()
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
 * @brief 添加任务到任务队列
 * @param task 要添加的任务函数
 * @details 将任务函数添加到任务队列中，等待后续处理
 *          使用互斥锁确保在多线程环境下安全地添加任务
 *          注释的代码是之前的多线程处理方式，当前已简化为单线程模式
 */
void HftMocker::postTask(Task task)
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
 * @brief 初始化高频策略工厂
 * @param cfg 策略工厂配置参数
 * @return 初始化是否成功，true表示成功，false表示失败
 * @details 根据配置对象初始化高频交易策略工厂
 *          首先加载配置的交易参数，如是否使用新价成交、错单率等
 *          然后加载指定的策略工厂动态库，获取工厂创建函数和删除函数
 *          最后创建并初始化策略实例
 *          这个函数是策略回测的关键步骤，确保策略模块可以正确运行
 */
bool HftMocker::init_hft_factory(WTSVariant* cfg)
{
    if (cfg == NULL)
        return false;

    const char* module = cfg->getCString("module");
    
    _use_newpx = cfg->getBoolean("use_newpx");
    _error_rate = cfg->getUInt32("error_rate");
    _match_this_tick = cfg->getBoolean("match_this_tick");

    log_info("HFT match params: use_newpx-{}, error_rate-{}, match_this_tick-{}", _use_newpx, _error_rate, _match_this_tick);

    DllHandle hInst = DLLHelper::load_library(module);
    if (hInst == NULL)
        return false;

    FuncCreateHftStraFact creator = (FuncCreateHftStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
    if (creator == NULL)
    {
        DLLHelper::free_library(hInst);
        return false;
    }

    _factory._module_inst = hInst;
    _factory._module_path = module;
    _factory._creator = creator;
    _factory._remover = (FuncDeleteHftStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
    _factory._fact = _factory._creator();

    WTSVariant* cfgStra = cfg->get("strategy");
    if(cfgStra)
    {
        _strategy = _factory._fact->createStrategy(cfgStra->getCString("name"), cfgStra->getCString("id"));
        _strategy->init(cfgStra->get("params"));
        _name = _strategy->id();
    }
    return true;
}
/**
 * @brief 处理递过来的造市数据
 * @param stdCode 标准合约代码
 * @param curTick 当前Tick数据
 * @param pxType 价格类型
 * @details 调用on_tick函数处理收到的造市数据
 *          这是报价处理的入口函数，由外部模块触发
 */
void HftMocker::handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType)
{
	on_tick(stdCode, curTick);
}

/**
 * @brief 处理递过来的逆序数据
 * @param stdCode 标准合约代码
 * @param curOrdDtl 当前逆序数据
 * @details 调用on_order_detail函数处理收到的逆序数据
 *          逆序数据指逻辑上按时间顺序排列的成交条件
 */
void HftMocker::handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl)
{
	on_order_detail(stdCode, curOrdDtl);
}

/**
 * @brief 处理递过来的委托队列数据
 * @param stdCode 标准合约代码
 * @param curOrdQue 当前委托队列数据
 * @details 调用on_order_queue函数处理收到的委托队列数据
 *          委托队列包含买卖相应价格的等待委托数量
 */
void HftMocker::handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue)
{
	on_order_queue(stdCode, curOrdQue);
}

/**
 * @brief 处理递过来的逐笔成交数据
 * @param stdCode 标准合约代码
 * @param curTrans 当前逐笔成交数据
 * @details 调用on_transaction函数处理收到的逐笔成交数据
 *          逐笔成交数据是市场中实际成交的明细信息
 */
void HftMocker::handle_transaction(const char* stdCode, WTSTransData* curTrans)
{
	on_transaction(stdCode, curTrans);
}

/**
 * @brief 处理递过来的K线关闭数据
 * @param stdCode 标准合约代码
 * @param period K线周期
 * @param times 周期基数
 * @param newBar 新的K线数据
 * @details 调用on_bar函数处理K线关闭事件
 *          用于在K线但登完成时触发策略的处理逻辑
 */
void HftMocker::handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	on_bar(stdCode, period, times, newBar);
}

/**
 * @brief 处理初始化事件
 * @details 先调用on_init函数进行初始化，然后调用on_channel_ready函数通知通道准备完毕
 *          该函数在回测开始前触发，用于策略实例的初始化工作
 */
void HftMocker::handle_init()
{
	on_init();
	on_channel_ready();
}

/**
 * @brief 处理定时调度事件
 * @param uDate 当前日期，格式YYYYMMDD
 * @param uTime 当前时间，格式HHMMSS或HHMMSS.mmm(毫秒)
 * @details 当前实现中已经注释了on_schedule函数的调用
 *          该函数原本用于按时间触发策略的调度事件
 */
void HftMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	//on_schedule(uDate, uTime);
}

/**
 * @brief 处理交易日开始事件
 * @param curTDate 当前交易日，格式YYYYMMDD
 * @details 调用on_session_begin函数处理一个交易日开始的事件
 *          用于在新交易日开始时进行必要的初始化工作
 */
void HftMocker::handle_session_begin(uint32_t curTDate)
{
	on_session_begin(curTDate);
}

/**
 * @brief 处理交易日结束事件
 * @param curTDate 当前交易日，格式YYYYMMDD
 * @details 调用on_session_end函数处理一个交易日结束的事件
 *          用于在交易日结束时进行清算、统计等操作
 */
void HftMocker::handle_session_end(uint32_t curTDate)
{
	on_session_end(curTDate);
}

/**
 * @brief 处理回测完成事件
 * @details 回测完成时执行的操作，包括输出结果并触发策略的回测结束事件
 *          先调用dump_outputs函数输出回测结果，然后触发on_bactest_end事件
 */
void HftMocker::handle_replay_done()
{
	dump_outputs();

	this->on_bactest_end();
}

/**
 * @brief 处理K线数据回调
 * @param stdCode 标准合约代码
 * @param period K线周期
 * @param times 周期基数
 * @param newBar 新的K线数据
 * @details 将K线数据事件转发给策略实例的on_bar函数处理
 *          如果策略实例不存在，则不做任何处理
 */
void HftMocker::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (_strategy)
		_strategy->on_bar(this, stdCode, period, times, newBar);
}

/**
 * @brief 启用或禁用计算钩子
 * @param bEnabled 是否启用钩子，默认为true
 * @details 设置钩子的有效性状态，并记录日志
 *          钩子机制用于在交易计算过程中插入控制逻辑，实现逻辑更新与执行的分离
 */
void HftMocker::enable_hook(bool bEnabled /* = true */)
{
	_hook_valid = bEnabled;

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calculating hook {}", bEnabled ? "enabled" : "disabled");
}

/**
 * @brief 安装计算钩子
 * @details 标记钩子已经安装，并记录相应日志
 *          安装钩子后，在处理行情数据时会触发钩子机制
 */
void HftMocker::install_hook()
{
	_has_hook = true;

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "HFT hook installed");
}

/**
 * @brief 推进一个Tick的计算
 * @details 实现计算线程与控制线程的同步通信机制
 *          如果钩子未安装，直接返回不做处理
 *          通知计算线程进行计算，并等待计算完成通知
 *          完成后重置状态标记，为下一次计算做准备
 */
void HftMocker::step_tick()
{
	if (!_has_hook)
		return;

	WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Notify calc thread, wait for calc done");
	while (!_resumed)
		_cond_calc.notify_all();

	{
		StdUniqueLock lock(_mtx_calc);
		_cond_calc.wait(_mtx_calc);
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc done notified");
		_resumed = false;
	}
}

/**
 * @brief 处理Tick数据
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @details 实现完整的Tick数据处理逻辑：
 *          1. 更新合约最新价格和动态盈亏
 *          2. 根据_match_this_tick选项决定处理顺序：
 *             - 如果为true，先触发策略的on_tick回调，再处理订单
 *             - 如果为false，先处理订单，再触发策略的on_tick回调
 *          3. 支持钩子机制，实现计算线程和数据线程的分离同步
 */
void HftMocker::on_tick(const char* stdCode, WTSTickData* newTick)
{
	_price_map[stdCode] = newTick->price();
	{
		std::unique_lock<std::recursive_mutex> lck(_mtx_control);
	}

	update_dyn_profit(stdCode, newTick);

	OrderIDs all_ids;
	for (auto it = _orders.begin(); it != _orders.end(); it++)
		all_ids.push_back(it->first);
	//如果开启了同tick撮合，则先触发策略的ontick，再处理订单
	//如果没开启同tick撮合，则先处理订单，再触发策略的ontick
	if (_match_this_tick)
	{
		if (_has_hook && _hook_valid)
		{
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Waiting for resume notify");
			StdUniqueLock lock(_mtx_calc);
			_cond_calc.wait(_mtx_calc);
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc resumed");
			_resumed = true;
		}

		on_tick_updated(stdCode, newTick);

		procTask();

		if (!_orders.empty())
		{
			StdLocker<StdRecurMutex> lock(_mtx_ords);
			OrderIDs ids;
			for (uint32_t localid : all_ids)
			{
				bool bNeedErase = procOrder(localid);
				if (bNeedErase)
					ids.emplace_back(localid);
			}

			for (uint32_t localid : ids)
			{
				_orders.erase(localid);
			}
		}
	}
	else
	{
		if (!_orders.empty())
		{
			StdLocker<StdRecurMutex> lock(_mtx_ords);
			OrderIDs ids;
			for (uint32_t localid : all_ids)
			{
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

		if (_has_hook && _hook_valid)
		{
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Waiting for resume notify");
			StdUniqueLock lock(_mtx_calc);
			_cond_calc.wait(_mtx_calc);
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc resumed");
			_resumed = true;
		}

		on_tick_updated(stdCode, newTick);

		procTask();
	}

	if (_has_hook && _hook_valid)
	{
		WTSLogger::log_dyn("strategy", _name.c_str(), LL_DEBUG, "Calc done, notify control thread");
		while (_resumed)
			_cond_calc.notify_all();
	}
}

/**
 * @brief Tick数据更新时触发策略回调
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @details 检查合约是否在订阅列表中，如果在则调用策略的on_tick函数
 *          这是将Tick数据传递给策略进行处理的关键函数
 */
void HftMocker::on_tick_updated(const char* stdCode, WTSTickData* newTick)
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
 * @details 调用on_ordque_updated函数进行进一步处理
 *          这是委托队列数据处理的入口函数
 */
void HftMocker::on_order_queue(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	on_ordque_updated(stdCode, newOrdQue);
}

/**
 * @brief 委托队列数据更新时触发策略回调
 * @param stdCode 标准合约代码
 * @param newOrdQue 新的委托队列数据
 * @details 如果策略实例存在，则调用策略的on_order_queue函数
 *          将委托队列数据转发给策略对象处理
 */
void HftMocker::on_ordque_updated(const char* stdCode, WTSOrdQueData* newOrdQue)
{
	if (_strategy)
		_strategy->on_order_queue(this, stdCode, newOrdQue);
}

/**
 * @brief 处理逆序数据
 * @param stdCode 标准合约代码
 * @param newOrdDtl 新的逆序数据
 * @details 调用on_orddtl_updated函数进行进一步处理
 *          这是逆序数据处理的入口函数
 */
void HftMocker::on_order_detail(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	on_orddtl_updated(stdCode, newOrdDtl);
}

/**
 * @brief 逆序数据更新时触发策略回调
 * @param stdCode 标准合约代码
 * @param newOrdDtl 新的逆序数据
 * @details 如果策略实例存在，则调用策略的on_order_detail函数
 *          将逆序数据转发给策略对象处理
 */
void HftMocker::on_orddtl_updated(const char* stdCode, WTSOrdDtlData* newOrdDtl)
{
	if (_strategy)
		_strategy->on_order_detail(this, stdCode, newOrdDtl);
}

/**
 * @brief 处理逐笔成交数据
 * @param stdCode 标准合约代码
 * @param newTrans 新的逐笔成交数据
 * @details 调用on_trans_updated函数进行进一步处理
 *          这是逐笔成交数据处理的入口函数
 */
void HftMocker::on_transaction(const char* stdCode, WTSTransData* newTrans)
{
	on_trans_updated(stdCode, newTrans);
}

/**
 * @brief 逐笔成交数据更新时触发策略回调
 * @param stdCode 标准合约代码
 * @param newTrans 新的逐笔成交数据
 * @details 如果策略实例存在，则调用策略的on_transaction函数
 *          将逐笔成交数据转发给策略对象处理
 */
void HftMocker::on_trans_updated(const char* stdCode, WTSTransData* newTrans)
{
	if (_strategy)
		_strategy->on_transaction(this, stdCode, newTrans);
}

/**
 * @brief 获取策略上下文ID
 * @return 当前策略实例的上下文ID
 * @details 返回在构造函数中生成的唯一策略上下文ID
 *          该ID用于在回测引擎中唯一标识当前策略实例
 */
uint32_t HftMocker::id()
{
	return _context_id;
}

/**
 * @brief 策略初始化回调
 * @details 触发策略实例的on_init函数进行初始化
 *          如果策略实例存在，调用其on_init函数进行策略初始化
 *          策略可以在此函数中设置各种初始参数和状态
 */
void HftMocker::on_init()
{
	if (_strategy)
		_strategy->on_init(this);
}

/**
 * @brief 交易日开始时调用的函数
 * @param curTDate 当前交易日，格式YYYYMMDD
 * @details 在每个交易日开始时执行的操作，包括：
 *          1. 重置所有标的冻结持仓为零，解除前一交易日的冻结持仓
 *          2. 记录持仓重置日志
 *          3. 触发策略的on_session_begin回调
 *          这是实现T+1交易规则的关键函数之一
 */
void HftMocker::on_session_begin(uint32_t curTDate)
{
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (!decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
			pInfo._frozen = 0;
		}
	}

	if (_strategy)
		_strategy->on_session_begin(this, curTDate);
}

/**
 * @brief 交易日结束时调用的函数
 * @param curTDate 当前交易日，格式YYYYMMDD
 * @details 在每个交易日结束时执行的操作，包括：
 *          1. 统计各标的的平仓盈亏和浮动盈亏
 *          2. 将非空持仓的标的信息记录到持仓日志
 *          3. 将资金信息记录到资金日志，包括平仓盈亏、浮动盈亏、动态权盈和手续费
 *          4. 触发策略的on_session_end回调
 *          这个函数主要用于每日结束时的持仓和资金统计，以及日志记录
 */
void HftMocker::on_session_end(uint32_t curTDate)
{
	uint32_t curDate = curTDate;// _replayer->get_trading_date();

	double total_profit = 0;
	double total_dynprofit = 0;

	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		const PosInfo& pInfo = it->second;
		total_profit += pInfo._closeprofit;
		total_dynprofit += pInfo._dynprofit;

		if (decimal::eq(pInfo._volume, 0.0))
			continue;

		_pos_logs << fmt::format("{},{},{},{:.2f},{:.2f}\n", curTDate, stdCode,
			pInfo._volume, pInfo._closeprofit, pInfo._dynprofit);
	}

	_fund_logs << fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curTDate,
		_fund_info._total_profit, _fund_info._total_dynprofit,
		_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees);

	if (_strategy)
		_strategy->on_session_end(this, curTDate);
}

/**
 * @brief 获取指定合约的未完成委托数量
 * @param stdCode 标准合约代码
 * @return 未完成的买卖委托净数量，买单为正，卖单为负
 * @details 遍历当前订单表，统计指定合约的所有未完成订单数量
 *          计算方式是将所有买单的未成交数量取正，卖单的未成交数量取负，然后相加
 *          返回的数值可用于人工智能策略中估计当前的总体委托方向和数量
 */
double HftMocker::stra_get_undone(const char* stdCode)
{
	double ret = 0;
	for (auto it = _orders.begin(); it != _orders.end(); it++)
	{
		OrderInfoPtr ordInfo = it->second;
		if (strcmp(ordInfo->_code, stdCode) == 0)
		{
			ret += ordInfo->_left * ordInfo->_isBuy ? 1 : -1;
		}
	}

	return ret;
}

/**
 * @brief 根据本地订单ID撤销单个订单
 * @param localid 要撤销的订单的本地ID
 * @return 撤单请求是否成功发送，始终返回true
 * @details 将撤单操作作为任务添加到任务队列中执行，实现异步撤单
 *          任务中的操作包括：
 *          1. 从订单列表中找到对应的订单
 *          2. 将订单的剩余数量置为0
 *          3. 触发on_order回调，标记订单已撤销
 *          4. 从订单列表中删除该订单
 *          所有操作都使用锁保护，确保线程安全
 */
bool HftMocker::stra_cancel(uint32_t localid)
{
	postTask([this, localid](){
		OrderInfoPtr ordInfo = NULL;
		{
			StdLocker<StdRecurMutex> lock(_mtx_ords);
			auto it = _orders.find(localid);
			if (it == _orders.end())
				return;

			ordInfo = it->second;
		}
		
		ordInfo->_left = 0;

		on_order(localid, ordInfo->_code, ordInfo->_isBuy, ordInfo->_total, ordInfo->_left, ordInfo->_price, true, ordInfo->_usertag);

		{
			StdLocker<StdRecurMutex> lock(_mtx_ords);
			_orders.erase(localid);
		}
	});

	return true;
}

/**
 * @brief 根据合约代码和买卖方向批量撤单
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买单，true表示买单，false表示卖单
 * @param qty 要撤销的数量，默认为0，表示撤销全部符合条件的订单
 * @return 所有被撤销的订单ID列表
 * @details 批量撤销指定合约和方向的未完成订单
 *          操作流程：
 *          1. 遍历所有订单，找出匹配的合约和方向
 *          2. 依次撤销这些订单并记录订单ID
 *          3. 如果指定了数量，会在累计撤销数量达到指定值时停止
 *          4. 返回已撤销的订单ID列表
 *          这个函数在需要批量撤销指定合约的所有委托或指定数量委托时非常有用
 */
OrderIDs HftMocker::stra_cancel(const char* stdCode, bool isBuy, double qty /* = 0 */)
{
	OrderIDs ret;
	uint32_t cnt = 0;
	for (auto it = _orders.begin(); it != _orders.end(); it++)
	{
		OrderInfoPtr ordInfo = it->second;
		if(ordInfo->_isBuy == isBuy && strcmp(ordInfo->_code, stdCode) == 0)
		{
			double left = ordInfo->_left;
			stra_cancel(it->first);
			ret.emplace_back(it->first);
			cnt++;
			if (left < qty)
				qty -= left;
			else
				break;
		}
	}

	return ret;
}

/**
 * @brief 策略买入下单接口
 * @param stdCode 标准合约代码
 * @param price 委托价格，如果为0则表示市价单
 * @param qty 委托数量
 * @param userTag 用户自定义标签，可用于订单跟踪
 * @param flag 标志，默认为0
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID列表，包含新下单的本地ID
 * @details 处理策略的买入请求，实现流程：
 *          1. 验证合约信息和下单数量
 *          2. 生成本地订单ID并创建订单信息
 *          3. 将订单信息添加到订单列表中
 *          4. 异步发送下单确认通知
 *          5. 返回订单ID列表
 *          该函数是策略交易的核心接口之一，用于发起买入操作
 */
OrderIDs HftMocker::stra_buy(const char* stdCode, double price, double qty, const char* userTag, int flag /* = 0 */, bool bForceClose /* = false */)
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

	uint32_t localid = makeLocalOrderID();

	OrderInfoPtr order(new OrderInfo);
	order->_localid = localid;
	strcpy(order->_code, stdCode);
	strcpy(order->_usertag, userTag);
	order->_isBuy = true;
	order->_price = price;
	order->_total = qty;
	order->_left = qty;

	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		_orders[localid] = order;		
	}

	postTask([this, localid](){
		const OrderInfoPtr& ordInfo = _orders[localid];
		on_entrust(localid, ordInfo->_code, true, "下单成功", ordInfo->_usertag);
	});

	OrderIDs ids;
	ids.emplace_back(localid);
	return ids;
}

/**
 * @brief 订单状态变化回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买单，true为买单，false为卖单
 * @param totalQty 订单总数量
 * @param leftQty 剩余未成交数量
 * @param price 委托价格
 * @param isCanceled 是否已撤单，默认为false
 * @param userTag 用户标签，默认为空字符串
 * @details 当订单状态发生变化时，此函数会被调用并将订单状态变化通知给策略
 *          当订单成交、撤单或部分成交时都会触发此回调
 *          策略可以基于该回调函数捕捉订单状态变化并进行相应处理
 */
void HftMocker::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */, const char* userTag /* = "" */)
{
	if(_strategy)
		_strategy->on_order(this, localid, stdCode, isBuy, totalQty, leftQty, price, isCanceled, userTag);
}

/**
 * @brief 订单成交回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买单成交，true为买单，false为卖单
 * @param vol 成交数量
 * @param price 成交价格
 * @param userTag 用户标签，默认为空字符串
 * @details 当订单成交时，此函数会被调用并处理成交后的持仓变化
 *          首先获取当前合约的持仓信息，计算成交后的最新持仓量
 *          然后将成交信息通知给策略实例
 */
void HftMocker::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price, const char* userTag/* = ""*/)
{
	const PosInfo& posInfo = _pos_map[stdCode];
	double curPos = posInfo._volume + vol * (isBuy ? 1 : -1);
	do_set_position(stdCode, curPos, price, userTag);
	if (_strategy)
		_strategy->on_trade(this, localid, stdCode, isBuy, vol, price, userTag);
}

/**
 * @brief 委托回报回调函数
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param bSuccess 委托下单是否成功
 * @param message 委托回报消息
 * @param userTag 用户标签，默认为空字符串
 * @details 当委托下单得到回报时，此函数会被调用并通知策略
 *          委托回报包含订单是否提交成功及相关的回报信息
 *          策略可以根据委托成功与否调整交易逻辑
 */
void HftMocker::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message, const char* userTag/* = ""*/)
{
	if (_strategy)
		_strategy->on_entrust(localid, bSuccess, message, userTag);
}

/**
 * @brief 交易通道就绪回调函数
 * @details 当交易通道准备就绪可以使用时，此函数会被调用
 *          通知策略可以开始正常交易操作
 *          该函数在策略初始化完成或通道重连后触发
 */
void HftMocker::on_channel_ready()
{
	if (_strategy)
		_strategy->on_channel_ready(this);
}

/**
 * @brief 更新持仓的动态盈亏
 * @param stdCode 标准合约代码
 * @param newTick 最新的Tick行情数据
 * @details 根据最新的行情数据计算并更新指定合约的浮动盈亏
 *          首先查找合约的持仓信息，如果持仓量为0则盈亏为0
 *          否则根据持仓方向选择对应的价格（多头用买一价，空头用卖一价）
 *          遍历所有持仓明细，计算每一笔持仓的当前盈亏，并累计总动态盈亏
 *          同时记录每笔持仓的最大盈利和最大亏损
 */
void HftMocker::update_dyn_profit(const char* stdCode, WTSTickData* newTick)
{
	auto it = _pos_map.find(stdCode);
	if (it != _pos_map.end())
	{
		PosInfo& pInfo = (PosInfo&)it->second;
		if (pInfo._volume == 0)
		{
			pInfo._dynprofit = 0;
		}
		else
		{
			bool isLong = decimal::gt(pInfo._volume, 0);
			double price = isLong ? newTick->bidprice(0) : newTick->askprice(0);

			WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
			double dynprofit = 0;
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				if (dInfo._profit > 0)
					dInfo._max_profit = max(dInfo._profit, dInfo._max_profit);
				else if (dInfo._profit < 0)
					dInfo._max_loss = min(dInfo._profit, dInfo._max_loss);

				dynprofit += dInfo._profit;
			}

			pInfo._dynprofit = dynprofit;
		}
	}
}

/**
 * @brief 处理订单模拟成交
 * @param localid 本地订单ID
 * @return 处理结果，true表示处理成功，false表示订单不存在
 * @details 模拟订单的成交过程，包含以下步骤：
 *          1. 查找指定订单ID对应的订单信息
 *          2. 根据错单率决定订单是否随机撤销
 *          3. 对于新下的订单，通知订单已提交成功
 *          4. 根据当前行情和市场状况决定订单是否可以成交
 *          5. 如果可以成交，根据成交规则生成成交数量和价格
 *          6. 触发订单和成交回调
 *          该函数是高频策略回测模拟器中的核心函数之一
 */
bool HftMocker::procOrder(uint32_t localid)
{
	auto it = _orders.find(localid);
	if (it == _orders.end())
		return false;

	OrderInfoPtr ordInfo = it->second;

	//第一步,如果在撤单概率中,则执行撤单
	if(_error_rate>0 && genRand(10000)<=_error_rate)
	{
		on_order(localid, ordInfo->_code, ordInfo->_isBuy, ordInfo->_total, ordInfo->_left, ordInfo->_price, true, ordInfo->_usertag);
		log_info("Random error order: {}", localid);
		return true;
	}
	else if(!ordInfo->_proced_after_placed)
	{
		//如果下单以后，还没处理过，则触发on_order
		on_order(localid, ordInfo->_code, ordInfo->_isBuy, ordInfo->_total, ordInfo->_left, ordInfo->_price, false, ordInfo->_usertag);
		ordInfo->_proced_after_placed = true;
	}

	WTSTickData* curTick = stra_get_last_tick(ordInfo->_code);
	if (curTick == NULL)
		return false;

	double curPx = curTick->price();
	double orderQty = ordInfo->_isBuy ? curTick->askqty(0) : curTick->bidqty(0);	//看对手盘的数量
	if (decimal::eq(orderQty, 0.0))
		return false;

	if (!_use_newpx)
	{
		curPx = ordInfo->_isBuy ? curTick->askprice(0) : curTick->bidprice(0);
		//if (curPx == 0.0)
		if(decimal::eq(curPx, 0.0))
		{
			curTick->release();
			return false;
		}
	}
	curTick->release();

	//如果没有成交条件,则退出逻辑
	if(!decimal::eq(ordInfo->_price, 0.0))
	{
		if(ordInfo->_isBuy && decimal::gt(curPx, ordInfo->_price))
		{
			//买单,但是当前价大于限价,不成交
			return false;
		}

		if (!ordInfo->_isBuy && decimal::lt(curPx, ordInfo->_price))
		{
			//卖单,但是当前价小于限价,不成交
			return false;
		}
	}

	/*
	 *	下面就要模拟成交了
	 */
	double maxQty = min(orderQty, ordInfo->_left);
	auto vols = splitVolume((uint32_t)maxQty);
	for(uint32_t curQty : vols)
	{
		on_trade(ordInfo->_localid, ordInfo->_code, ordInfo->_isBuy, curQty, curPx, ordInfo->_usertag);

		ordInfo->_left -= curQty;
		on_order(localid, ordInfo->_code, ordInfo->_isBuy, ordInfo->_total, ordInfo->_left, ordInfo->_price, false, ordInfo->_usertag);

		double curPos = stra_get_position(ordInfo->_code);

		_sig_logs << _replayer->get_date() << "." << _replayer->get_raw_time() << "." << _replayer->get_secs() << ","
			<< (ordInfo->_isBuy ? "+" : "-") << curQty << "," << curPos << "," << curPx << std::endl;
	}

	//if(ordInfo->_left == 0)
	if(decimal::eq(ordInfo->_left, 0.0))
	{
		return true;
	}

	return false;
}

/**
 * @brief 策略卖出下单接口
 * @param stdCode 标准合约代码
 * @param price 委托价格，如果为0则表示市价单
 * @param qty 委托数量
 * @param userTag 用户自定义标签，可用于订单跟踪
 * @param flag 标志，默认为0
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID列表，包含新下单的本地ID
 * @details 处理策略的卖出请求，实现流程：
 *          1. 验证合约信息和下单数量
 *          2. 对于不能做空的合约，检查是否有足够的可用持仓
 *          3. 生成本地订单ID并创建订单信息
 *          4. 将订单信息添加到订单列表中
 *          5. 异步发送下单确认通知
 *          6. 返回订单ID列表
 *          该函数与stra_buy类似，但增加了对不可做空合约的持仓检查
 */
OrderIDs HftMocker::stra_sell(const char* stdCode, double price, double qty, const char* userTag, int flag /* = 0 */, bool bForceClose /* = false */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of %s", stdCode);
		return OrderIDs();
	}

	if (decimal::le(qty, 0))
	{
		log_error("Entrust error: qty {} <= 0", qty);
		return OrderIDs();
	}

	//如果不能做空，则要看可用持仓
	if(!commInfo->canShort())
	{
		double curPos = stra_get_position(stdCode, true);//只读可用持仓
		if(decimal::gt(qty, curPos))
		{
			log_error("No enough position of {} to sell", stdCode);
			return OrderIDs();
		}
	}

	uint32_t localid = makeLocalOrderID();

	OrderInfoPtr order(new OrderInfo);
	order->_localid = localid;
	strcpy(order->_code, stdCode);
	strcpy(order->_usertag, userTag);
	order->_isBuy = false;
	order->_price = price;
	order->_total = qty;
	order->_left = qty;

	{
		StdLocker<StdRecurMutex> lock(_mtx_ords);
		_orders[localid] = order;
	}

	postTask([this, localid]() {
		const OrderInfoPtr& ordInfo = _orders[localid];
		on_entrust(localid, ordInfo->_code, true, "下单成功", ordInfo->_usertag);
	});

	OrderIDs ids;
	ids.emplace_back(localid);
	return ids;
}

/**
 * @brief 获取合约品种信息
 * @param stdCode 标准合约代码
 * @return 对应的品种信息对象指针
 * @details 从回放器中获取指定合约的品种信息
 *          品种信息包括手续费率、交易时段、合约单位等重要参数
 *          策略可以通过该函数查询品种信息以进行交易决策
 */
WTSCommodityInfo* HftMocker::stra_get_comminfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

/**
 * @brief 获取合约的原始代码
 * @param stdCode 标准合约代码
 * @return 对应的原始合约代码
 * @details 从标准化的合约代码获取原始的合约代码
 *          在WonderTrader中，所有合约都会标准化为统一格式，而原始代码是各交易所原始的合约代码
 *          此功能在需要与外部系统对接或显示原始代码时非常有用
 */
std::string HftMocker::stra_get_rawcode(const char* stdCode)
{
	return _replayer->get_rawcode(stdCode);
}

/**
 * @brief 获及k线历史数据
 * @param stdCode 标准合约代码
 * @param period 周期字符串，如"m1"表示1分钟，"d1"表示日线
 * @param count 请求的K线条数
 * @return K线切片数据指针，包含请求的K线数据
 * @details 从回放器中获取指定合约的K线切片数据
 *          周期格式为单个字符加数字，比如“m5”表礨5分钟线，“d1”表示日线
 *          函数分离出基础周期和倍数，然后调用回放器的接口获及数据
 *          K线数据是策略分析和判断的重要依据
 */
WTSKlineSlice* HftMocker::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	return _replayer->get_kline_slice(stdCode, basePeriod, count, times);
}

/**
 * @brief 获取Tick历史数据
 * @param stdCode 标准合约代码
 * @param count 请求的Tick数量
 * @return Tick切片数据指针，包含指定数量的最新Tick数据
 * @details 从回放器中获取指定合约的Tick历史数据
 *          Tick数据包含买一价、卖一价、最新价、成交量等实时行情信息
 *          高频策略可以基于Tick数据进行快速响应和大量交易决策
 */
WTSTickSlice* HftMocker::stra_get_ticks(const char* stdCode, uint32_t count)
{
	return _replayer->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取委托队列历史数据
 * @param stdCode 标准合约代码
 * @param count 请求的委托队列数量
 * @return 委托队列切片数据指针，包含指定数量的委托队列数据
 * @details 从回放器中获取指定合约的委托队列历史数据
 *          委托队列数据包含各个价位上的委托数量排队情况，可以看到买卖双方的充足程度
 *          高频交易可以使用委托队列信息分析市场深度和价格层的压力
 */
WTSOrdQueSlice* HftMocker::stra_get_order_queue(const char* stdCode, uint32_t count)
{
	return _replayer->get_order_queue_slice(stdCode, count);
}

/**
 * @brief 获取逆序明细历史数据
 * @param stdCode 标准合约代码
 * @param count 请求的逆序明细数量
 * @return 逆序明细切片数据指针，包含指定数量的逆序明细数据
 * @details 从回放器中获取指定合约的逆序明细历史数据
 *          逆序明细数据包含交易所额外提供的委托明细信息，如委托价格、方向、时间等
 *          高频策略可以利用这些数据深入分析市场的微观结构和订单流
 */
WTSOrdDtlSlice* HftMocker::stra_get_order_detail(const char* stdCode, uint32_t count)
{
	return _replayer->get_order_detail_slice(stdCode, count);
}

/**
 * @brief 获取逐笔成交历史数据
 * @param stdCode 标准合约代码
 * @param count 请求的逐笔成交数量
 * @return 逐笔成交切片数据指针，包含指定数量的逐笔成交数据
 * @details 从回放器中获取指定合约的逐笔成交历史数据
 *          逐笔成交数据包含每笔成交的价格、数量、方向、时间等详细信息
 *          高频策略可以通过逐笔成交数据分析市场的短期趋势和交易活跃度
 */
WTSTransSlice* HftMocker::stra_get_transaction(const char* stdCode, uint32_t count)
{
	return _replayer->get_transaction_slice(stdCode, count);
}

/**
 * @brief 获取最新的Tick数据
 * @param stdCode 标准合约代码
 * @return 最新的Tick数据指针，如果找不到则返回NULL
 * @details 获取指定合约的最新Tick数据，查找顺序为：
 *          1. 先从本地缓存_ticks中查找，如果找到则增加引用计数并返回
 *          2. 如果本地缓存中没有，则从回放器中获取
 *          这个函数在高频策略中经常使用，用于获取合约的最新行情
 *          注意返回的Tick数据需要手动释放引用计数
 */
WTSTickData* HftMocker::stra_get_last_tick(const char* stdCode)
{
	if (_ticks != NULL)
	{
		auto it = _ticks->find(stdCode);
		if (it != _ticks->end())
		{
			WTSTickData* lastTick = (WTSTickData*)it->second;
			if (lastTick)
				lastTick->retain();
			return lastTick;
		}
	}

	return _replayer->get_last_tick(stdCode);
}

/**
 * @brief 获取合约持仓量
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只返回可用持仓，默认为false返回总持仓
 * @param flag 标志参数，默认为3，暂时未使用
 * @return 合约持仓量，正数表示多头持仓，负数表示空头持仓
 * @details 获取指定合约的持仓情况：
 *          1. 如果bOnlyValid为true，则返回总持仓减去冻结持仓，主要用于交易权限检查
 *          2. 如果bOnlyValid为false，则返回总持仓数量
 *          注意关于冻结持仓的约定：空头持仓的_frozen应为0，只有多头持仓收到影响
 */
double HftMocker::stra_get_position(const char* stdCode, bool bOnlyValid/* = false*/, int flag/* = 3*/)
{
	const PosInfo& pInfo = _pos_map[stdCode];
	if (bOnlyValid)
	{
		//这里理论上，只有多头才会进到这里
		//其他地方要保证，空头持仓的话，_frozen要为0
		return pInfo._volume - pInfo._frozen;
	}
	else
	return pInfo._volume;
}

/**
 * @brief 获取指定合约的持仓动态盈亏
 * @param stdCode 标准合约代码
 * @return 当前持仓的浮动盈亏，如果无持仓则返回0
 * @details 查询指定合约当前持仓的浮动盈亏情况
 *          首先从持仓映射表中查找指定合约的持仓信息
 *          如果找到则返回其动态盈亏值，否则返回0
 *          此值在update_dyn_profit函数中更新，每次收到新Tick时计算
 */
double HftMocker::stra_get_position_profit(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0.0;

	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

/**
 * @brief 获取指定合约的持仓平均价格
 * @param stdCode 标准合约代码
 * @return 当前持仓的平均成本价，如果无持仓则返回0
 * @details 计算指定合约当前持仓的平均成本价
 *          计算流程：
 *          1. 从持仓映射表中查找指定合约的持仓信息
 *          2. 如果找不到或持仓量为0，则返回0
 *          3. 遍历所有持仓明细，将每笔持仓的价格*数量累计
 *          4. 将总金额除以总持仓量得到平均价格
 *          这个函数对于分析持仓的盈亏状况和正确设置止损价非常重要
 */
double HftMocker::stra_get_position_avgpx(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0.0;

	const PosInfo& pInfo = it->second;
	if (decimal::eq(pInfo._volume, 0.0))
		return 0;

	double amount = 0.0;
	for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
	{
		const DetailInfo& dInfo = *dit;
		amount += dInfo._price*dInfo._volume;
	}

	return amount / pInfo._volume;
}

/**
 * @brief 获取合约当前价格
 * @param stdCode 标准合约代码
 * @return 合约当前价格
 * @details 从回放器中获取指定合约的当前价格
 *          这个函数在计算盈亏和执行交易决策时经常使用
 *          当前价格通常是指最新成交价
 */
double HftMocker::stra_get_price(const char* stdCode)
{
	return _replayer->get_cur_price(stdCode);
}

/**
 * @brief 获取当前交易日期
 * @return 当前交易日期，格式YYYYMMDD
 * @details 从回放器中获取当前回测的交易日期
 *          策略可以使用此函数获取日期信息进行交易决策
 *          比如判断是否为月初、月底或特定日期
 */
uint32_t HftMocker::stra_get_date()
{
	return _replayer->get_date();
}

/**
 * @brief 获取当前交易时间
 * @return 当前交易时间，格式HHMMSS
 * @details 从回放器中获取当前回测的原始交易时间
 *          策略可以使用此函数获取时间信息进行交易决策
 *          比如判断是否在开盘、收盘或中午休市时段
 */
uint32_t HftMocker::stra_get_time()
{
	return _replayer->get_raw_time();
}

/**
 * @brief 获取当前的秒数
 * @return 当前秒数，范围0-59
 * @details 从回放器中获取当前回测的秒数
 *          与get_time函数配合使用，可以获取更精细的时间信息
 *          特别适用于高频交易中需要精确秒级时间的场景
 */
uint32_t HftMocker::stra_get_secs()
{
	return _replayer->get_secs();
}

/**
 * @brief 订阅合约的Tick数据
 * @param stdCode 标准合约代码
 * @details 订阅指定合约的Tick数据，包含两个操作：
 *          1. 将合约代码插入到本地的订阅集合中记录
 *          2. 通知回放器订阅该合约的Tick数据
 *          当收到Tick数据时，on_tick_updated函数会先检查该合约是否在订阅列表中
 *          这是策略获取实时行情数据的必要步骤
 */
void HftMocker::stra_sub_ticks(const char* stdCode)
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
 * @brief 订阅合约的委托队列数据
 * @param stdCode 标准合约代码
 * @details 订阅指定合约的委托队列数据
 *          通知回放器订阅该合约的委托队列数据
 *          委托队列数据包含各个价位的委托排队情况，在高频策略中可用于分析市场深度
 *          查看各价位的流动性和供需关系
 */
void HftMocker::stra_sub_order_queues(const char* stdCode)
{
	_replayer->sub_order_queue(_context_id, stdCode);
}

/**
 * @brief 订阅合约的逆序明细数据
 * @param stdCode 标准合约代码
 * @details 订阅指定合约的逆序明细数据
 *          通知回放器订阅该合约的逆序明细数据
 *          逆序明细包含的是交易所发布的逻辑时间排序的委托数据
 *          高频策略可以基于这些数据进行市场微观结构分析
 */
void HftMocker::stra_sub_order_details(const char* stdCode)
{
	_replayer->sub_order_detail(_context_id, stdCode);
}

/**
 * @brief 订阅合约的逐笔成交数据
 * @param stdCode 标准合约代码
 * @details 订阅指定合约的逐笔成交数据
 *          通知回放器订阅该合约的逐笔成交数据
 *          逐笔成交数据包含市场上实际成交的每笔详细信息
 *          高频策略可以基于这些数据分析市场的交易方向和活跃度
 */
void HftMocker::stra_sub_transactions(const char* stdCode)
{
	_replayer->sub_transaction(_context_id, stdCode);
}

void HftMocker::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

void HftMocker::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

void HftMocker::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

void HftMocker::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

const char* HftMocker::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	return defVal;
}

void HftMocker::stra_save_user_data(const char* key, const char* val)
{
	_user_datas[key] = val;
	_ud_modified = true;
}

void HftMocker::dump_outputs()
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


	filename = folder + "signals.csv";
	content = "time, action, position, price\n";
	content += _sig_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "positions.csv";
	content = "date,code,volume,closeprofit,dynprofit\n";
	if (!_pos_logs.str().empty()) content += _pos_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();
		for (auto it = _user_datas.begin(); it != _user_datas.end(); it++)
		{
			root.AddMember(rj::Value(it->first.c_str(), allocator), rj::Value(it->second.c_str(), allocator), allocator);
		}

		filename = folder;
		filename += "ud_";
		filename += _name;
		filename += ".json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}
}

void HftMocker::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee, const char* userTag/* = ""*/)
{
	_trade_logs << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE")
		<< "," << price << "," << qty << "," << fee << "," << userTag << "\n";
}

void HftMocker::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double maxprofit, double maxloss,
	double totalprofit /* = 0 */, const char* enterTag/* = ""*/, const char* exitTag/* = ""*/)
{
	_close_logs << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
		<< "," << closeTime << "," << closepx << "," << qty << "," << profit << "," << maxprofit << "," << maxloss << ","
		<< totalprofit << "," << enterTag << "," << exitTag << "\n";
}

/**
 * @brief 设置合约持仓量
 * @param stdCode 标准合约代码
 * @param qty 要设置的持仓量，正数表示多头持仓，负数表示空头持仓
 * @param price 成交价格，默认为0，表示使用当前行情价格
 * @param userTag 用户自定义标签，默认为空字符串
 * @details 该函数处理合约持仓变化的核心逻辑，包括开仓、平仓和持仓量调整
 *          首先获取当前合约的持仓信息和当前时间
 *          如果指定价格为0，则使用当前合约行情的价格
 *          如果新持仓量与当前持仓量相等，则直接返回
 *          根据新的持仓量和当前持仓量的差异，实现开仓、平仓或反手等操作
 *          该函数会更新持仓明细、计算交易费用并记录成交日志
 */
void HftMocker::do_set_position(const char* stdCode, double qty, double price /* = 0.0 */, const char* userTag /*= ""*/)
{
	PosInfo& pInfo = _pos_map[stdCode];
	double curPx = price;
	if (decimal::eq(price, 0.0))
		curPx = _price_map[stdCode];
	uint64_t curTm = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_min_time()*100000 + _replayer->get_secs();
	uint32_t curTDate = _replayer->get_trading_date();

	//手数相等则不用操作了
	if (decimal::eq(pInfo._volume, qty))
		return;

	log_debug("[{:04d}.{:05d}] {} position updated: {} -> {}", _replayer->get_min_time(), _replayer->get_secs(), stdCode, pInfo._volume, qty);

	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
		return;

	//成交价
	double trdPx = curPx;

	double diff = qty - pInfo._volume;
	bool isBuy = decimal::gt(diff, 0.0);
	if (decimal::gt(pInfo._volume*diff, 0))//当前持仓和仓位变化方向一致, 增加一条明细, 增加数量即可
	{
		pInfo._volume = qty;
		//如果T+1，则冻结仓位要增加
		if (commInfo->isT1())
		{
			//ASSERT(diff>0);
			pInfo._frozen += diff;
			log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
		}

		DetailInfo dInfo;
		dInfo._long = decimal::gt(qty, 0);
		dInfo._price = trdPx;
		dInfo._volume = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		strcpy(dInfo._usertag, userTag);
		pInfo._details.emplace_back(dInfo);

		double fee = _replayer->calc_fee(stdCode, trdPx, abs(diff), 0);
		_fund_info._total_fees += fee;

		log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(diff), fee, userTag);
	}
	else
	{//持仓方向和仓位变化方向不一致,需要平仓
		double left = abs(diff);

		pInfo._volume = qty;
		if (decimal::eq(pInfo._volume, 0))
			pInfo._dynprofit = 0;
		uint32_t count = 0;
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
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
				count++;

			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;
			pInfo._closeprofit += profit;
			pInfo._dynprofit = pInfo._dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放
			_fund_info._total_profit += profit;

			double fee = _replayer->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, fee, userTag);
			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, maxProf, maxLoss, pInfo._closeprofit, dInfo._usertag, userTag);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		while (count > 0)
		{
			auto it = pInfo._details.begin();
			pInfo._details.erase(it);
			count--;
		}

		//最后,如果还有剩余的,则需要反手了
		if (left > 0)
		{
			left = left * qty / abs(qty);

			//如果T+1，则冻结仓位要增加
			if (commInfo->isT1())
			{
				pInfo._frozen += left;
				log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
			}

			DetailInfo dInfo;
			dInfo._long = decimal::gt(qty, 0);
			dInfo._price = trdPx;
			dInfo._volume = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			strcpy(dInfo._usertag, userTag);
			pInfo._details.emplace_back(dInfo);

			//这里还需要写一笔成交记录
			double fee = _replayer->calc_fee(stdCode, trdPx, abs(left), 0);
			_fund_info._total_fees += fee;
			log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), fee, userTag);
		}
	}
}
