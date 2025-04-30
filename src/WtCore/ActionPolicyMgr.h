/*!
 * \file ActionPolicyMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 行为策略管理器头文件
 * \details 该文件定义了行为策略管理器，用于管理交易行为的规则和策略。
 *          行为策略管理器负责加载和管理交易动作的规则，如开仓、平仓等动作的限制条件。
 *          通过这些规则，可以对交易行为进行精细化控制，实现风险管理。
 */
#pragma once
#include <vector>
#include <stdint.h>
#include <string.h>

#include "../Includes/FasterDefs.h"


NS_WTP_BEGIN
class WTSVariant;

/**
 * @brief 交易动作类型枚举
 * @details 定义了交易系统中支持的各种交易动作类型，包括开仓、平仓、平今和平昨等
 */
typedef enum tagActionType
{
	AT_Unknown = 8888,   ///< 未知动作类型，用于初始化或错误状态
	AT_Open = 9999,		///< 开仓动作，建立新的头寸
	AT_Close,			///< 平仓动作，不区分今仓和昨仓
	AT_CloseToday,		///< 平今仓动作，专门平掉当天建立的头寸
	AT_CloseYestoday	///< 平昨仓动作，专门平掉昨天及更早建立的头寸
} ActionType;

/**
 * @brief 交易动作规则结构体
 * @details 定义了交易动作的规则，包括动作类型、手数限制等信息
 *          这些规则用于控制交易行为，实现风险管理
 */
typedef struct _ActionRule
{
	ActionType	_atype;		///< 动作类型，指定该规则适用于哪种交易动作
	uint32_t	_limit;		///< 总手数限制，限制该动作的总交易量
	uint32_t	_limit_l;	///< 多头手数限制，限制多头方向的交易量
	uint32_t	_limit_s;	///< 空头手数限制，限制空头方向的交易量
	bool		_pure;		///< 纯净标志，主要针对AT_CloseToday和AT_CloseYestoday，用于判断是否是净今仓或者净昨仓

	/**
	 * @brief 构造函数
	 * @details 初始化规则结构体，将所有成员设置为0
	 */
	_ActionRule()
	{
		memset(this, 0, sizeof(_ActionRule));
	}
} ActionRule;

/**
 * @brief 交易动作规则组类型
 * @details 定义了交易动作规则的集合，用于管理多个相关的交易动作规则
 */
typedef std::vector<ActionRule>	ActionRuleGroup;

/**
 * @brief 行为策略管理器类
 * @details 管理交易动作的规则和策略，负责加载和管理交易动作的限制条件
 *          通过这些规则，可以对交易行为进行精细化控制，实现风险管理
 */
class ActionPolicyMgr
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化行为策略管理器对象
	 */
	ActionPolicyMgr();

	/**
	 * @brief 析构函数
	 * @details 清理行为策略管理器对象的资源
	 */
	~ActionPolicyMgr();

public:
	/**
	 * @brief 初始化函数
	 * @details 从指定的配置文件中加载交易动作的规则和策略
	 * @param filename 配置文件名称
	 * @return 初始化是否成功，成功返回true，失败返回false
	 */
	bool init(const char* filename);

	/**
	 * @brief 获取交易品种的动作规则组
	 * @details 根据品种标识符获取对应的交易动作规则组
	 * @param pid 品种标识符
	 * @return 交易动作规则组的常量引用
	 */
	const ActionRuleGroup& getActionRules(const char* pid);

private:
	/**
	 * @brief 规则映射类型
	 * @details 定义了从品种规则名称到规则组的映射
	 */
	typedef wt_hashmap<std::string, ActionRuleGroup> RulesMap;

	RulesMap	_rules;	///< 规则表，存储所有的规则组

	wt_hashmap<std::string, std::string> _comm_rule_map;	///< 品种规则映射，将品种映射到对应的规则名称
};

NS_WTP_END
