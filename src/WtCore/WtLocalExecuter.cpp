/*!
 * \file WtExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 本地执行器实现文件
 * \details 实现了本地执行器类，用于处理交易指令的执行、仓位管理和交易回报等
 */
#include "WtLocalExecuter.h"
#include "TraderAdapter.h"
#include "WtEngine.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;


/**
 * @brief 构造函数
 * @param factory 执行器工厂指针
 * @param name 执行器名称
 * @param dataMgr 数据管理器指针
 * @details 初始化执行器的基本参数，包括名称、工厂、数据管理器等
 */
WtLocalExecuter::WtLocalExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr)
	: IExecCommand(name)
	, _factory(factory)
	, _data_mgr(dataMgr)
	, _channel_ready(false)
	, _scale(1.0)
	, _auto_clear(true)
	, _trader(NULL)
{
}


/**
 * @brief 析构函数
 * @details 清理执行器资源，如果存在线程池，会等待所有任务完成再销毁对象
 */
WtLocalExecuter::~WtLocalExecuter()
{
	if (_pool)
		_pool->wait();
}

/**
 * @brief 设置交易适配器
 * @param adapter 交易适配器指针
 * @details 设置交易适配器并检查交易通道是否就绪，更新内部通道状态
 */
void WtLocalExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
	//设置的时候读取一下trader的状态
	if(_trader)
		_channel_ready = _trader->isReady();
}

/**
 * @brief 初始化执行器
 * @param params 配置参数对象
 * @return 初始化是否成功
 * @details 根据配置参数初始化执行器，包括设置缩放比例、严格同步模式、创建线程池、
 * 读取自动清理策略和代码组等配置。这些配置将影响执行器的行为和性能。
 */
bool WtLocalExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	// 设置缩放比例
	_scale = params->getDouble("scale");
	// 设置是否严格同步
	_strict_sync  = params->getBoolean("strict_sync");

	// 创建线程池（如果配置了池大小）
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

/**
 * @brief 获取执行单元
 * @param stdCode 标准合约代码
 * @param bAutoCreate 是否自动创建，默认为true
 * @return 执行单元指针
 * @details 根据标准合约代码获取相应的执行单元。如果已存在，直接返回；
 * 如果不存在且bAutoCreate为true，则创建新的执行单元并初始化。
 * 执行单元的策略配置来自配置文件中的policy部分，优先使用与商品ID匹配的策略，
 * 如果找不到对应策略，则使用default策略。
 */
ExecuteUnitPtr WtLocalExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	// 提取合约信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	// 获取商品ID
	std::string commID = codeInfo.stdCommID();

	// 获取策略配置
	WTSVariant* policy = _config->get("policy");
	// 默认使用商品ID作为策略名
	std::string des = commID;
	// 如果没有对应的策略，使用default策略
	if (!policy->has(commID.c_str()))
		des = "default";

	// 锁定单元容器进行线程安全的访问
	SpinLock lock(_mtx_units);

	// 检查执行单元是否已存在
	auto it = _unit_map.find(stdCode);
	if(it != _unit_map.end())
	{
		return it->second;
	}

	// 如果需要自动创建
	if (bAutoCreate)
	{
		// 获取策略配置
		WTSVariant* cfg = policy->get(des.c_str());

		// 获取执行单元类型名称
		const char* name = cfg->getCString("name");
		// 创建执行单元
		ExecuteUnitPtr unit = _factory->createExeUnit(name);
		if (unit != NULL)
		{
			// 将新建的执行单元添加到映射中
			_unit_map[stdCode] = unit;
			// 初始化执行单元
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
/**
 * @brief 获取tick数据切片
 * @param stdCode 标准合约代码
 * @param count 请求的tick数量
 * @param etime 结束时间，默认为0（不使用）
 * @return tick数据切片指针
 * @details 从数据管理器中获取指定合约的最近count个tick数据。
 * 如果数据管理器不可用，则返回NULL。
 */
WTSTickSlice* WtLocalExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	// 检查数据管理器是否可用
	if (_data_mgr == NULL)
		return NULL;

	// 从数据管理器获取tick切片
	return _data_mgr->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取最新的tick数据
 * @param stdCode 标准合约代码
 * @return 最新的tick数据指针
 * @details 从数据管理器中获取指定合约的最新tick数据。
 * 如果数据管理器不可用，则返回NULL。
 * 注意：这个函数会增加返回的tick数据的引用计数，使用后需要调用release释放。
 */
WTSTickData* WtLocalExecuter::grabLastTick(const char* stdCode)
{
	// 检查数据管理器是否可用
	if (_data_mgr == NULL)
		return NULL;

	// 从数据管理器获取最新tick数据
	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取指定合约的持仓量
 * @param stdCode 标准合约代码
 * @param validOnly 是否只计算有效持仓，默认为true
 * @param flag 持仓标记，默认为3（全部持仓），1表示多头持仓，2表示空头持仓
 * @return 持仓量
 * @details 从交易适配器中获取指定合约的持仓量。
 * 如果交易适配器不可用，则返回0。
 */
double WtLocalExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return 0.0;

	// 从交易适配器获取持仓量
	return _trader->getPosition(stdCode, validOnly, flag);
}

/**
 * @brief 获取未完成的委托数量
 * @param stdCode 标准合约代码
 * @return 未完成的委托数量
 * @details 从交易适配器中获取指定合约的未完成委托数量。
 * 如果交易适配器不可用，则返回0。
 */
double WtLocalExecuter::getUndoneQty(const char* stdCode)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return 0.0;

	// 从交易适配器获取未完成委托数量
	return _trader->getUndoneQty(stdCode);
}

/**
 * @brief 获取指定合约的订单列表
 * @param stdCode 标准合约代码
 * @return 订单映射指针
 * @details 从交易适配器中获取指定合约的所有订单。
 * 如果交易适配器不可用，则返回NULL。
 */
OrderMap* WtLocalExecuter::getOrders(const char* stdCode)
{
	// 检查交易适配器是否可用
	if (NULL == _trader)
		return NULL;

	// 从交易适配器获取订单列表
	return _trader->getOrders(stdCode);
}

/**
 * @brief 发送买入委托
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID集合
 * @details 通过交易适配器发送买入委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器发送买入委托，偏移量设置为0
	return _trader->buy(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 发送卖出委托
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return 订单ID集合
 * @details 通过交易适配器发送卖出委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器发送卖出委托，偏移量设置为0
	return _trader->sell(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 取消指定订单
 * @param localid 本地订单ID
 * @return 取消是否成功
 * @details 通过交易适配器取消指定的订单。
 * 如果交易通道未就绪，则返回false。
 */
bool WtLocalExecuter::cancel(uint32_t localid)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return false;

	// 通过交易适配器取消订单
	return _trader->cancel(localid);
}

/**
 * @brief 批量取消指定合约的委托
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入委托
 * @param qty 要取消的数量
 * @return 被取消的订单ID集合
 * @details 通过交易适配器批量取消指定合约、指定方向的委托。
 * 如果交易通道未就绪，则返回空的订单ID集合。
 */
OrderIDs WtLocalExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	// 检查交易通道是否就绪
	if (!_channel_ready)
		return OrderIDs();

	// 通过交易适配器批量取消委托
	return _trader->cancel(stdCode, isBuy, qty);
}

/**
 * @brief 写入日志
 * @param message 日志消息
 * @details 将消息写入到日志系统中，并自动添加执行器名称前缀。
 * 使用线程本地存储的缓冲区来提高性能。
 */
void WtLocalExecuter::writeLog(const char* message)
{
	// 使用线程本地存储的缓冲区，避免多线程冲突
	static thread_local char szBuf[2048] = { 0 };
	// 格式化消息，添加执行器名称前缀
	fmtutil::format_to(szBuf, "[{}]", _name.c_str());
	// 连接原始消息
	strcat(szBuf, message);
	// 写入日志
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/**
 * @brief 获取商品信息
 * @param stdCode 标准合约代码
 * @return 商品信息指针
 * @details 从交易引擎核心中获取指定合约的商品信息。
 * 商品信息包含合约的交易所、品种、合约代码、手续费率、保证金率等信息。
 */
WTSCommodityInfo* WtLocalExecuter::getCommodityInfo(const char* stdCode)
{
	// 从交易引擎核心获取商品信息
	return _stub->get_comm_info(stdCode);
}

/**
 * @brief 获取交易时段信息
 * @param stdCode 标准合约代码
 * @return 交易时段信息指针
 * @details 从交易引擎核心中获取指定合约的交易时段信息。
 * 交易时段信息包含开盘时间、收盘时间、交易时段等信息。
 */
WTSSessionInfo* WtLocalExecuter::getSessionInfo(const char* stdCode)
{
	// 从交易引擎核心获取交易时段信息
	return _stub->get_sess_info(stdCode);
}

/**
 * @brief 获取当前时间
 * @return 当前时间戳，格式为时间戳格式
 * @details 从交易引擎核心中获取当前的真实时间。
 * 这个时间用于执行器的时间同步和交易决策。
 */
uint64_t WtLocalExecuter::getCurTime()
{
	// 从交易引擎核心获取当前真实时间
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


#pragma region 外部接口
/**
 * @brief 持仓变动回调
 * @param stdCode 标准合约代码
 * @param diffPos 持仓变动量，正值表示增加，负值表示减少
 * @details 当策略引擎要求修改目标仓位时调用此函数。
 * 函数将更新内部持仓目标，并将缩放后的持仓目标传送给执行单元。
 * 同时考虑了订单限制和缩放比例。
 */
void WtLocalExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	// 获取合约对应的执行单元，如果不存在会自动创建
	ExecuteUnitPtr unit = getUnit(stdCode);
	if (unit == NULL)
		return;

	// 计算新的目标仓位
	double oldVol = _target_pos[stdCode];
	double newVol = oldVol + diffPos;
	_target_pos[stdCode] = newVol;

	// 应用缩放比例并四舍五入
	double traderTarget = round(newVol * _scale);

	// 如果持仓有变化，记录日志
	if(!decimal::eq(diffPos, 0))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale:{}", stdCode, oldVol, newVol, traderTarget, _scale);
	}

	// 检查合约是否有订单限制
	if (_trader && !_trader->checkOrderLimits(stdCode))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is disabled", stdCode);
		return;
	}

	// 通知执行单元设置新的目标仓位
	unit->self()->set_position(stdCode, traderTarget);
}

/**
 * @brief 批量设置多个合约的目标仓位
 * @param targets 目标仓位哈希表，键为合约代码，值为目标仓位
 * @details 该函数会处理多个合约的目标仓位设置。
 * 首先处理组合合约情况，将组合合约转换为实际合约的目标仓位。
 * 然后对每个合约分别设置目标仓位，并处理旧目标中存在但新目标中不存在的合约。
 * 如果配置了线程池，将使用线程池并行处理仓位设置。
 */
void WtLocalExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	/*
	 *	先要把目标头寒进行组合匹配
	 */
	// 创建实际目标仓位的副本，用于处理组合合约
	auto real_targets = targets;
	// 遍历所有的合约组合
	for(auto& v : _groups)
	{
		const CodeGroupPtr& gpInfo = v.second;
		bool bHit = false;
		double gpQty = DBL_MAX;
		// 检查组合中的每个合约是否存在于目标仓位中
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

		// 如果组合成功匹配且数量大于0
		if(bHit && decimal::gt(gpQty, 0))
		{
			// 设置组合合约的目标仓位
			real_targets[gpInfo->_name] = gpQty;
			// 从原始合约中减去组合合约占用的部分
			for (auto& vi : gpInfo->_items)
			{
				double unit = vi.second;
				real_targets[vi.first] -= gpQty * unit;
			}
		}
	}

	// 遍历所有目标仓位，设置执行单元的目标仓位
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;
		// 获取或创建执行单元
		ExecuteUnitPtr unit = getUnit(stdCode);
		if (unit == NULL)
			continue;

		// 更新内部目标仓位记录
		double oldVol = _target_pos[stdCode];
		_target_pos[stdCode] = newVol;
		// 账户的理论持仓要经过缩放比例修正
		double traderTarget = round(newVol * _scale);

		// 如果仓位变化，记录日志
		if(!decimal::eq(oldVol, newVol))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Target position of {} changed: {} -> {} : {} with scale{}", stdCode, oldVol, newVol, traderTarget, _scale);
		}

		// 检查合约是否有订单限制
		if (_trader && !_trader->checkOrderLimits(stdCode))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "{} is disabled due to entrust limit control ", stdCode);
			continue;
		}

		// 如果配置了线程池，使用线程池并行处理
		if (_pool)
		{
			std::string code = stdCode;
			_pool->schedule([unit, code, traderTarget](){
				unit->self()->set_position(code.c_str(), traderTarget);
			});
		}
		else
		{
			// 直接设置执行单元的目标仓位
			unit->self()->set_position(stdCode, traderTarget);
		}
	}

	//在原来的目标头寒中，但是不在新的目标头寒中，则需要自动设置为0
	for (auto it = _target_pos.begin(); it != _target_pos.end(); it++)
	{
		const char* code = it->first.c_str();
		double& pos = (double&)it->second;
		auto tit = targets.find(code);
		// 如果新目标中存在该合约，跳过
		if(tit != targets.end())
			continue;

		// 旧目标中有，新目标中没有的合约，自动设置为0
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "{} is not in target, set to 0 automatically", code);

		ExecuteUnitPtr unit = getUnit(code);
		if (unit == NULL)
			continue;

		//unit->self()->set_position(code, 0);
		// 如果配置了线程池，使用线程池并行处理
		if (_pool)
		{
			_pool->schedule([unit, code](){
				unit->self()->set_position(code, 0);
			});
		}
		else
		{
			// 直接设置执行单元的目标仓位为0
			unit->self()->set_position(code, 0);
		}

		// 更新内部目标仓位记录
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

/**
 * @brief 实时行情回调
 * @param stdCode 标准合约代码
 * @param newTick 新的行情数据
 * @details 当收到新的行情数据时调用此函数，并将其转发给相应的执行单元。
 * 如果启用了线程池，则异步处理行情数据。
 */
void WtLocalExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	// 尝试获取执行单元，但不自动创建
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	// 如果有线程池，则异步处理
	if (_pool)
	{
		// 增加引用计数，防止在异步执行过程中被释放
		newTick->retain();
		_pool->schedule([unit, newTick](){
			unit->self()->on_tick(newTick);
			// 异步处理完成后释放
			newTick->release();
		});
	}
	else
	{
		// 直接同步处理
		unit->self()->on_tick(newTick);
	}
}

/**
 * @brief 成交回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入
 * @param vol 成交量
 * @param price 成交价格
 * @details 当收到成交回报时调用此函数，并将其转发给相应的执行单元。
 * 如果启用了线程池，则异步处理成交数据。
 */
void WtLocalExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	// 尝试获取执行单元，但不自动创建
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	// 如果有线程池，则异步处理
	if (_pool)
	{
		// 需要复制字符串，因为在lambda中使用c_str()可能会导致内存问题
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, vol, price](){
			unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
		});
	}
	else
	{
		// 直接同步处理
		unit->self()->on_trade(localid, stdCode, isBuy, vol, price);
	}
}

/**
 * @brief 订单状态回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param isBuy 是否为买入
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 委托价格
 * @param isCanceled 是否已取消，默认为false
 * @details 当订单状态变化时调用此函数，并将状态变化转发给相应的执行单元。
 * 如果启用了线程池，则异步处理订单状态。
 */
void WtLocalExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	// 尝试获取执行单元，但不自动创建
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	// 如果有线程池，则异步处理
	if (_pool)
	{
		// 需要复制字符串，因为在lambda中使用c_str()可能会导致内存问题
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, leftQty, price, isCanceled](){
			unit->self()->on_order(localid, code.c_str(), isBuy, leftQty, price, isCanceled);
		});
	}
	else
	{
		// 直接同步处理
		unit->self()->on_order(localid, stdCode, isBuy, leftQty, price, isCanceled);
	}
}

/**
 * @brief 委托回报回调
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param bSuccess 委托是否成功
 * @param message 委托回报消息
 * @details 当收到委托回报时调用此函数，并将回报信息转发给相应的执行单元。
 * 如果启用了线程池，则异步处理委托回报。
 */
void WtLocalExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	// 尝试获取执行单元，但不自动创建
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	// 如果有线程池，则异步处理
	if (_pool)
	{
		// 需要复制字符串，因为在lambda中使用c_str()可能会导致内存问题
		std::string code = stdCode;
		std::string msg = message;
		_pool->schedule([unit, localid, code, bSuccess, msg](){
			unit->self()->on_entrust(localid, code.c_str(), bSuccess, msg.c_str());
		});
	}
	else
	{
		// 直接同步处理
		unit->self()->on_entrust(localid, stdCode, bSuccess, message);
	}
}

/**
 * @brief 交易通道就绪回调
 * @details 当交易通道就绪时调用此函数，设置通道状态并将此消息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_channel_ready()
{
	// 设置通道就绪状态
	_channel_ready = true;
	// 锁定执行单元容器以进行安全遍历
	SpinLock lock(_mtx_units);
	// 遍历所有执行单元
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			// 如果有线程池，则异步处理
			if (_pool)
			{
				_pool->schedule([unitPtr](){
					unitPtr->self()->on_channel_ready();
				});
			}
			else
			{
				// 直接同步处理
				unitPtr->self()->on_channel_ready();
			}
		}
	}
}

/**
 * @brief 交易通道断开回调
 * @details 当交易通道断开时调用此函数，设置通道状态并将此消息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_channel_lost()
{
	// 设置通道断开状态
	_channel_ready = false;
	// 锁定执行单元容器以进行安全遍历
	SpinLock lock(_mtx_units);
	// 遍历所有执行单元
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			// 如果有线程池，则异步处理
			if (_pool)
			{
				_pool->schedule([unitPtr](){
					unitPtr->self()->on_channel_lost();
				});
			}
			else
			{
				// 直接同步处理
				unitPtr->self()->on_channel_lost();
			}
		}
	}
}

/**
 * @brief 账户资金变动回调
 * @param currency 货币代码
 * @param prebalance 前结余额
 * @param balance 静态权益
 * @param dynbalance 动态权益
 * @param avaliable 可用资金
 * @param closeprofit 平仓盈亏
 * @param dynprofit 浮动盈亏
 * @param margin 占用保证金
 * @param fee 手续费
 * @param deposit 入金
 * @param withdraw 出金
 * @details 当收到账户资金变动消息时调用此函数，并将更新信息转发给所有执行单元。
 * 如果启用了线程池，则异步通知所有执行单元。
 */
void WtLocalExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance, 
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	// 锁定执行单元容器以进行安全遍历
	SpinLock lock(_mtx_units);
	// 遍历所有执行单元
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
			// 如果有线程池，则异步处理
			if (_pool)
			{
				// 需要复制字符串，因为在lambda中使用c_str()可能会导致内存问题
				std::string strCur = currency;
				_pool->schedule([unitPtr, strCur, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw]() {
					unitPtr->self()->on_account(strCur.c_str(), prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
				});
			}
			else
			{
				// 直接同步处理
				unitPtr->self()->on_account(currency, prebalance, balance, dynbalance, avaliable, closeprofit, dynprofit, margin, fee, deposit, withdraw);
			}
		}
	}
}

/**
 * @brief 持仓信息回调
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头持仓
 * @param prevol 之前持仓量
 * @param preavail 之前可用持仓量
 * @param newvol 新持仓量
 * @param newavail 新可用持仓量
 * @param tradingday 交易日
 * @details 当收到持仓变动信息时调用此函数，并将合约代码添加到通道持仓集合中。
 * 此函数还包含自动清理过期主力合约的逻辑，通过检查当前合约是否是上一期的主力合约，
 * 并根据包含和排除列表决定是否进行自动清仓。
 */
void WtLocalExecuter::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
    // 将合约添加到通道持仓集合中
    _channel_holds.insert(stdCode);

    /*
     *  By Wesley @ 2021.12.14
     *  先检查自动清理过期主力合约的标记是否为true
     *  如果不为true，则直接退出该逻辑
     */
    if (!_auto_clear)
        return;

    // 如果不是分月期货合约，直接退出
    if (!CodeHelper::isStdMonthlyFutCode(stdCode))
        return;

    // 获取热力合约管理器
    IHotMgr* hotMgr = _stub->get_hot_mon();
    // 解析标准合约代码
    CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
    // 获取上一期的主力合约
    std::string prevCode = hotMgr->getPrevRawCode(cInfo._exchg, cInfo._product, tradingday);

    // 如果当前合约不是上一期的主力合约，则直接退出
    if (prevCode != cInfo._code)
        return;

    // 记录日志，显示找到了上一期的主力合约
    WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Prev hot contract of {}.{} on {} is {}", cInfo._exchg, cInfo._product, tradingday, prevCode);

    // 创建产品完整标识符
    thread_local static char fullPid[64] = { 0 };
    fmtutil::format_to(fullPid, "{}.{}", cInfo._exchg, cInfo._product);

    // 先检查排除列表
    // 如果在排除列表中，则直接退出
    auto it = _clear_excludes.find(fullPid);
    if(it != _clear_excludes.end())
    {
        WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's in exclude list", stdCode);
        return;
    }

    // 如果包含列表不为空，再检查是否在包含列表中
    // 如果为空，则全部清理，不再进入该逻辑
    if(!_clear_includes.empty())
    {
        it = _clear_includes.find(fullPid);
        if (it == _clear_includes.end())
        {
            WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, won't be cleared for it's not in include list", stdCode);
            return;
        }
    }

    // 最后再进行自动清理
    WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "Position of {}, as prev hot contract, will be cleared", stdCode);
    // 获取执行单元
    ExecuteUnitPtr unit = getUnit(stdCode);
    if (unit)
    {
        // 如果有线程池，则异步处理
        if (_pool)
        {
            std::string code = stdCode;
            _pool->schedule([unit, code](){
                unit->self()->clear_all_position(code.c_str());
            });
        }
        else
        {
            // 直接同步处理
            unit->self()->clear_all_position(stdCode);
        }
    }
}

#pragma endregion 外部接口
