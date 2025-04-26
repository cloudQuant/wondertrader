/*!
 * \file WtCtaEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 * 
 * WtCtaEngine类的实现文件，实现了CTA策略引擎的各项功能
 * CTA（Commodity Trading Advisor）策略引擎是WonderTrader框架中的核心组件之一
 * 负责协调多个CTA策略的运行，处理行情数据和交易信号
 * 实现了定时器、交易执行等功能
 */
#define WIN32_LEAN_AND_MEAN

#include "WtCtaEngine.h"
#include "WtDtMgr.h"
#include "WtCtaTicker.h"
#include "WtHelper.h"
#include "TraderAdapter.h"
#include "EventNotifier.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSRiskDef.hpp"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

/**
 * @brief WtCtaEngine类的构造函数
 * 
 * 初始化CTA策略引擎，将定时器指针设置为NULL
 */
WtCtaEngine::WtCtaEngine()
	: _tm_ticker(NULL)
{
	
}

/**
 * @brief WtCtaEngine类的析构函数
 * 
 * 清理CTA策略引擎的资源，包括：
 * 1. 释放定时器资源
 * 2. 释放配置对象资源
 */
WtCtaEngine::~WtCtaEngine()
{
	if (_tm_ticker)
	{
		delete _tm_ticker;
		_tm_ticker = NULL;
	}

	if (_cfg)
		_cfg->release();
}

/**
 * @brief 启动CTA策略引擎
 * 
 * 该方法完成以下工作：
 * 1. 创建并初始化CTA实时定时器
 * 2. 将运行中的策略信息保存到marker.json文件中
 * 3. 启动定时器
 * 4. 启动风控监控器（如果配置了的话）
 */
void WtCtaEngine::run()
{
	_tm_ticker = new WtCtaRtTicker(this);
	WTSVariant* cfgProd = _cfg->get("product");
	_tm_ticker->init(_data_mgr->reader(), cfgProd->getCString("session"));

	//启动之前,先把运行中的策略落地
	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		rj::Value jStraList(rj::kArrayType);
		for (auto& m : _ctx_map)
		{
			const CtaContextPtr& ctx = m.second;
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

		rj::Value jExecList(rj::kArrayType);
		_exec_mgr.enum_executer([&jExecList, &allocator](ExecCmdPtr executer) {
			if(executer)
				jExecList.PushBack(rj::Value(executer->name(), allocator), allocator);
		});

		root.AddMember("executers", jExecList, allocator);

		root.AddMember("engine", rj::Value("CTA", allocator), allocator);

		std::string filename = WtHelper::getBaseDir();
		filename += "marker.json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}

	_tm_ticker->run();

	if (_risk_mon)
		_risk_mon->self()->run();
}

/**
 * @brief 初始化CTA策略引擎
 * 
 * 该方法完成以下工作：
 * 1. 调用父类WtEngine的init方法初始化基本组件
 * 2. 保存配置对象
 * 3. 设置过滤器管理器
 * 4. 根据配置创建线程池
 * 
 * @param cfg 配置对象
 * @param bdMgr 基础数据管理器
 * @param dataMgr 数据管理器
 * @param hotMgr 主力合约管理器
 * @param notifier 事件通知器，默认为NULL
 */
void WtCtaEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier /* = NULL */)
{
	WtEngine::init(cfg, bdMgr, dataMgr, hotMgr, notifier);

	_cfg = cfg;
	_cfg->retain();

	_exec_mgr.set_filter_mgr(&_filter_mgr);

	uint32_t poolsize = cfg->getUInt32("poolsize");
	if (poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}
	WTSLogger::info("Engine task poolsize is {}", poolsize);
}

/**
 * @brief 添加策略上下文
 * 
 * 将策略上下文添加到策略映射表中，以便引擎管理和调度
 * 
 * @param ctx 策略上下文指针
 */
void WtCtaEngine::addContext(CtaContextPtr ctx)
{
	uint32_t sid = ctx->id();
	_ctx_map[sid] = ctx;
}

/**
 * @brief 获取策略上下文
 * 
 * 根据策略ID从策略映射表中获取对应的策略上下文
 * 
 * @param id 策略ID
 * @return CtaContextPtr 策略上下文指针，如果未找到则返回空指针
 */
CtaContextPtr WtCtaEngine::getContext(uint32_t id)
{
	auto it = _ctx_map.find(id);
	if (it == _ctx_map.end())
		return CtaContextPtr();

	return it->second;
}

/**
 * @brief 引擎初始化回调函数
 * 
 * 该方法在引擎初始化时被调用，主要完成以下工作：
 * 1. 清除缓存的目标仓位
 * 2. 调用每个策略上下文的初始化方法
 * 3. 处理策略的初始持仓，并应用过滤器
 * 4. 将初始持仓信息传递给执行器
 */
void WtCtaEngine::on_init()
{
	//wt_hashmap<std::string, double> target_pos;
	_exec_mgr.clear_cached_targets();
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		CtaContextPtr& ctx = (CtaContextPtr&)it->second;
		ctx->on_init();

		const auto& exec_ids = _exec_mgr.get_route(ctx->name());

		ctx->enum_position([this, ctx, exec_ids](const char* stdCode, double qty){

			double oldQty = qty;
			bool bFilterd = _filter_mgr.is_filtered_by_strategy(ctx->name(), qty);
			if (!bFilterd)
			{
				if (!decimal::eq(qty, oldQty))
				{
					//输出日志
					WTSLogger::info("[Filters] Target position of {} of strategy {} reset by strategy filter: {} -> {}", 
						stdCode, ctx->name(), oldQty, qty);
				}

				std::string realCode = stdCode;
				CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
				if(strlen(cInfo._ruletag) > 0)
				{
					std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
					realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
				}

				for(auto& execid : exec_ids)
					_exec_mgr.add_target_to_cache(realCode.c_str(), qty, execid.c_str());
			}
			else
			{
				//输出日志
				WTSLogger::info("[Filters] Target position of {} of strategy {} ignored by strategy filter", stdCode, ctx->name());
			}
		}, true);
	}

	bool bRiskEnabled = false;
	if (!decimal::eq(_risk_volscale, 1.0) && _risk_date == _cur_tdate)
	{
		WTSLogger::log_by_cat("risk", LL_INFO, "Risk scale of portfolio is {:.2f}", _risk_volscale);
		bRiskEnabled = true;
	}

	////初始化仓位打印出来
	//for (auto it = target_pos.begin(); it != target_pos.end(); it++)
	//{
	//	const auto& stdCode = it->first;
	//	double& pos = (double&)it->second;

	//	if (bRiskEnabled && !decimal::eq(pos, 0))
	//	{
	//		double symbol = pos / abs(pos);
	//		pos = decimal::rnd(abs(pos)*_risk_volscale)*symbol;
	//	}

	//	WTSLogger::info("Portfolio initial position of {} is {}", stdCode.c_str(), pos);
	//}

	_exec_mgr.commit_cached_targets(bRiskEnabled?_risk_volscale:1.0);

	if (_evt_listener)
		_evt_listener->on_initialize_event();
}

/**
 * @brief 交易日开始回调函数
 * 
 * 该方法在每个交易日开始时被调用，主要完成以下工作：
 * 1. 记录交易日开始的日志
 * 2. 遍历所有策略上下文，调用其on_session_begin方法
 * 3. 触发交易日开始事件
 * 4. 将引擎标记为就绪状态
 */
void WtCtaEngine::on_session_begin()
{
	WTSLogger::info("Trading day {} begun", _cur_tdate);
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		CtaContextPtr& ctx = (CtaContextPtr&)it->second;
		ctx->on_session_begin(_cur_tdate);
	}

	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, true);

	_ready = true;
}

/**
 * @brief 交易日结束回调函数
 * 
 * 该方法在每个交易日结束时被调用，主要完成以下工作：
 * 1. 调用父类的on_session_end方法
 * 2. 遍历所有策略上下文，调用其on_session_end方法
 * 3. 记录交易日结束日志
 * 4. 触发交易日结束事件
 */
void WtCtaEngine::on_session_end()
{
	WtEngine::on_session_end();

	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		CtaContextPtr& ctx = (CtaContextPtr&)it->second;
		ctx->on_session_end(_cur_tdate);
	}

	WTSLogger::info("Trading day {} ended", _cur_tdate);
	if (_evt_listener)
		_evt_listener->on_session_event(_cur_tdate, false);
}

/**
 * @brief 定时调度回调函数
 * 
 * 该方法由定时器定期调用，是策略运行的核心驱动函数，主要完成以下工作：
 * 1. 加载过滤器并清除缓存的目标仓位
 * 2. 调用策略的on_schedule方法进行策略计算
 * 3. 收集各策略的目标仓位并应用过滤器
 * 4. 处理风控检查和仓位调整
 * 5. 将目标仓位传递给执行器
 * 
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS
 */
void WtCtaEngine::on_schedule(uint32_t curDate, uint32_t curTime)
{
	//去检查一下过滤器
	_filter_mgr.load_filters();
	_exec_mgr.clear_cached_targets();
	wt_hashmap<std::string, double> target_pos;
	if(_pool)
	{
		/*
		 *	By Wesley @ 2023.06.27
		 *	如果通过线程池并发
		 *	先并发所有的on_schedule
		 *	然后再wait所有任务结束
		 *	最后再统一读取全部持仓
		 */
		for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
		{
			CtaContextPtr& ctx = (CtaContextPtr&)it->second;
			_pool->schedule([ctx, curDate, curTime] (){
				ctx->on_schedule(curDate, curTime);
			});
		}

		/*
		 *	By Wesley @ 2023.06.27
		 *	等待全部on_schedule执行完成
		 */
		_pool->wait();
		
		for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
		{
			CtaContextPtr& ctx = (CtaContextPtr&)it->second;
			const auto& exec_ids = _exec_mgr.get_route(ctx->name());
			ctx->enum_position([this, ctx, exec_ids, &target_pos](const char* stdCode, double qty) {

				double oldQty = qty;
				bool bFilterd = _filter_mgr.is_filtered_by_strategy(ctx->name(), qty);
				if (!bFilterd)
				{
					if (!decimal::eq(qty, oldQty))
					{
						//输出日志
						WTSLogger::info("[Filters] Target position of {} of strategy {} reset by strategy filter: {} -> {}",
							stdCode, ctx->name(), oldQty, qty);
					}

					std::string realCode = stdCode;
					CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
					if (strlen(cInfo._ruletag) > 0)
					{
						std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
						realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
					}

					double& vol = target_pos[realCode];
					vol += qty;
					for (auto& execid : exec_ids)
						_exec_mgr.add_target_to_cache(realCode.c_str(), qty, execid.c_str());
				}
				else
				{
					//输出日志
					WTSLogger::info("[Filters] Target position of {} of strategy {} ignored by strategy filter", stdCode, ctx->name());
				}
			}, true);
		}
	}
	else
	{
		for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
		{
			CtaContextPtr& ctx = (CtaContextPtr&)it->second;
			ctx->on_schedule(curDate, curTime);
			const auto& exec_ids = _exec_mgr.get_route(ctx->name());
			ctx->enum_position([this, ctx, exec_ids, &target_pos](const char* stdCode, double qty) {

				double oldQty = qty;
				bool bFilterd = _filter_mgr.is_filtered_by_strategy(ctx->name(), qty);
				if (!bFilterd)
				{
					if (!decimal::eq(qty, oldQty))
					{
						//输出日志
						WTSLogger::info("[Filters] Target position of {} of strategy {} reset by strategy filter: {} -> {}",
							stdCode, ctx->name(), oldQty, qty);
					}

					std::string realCode = stdCode;
					CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
					if (strlen(cInfo._ruletag) > 0)
					{
						std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
						realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
					}

					double& vol = target_pos[realCode];
					vol += qty;
					for (auto& execid : exec_ids)
						_exec_mgr.add_target_to_cache(realCode.c_str(), qty, execid.c_str());
				}
				else
				{
					//输出日志
					WTSLogger::info("[Filters] Target position of {} of strategy {} ignored by strategy filter", stdCode, ctx->name());
				}
			}, true);
		}
	}
	

	bool bRiskEnabled = false;
	if(!decimal::eq(_risk_volscale, 1.0) && _risk_date == _cur_tdate)
	{
		WTSLogger::log_by_cat("risk", LL_INFO, "Risk scale of strategy group is {:.2f}", _risk_volscale);
		bRiskEnabled = true;
	}

	//处理组合理论部位
	for (auto it = target_pos.begin(); it != target_pos.end(); it++)
	{
		const auto& stdCode = it->first;
		double& pos = (double&)it->second;

		if (bRiskEnabled && !decimal::eq(pos, 0))
		{
			double symbol = pos / abs(pos);
			pos = decimal::rnd(abs(pos)*_risk_volscale)*symbol;
		}

		append_signal(stdCode.c_str(), pos, true);
	}

	for(auto& m : _pos_map)
	{
		const auto& stdCode = m.first;
		if (target_pos.find(stdCode) == target_pos.end())
		{
			if(!decimal::eq(m.second->_volume, 0))
			{
				//这里是通知WtEngine去更新组合持仓数据
				append_signal(stdCode.c_str(), 0, true);

				WTSLogger::error("Instrument {} not in target positions, setup to 0 automatically", stdCode.c_str());
			}

			//因为组合持仓里会有过期的合约代码存在，所以这里在丢给执行以前要做一个检查
			auto cInfo = get_contract_info(stdCode.c_str());
			if (cInfo != NULL)
			{
				//target_pos[stdCode] = 0;
				_exec_mgr.add_target_to_cache(stdCode.c_str(), 0);
			}
		}
	}

	push_task([this](){
		update_fund_dynprofit();
		/*
		 *	By Wesley @ 2023.01.30
		 *	增加一个定时刷新交易账号资金的入口
		 */
		_adapter_mgr->refresh_funds();
	});

	//_exec_mgr.set_positions(target_pos);
	_exec_mgr.commit_cached_targets(bRiskEnabled ? _risk_volscale : 1);

	save_datas();

	if (_evt_listener)
		_evt_listener->on_schedule_event(curDate, curTime);
}

/**
 * @brief 处理推送的行情数据
 * 
 * 该方法接收外部推送的行情数据，并将其转发给定时器的on_tick方法处理
 * 
 * @param newTick 新的行情tick数据
 */
void WtCtaEngine::handle_push_quote(WTSTickData* newTick)
{
	if (_tm_ticker)
		_tm_ticker->on_tick(newTick);
}

/**
 * @brief 处理仓位变化
 * 
 * 该方法处理策略仓位的变化，主要完成以下工作：
 * 1. 应用仓位过滤器
 * 2. 处理主力合约换月
 * 3. 计算目标仓位
 * 4. 应用风控比例
 * 5. 更新信号并保存数据
 * 6. 将目标仓位传递给执行器
 * 
 * @param straName 策略名称
 * @param stdCode 标准化合约代码
 * @param diffPos 仓位变化量
 */
void WtCtaEngine::handle_pos_change(const char* straName, const char* stdCode, double diffPos)
{
	//这里是持仓增量,所以不用处理未过滤的情况,因为增量情况下,不会改变目标diffQty
	if(_filter_mgr.is_filtered_by_strategy(straName, diffPos, true))
	{
		//输出日志
		WTSLogger::info("[Filters] Target position of {} of strategy {} ignored by strategy filter", stdCode, straName);
		return;
	}

	std::string realCode = stdCode;
	//const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (strlen(cInfo._ruletag) > 0)
	{
		std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
		realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	}

	/*
	 *	这里必须要算一个总的目标仓位
	 */
	PosInfoPtr& pInfo = _pos_map[realCode];	
	if (pInfo == NULL)
		pInfo.reset(new PosInfo);

	bool bRiskEnabled = false;
	if (!decimal::eq(_risk_volscale, 1.0) && _risk_date == _cur_tdate)
	{
		WTSLogger::log_by_cat("risk", LL_INFO, "Risk scale of portfolio is {:.2f}", _risk_volscale);
		bRiskEnabled = true;
	}

	if (bRiskEnabled && !decimal::eq(diffPos, 0))
	{
		double symbol = diffPos / abs(diffPos);
		diffPos = decimal::rnd(abs(diffPos)*_risk_volscale)*symbol;
	}

	double targetPos = pInfo->_volume + diffPos;

	append_signal(realCode.c_str(), targetPos, false);
	save_datas();

	/*
	 *	如果策略绑定了执行通道
	 *	那么就只提交增量
	 *	如果策略没有绑定执行通道，就提交全量
	 */
	const auto& exec_ids = _exec_mgr.get_route(straName);
	for(auto& execid : exec_ids)
		_exec_mgr.handle_pos_change(realCode.c_str(), targetPos, diffPos, execid.c_str());
}

/**
 * @brief 处理tick数据
 * 
 * 该方法接收外部推送的tick数据，主要完成以下工作：
 * 1. 调用父类的on_tick方法
 * 2. 将tick数据传递给数据管理器
 * 3. 将tick数据传递给执行器
 * 4. 根据复权模式处理tick数据
 * 5. 将处理后的tick数据分发给订阅的策略
 * 
 * @param stdCode 标准化合约代码
 * @param curTick 新的tick数据
 */
void WtCtaEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	WtEngine::on_tick(stdCode, curTick);

	_data_mgr->handle_push_quote(stdCode, curTick);

	//如果是真实代码, 则要传递给执行器
	/*
	 *	这里不再做判断，直接全部传递给执行器管理器，因为执行器可能会处理未订阅的合约
	 *	主要场景为主力合约换月期间
	 *	By Wesley @ 2021.08.19
	 */
	{
		//是否主力合约代码的标记, 主要用于给执行器发数据的
		_exec_mgr.handle_tick(stdCode, curTick);
	}

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
		if (sit == _tick_sub_map.end())
			return;

		uint32_t flag = get_adjusting_flag();
		WTSTickData* adjTick = nullptr;

		//By Wesley
		//这里做一个拷贝，虽然有点开销，但是可以规避掉一些问题，比如ontick的时候订阅tick
		SubList sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			uint32_t sid = it->first;
				

			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				CtaContextPtr& ctx = (CtaContextPtr&)cit->second;
				uint32_t opt = it->second.second;
					
				if (opt == 0)
				{
					/*
					 *	By Wesley @ 2023.06.27
					 *	如果使用线程池，则到线程池里去调度
					 */
					if(_pool)
					{
						_pool->schedule([ctx, stdCode, curTick]() {
							ctx->on_tick(stdCode, curTick);
						});
					}
					else
						ctx->on_tick(stdCode, curTick);
				}
				else
				{
					std::string wCode = stdCode;
					wCode = fmt::format("{}{}", stdCode, opt == 1 ? SUFFIX_QFQ : SUFFIX_HFQ);
					if (opt == 1)
					{
						if (_pool)
						{
							_pool->schedule([ctx, wCode, curTick]() {
								ctx->on_tick(wCode.c_str(), curTick);
							});
						}
						else
							ctx->on_tick(wCode.c_str(), curTick);
					}
					else //(opt == 2)
					{
						if (adjTick == nullptr)
						{
							WTSTickData* adjTick = WTSTickData::create(curTick->getTickStruct());
							WTSTickStruct& adjTS = adjTick->getTickStruct();
							adjTick->setContractInfo(curTick->getContractInfo());

							//这里做一个复权因子的处理
							double factor = get_exright_factor(stdCode);
							adjTS.open *= factor;
							adjTS.high *= factor;
							adjTS.low *= factor;
							adjTS.price *= factor;

							adjTS.settle_price *= factor;

							adjTS.pre_close *= factor;
							adjTS.pre_settle *= factor;

							/*
							 *	By Wesley @ 2022.08.15
							 *	这里对tick的复权做一个完善
							 */
							if (flag & 1)
							{
								adjTS.total_volume /= factor;
								adjTS.volume /= factor;
							}

							if (flag & 2)
							{
								adjTS.total_turnover *= factor;
								adjTS.turn_over *= factor;
							}

							if (flag & 4)
							{
								adjTS.open_interest /= factor;
								adjTS.diff_interest /= factor;
								adjTS.pre_interest /= factor;
							}

							_price_map[wCode] = adjTS.price;
						}

						if (_pool)
						{
							_pool->schedule([ctx, wCode, adjTick]() {
								ctx->on_tick(wCode.c_str(), adjTick);
							});
						}
						else
							ctx->on_tick(wCode.c_str(), adjTick);

					}
				}
			}				
		}

		if(nullptr != adjTick)
			adjTick->release();
		/*
		 *	By Wesley @ 223.06.27
		 *	这里一定要等待线程池全部调度完成
		 */
		if (_pool)
			_pool->wait();
	}
	
}

/**
 * @brief 处理K线闭合事件
 * 
 * 该方法接收外部推送的K线闭合事件，并将其转发给订阅的策略上下文
 * 
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1、m5、d1等
 * @param times K线倍数
 * @param newBar 新的K线数据
 */
void WtCtaEngine::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}-{}-{}", stdCode, period, times);

	const SubList& sids = _bar_sub_map[key];
	for (auto it = sids.begin(); it != sids.end(); it++)
	{
		uint32_t sid = it->first;
		auto cit = _ctx_map.find(sid);
		if(cit != _ctx_map.end())
		{
			CtaContextPtr& ctx = (CtaContextPtr&)cit->second;
			if (_pool)
			{
				_pool->schedule([ctx, stdCode, period, times, newBar]() {
					ctx->on_bar(stdCode, period, times, newBar);
				});
			}
			else
				ctx->on_bar(stdCode, period, times, newBar);
		}
	}

	/*
	 *	By Wesley @ 223.06.27
	 *	这里一定要等待线程池全部调度完成
	 */
	if (_pool)
		_pool->wait();

	WTSLogger::info("KBar [{}] @ {} closed", key, period[0] == 'd' ? newBar->date : newBar->time);
}

/**
 * @brief 检查当前是否在交易时段内
 * 
 * 该方法通过调用定时器的is_in_trading方法来判断当前是否处于交易时段
 * 
 * @return true 如果当前在交易时段内
 * @return false 如果当前不在交易时段内
 */
bool WtCtaEngine::isInTrading()
{
	return _tm_ticker->is_in_trading();
}

/**
 * @brief 将时间转换为分钟数
 * 
 * 该方法通过调用定时器的time_to_mins方法来将时间转换为分钟数
 * 
 * @param uTime 时间，格式为HHMMSS
 * @return uint32_t 转换后的分钟数
 */
uint32_t WtCtaEngine::transTimeToMin(uint32_t uTime)
{
	return _tm_ticker->time_to_mins(uTime);
}

/**
 * @brief 获取品种信息
 * 
 * 根据标准化合约代码获取对应的品种信息
 * 
 * @param stdCode 标准化合约代码
 * @return WTSCommodityInfo* 品种信息指针
 */
WTSCommodityInfo* WtCtaEngine::get_comm_info(const char* stdCode)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	return _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
}

/**
 * @brief 获取交易时段信息
 * 
 * 根据标准化合约代码获取对应的交易时段信息
 * 
 * @param stdCode 标准化合约代码
 * @return WTSSessionInfo* 交易时段信息指针，如果未找到对应的品种信息则返回NULL
 */
WTSSessionInfo* WtCtaEngine::get_sess_info(const char* stdCode)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return cInfo->getSessionInfo();
}

/**
 * @brief 获取当前的实时时间戳
 * 
 * 根据当前日期和时间生成一个64位的时间戳
 * 
 * @return uint64_t 64位时间戳
 */
uint64_t WtCtaEngine::get_real_time()
{
	return TimeUtils::makeTime(_cur_date, _cur_raw_time * 100000 + _cur_secs);
}

/**
 * @brief 通知图表标记
 * 
 * 通过事件通知器发送图表标记信息，用于在图表上显示特定的标记
 * 
 * @param time 时间戳
 * @param straId 策略ID
 * @param price 价格
 * @param icon 图标
 * @param tag 标签
 */
void WtCtaEngine::notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag)
{
	if (_notifier)
		_notifier->notify_chart_marker(time, straId, price, icon, tag);
}

/**
 * @brief 通知图表指标
 * 
 * 通过事件通知器发送图表指标信息，用于在图表上绘制指标线
 * 
 * @param time 时间戳
 * @param straId 策略ID
 * @param idxName 指标名称
 * @param lineName 线条名称
 * @param val 指标值
 */
void WtCtaEngine::notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val)
{
	if (_notifier)
		_notifier->notify_chart_index(time, straId, idxName, lineName, val);
}

/**
 * @brief 通知交易信息
 * 
 * 通过事件通知器发送交易信息，用于记录和显示交易执行情况
 * 
 * @param straId 策略ID
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头，true为多头，false为空头
 * @param isOpen 是否为开仓，true为开仓，false为平仓
 * @param curTime 当前时间戳
 * @param price 成交价格
 * @param userTag 用户标签
 */
void WtCtaEngine::notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag)
{
	if (_notifier)
		_notifier->notify_trade(straId, stdCode, isLong, isOpen, curTime, price, userTag);
}