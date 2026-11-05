/**
 * @file WtSelStraFact.cpp
 * @brief 选股策略工厂类实现
 * 
 * @details 实现了选股策略工厂类的各个方法，负责选股策略的创建、枚举和删除
 */

#include "WtSelStraFact.h"
#include "WtStraDtSel.h"

#include <string.h>
#include <boost/config.hpp>

/** 工厂名称常量 */
const char* FACT_NAME = "WtSelStraFact";

/**
 * @brief 外部C接口定义
 * 
 * @details 提供给外部调用的C接口，用于创建和删除选股策略工厂实例
 */
extern "C"
{
	/**
	 * @brief 创建选股策略工厂的外部接口
	 * @return 返回新创建的选股策略工厂实例
	 */
	EXPORT_FLAG ISelStrategyFact* createSelStrategyFact()
	{
		ISelStrategyFact* fact = new WtSelStraFact();
		return fact;
	}

	/**
	 * @brief 删除选股策略工厂的外部接口
	 * @param fact 要删除的选股策略工厂实例
	 */
	EXPORT_FLAG void deleteSelStrategyFact(ISelStrategyFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
};

/**
 * @brief 选股策略工厂类构造函数
 */
WtSelStraFact::WtSelStraFact()
{
	// 构造函数为空，无需初始化
}

/**
 * @brief 选股策略工厂类析构函数
 */
WtSelStraFact::~WtSelStraFact()
{
	// 析构函数为空，无需清理资源
}

/**
 * @brief 创建选股策略
 * @param name 策略名称
 * @param id 策略ID
 * @return 返回创建的策略对象指针，如果策略名称不支持则返回NULL
 * 
 * @details 根据策略名称创建相应的选股策略实例，当前支持DualThrustSelection策略
 */
SelStrategy* WtSelStraFact::createStrategy(const char* name, const char* id)
{
	// 判断是否为DualThrustSelection策略
	if (strcmp(name, "DualThrustSelection") == 0)
		return new WtStraDtSel(id);

	// 不支持的策略名称返回NULL
	return NULL;
}

/**
 * @brief 删除策略
 * @param stra 要删除的策略对象指针
 * @return 是否成功删除策略
 * 
 * @details 判断要删除的策略是否为空或是否由本工厂创建，如果是则进行删除
 */
bool WtSelStraFact::deleteStrategy(SelStrategy* stra)
{
	// 策略对象为空，直接返回成功
	if (stra == NULL)
		return true;

	// 如果策略不是由本工厂创建的，返回失败
	if (strcmp(stra->getFactName(), FACT_NAME) != 0)
		return false;

	// 删除策略并返回成功
	delete stra;
	return true;
}

/**
 * @brief 枚举支持的所有策略
 * @param cb 回调函数，用于接收策略信息
 * 
 * @details 调用回调函数列举所有支持的策略，包括工厂名称、策略名称和是否可用
 */
void WtSelStraFact::enumStrategy(FuncEnumSelStrategyCallback cb)
{
	// 列举工厂支持的DualThrustSelection策略
	cb(FACT_NAME, "DualThrustSelection", true);
}

/**
 * @brief 获取工厂名称
 * @return 返回工厂名称
 * 
 * @details 返回选股策略工厂的唯一标识名称
 */
const char* WtSelStraFact::getName()
{
	// 返回工厂名称常量
	return FACT_NAME;
}


//////////////////////////////////////////////////////////////////////////
// Versioned strategy plugin entry (iteration 03, card 4b=A). The legacy
// exports above stay for older hosts; current hosts use only this table.
// The factory and its strategies start no asynchronous work, so the plugin
// advertises synchronous teardown (card 9a).
#include "../Includes/IStrategyPluginV2.h"

namespace
{
	void* WT_STRA_PLUGIN_V2_CALL create_factory_v2()
	{
		try
		{
			ISelStrategyFact* fact = new WtSelStraFact();
			return static_cast<void*>(fact);
		}
		catch (...)
		{
			return NULL;
		}
	}

	void WT_STRA_PLUGIN_V2_CALL destroy_factory_v2(void* fact)
	{
		delete static_cast<ISelStrategyFact*>(fact);
	}

	const WtStrategyPluginApiV2 g_strategy_plugin_api_v2 = {
		sizeof(WtStrategyPluginApiV2),
		WT_STRA_PLUGIN_V2_ABI_VERSION,
		WT_STRA_PLUGIN_V2_FAMILY_SEL,
		WT_STRA_PLUGIN_V2_OBJECT_REVISION,
		WT_STRA_PLUGIN_V2_CAP_SYNC_TEARDOWN,
		{ 0, 0, 0 },
		create_factory_v2,
		destroy_factory_v2
	};
}

WT_STRA_PLUGIN_V2_EXTERN_C WT_STRA_PLUGIN_V2_EXPORT
const WtStrategyPluginApiV2* WT_STRA_PLUGIN_V2_CALL getSelStrategyPluginApiV2(uint32_t requested_major)
{
	return requested_major == WT_STRA_PLUGIN_V2_ABI_MAJOR ? &g_strategy_plugin_api_v2 : NULL;
}
