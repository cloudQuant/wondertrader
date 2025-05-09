/*!
 * \file ActionPolicyMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易动作策略管理器
 *
 * 该文件定义了WonderTrader中的交易动作策略管理器，用于管理不同品种的交易动作规则，
 * 实现对开平仓动作的精细化控制，包括数量限制、多空方向限制等功能。
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
 * 
 * 定义了交易系统中支持的各种交易动作类型，包括开仓、平仓、平今和平昨
 */
typedef enum tagActionType
{
	AT_Unknown = 8888,    /**< 未知动作类型 */
	AT_Open = 9999,		/**< 开仓动作 */
	AT_Close,			/**< 平仓动作（不区分平今平昨） */
	AT_CloseToday,		/**< 平今仓动作 */
	AT_CloseYestoday	/**< 平昨仓动作 */
} ActionType;

/**
 * @brief 交易动作规则结构体
 * 
 * 定义了一条交易动作规则，包括动作类型、数量限制、方向限制等
 */
typedef struct _ActionRule
{
	ActionType	_atype;		/**< 动作类型，如开仓、平仓等 */
	uint32_t	_limit;		/**< 总手数限制，不区分方向 */
	uint32_t	_limit_l;	/**< 多头方向手数限制 */
	uint32_t	_limit_s;	/**< 空头方向手数限制 */
	bool		_pure;		/**< 是否为纯净模式，主要针对AT_CloseToday和AT_CloseYestoday，用于判断是否是净今仓或者净昨仓 */

	/**
	 * @brief 构造函数
	 * 
	 * 将结构体所有成员初始化为0
	 */
	_ActionRule()
	{
		memset(this, 0, sizeof(_ActionRule));
	}
} ActionRule;

/**
 * @brief 交易动作规则组
 * 
 * 定义了一组交易动作规则，一个交易品种或品种组可以关联一个规则组
 */
typedef std::vector<ActionRule>	ActionRuleGroup;

/**
 * @brief 交易动作策略管理器
 * 
 * 管理交易系统中不同品种的交易动作规则，负责从配置文件加载规则
 * 并为交易执行器提供对应品种的交易动作规则
 */
class ActionPolicyMgr
{
public:
	/**
	 * @brief 构造函数
	 */
	ActionPolicyMgr();

	/**
	 * @brief 析构函数
	 */
	~ActionPolicyMgr();

public:
	/**
	 * @brief 初始化动作策略管理器
	 * 
	 * 从指定的配置文件中加载交易动作规则
	 * 
	 * @param filename 配置文件路径
	 * @return bool 初始化是否成功
	 */
	bool init(const char* filename);

	/**
	 * @brief 获取指定品种的交易动作规则组
	 * 
	 * 根据品种代码查找相应的交易动作规则组，如果未找到则返回默认组
	 * 
	 * @param pid 品种代码
	 * @return const ActionRuleGroup& 交易动作规则组引用
	 */
	const ActionRuleGroup& getActionRules(const char* pid);

private:
	/** @brief 规则组映射表类型定义 */
	typedef wt_hashmap<std::string, ActionRuleGroup> RulesMap;
	
	RulesMap	_rules;	/**< 规则组映射表，键为规则组名称，值为规则组 */

	wt_hashmap<std::string, std::string> _comm_rule_map;	/**< 品种与规则组映射表，键为品种代码，值为规则组名称 */
};

NS_WTP_END
