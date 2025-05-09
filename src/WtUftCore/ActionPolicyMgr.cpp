/*!
 * \file ActionPolicyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易动作策略管理器实现
 *
 * 本文件实现了交易动作策略管理器，负责从配置文件中加载
 * 交易动作规则，并为交易执行器提供品种的交易动作规则查询功能。
 */
#include "ActionPolicyMgr.h"

#include "../Share/StdUtils.hpp"
#include "../WTSTools/WTSLogger.h"

#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

/**
 * @brief 构造函数
 * 
 * 初始化交易动作策略管理器
 */
ActionPolicyMgr::ActionPolicyMgr()
{
}


/**
 * @brief 析构函数
 * 
 * 清理交易动作策略管理器分配的资源
 */
ActionPolicyMgr::~ActionPolicyMgr()
{
}

/**
 * @brief 初始化动作策略管理器
 * 
 * 从配置文件中加载交易动作规则，并建立品种与规则组之间的映射关系
 * 配置文件包含多个规则组，每个规则组包含多个交易动作规则及其应用的品种筛选器
 * 
 * @param filename 配置文件路径
 * @return bool 初始化是否成功，true表示成功，false表示失败
 */
bool ActionPolicyMgr::init(const char* filename)
{
	// 从配置文件加载数据
	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename);
	if (cfg == NULL)
		return false;

	// 遍历配置文件中的所有规则组
	auto keys = cfg->memberNames();
	for (auto it = keys.begin(); it != keys.end(); it++)
	{
		// 获取规则组名称
		const char* gpName = (*it).c_str();
		WTSVariant*	vGpItem = cfg->get(gpName);
		// 为每个规则组创建一个全新的ActionRuleGroup
		ActionRuleGroup& gp = _rules[gpName];

		// 加载该规则组的所有交易动作规则
		WTSVariant* vOrds = vGpItem->get("order");
		if(vOrds != NULL && vOrds->isArray())
		{
			// 遍历处理每一个交易动作规则
			for (uint32_t i = 0; i < vOrds->size(); i++)
			{
				WTSVariant* vObj = vOrds->get(i);
				ActionRule aRule;
				
				// 从配置中提取动作类型和限制值
				const char* action = vObj->getCString("action");
				uint32_t uLimit = vObj->getUInt32("limit");
				uint32_t uLimitS = vObj->getUInt32("limit_s");
				uint32_t uLimitL = vObj->getUInt32("limit_l");
				
				// 将字符串动作类型转换为ActionType枚举值
				if (wt_stricmp(action, "open") == 0)
					aRule._atype = AT_Open;
				else if (wt_stricmp(action, "close") == 0)
					aRule._atype = AT_Close;
				else if (wt_stricmp(action, "closetoday") == 0)
					aRule._atype = AT_CloseToday;
				else if (wt_stricmp(action, "closeyestoday") == 0)
					aRule._atype = AT_CloseYestoday;
				else 
				{
					// 如果动作类型不识别，记录错误并跳过该条规则
					WTSLogger::error("Loading action policy failed: unrecognized type {}", action);
					continue;
				}

				// 设置规则的限制参数
				aRule._limit = uLimit;    // 总手数限制
				aRule._limit_s = uLimitS; // 空头限制
				aRule._limit_l = uLimitL; // 多头限制
				aRule._pure = vObj->getBoolean("pure"); // 是否为纯净模式
				
				// 将规则添加到规则组中
				gp.emplace_back(aRule);
			}
		}

		// 加载该规则组适用的品种过滤器
		WTSVariant* filters = vGpItem->get("filters");
		if(filters!=NULL && filters->isArray() && filters->size()>0)
		{
			// 遍历处理每一个品种代码
			for (uint32_t i = 0; i < filters->size(); i++)
			{
				// 将品种代码与规则组名称建立映射关系
				const char* commid = filters->get(i)->asCString();
				_comm_rule_map[commid] = gpName;
			}
		}
	}

	// 释放配置对象内存
	cfg->release();
	return true;
}

/**
 * @brief 获取指定品种的交易动作规则组
 * 
 * 根据品种代码查找相应的交易动作规则组
 * 如果未找到指定品种的规则组，则使用默认组
 * 
 * @param pid 品种代码
 * @return const ActionRuleGroup& 交易动作规则组引用
 */
const ActionRuleGroup& ActionPolicyMgr::getActionRules(const char* pid)
{
	// 默认使用"default"规则组
	std::string gpName = "default";

	{// 先找到品种对应的规则组名称
		auto it = _comm_rule_map.find(pid);
		if (it != _comm_rule_map.end())
			gpName = it->second; // 如果找到对应的规则组，则使用该规则组
	}

	{
		// 根据规则组名称查找规则组
		auto it = _rules.find(gpName);
		if (it == _rules.end())
		{
			// 如果指定的规则组不存在，则使用默认规则组
			it = _rules.find("default");
			WTSLogger::error("Action policy group {} not exists, changed to default group", gpName.c_str());
		}

		// 断言规则组必定存在，这是一个程序防御机制
		assert(it != _rules.end());
		return it->second; // 返回找到的规则组
	}
}
