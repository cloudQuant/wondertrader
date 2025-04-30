/*!
 * @file WtExecMgr.cpp
 * @author wondertrader
 * @date 2022/05/01
 * 
 * @brief 执行管理器实现文件
 * @details 执行管理器负责管理交易执行器，处理目标仓位变化，并将交易指令路由到相应的执行器
 */

#include "WtExecMgr.h"
#include "WtFilterMgr.h"

#include "../Share/decimal.h"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

//////////////////////////////////////////////////////////////////////////
#pragma region "WtExecuterMgr"
/**
 * @brief 枚举所有执行器
 * @details 遍历所有执行器，并对每个执行器调用回调函数
 * @param cb 执行器枚举回调函数
 */
void WtExecuterMgr::enum_executer(EnumExecuterCb cb)
{
	for (auto& v : _executers)
	{
		ExecCmdPtr& executer = (ExecCmdPtr&)v.second;
		cb(executer);
	}
}

/**
 * @brief 设置目标仓位
 * @details 设置多个合约的目标仓位，并应用过滤器规则，然后将目标仓位传递给所有执行器
 * @param target_pos 目标仓位映射表，键为合约代码，值为目标仓位
 */
void WtExecuterMgr::set_positions(wt_hashmap<std::string, double> target_pos)
{
	// 如果过滤器管理器存在，则应用过滤规则
	if(_filter_mgr != NULL)
	{
		wt_hashmap<std::string, double> des_port;
		for(auto& m : target_pos)
		{
			const auto& stdCode = m.first;
			double& desVol = (double&)m.second;
			double oldVol = desVol;

			// 检查合约是否被过滤，如果被过滤则desVol可能会被修改
			bool isFltd = _filter_mgr->is_filtered_by_code(stdCode.c_str(), desVol);
			if (!isFltd)
			{
				// 如果目标仓位被过滤器修改，输出日志
				if (!decimal::eq(desVol, oldVol))
				{
					WTSLogger::info("[Filters] {} target position reset by code filter: {} -> {}", stdCode.c_str(), oldVol, desVol);
				}

				// 将过滤后的仓位添加到新的映射表中
				des_port[stdCode] = desVol;
			}
			else
			{
				// 如果合约被完全过滤掉，输出日志
				WTSLogger::info("[Filters] {} target position ignored by filter", stdCode.c_str());
			}
		}

		// 用过滤后的映射表替换原始映射表
		des_port.swap(target_pos);
	}

	// 遍历所有执行器，并设置目标仓位
	for (auto& v : _executers)
	{
		ExecCmdPtr& executer = (ExecCmdPtr&)v.second;

		// 检查执行器是否被过滤
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] Executer {} is filtered, all signals will be ignored", executer->name());
			continue;
		}
		// 将目标仓位设置到执行器
		executer->set_position(target_pos);
	}
}

/**
 * @brief 处理仓位变化
 * @details 处理单个合约的目标仓位变化，并将变化传递给相应的执行器
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位
 * @param diffPos 仓位差值
 * @param execid 执行器ID，默认为"ALL"，表示所有执行器
 */
void WtExecuterMgr::handle_pos_change(const char* stdCode, double targetPos, double diffPos, const char* execid /* = "ALL" */)
{
	// 如果过滤器管理器存在，则应用过滤规则
	if(_filter_mgr != NULL)
	{
		double oldVol = targetPos;
		// 检查合约是否被过滤，如果被过滤则targetPos可能会被修改
		bool isFltd = _filter_mgr->is_filtered_by_code(stdCode, targetPos);
		if (!isFltd)
		{
			// 如果目标仓位被过滤器修改，输出日志并重新计算差量
			if (!decimal::eq(targetPos, oldVol))
			{
				WTSLogger::info("[Filters] {} target position reset by filter: {} -> {}", stdCode, oldVol, targetPos);
				// 差量也要重算，将过滤器导致的仓位变化添加到差量中
				diffPos += (targetPos - oldVol);
			}
		}
		else
		{
			// 如果合约被完全过滤掉，输出日志并直接返回
			WTSLogger::info("[Filters] {} target position ignored by filter", stdCode);
			return;
		}
	}

	// 遍历所有执行器
	for (auto& v : _executers)
	{
		ExecCmdPtr& executer = (ExecCmdPtr&)v.second;

		// 检查执行器是否被过滤
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] All signals to executer {} are ignored by executer filter", executer->name());
			continue;
		}

		// 检查执行器是否在路由表中，如果不在且execid为"ALL"，或者执行器名称与execid相同，则通知仓位变化
		auto it = _routed_executers.find(executer->name());
		if (it == _routed_executers.end() && strcmp(execid, "ALL") == 0)
			executer->on_position_changed(stdCode, diffPos);
		else if(strcmp(executer->name(), execid) == 0)
			executer->on_position_changed(stdCode, diffPos);
	}
}

/**
 * @brief 处理行情数据
 * @details 将最新的行情数据传递给所有执行器
 * @param stdCode 标准化合约代码
 * @param curTick 当前行情数据
 */
void WtExecuterMgr::handle_tick(const char* stdCode, WTSTickData* curTick)
{
	// 注释掉的旧循环方式
	//for (auto it = _executers.begin(); it != _executers.end(); it++)
	//{
	//	ExecCmdPtr& executer = (*it);
	//	executer->on_tick(stdCode, curTick);
	//}

	// 遍历所有执行器，并将行情数据传递给每个执行器
	for (auto& v : _executers)
	{
		ExecCmdPtr& executer = (ExecCmdPtr&)v.second;
		executer->on_tick(stdCode, curTick);
	}
}

/**
 * @brief 将目标仓位加入缓存
 * @details 将单个合约的目标仓位加入缓存，以便后续一次性提交
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位
 * @param execid 执行器ID，默认为"ALL"，表示所有执行器
 */
void WtExecuterMgr::add_target_to_cache(const char* stdCode, double targetPos, const char* execid /* = "ALL" */)
{
	// 获取指定执行器的目标仓位映射表，如果不存在则创建
	TargetsMap& targets = _all_cached_targets[execid];
	// 获取合约的目标仓位，如果不存在则创建并初始化为0
	double& vol = targets[stdCode];
	// 累加目标仓位
	vol += targetPos;
}

/**
 * @brief 提交缓存的目标仓位
 * @details 将缓存中的所有目标仓位一次性提交给相应的执行器
 * @param scale 风控系数，默认为1.0，可以用于调整所有仓位的比例
 */
void WtExecuterMgr::commit_cached_targets(double scale /* = 1.0 */)
{
	// 遍历所有缓存的目标仓位
	for(auto& v : _all_cached_targets)
	{	
		// 先对组合进行缩放
		const char* execid = v.first.c_str();
		TargetsMap& target_pos = (TargetsMap&)v.second;
		for(auto& item : target_pos)
		{
			const auto& stdCode = item.first;
			double& pos = (double&)item.second;

			// 如果仓位为0，则跳过
			if(decimal::eq(pos, 0))
				continue;

			// 保留仓位的正负方向，并根据缩放系数调整仓位大小
			double symbol = pos / abs(pos);
			pos = decimal::rnd(abs(pos)*scale)*symbol;
		}

		// 然后根据过滤器调整目标仓位
		if (_filter_mgr != NULL)
		{
			TargetsMap des_port;
			for (auto& m : target_pos)
			{
				const auto& stdCode = m.first;
				double& desVol = (double&)m.second;
				double oldVol = desVol;

				// 检查合约是否被过滤，如果被过滤则desVol可能会被修改
				bool isFltd = _filter_mgr->is_filtered_by_code(stdCode.c_str(), desVol);
				if (!isFltd)
				{
					// 如果目标仓位被过滤器修改，输出日志
					if (!decimal::eq(desVol, oldVol))
					{
						WTSLogger::info("[Filters] {} target position reset by code filter: {} -> {}", stdCode.c_str(), oldVol, desVol);
					}

					// 将过滤后的仓位添加到新的映射表中
					des_port[stdCode] = desVol;
				}
				else
				{
					// 如果合约被完全过滤掉，输出日志
					WTSLogger::info("[Filters] {} target position ignored by filter", stdCode.c_str());
				}
			}

			// 用过滤后的映射表替换原始映射表
			target_pos.swap(des_port);
		}
	}

	// 遍历所有执行器
	for (auto& e : _executers)
	{
		ExecCmdPtr& executer = (ExecCmdPtr&)e.second;
		// 检查执行器是否被过滤
		if (_filter_mgr && _filter_mgr->is_filtered_by_executer(executer->name()))
		{
			WTSLogger::info("[Filters] Executer {} is filtered, all signals will be ignored", executer->name());
			continue;
		}

		// 先找自己对应的组合
		auto it = _all_cached_targets.find(executer->name());

		// 如果找不到，就找全部组合
		if (it == _all_cached_targets.end())
			it = _all_cached_targets.find("ALL");

		// 如果仍然找不到，则跳过当前执行器
		if (it == _all_cached_targets.end())
			continue;

		// 将目标仓位设置到执行器
		executer->set_position(it->second);
	}

	// 提交完了以后，清理掉全部缓存的目标仓位
	_all_cached_targets.clear();
}

/**
 * @brief 加载路由规则
 * @details 从配置中加载策略到执行器的路由规则
 * @param config 配置对象，应为数组类型，每个元素包含策略名称和执行器信息
 * @return 是否成功加载路由规则
 */
bool WtExecuterMgr::load_router_rules(WTSVariant* config)
{
	// 检查配置是否有效
	if (config == NULL || !config->isArray())
		return false;

	// 遍历配置数组中的每个路由规则
	for(uint32_t i = 0; i < config->size(); i++)
	{
		WTSVariant* item = config->get(i);
		// 获取策略名称
		const char* straName = item->getCString("strategy");
		// 获取执行器配置
		WTSVariant* itemExec = item->get("executer");
		// 如果执行器配置是数组，表示一个策略可以路由到多个执行器
		if(itemExec->isArray())
		{
			uint32_t cnt = itemExec->size();
			for(uint32_t k = 0; k < cnt; k++)
			{
				// 获取执行器ID
				const char* execId = itemExec->get(k)->asCString();
				// 将执行器ID添加到策略的路由规则中
				_router_rules[straName].insert(execId);
				WTSLogger::info("Signal of strategy {} will be routed to executer {}", straName, execId);
				// 将执行器ID添加到已路由执行器集合中
				_routed_executers.insert(execId);
			}
		}
		else
		{
			// 如果执行器配置是单个值，表示策略只路由到一个执行器
			const char* execId = itemExec->asCString();
			// 将执行器ID添加到策略的路由规则中
			_router_rules[straName].insert(execId);
			WTSLogger::info("Signal of strategy {} will be routed to executer {}", straName, execId);
			// 将执行器ID添加到已路由执行器集合中
			_routed_executers.insert(execId);
		}
	}

	// 输出加载的路由规则数量
	WTSLogger::info("{} router rules loaded", _router_rules.size());

	return true;
}

#pragma endregion "WtExecuterMgr"
