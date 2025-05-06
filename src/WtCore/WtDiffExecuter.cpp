/*!
 * \file WtDiffExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 差量执行器实现文件
 * \details 差量执行器(WtDiffExecuter)用于处理目标仓位与实际仓位之间的差值，
 *          通过计算差量并执行相应的交易操作，实现仓位的精确控制。
 */
#include "WtDiffExecuter.h"
#include "TraderAdapter.h"
#include "WtEngine.h"
#include "WtHelper.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/IDataManager.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IHotMgr.h"
#include "../Includes/IBaseDataMgr.h"
#include "../Share/decimal.h"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

USING_NS_WTP;


/**
 * @brief 差量执行器构造函数
 * 
 * @param factory 执行器工厂指针
 * @param name 执行器名称
 * @param dataMgr 数据管理器指针
 * @param bdMgr 基础数据管理器指针
 */
WtDiffExecuter::WtDiffExecuter(WtExecuterFactory* factory, const char* name, IDataManager* dataMgr, IBaseDataMgr* bdMgr)
	: IExecCommand(name)
	, _factory(factory)
	, _data_mgr(dataMgr)
	, _channel_ready(false)
	, _scale(1.0)
	, _trader(NULL)
	, _bd_mgr(bdMgr)
{
}


/**
 * @brief 差量执行器析构函数
 * @details 等待线程池中的任务完成再退出
 */
WtDiffExecuter::~WtDiffExecuter()
{
	if (_pool)
		_pool->wait();
}

/**
 * @brief 设置交易适配器
 * 
 * @param adapter 交易适配器指针
 * @details 设置交易适配器并检查交易通道是否就绪
 */
void WtDiffExecuter::setTrader(TraderAdapter* adapter)
{
	_trader = adapter;
	//设置的时候读取一下trader的状态
	if(_trader)
		_channel_ready = _trader->isReady();
}

/**
 * @brief 初始化差量执行器
 * 
 * @param params 配置参数
 * @return bool 初始化是否成功
 * @details 设置缩放比例、创建线程池并加载数据
 */
bool WtDiffExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getDouble("scale");

	uint32_t poolsize = params->getUInt32("poolsize");
	if(poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	load_data();

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff executer inited, scale: {}, thread poolsize: {}", _name, _scale, poolsize);

	return true;
}

/**
 * @brief 加载执行器数据
 * 
 * @details 从文件中读取执行器的目标仓位和差量数据
 *          数据存储在JSON格式的文件中，包含“targets”和“diffs”两部分
 */
void WtDiffExecuter::load_data()
{
	//读取执行器的理论部位，以及待执行的差量
	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
		return;

	if(root.HasMember("targets"))
	{
		const rj::Value& jTargets = root["targets"];
		for (const rj::Value& jItem : jTargets.GetArray())
		{
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr->getContract(cInfo._code, cInfo._exchg);
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["target"].GetDouble();
			_target_pos[stdCode] = pos;
		}
	}

	if (root.HasMember("diffs"))
	{
		const rj::Value& jDiffs = root["diffs"];
		for (const rj::Value& jItem : jDiffs.GetArray())
		{
			const char* stdCode = jItem["code"].GetString();
			CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
			WTSContractInfo* ct = _bd_mgr->getContract(cInfo._code, cInfo._exchg);
			if (ct == NULL)
			{
				WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Ticker {} is not valid", _name, stdCode);
				continue;
			}

			double pos = jItem["diff"].GetDouble();
			_diff_pos[stdCode] = pos;
		}
	}
}

/**
 * @brief 保存执行器数据
 * 
 * @details 将目标仓位和差量数据保存到JSON格式的文件中
 *          文件包含“targets”和“diffs”两部分，分别存储目标仓位和差量仓位
 */
void WtDiffExecuter::save_data()
{
	std::string filename = WtHelper::getExecDataDir();
	filename += _name + ".json";

	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	{//目标持仓数据保存
		rj::Value jTarget(rj::kArrayType);

		for (auto& v : _target_pos)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("target", v.second, allocator);

			jTarget.PushBack(jItem, allocator);
		}

		root.AddMember("targets", jTarget, allocator);
	}

	{//差量持仓数据保存
		rj::Value jDiff(rj::kArrayType);

		for (auto& v : _diff_pos)
		{
			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("code", rj::Value(v.first.c_str(), allocator), allocator);
			jItem.AddMember("diff", v.second, allocator);

			jDiff.PushBack(jItem, allocator);
		}

		root.AddMember("diffs", jDiff, allocator);
	}

	{
		std::string filename = WtHelper::getExecDataDir();
		filename += _name + ".json";

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

/**
 * @brief 获取指定合约的执行单元
 * 
 * @param stdCode 标准合约代码
 * @param bAutoCreate 如果不存在是否自动创建
 * @return ExecuteUnitPtr 执行单元指针
 * @details 根据合约代码获取执行单元，如果不存在且bAutoCreate为true，
 *          则根据配置创建新的执行单元
 */
ExecuteUnitPtr WtDiffExecuter::getUnit(const char* stdCode, bool bAutoCreate /* = true */)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, NULL);
	std::string commID = codeInfo.stdCommID();

	WTSVariant* policy = _config->get("policy");
	std::string des = commID;
	if (!policy->has(commID.c_str()))
		des = "default";

	//SpinLock lock(_mtx_units);

	auto it = _unit_map.find(stdCode);
	if(it != _unit_map.end())
	{
		return it->second;
	}

	if (bAutoCreate)
	{
		WTSVariant* cfg = policy->get(des.c_str());

		const char* name = cfg->getCString("name");
		ExecuteUnitPtr unit = _factory->createDiffExeUnit(name);
		if (unit != NULL)
		{
			_unit_map[stdCode] = unit;
			unit->self()->init(this, stdCode, cfg);

			//如果通道已经就绪，则直接通知执行单元
			if (_channel_ready)
				unit->self()->on_channel_ready();
		}
		else
		{
			WTSLogger::error("Creating ExecUnit {} failed", name);
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
/**
 * @brief 以下是ExecuteContext接口的实现
 * @details 这些方法提供了执行单元与执行器之间的交互接口
 */
#pragma region Context回调接口
/**
 * @brief 获取指定合约的tick数据切片
 * 
 * @param stdCode 标准合约代码
 * @param count 需要获取的tick数量
 * @param etime 结束时间，默认为0
 * @return WTSTickSlice* tick数据切片指针
 */
WTSTickSlice* WtDiffExecuter::getTicks(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取指定合约的最新tick数据
 * 
 * @param stdCode 标准合约代码
 * @return WTSTickData* 最新tick数据指针
 */
WTSTickData* WtDiffExecuter::grabLastTick(const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;

	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取指定合约的持仓量
 * 
 * @param stdCode 标准合约代码
 * @param validOnly 是否只返回有效持仓，默认为true
 * @param flag 持仓标记，默认为3（同时返回多空持仓）
 * @return double 持仓量
 */
double WtDiffExecuter::getPosition(const char* stdCode, bool validOnly /* = true */, int32_t flag /* = 3 */)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getPosition(stdCode, validOnly, flag);
}

/**
 * @brief 获取指定合约的未完成委托数量
 * 
 * @param stdCode 标准合约代码
 * @return double 未完成委托数量
 */
double WtDiffExecuter::getUndoneQty(const char* stdCode)
{
	if (NULL == _trader)
		return 0.0;

	return _trader->getUndoneQty(stdCode);
}

/**
 * @brief 获取指定合约的订单列表
 * 
 * @param stdCode 标准合约代码
 * @return OrderMap* 订单映射表指针
 */
OrderMap* WtDiffExecuter::getOrders(const char* stdCode)
{
	if (NULL == _trader)
		return NULL;

	return _trader->getOrders(stdCode);
}

/**
 * @brief 发送买入委托
 * 
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 */
OrderIDs WtDiffExecuter::buy(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->buy(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 发送卖出委托
 * 
 * @param stdCode 标准合约代码
 * @param price 委托价格
 * @param qty 委托数量
 * @param bForceClose 是否强制平仓，默认为false
 * @return OrderIDs 订单ID列表
 */
OrderIDs WtDiffExecuter::sell(const char* stdCode, double price, double qty, bool bForceClose/* = false*/)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->sell(stdCode, price, qty, 0, bForceClose);
}

/**
 * @brief 根据本地订单ID撤销订单
 * 
 * @param localid 本地订单ID
 * @return bool 撤单是否成功
 */
bool WtDiffExecuter::cancel(uint32_t localid)
{
	if (!_channel_ready)
		return false;

	return _trader->cancel(localid);
}

/**
 * @brief 根据合约代码、买卖方向和数量撤销订单
 * 
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param qty 要撤销的数量
 * @return OrderIDs 撤销的订单ID列表
 */
OrderIDs WtDiffExecuter::cancel(const char* stdCode, bool isBuy, double qty)
{
	if (!_channel_ready)
		return OrderIDs();

	return _trader->cancel(stdCode, isBuy, qty);
}

/**
 * @brief 写入日志
 * 
 * @param message 日志消息
 * @details 将消息写入日志系统，并自动添加执行器名称前缀
 */
void WtDiffExecuter::writeLog(const char* message)
{
	static thread_local char szBuf[2048] = { 0 };
	fmtutil::format_to(szBuf, "[{}] {}", _name.c_str(), message);
	WTSLogger::log_dyn_raw("executer", _name.c_str(), LL_INFO, szBuf);
}

/**
 * @brief 获取商品信息
 * 
 * @param stdCode 标准合约代码
 * @return WTSCommodityInfo* 商品信息指针
 * @details 从引擎中获取指定合约的商品信息
 */
WTSCommodityInfo* WtDiffExecuter::getCommodityInfo(const char* stdCode)
{
	return _stub->get_comm_info(stdCode);
}

/**
 * @brief 获取交易时段信息
 * 
 * @param stdCode 标准合约代码
 * @return WTSSessionInfo* 交易时段信息指针
 * @details 从引擎中获取指定合约的交易时段信息
 */
WTSSessionInfo* WtDiffExecuter::getSessionInfo(const char* stdCode)
{
	return _stub->get_sess_info(stdCode);
}

/**
 * @brief 获取当前时间
 * 
 * @return uint64_t 当前时间戳
 * @details 从引擎中获取当前实时时间
 */
uint64_t WtDiffExecuter::getCurTime()
{
	return _stub->get_real_time();
	//return TimeUtils::makeTime(_stub->get_date(), _stub->get_raw_time() * 100000 + _stub->get_secs());
}

#pragma endregion Context回调接口
//ExecuteContext
//////////////////////////////////////////////////////////////////////////


/**
 * @brief 以下是外部接口的实现
 * @details 这些方法提供了与外部系统交互的接口，包括处理仓位变化、行情更新、交易回报等
 */
#pragma region 外部接口
/**
 * @brief 处理仓位变化
 * 
 * @param stdCode 标准合约代码
 * @param diffPos 仓位变化量
 * @details 当收到仓位变化通知时，更新目标仓位和差量仓位，
 *          并通知相应的执行单元进行处理
 */
void WtDiffExecuter::on_position_changed(const char* stdCode, double diffPos)
{
	ExecuteUnitPtr unit = getUnit(stdCode, true);
	if (unit == NULL)
		return;

	//如果差量为0，则直接返回
	if (decimal::eq(diffPos, 0))
		return;

	diffPos = round(diffPos*_scale);

	double oldVol = _target_pos[stdCode];
	double& targetPos = _target_pos[stdCode];
	targetPos += diffPos;

	/*
	 *	By Sunseeeeeker @ 2023.01.10
	 *	更新差量
	*/
	double& thisDiff = _diff_pos[stdCode];
	double prevDiff = thisDiff;
	thisDiff += diffPos;

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed additonally: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, targetPos, prevDiff, thisDiff);

	if (_trader && !_trader->checkOrderLimits(stdCode))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is disabled", _name, stdCode);
		return;
	}

	//TODO 差量执行还要再看一下
	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([unit, code, thisDiff]() {
			unit->self()->set_position(code.c_str(), thisDiff);
		});
	}
	else
	{
		unit->self()->set_position(stdCode, thisDiff);
	}
}

/**
 * @brief 设置一组目标仓位
 * 
 * @param targets 目标仓位映射表，键为合约代码，值为目标仓位量
 * @details 批量设置目标仓位，并计算差量。如果原目标仓位中的合约不在新的目标中，
 *          则自动将其设置为0。最后保存数据到文件。
 */
void WtDiffExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;
		ExecuteUnitPtr unit = getUnit(stdCode);
		if (unit == NULL)
			continue;

		newVol = round(newVol*_scale);
		double oldVol = _target_pos[stdCode];
		_target_pos[stdCode] = newVol;
		if (decimal::eq(oldVol, newVol))
			continue;

		//差量更新
		double& thisDiff = _diff_pos[stdCode];
		double prevDiff = thisDiff;
		thisDiff += (newVol - oldVol);

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Target position of {} changed: {} -> {}, diff postion changed: {} -> {}", _name, stdCode, oldVol, newVol, prevDiff, thisDiff);

		if (_trader && !_trader->checkOrderLimits(stdCode))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_WARN, "[{}] {} is disabled due to entrust limit control ", _name, stdCode);
			continue;
		}

		//TODO 差量执行还要再看一下
		if (_pool)
		{
			std::string code = stdCode;
			_pool->schedule([unit, code, thisDiff](){
				unit->self()->set_position(code.c_str(), thisDiff);
			});
		}
		else
		{
			unit->self()->set_position(stdCode, thisDiff);
		}
	}

	//在原来的目标头寸中，但是不在新的目标头寸中，则需要自动设置为0
	for (auto it = _target_pos.begin(); it != _target_pos.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double& pos = (double&)it->second;
		auto tit = targets.find(stdCode);
		if(tit != targets.end())
			continue;

		WTSContractInfo* cInfo = _bd_mgr->getContract(stdCode);
		if(cInfo == NULL)
			continue;

		if(pos != 0)
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] {} is not in target, set to 0 automatically", _name, stdCode);

			ExecuteUnitPtr unit = getUnit(stdCode);
			if (unit == NULL)
				continue;

			//更新差量
			double& thisDiff = _diff_pos[stdCode];
			double prevDiff = thisDiff;

			//WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[DiffExecuter][set_position][{}] {} is not in target, thisDiff: {}, prevDiff: {}, pos: {}, new thisDiff: {}", _name, stdCode, thisDiff, prevDiff, pos, thisDiff + pos);

			thisDiff -= -pos;
			pos = 0;

			if (_pool)
			{
				std::string code = stdCode;
				_pool->schedule([unit, code, thisDiff]() {
					unit->self()->set_position(code.c_str(), thisDiff);
				});
			}
			else
			{
				unit->self()->set_position(stdCode, thisDiff);
			}
		}
	}

	save_data();
}

/**
 * @brief 处理行情数据更新
 * 
 * @param stdCode 合约代码
 * @param newTick 最新的行情数据
 */
void WtDiffExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
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

/**
 * @brief 处理成交回报
 * 
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param vol 成交数量
 * @param price 成交价格
 */
void WtDiffExecuter::on_trade(uint32_t localid, const char* stdCode, bool isBuy, double vol, double price)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

	if (localid == 0)
		return;

	//如果localid不为0，则更新差量
	double& curDiff = _diff_pos[stdCode];
	double prevDiff = curDiff;
	curDiff -= vol * (isBuy ? 1 : -1);

	WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} updated by trade: {} -> {}", _name, stdCode, prevDiff, curDiff);
	save_data();

	if (_pool)
	{
		std::string code = stdCode;
		_pool->schedule([localid, unit, code, isBuy, vol, price]() {
			unit->self()->on_trade(localid, code.c_str(), isBuy, vol, price);
		});
	}
	else
	{
		unit->self()->on_trade(localid, stdCode, isBuy, vol, price);
	}
}

/**
 * @brief 处理订单状态回报
 * 
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param isBuy 是否为买单
 * @param totalQty 总数量
 * @param leftQty 剩余数量
 * @param price 价格
 * @param isCanceled 是否已撤销
 */
void WtDiffExecuter::on_order(uint32_t localid, const char* stdCode, bool isBuy, double totalQty, double leftQty, double price, bool isCanceled /* = false */)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

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

/**
 * @brief 处理委托回报
 * 
 * @param localid 本地订单ID
 * @param stdCode 合约代码
 * @param bSuccess 是否成功
 * @param message 回报消息
 */
void WtDiffExecuter::on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message)
{
	ExecuteUnitPtr unit = getUnit(stdCode, false);
	if (unit == NULL)
		return;

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

/**
 * @brief 处理交易通道就绪事件
 * 
 * @details 当交易通道就绪时，通知所有执行单元并恢复差量持仓
 */
void WtDiffExecuter::on_channel_ready()
{
	_channel_ready = true;
	//SpinLock lock(_mtx_units);
	for (auto it = _unit_map.begin(); it != _unit_map.end(); it++)
	{
		ExecuteUnitPtr& unitPtr = (ExecuteUnitPtr&)it->second;
		if (unitPtr)
		{
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

	for(auto& v : _diff_pos)
	{
		const char* stdCode = v.first.c_str();
		ExecuteUnitPtr unit = getUnit(stdCode);
		if (unit == NULL)
			continue;
		double thisDiff = _diff_pos[stdCode];

		if (_pool)
		{
			std::string code = stdCode;
			_pool->schedule([unit, code, thisDiff]() {
				unit->self()->set_position(code.c_str(), thisDiff);
			});
		}
		else
		{
			unit->self()->set_position(stdCode, thisDiff);
		}

		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}] Diff of {} recovered to {}", _name, stdCode, thisDiff);
	}
}

/**
 * @brief 处理交易通道断开事件
 * 
 * @details 当交易通道断开时，通知所有执行单元
 */
void WtDiffExecuter::on_channel_lost()
{
	_channel_ready = false;
	//SpinLock lock(_mtx_units);
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

/**
 * @brief 处理账户资金更新
 * 
 * @param currency 货币类型
 * @param prebalance 前结余额
 * @param balance 结余额
 * @param dynbalance 动态权益
 * @param avaliable 可用资金
 * @param closeprofit 平仓盈亏
 * @param dynprofit 浮动盈亏
 * @param margin 保证金
 * @param fee 手续费
 * @param deposit 入金
 * @param withdraw 出金
 */
void WtDiffExecuter::on_account(const char* currency, double prebalance, double balance, double dynbalance,
	double avaliable, double closeprofit, double dynprofit, double margin, double fee, double deposit, double withdraw)
{
	//SpinLock lock(_mtx_units);
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

/**
 * @brief 处理持仓更新
 * 
 * @param stdCode 合约代码
 * @param isLong 是否为多头仓位
 * @param prevol 前持仓量
 * @param preavail 前可用持仓量
 * @param newvol 新持仓量
 * @param newavail 新可用持仓量
 * @param tradingday 交易日
 */
void WtDiffExecuter::on_position(const char* stdCode, bool isLong, double prevol, double preavail, double newvol, double newavail, uint32_t tradingday)
{
	// 注意：当前实现为空，可能在子类中实现
}

#pragma endregion 外部接口