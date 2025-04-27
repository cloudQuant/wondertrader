/*!
 * \file WtHftEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 高频交易引擎实现文件
 * \details 本文件实现了WonderTrader高频交易引擎类(WtHftEngine)
 *          高频交易引擎负责管理和执行高频交易策略
 *          包括接收市场数据、处理订单和成交信息、管理策略上下文等
 */
#define WIN32_LEAN_AND_MEAN

#include "WtHftEngine.h"
#include "WtHftTicker.h"
#include "WtDtMgr.h"
#include "TraderAdapter.h"
#include "WtHelper.h"

#include "../Share/decimal.h"
#include "../Share/CodeHelper.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

USING_NS_WTP;

/**
 * @brief 高频交易引擎构造函数
 * @details 初始化高频交易引擎对象，将配置和定时器成员设置为空
 */
WtHftEngine::WtHftEngine()
	: _cfg(NULL)
	, _tm_ticker(NULL)
{
}


/**
 * @brief 高频交易引擎析构函数
 * @details 清理高频交易引擎资源，包括停止并释放定时器、释放配置对象
 */
WtHftEngine::~WtHftEngine()
{
	if (_tm_ticker)
	{
		_tm_ticker->stop();
		delete _tm_ticker;
		_tm_ticker = NULL;
	}

	if (_cfg)
		_cfg->release();
}

/**
 * @brief 初始化高频交易引擎
 * @param cfg 配置项
 * @param bdMgr 基础数据管理器
 * @param dataMgr 数据管理器
 * @param hotMgr 主力合约管理器
 * @param notifier 事件通知器，默认为空
 * @details 初始化高频交易引擎，首先调用基类的init函数，然后保存并增加配置对象的引用计数
 */
void WtHftEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier /* = NULL */)
{
	WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier);

	_cfg = cfg;
	_cfg->retain();
}

/**
 * @brief 启动高频交易引擎
 * @details 启动高频交易引擎的运行，包括以下步骤：
 *          1. 初始化所有高频策略上下文
 *          2. 创建并初始化高频实时定时器
 *          3. 将运行中的策略和交易通道信息写入marker.json文件
 *          4. 启动定时器开始运行
 */
void WtHftEngine::run()
{
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		HftContextPtr& ctx = (HftContextPtr&)it->second;
		ctx->on_init();
	}

	_tm_ticker = new WtHftRtTicker(this);
	WTSVariant* cfgProd = _cfg->get("product");
	_tm_ticker->init(_data_mgr->reader(), cfgProd->getCString("session"));

	//启动之前,先把运行中的策略落地
	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		rj::Value jStraList(rj::kArrayType);
		for (auto& m : _ctx_map)
		{
			const HftContextPtr& ctx = m.second;
			jStraList.PushBack(rj::Value(ctx->name(), allocator), allocator);
		}

		root.AddMember("marks", jStraList, allocator);

		rj::Value jChnlList(rj::kArrayType);
		for (auto& m : _adapter_mgr->getAdapters())
		{
			const TraderAdapterPtr& adapter = m.second;
			jChnlList.PushBack(rj::Value(adapter->id(), allocator), allocator);
		}

		root.AddMember("channels", jChnlList, allocator);

		root.AddMember("engine", rj::Value("HFT", allocator), allocator);

		std::string filename = WtHelper::getBaseDir();
		filename += "marker.json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}

	_tm_ticker->run();
}

/**
 * @brief 处理推送的行情数据
 * @param newTick 新的Tick数据
 * @details 将推送的行情数据转发给高频实时定时器进行处理
 *          如果定时器存在，则调用定时器的on_tick函数
 */
void WtHftEngine::handle_push_quote(WTSTickData* newTick)
{
	if (_tm_ticker)
		_tm_ticker->on_tick(newTick);
}

/**
 * @brief 处理推送的委托明细数据
 * @param curOrdDtl 当前委托明细数据
 * @details 将推送的委托明细数据分发给订阅了该合约的策略
 *          首先获取合约代码，然后查找订阅该合约的策略列表
 *          对于每个订阅的策略，调用其on_order_detail函数处理委托明细数据
 *          注意：Level2数据一般用于HFT场景，不做复权处理
 */
void WtHftEngine::handle_push_order_detail(WTSOrdDtlData* curOrdDtl)
{
	const char* stdCode = curOrdDtl->code();
	auto sit = _orddtl_sub_map.find(stdCode);
	if (sit != _orddtl_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = it->first;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				HftContextPtr& ctx = (HftContextPtr&)cit->second;
				ctx->on_order_detail(stdCode, curOrdDtl);
			}
		}
	}
}

/**
 * @brief 处理推送的委托队列数据
 * @param curOrdQue 当前委托队列数据
 * @details 将推送的委托队列数据分发给订阅了该合约的策略
 *          首先获取合约代码，然后查找订阅该合约的策略列表
 *          对于每个订阅的策略，调用其on_order_queue函数处理委托队列数据
 *          注意：Level2数据一般用于HFT场景，不做复权处理
 */
void WtHftEngine::handle_push_order_queue(WTSOrdQueData* curOrdQue)
{
	const char* stdCode = curOrdQue->code();
	auto sit = _ordque_sub_map.find(stdCode);
	if (sit != _ordque_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = it->first;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				HftContextPtr& ctx = (HftContextPtr&)cit->second;
				ctx->on_order_queue(stdCode, curOrdQue);
			}
		}
	}
}

/**
 * @brief 处理推送的成交明细数据
 * @param curTrans 当前成交明细数据
 * @details 将推送的成交明细数据分发给订阅了该合约的策略
 *          首先获取合约代码，然后查找订阅该合约的策略列表
 *          对于每个订阅的策略，调用其on_transaction函数处理成交明细数据
 *          注意：Level2数据一般用于HFT场景，不做复权处理
 */
void WtHftEngine::handle_push_transaction(WTSTransData* curTrans)
{
	const char* stdCode = curTrans->code();
	auto sit = _trans_sub_map.find(stdCode);
	if (sit != _trans_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = it->first;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				HftContextPtr& ctx = (HftContextPtr&)cit->second;
				ctx->on_transaction(stdCode, curTrans);
			}
		}
	}
}

/**
 * @brief 订阅委托明细数据
 * @param sid 策略ID
 * @param stdCode 标准合约代码
 * @details 为指定策略订阅指定合约的委托明细数据
 *          首先处理合约代码，如果带有复权后缀（+或-），则去除后缀
 *          然后将策略ID添加到该合约的订阅列表中，订阅标记设置为0（无复权）
 */
void WtHftEngine::sub_order_detail(uint32_t sid, const char* stdCode)
{
	std::size_t length = strlen(stdCode);
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		length--;

	SubList& sids = _orddtl_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, 0);
}

/**
 * @brief 订阅委托队列数据
 * @param sid 策略ID
 * @param stdCode 标准合约代码
 * @details 为指定策略订阅指定合约的委托队列数据
 *          首先处理合约代码，如果带有复权后缀（+或-），则去除后缀
 *          然后将策略ID添加到该合约的订阅列表中，订阅标记设置为0（无复权）
 */
void WtHftEngine::sub_order_queue(uint32_t sid, const char* stdCode)
{
	std::size_t length = strlen(stdCode);
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		length--;

	SubList& sids = _ordque_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, 0);
}

/**
 * @brief 订阅成交明细数据
 * @param sid 策略ID
 * @param stdCode 标准合约代码
 * @details 为指定策略订阅指定合约的成交明细数据
 *          首先处理合约代码，如果带有复权后缀（+或-），则去除后缀
 *          然后将策略ID添加到该合约的订阅列表中，订阅标记设置为0（无复权）
 */
void WtHftEngine::sub_transaction(uint32_t sid, const char* stdCode)
{
	std::size_t length = strlen(stdCode);
	if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		length--;

	SubList& sids = _trans_sub_map[std::string(stdCode, length)];
	sids[sid] = std::make_pair(sid, 0);
}

/**
 * @brief Tick数据回调函数
 * @param stdCode 标准合约代码
 * @param curTick 当前Tick数据
 * @details 当新的Tick数据到达时调用，处理并分发给相关策略
 *          首先调用基类的on_tick函数，然后将数据推送给数据管理器
 *          最后根据订阅情况将数据分发给相关策略，并处理不同的复权模式
 */
void WtHftEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	WtEngine::on_tick(stdCode, curTick);

	_data_mgr->handle_push_quote(stdCode, curTick);

	/*
	 *	By Wesley @ 2022.02.07
	 *	这里做了一个彻底的调整
	 *	第一，检查订阅标记，如果标记为0，即无复权模式，则直接按照原始代码触发ontick
	 *	第二，如果标记为1，即前复权模式，则将代码转成xxxx-，再触发ontick
	 *	第三，如果标记为2，即后复权模式，则将代码转成xxxx+，再把tick数据做一个修正，再触发ontick
	 */
	if(_ready)
	{
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			const SubList& sids = sit->second;
			for (auto it = sids.begin(); it != sids.end(); it++)
			{
				uint32_t sid = it->first;

				auto cit = _ctx_map.find(sid);
				if (cit != _ctx_map.end())
				{
					HftContextPtr& ctx = (HftContextPtr&)cit->second;
					uint32_t opt = it->second.second;

					if (opt == 0)
					{
						ctx->on_tick(stdCode, curTick);
					}
					else
					{
						std::string wCode = stdCode;
						wCode = fmt::format("{}{}", stdCode, opt == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
						if (opt == 1)
						{
							ctx->on_tick(wCode.c_str(), curTick);
						}
						else //(opt == 2)
						{
							WTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());
							WTSTickStruct& newTS = newTick->getTickStruct();
							newTick->setContractInfo(curTick->getContractInfo());

							//这里做一个复权因子的处理
							double factor = get_exright_factor(stdCode, curTick->getContractInfo()->getCommInfo());
							newTS.open *= factor;
							newTS.high *= factor;
							newTS.low *= factor;
							newTS.price *= factor;

							_price_map[wCode] = newTS.price;

							ctx->on_tick(wCode.c_str(), newTick);
							newTick->release();
						}
					}
				}
			}
		}
	}
}

/**
 * @brief K线数据回调函数
 * @param stdCode 标准合约代码
 * @param period 周期标识，如"m1"表示1分钟，"d1"表示日线
 * @param times 周期倍数
 * @param newBar 新的K线数据
 * @details 当新的K线数据生成时调用，处理并分发给订阅了该K线的策略
 *          首先根据合约代码、周期和倍数生成唯一的订阅键
 *          然后遍历订阅了该K线的策略，调用其on_bar函数处理K线数据
 */
void WtHftEngine::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}-{}-{}", stdCode, period, times);

	const SubList& sids = _bar_sub_map[key];
	for (auto it = sids.begin(); it != sids.end(); it++)
	{
		uint32_t sid = it->first;
		auto cit = _ctx_map.find(sid);
		if (cit != _ctx_map.end())
		{
			HftContextPtr& ctx = (HftContextPtr&)cit->second;
			ctx->on_bar(stdCode, period, times, newBar);
		}
	}
}

/**
 * @brief 交易日开始回调函数
 * @details 在每个交易日开始时调用，执行交易日开始时的相关操作
 *          1. 记录交易日开始日志
 *          2. 调用基类的on_session_begin函数
 *          3. 通知所有策略上下文交易日开始
 *          4. 通知事件监听器交易日开始事件
 *          5. 将引擎状态设置为就绪
 */
void WtHftEngine::on_session_begin()
{
	WTSLogger::info("Trading day {} begun", _cur_tdate);
	WtEngine::on_session_begin();

	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		HftContextPtr& ctx = (HftContextPtr&)it->second;
		ctx->on_session_begin(_cur_tdate);
	}

	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, true);

	_ready = true;
}

/**
 * @brief 交易日结束回调函数
 * @details 在每个交易日结束时调用，执行交易日结束时的相关操作
 *          1. 调用基类的on_session_end函数
 *          2. 通知所有策略上下文交易日结束
 *          3. 记录交易日结束日志
 *          4. 通知事件监听器交易日结束事件
 */
void WtHftEngine::on_session_end()
{
	WtEngine::on_session_end();

	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		HftContextPtr& ctx = (HftContextPtr&)it->second;
		ctx->on_session_end(_cur_tdate);
	}

	WTSLogger::info("Trading day {} ended", _cur_tdate);
	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, false);
}

/**
 * @brief 分钟结束回调函数
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMMSS或HHMMSS000
 * @details 在每分钟结束时调用，执行分钟结束时的相关操作
 *          注意：当前版本已去掉高频策略的on_schedule调用，实际上这个函数不执行任何操作
 */
void WtHftEngine::on_minute_end(uint32_t curDate, uint32_t curTime)
{
	//已去掉高频策略的on_schedule
	//for(auto& cit : _ctx_map)
	//{
	//	HftContextPtr& ctx = cit.second;
	//	ctx->on_schedule(curDate, curTime);
	//}
}

/**
 * @brief 添加高频策略上下文
 * @param ctx 高频策略上下文指针
 * @details 将高频策略上下文添加到引擎中进行管理
 *          首先获取策略上下文的ID，然后将其添加到策略映射表中
 */
void WtHftEngine::addContext(HftContextPtr ctx)
{
	uint32_t sid = ctx->id();
	_ctx_map[sid] = ctx;
}

/**
 * @brief 获取高频策略上下文
 * @param id 策略ID
 * @return HftContextPtr 高频策略上下文指针
 * @details 根据策略ID获取对应的高频策略上下文
 *          如果策略ID不存在，则返回空指针
 */
HftContextPtr WtHftEngine::getContext(uint32_t id)
{
	auto it = _ctx_map.find(id);
	if (it == _ctx_map.end())
		return HftContextPtr();

	return it->second;
}

WTSOrdQueSlice* WtHftEngine::get_order_queue_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_queue_slice(code, count);
}

WTSOrdDtlSlice* WtHftEngine::get_order_detail_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_detail_slice(code, count);
}

WTSTransSlice* WtHftEngine::get_transaction_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_transaction_slice(code, count);
}