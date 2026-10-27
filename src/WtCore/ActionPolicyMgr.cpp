/*!
 * \file ActionPolicyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行为策略管理器实现文件
 * \details 该文件实现了行为策略管理器，用于加载和管理交易动作的规则和策略。
 *          通过这些规则，可以对交易行为进行精细化控制，实现风险管理。
 */
#include "ActionPolicyMgr.h"

#include "../Share/StdUtils.hpp"
#include "../WTSTools/WTSLogger.h"

#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

USING_NS_WTP;

/**
 * @brief 行为策略管理器构造函数
 * @details 初始化行为策略管理器对象
 */
ActionPolicyMgr::ActionPolicyMgr()
{
	// 构造函数不需要额外的初始化操作
}


/**
 * @brief 行为策略管理器析构函数
 * @details 清理行为策略管理器对象的资源
 */
ActionPolicyMgr::~ActionPolicyMgr()
{
	// 析构函数不需要额外的清理操作，因为所有成员变量都会自动清理
}

/**
 * @brief 初始化行为策略管理器
 * @details 从指定的配置文件中加载交易动作的规则和策略
 * @param filename 配置文件名称
 * @return 初始化是否成功，成功返回true，失败返回false
 */
bool ActionPolicyMgr::init(const char* filename)
{
	// 从文件中加载配置
	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename);
	if (cfg == NULL)
		return false;

	// 遍历配置中的所有规则组
	auto keys = cfg->memberNames();
	for (auto it = keys.begin(); it != keys.end(); it++)
	{
		// 获取规则组名称
		const char* gpName = (*it).c_str();
		// 获取规则组配置
		WTSVariant*	vGpItem = cfg->get(gpName);
		// 创建或获取规则组
		ActionRuleGroup& gp = _rules[gpName];

		// 加载订单动作规则
		WTSVariant* vOrds = vGpItem->get("order");
		if(vOrds != NULL && vOrds->isArray())
		{
			// 遍历所有订单动作规则
			for (uint32_t i = 0; i < vOrds->size(); i++)
			{
				WTSVariant* vObj = vOrds->get(i);
				ActionRule aRule;
				
				// 解析动作类型和限制值
				const char* action = vObj->getCString("action");
				uint32_t uLimit = vObj->getUInt32("limit");
				uint32_t uLimitS = vObj->getUInt32("limit_s");
				uint32_t uLimitL = vObj->getUInt32("limit_l");
				
				// 根据动作名称设置动作类型
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
					// 如果动作类型不识别，记录错误并跳过当前规则
					WTSLogger::error("Loading action policy failed: unrecognized type {}", action);
					continue;
				}

				// 设置规则的限制值
				aRule._limit = uLimit;
				aRule._limit_s = uLimitS;
				aRule._limit_l = uLimitL;
				aRule._pure = vObj->getBoolean("pure");
				
				// 将规则添加到规则组中
				gp.emplace_back(aRule);
			}
		}

		// 加载品种过滤器
		WTSVariant* filters = vGpItem->get("filters");
		if(filters!=NULL && filters->isArray() && filters->size()>0)
		{
			// 遍历所有品种过滤器
			for (uint32_t i = 0; i < filters->size(); i++)
			{
				// 将品种映射到对应的规则组
				const char* commid = filters->get(i)->asCString();
				_comm_rule_map[commid] = gpName;
			}
		}
	}

	// 释放配置对象
	cfg->release();
	return true;
}

/**
 * @brief 获取交易品种的动作规则组
 * @details 根据品种标识符获取对应的交易动作规则组，如果找不到将使用默认规则组
 * @param pid 品种标识符
 * @return 交易动作规则组的常量引用
 */
const ActionRuleGroup& ActionPolicyMgr::getActionRules(const char* pid)
{
	// 默认使用"default"规则组
	std::string gpName = "default";

	{// 先找到品种对应的规则组名称
		// 在品种规则映射中查找指定的品种
		auto it = _comm_rule_map.find(pid);
		// 如果找到了，则使用对应的规则组名称
		if (it != _comm_rule_map.end())
			gpName = it->second;
	}

	{
		// 在规则表中查找规则组
		auto it = _rules.find(gpName);
		// 如果找不到指定的规则组，则使用默认规则组
		if (it == _rules.end())
		{
			it = _rules.find("default");
			// 记录错误日志
			WTSLogger::error("Action policy group {} not exists, changed to default group", gpName.c_str());
		}

		// 断言默认规则组必须存在
		assert(it != _rules.end());
		// 返回规则组
		return it->second;
	}
}
