/*!
 * \file WtArbiExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 套利交易执行器实现
 * \details 该文件实现了套利交易执行器，用于执行多个相关合约的组合交易策略
 */
#include "WtArbiExecuter.h"
#include "TraderAdapter.h"
#include "WtEngine.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;


/*!
 * \brief 构造函数
 * \details 初始化套利交易执行器对象，设置基本参数和初始状态
 * \param factory 执行器工厂指针，用于创建执行单元
 * \param name 执行器名称
 * \param dataMgr 数据管理器指针，用于获取市场数据
 */
WtArbiExecuter::WtArbiExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr)
	: IExecCommand(name)
	, _factory(factory)
	, _data_mgr(dataMgr)
	, _channel_ready(false)
	, _scale(1.0)
	, _auto_clear(true)
	, _trader(NULL)
{
}


/*!
 * \brief 析构函数
 * \details 清理套利交易执行器资源，等待线程池中的任务完成
 */
WtArbiExecuter::~WtArbiExecuter()
{
	if (_pool)
		_pool->wait();
}

/*!
 * \brief 设置交易适配器
 * \details 设置交易适配器并读取其当前状态以更新通道就绪状态
 * \param adapter 交易适配器指针
 */
void WtArbiExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
	//设置的时候读取一下trader的状态
	if(_trader)
		_channel_ready = _trader->isReady();
}

/*!
 * \brief 初始化执行器
 * \details 根据传入的参数配置初始化套利执行器，设置缩放倍数、自动清理策略、线程池和合约组
 * \param params 初始化参数，包含执行器的各种配置
 * \return 是否初始化成功
 */
bool WtArbiExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getDouble("scale");
	_strict_sync  = params->getBoolean("strict_sync");

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	/*
	 *	By Wesley @ 2021.12.14
	 *	从配置文件中读取自动清理的策略
	 *	active: 是否启用
	 *	includes: 包含列表，格式如CFFEX.IF
	 *	excludes: 排除列表，格式如CFFEX.IF
	 */
	WTSVariant* cfgClear = params->get("clear");
	if(cfgClear)
	{
		_auto_clear = cfgClear->getBoolean("active");
		WTSVariant* cfgItem = cfgClear->get("includes");
		if(cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
				_clear_includes.insert(cfgItem->asCString());
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for(uint32_t i = 0; i < cfgItem->size(); i++)
					_clear_includes.insert(cfgItem->get(i)->asCString());
			}
		}

		cfgItem = cfgClear->get("excludes");
		if (cfgItem)
		{
			if (cfgItem->type() == WTSVariant::VT_String)
				_clear_excludes.insert(cfgItem->asCString());
			else if (cfgItem->type() == WTSVariant::VT_Array)
			{
				for (uint32_t i = 0; i < cfgItem->size(); i++)
					_clear_excludes.insert(cfgItem->get(i)->asCString());
			}
		}
	}

	WTSVariant* cfgGroups = params->get("groups");
	if (cfgGroups)
	{
		auto names = cfgGroups->memberNames();
		for(const std::string& gpname : names)
		{
			CodeGroupPtr& gpInfo = _groups[gpname];
			if (gpInfo == NULL)
			{
				gpInfo.reset(new CodeGroup);
				wt_strcpy(gpInfo->_name, gpname.c_str(), gpname.size());
			}

			WTSVariant* cfgGrp = cfgGroups->get(gpname.c_str());
			auto codes = cfgGrp->memberNames();
			for(const std::string& code : codes)
			{
				gpInfo->_items[code] = cfgGrp->getDouble(code.c_str());
				_code_to_groups[code] = gpInfo;
			}
		}
	}

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Local executer inited, scale: {}, auto_clear: {}, strict_sync: {}, thread poolsize: {}, code_groups: {}",
		_scale, _auto_clear, _strict_sync, poolsize, _groups.size());

	return true;
}

/*! 
 * \brief 获取执行单元
 * \details 根据标准合约代码获取执行单元，如果单元不存在且允许自动创建，则创建新的执行单元
 * \param stdCode 标准合约代码
 * \param bAutoCreate 是否自动创建，默认为true
 * \return 执行单元指针
 */
ExecuteUnitPtr WtArbiExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	std::string commID = codeInfo.stdCommID();

	WTSVariant* policy = _config->get("policy");
	std::string des = commID;
	if (!policy->has(commID.c_str()))
		des = "default";

	SpinLock lock(_mtx_units);

	auto it = _unit_map.find(stdCode);
	if(it != _unit_map.end())
	{
		return it->second;
	}

	if (bAutoCreate)
	{
		WTSVariant* cfg = policy->get(des.c_str());

		const char* name = cfg->getCString("name");
		ExecuteUnitPtr unit = _factory->createExeUnit(name);
		if (unit != NULL)
		{
			_unit_map[stdCode] = unit;
			unit->self()->init(this, stdCode, cfg);

			//如果通道已经就绪，则直接通知执行单元
			if (_channel_ready)
				unit->self()->on_channel_ready();
		}
		return unit;
	}
	else
	{
		return ExecuteUnitPtr();
	}
}


//////////////////////////////////////////////////////////////////////////
//ExecuteContext
#pragma region Context回调接口

/*! 
 * \brief 获取Tick切片数据
 * \details 从数据管理器获取指定合约的Tick切片数据
 * \param stdCode 标准合约代码
 * \param count 请求的Tick数量
 * \param etime 结束时间，默认为0表示当前
 * \return Tick切片数据指针
 */
WTSTickSlice* WtArbiExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->get_tick_slice(stdCode, count);
}

/*! 
 * \brief 获取最新Tick数据
 * \details 从数据管理器中获取指定合约的最新Tick数据
 * \param stdCode 标准合约代码
 * \return 最新Tick数据指针
 */
WTSTickData* WtArbiExecuter::grabLastTick(const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->grab_last_tick(stdCode);
}

/*! 
 * \brief 获取持仓量
 * \details 从交易适配器中获取指定合约的持仓量
 * \param stdCode 标准合约代码
 * \param validOnly 是否只计算有效仓位，默认为true
 * \param flag 仓位标记，默认为3
 * \return 持仓数量
 */
double WtArbiExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getPosition(stdCode, validOnly, flag);
}

/*! 
 * \brief 获取未完成数量
 * \details 从交易适配器中获取指定合约的未完成委托数量
 * \param stdCode 标准合约代码
 * \return 未完成委托数量
 */
double WtArbiExecuter::getUndoneQty(const char* stdCode)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getUndoneQty(stdCode);
}

/*! 
 * \brief 获取委托映射
 * \details 从交易适配器中获取指定合约的委托映射
 * \param stdCode 标准合约代码
 * \return 委托映射指针
 */
OrderMap* WtArbiExecuter::getOrders(const char* stdCode)
{
	if (NULL == _trader)
		return NULL;

	return _trader->getOrders(stdCode);
}

/*!
 * \brief 买入委托
 * \details 发出买入委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param price 委托价格
 * \param qty 委托数量
 * \param bForceClose 是否强制平仓，默认为false
 * \return 委托ID列表
 */
OrderIDs WtArbiExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->buy(stdCode, price, qty, 0, bForceClose);
}

/*!
 * \brief 卖出委托
 * \details 发出卖出委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param price 委托价格
 * \param qty 委托数量
 * \param bForceClose 是否强制平仓，默认为false
 * \return 委托ID列表
 */
OrderIDs WtArbiExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->sell(stdCode, price, qty, 0, bForceClose);
}

/*!
 * \brief 撤销委托
 * \details 根据委托ID撤销指定委托，需要交易通道就绪
 * \param localid 本地委托ID
 * \return 是否撤销成功
 */
bool WtArbiExecuter::cancel(uint32_t localid)
{
	if (!_channel_ready)
		return false;

	return _trader->cancel(localid);
}

/*!
 * \brief 撤销委托
 * \details 根据合约代码、方向和数量撤销委托，需要交易通道就绪
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param qty 要撤销的数量
 * \return 撤销的委托ID列表
 */
OrderIDs WtArbiExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->cancel(stdCode, isBuy, qty);
}

/*!
 * \brief 输出日志
 * \details 输出日志信息，并添加执行器名称前缀
 * \param message 日志消息
 */
void WtArbiExecuter::writeLog(const char* message)
{
	static thread_local char szBuf[2048] = { 0 };
	fmtutil::format_to(szBuf, "[{}]", _name.c_str());
	strcat(szBuf, message);
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/*!
 * \brief 获取商品信息
 * \details 从执行器绑定的执行核心中获取商品信息
 * \param stdCode 标准合约代码
 * \return 商品信息指针
 */
WTSCommodityInfo* WtArbiExecuter::getCommodityInfo(const char* stdCode)
{
	return _stub->get_comm_info(stdCode);
}

/*!
 * \brief 获取交易时段信息
 * \details 从执行器绑定的执行核心中获取交易时段信息
 * \param stdCode 标准合约代码
 * \return 交易时段信息指针
 */
WTSSessionInfo* WtArbiExecuter::getSessionInfo(const char* stdCode)
{
	return _stub->get_sess_info(stdCode);
}

/*!
 * \brief 获取当前时间
 * \details 从执行器绑定的执行核心中获取当前时间
 * \return 当前时间戳
 */
uint64_t WtArbiExecuter::getCurTime()
{
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


#pragma region 外部接口
/*!
 * \brief 设置目标仓位
 * \details 根据传入的目标仓位映射表设置合约的目标仓位，并自动处理交易逻辑
 * \param targets 目标仓位映射表，键为标准合约代码，值为目标仓位
 */
void WtArbiExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	/*
	 *	先要把目标头寸进行组合匹配
	 */
	auto real_targets = targets;
	for(auto& v : _groups)
	{
		const CodeGroupPtr& gpInfo = v.second;
		bool bHit = false;
		double gpQty = DBL_MAX;
		for(auto& vi : gpInfo->_items)
		{
			double unit = vi.second;
			auto it = real_targets.find(vi.first);
			if (it == real_targets.end())
			{
				bHit = false;
				break;
			}
			else
			{
				bHit = true;
				//计算最小的组合单位数量
				gpQty = std::min(gpQty, decimal::mod(it->second, unit));
			}
		}

		if(bHit && decimal::gt(gpQty, 0))
		{
			real_targets[gpInfo->_name] = gpQty;
			for (auto& vi : gpInfo->_items)
			{
				double unit = vi.second;
				real_targets[vi.first] -= gpQty * unit;
			}
		}
	}


	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;
		ExecuteUnitPtr unit = getUnit(stdCode);
		if (unit == NULL)
			continue;

		double oldVol = _target_pos[stdCode];
		_target_pos[stdCode] = newVol;
		// 账户的理论持仓要经过修正
		double traderTarget = round(newVol * _scale);

		if(!decimal::eq(oldVol, newVol))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale{}", stdCode, oldVol, newVol, traderTarget, _scale);
		}

		if (_trader && !_trader->checkOrderLimits(stdCode))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "{} is disabled due to entrust limit control ", stdCode);
			continue;
		}

		if (_pool)
		{
			std::string code = stdCode;
			_pool->schedule([unit, code, traderTarget](){
				unit->self()->set_position(code.c_str(), traderTarget);
			});
		}
		else
		{
			unit->self()->set_position(stdCode, traderTarget);
		}
	}

	//在原来的目标头寸中，但是不在新的目标头寸中，则需要自动设置为0
	for (auto it = _target_pos.begin(); it != _target_pos.end(); it++)
	{
		const char* code = it->first.c_str();
		double& pos = (double&)it->second;
		auto tit = targets.find(code);
		if(tit != targets.end())
			continue;

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is not in target, set to 0 automatically", code);

		ExecuteUnitPtr unit = getUnit(code);
		if (unit == NULL)
			continue;

		//unit->self()->set_position(code, 0);
		if (_pool)
		{
			_pool->schedule([unit, code](){
				unit->self()->set_position(code, 0);
			});
		}
		else
		{
			unit->self()->set_position(code, 0);
		}

		pos = 0;
	}

	//如果开启了严格同步，则需要检查通道持仓
	//如果通道持仓不在管理中，则直接平掉
	if(_strict_sync)
	{
		for(const std::string& stdCode : _channel_holds)
		{
			auto it = _target_pos.find(stdCode.c_str());
			if(it != _target_pos.end())
				continue;

			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is not in management, set to 0 due to strict sync mode", stdCode.c_str());

			ExecuteUnitPtr unit = getUnit(stdCode.c_str());
			if (unit == NULL)
				continue;

			if (_pool)
			{
				std::string code = stdCode.c_str();
				_pool->schedule([unit, code]() {
					unit->self()->set_position(code.c_str(), 0);
				});
			}
			else
			{
				unit->self()->set_position(stdCode.c_str(), 0);
			}
		}
	}
}

/*!
 * \brief 处理实时行情
 * \details 接收并处理实时行情数据，将行情转发给相应执行单元
 * \param stdCode 标准合约代码
 * \param newTick 最新的行情数据
 */
void WtArbiExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//unit->self()->on_tick(newTick);
	if (_pool)
	{
		newTick->retain();
		_pool->schedule([unit, newTick](){
			unit->self()->on_tick(newTick);
			newTick->release();
		});
	}
	else
	{
		unit->self()->on_tick(newTick);
	}
}

/*!
 * \brief 处理仓位变更
 * \details 接收仓位变更通知，并更新内部仓位缓存
 *          如果合约已存在于仓位缓存中，则将差值叠加到现有仓位
 *          如果合约不存在于仓位缓存中，则创建新的仓位记录
 *          最后记录日志，输出最新的仓位信息
 * \param stdCode 标准合约代码
 * \param diffPos 仓位变化量
 */
void WtArbiExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	ExecuteUnitPtr unit = getUnit(stdCode);
	if (unit == NULL)
		return;

	double oldVol = _target_pos[stdCode];
	double newVol = oldVol + diffPos;
	_target_pos[stdCode] = newVol;

	double traderTarget = round(newVol * _scale);

	if(!decimal::eq(diffPos, 0))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale:{}", stdCode, oldVol, newVol, traderTarget, _scale);
	}

	if (_trader && !_trader->checkOrderLimits(stdCode))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is disabled", stdCode);
		return;
	}

	unit->self()->set_position(stdCode, traderTarget);
}

/*!
 * \brief 处理成交回报
 * \details 接收并处理成交回报，将成交信息转发给相应的执行单元
 *          如果启用了线程池，则在独立线程中异步处理成交信息
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param vol 成交数量
 * \param price 成交价格
 */
void WtArbiExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//unit->self()->on_trade(stdCode, isBuy, vol, price);
	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, vol, price](){
			unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
		});
	}
	else
	{
		unit->self()->on_trade(localid, stdCode, isBuy, vol, price);
	}
}

/*!
 * \brief 处理委托回报
 * \details 接收并处理委托回报，将委托信息转发给相应的执行单元
 *          如果启用了线程池，则在独立线程中异步处理委托信息
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param isBuy 是否买入方向
 * \param totalQty 总委托数量
 * \param leftQty 剩余委托数量
 * \param price 委托价格
 * \param isCanceled 是否已撤销，默认为false
 */
void WtArbiExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//unit->self()->on_order(localid, stdCode, isBuy, leftQty, price, isCanceled);
	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, leftQty, price, isCanceled](){
			unit->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		});
	}
	else
	{
		unit->self()->on_order(localid, stdCode, isBuy, leftQty, price, isCanceled);
	}
}

/*!
 * \brief 处理委托下达回报
 * \details 接收并处理委托下达回报，将委托下达结果转发给相应的执行单元
 *          委托下达回报包含委托是否成功下达以及相关消息
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 * \param localid 本地委托ID
 * \param stdCode 标准合约代码
 * \param bSuccess 委托是否成功下达
 * \param message 相关消息，如错误原因
 */
void WtArbiExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	//unit->self()->on_entrust(localid, stdCode, bSuccess, message);
	if (_pool)
	{
		std::string code = stdCode;
		std::string msg = message;
		_pool->schedule([unit, localid, code, bSuccess, msg](){
			unit->self()->on_entrust(localid, code.c_str(), bSuccess, msg.c_str());
		});
	}
	else
	{
		unit->self()->on_entrust(localid, stdCode, bSuccess, message);
	}
}

/*!
 * \brief 处理交易通道就绪事件
 * \details 当交易通道就绪时，更新内部通道就绪状态并通知所有执行单元
 *          通过循环遍历所有执行单元，将通道就绪事件转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 */
void WtArbiExecuter::on_channel_ready()
{
	_channel_ready = true;
	SpinLock lock(_mtx_units);
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			//unitPtr->self()->on_channel_ready();
			if (_pool)
			{
				_pool->schedule([unitPtr](){
					unitPtr->self()->on_channel_ready();
				});
			}
			else
			{
				unitPtr->self()->on_channel_ready();
			}
		}
	}
}

/*!
 * \brief 处理交易通道丢失事件
 * \details 当交易通道丢失时，更新内部通道状态并通知所有执行单元
 *          通过循环遍历所有执行单元，将通道丢失事件转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 */
void WtArbiExecuter::on_channel_lost()
{
	_channel_ready = false;
	SpinLock lock(_mtx_units);
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			if (_pool)
			{
				_pool->schedule([unitPtr](){
					unitPtr->self()->on_channel_lost();
				});
			}
			else
			{
				unitPtr->self()->on_channel_lost();
			}
		}
	}
}

/*!
 * \brief 处理账户资金更新
 * \details 接收并处理账户资金更新通知，将资金信息转发给所有的执行单元
 *          通过循环遍历所有执行单元，将账户资金信息转发给每一个执行单元
 *          如果启用了线程池，则在独立线程中异步处理
 *          否则在当前线程中同步处理
 * \param currency 货币代码
 * \param prebalance 前一交易日结算后的账户余额
 * \param balance 当前结算后的账户余额
 * \param dynbalance 动态权益（包含浮动盈亏）
 * \param avaliable 可用资金
 * \param closeprofit 平仓盈亏
 * \param dynprofit 浮动盈亏
 * \param margin 保证金
 * \param fee 手续费
 * \param deposit 入金
 * \param withdraw 出金
 */
void WtArbiExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance, 
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	SpinLock lock(_mtx_units);
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			if (_pool)
			{
				std::string strCur = currency;
				_pool->schedule([unitPtr, strCur, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw]() {
					unitPtr->self()->on_account(strCur.c_str(), prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
				});
			}
			else
			{
				unitPtr->self()->on_account(currency, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
			}
		}
	}
}

/*!
 * \brief 处理持仓更新通知
 * \details 接收交易账户持仓更新通知，处理两个主要功能：
 *          1. 记录交易通道持有的合约到集合中，用于后续严格同步功能
 *          2. 实现过期主力合约自动清理功能：
 *             - 首先检查是否启用了自动清理功能
 *             - 然后判断是否为分月期货合约
 *             - 确认当前合约是否为上一期的主力合约
 *             - 考虑排除列表和包含列表的设置
 *             - 最后执行自动清理操作
 * \param stdCode 标准合约代码
 * \param isLong 是否为多头持仓
 * \param prevol 之前的持仓量
 * \param preavail 之前的可用持仓量
 * \param newvol 新的持仓量
 * \param newavail 新的可用持仓量
 * \param tradingday 交易日
 */
void WtArbiExecuter::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
	_channel_holds.insert(stdCode);

	/*
	 *	By Wesley @ 2021.12.14
	 *	先检查自动清理过期主力合约的标记是否为true
	 *	如果不为true，则直接退出该逻辑
	 */
	if (!_auto_clear)
		return;

	//如果不是分月期货合约，直接退出
	if (!CodeHelper::isStdMonthlyFutCode(stdCode))
		return;

	IHotMgr* hotMgr = _stub->get_hot_mon();
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	//获取上一期的主力合约
	std::string prevCode = hotMgr->getPrevRawCode(cInfo._exchg, cInfo._product, tradingday);

	//如果当前合约不是上一期的主力合约，则直接退出
	if (prevCode != cInfo._code)
		return;

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Prev hot contract of {}.{} on {} is {}", cInfo._exchg, cInfo._product, tradingday, prevCode);

	thread_local static char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", cInfo._exchg, cInfo._product);

	//先检查排除列表
	//如果在排除列表中，则直接退出
	auto it = _clear_excludes.find(fullPid);
	if(it != _clear_excludes.end())
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's in exclude list", stdCode);
		return;
	}

	//如果包含列表不为空，再检查是否在包含列表中
	//如果为空，则全部清理，不再进入该逻辑
	if(!_clear_includes.empty())
	{
		it = _clear_includes.find(fullPid);
		if (it == _clear_includes.end())
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's not in include list", stdCode);
			return;
		}
	}

	//最后再进行自动清理
	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, will be cleared", stdCode);
	ExecuteUnitPtr unit = getUnit(stdCode);
	if (unit)
	{
		if (_pool)
		{
			std::string code = stdCode;
			_pool->schedule([unit, code](){
				unit->self()->clear_all_position(code.c_str());
			});
		}
		else
		{
			unit->self()->clear_all_position(stdCode);
		}
	}
}

#pragma endregion 外部接口
