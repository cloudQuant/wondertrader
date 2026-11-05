/*!
 * @file WtHftStraFact.cpp
 * @author wondertrader
 *
 * @brief 高频交易策略工厂类实现
 * @details 实现了高频交易策略的创建、枚举和管理功能
 */

#include "WtHftStraFact.h"
#include "WtHftStraDemo.h"

#include <string.h>

/** 高频交易策略工厂名称 */
const char* FACT_NAME = "WtHftStraFact";

/**
 * @brief 导出的C接口函数，用于动态加载库
 */
extern "C"
{
	/**
	 * @brief 创建策略工厂实例的导出函数
	 * @return 新创建的策略工厂实例
	 * @details 由主框架在动态加载库时调用，创建高频交易策略工厂实例
	 */
	EXPORT_FLAG IHftStrategyFact* createStrategyFact()
	{
		IHftStrategyFact* fact = new WtHftStraFact();
		return fact;
	}

	/**
	 * @brief 删除策略工厂实例的导出函数
	 * @param fact 要删除的策略工厂实例
	 * @details 由主框架在卸载动态库时调用，清理高频交易策略工厂实例
	 */
	EXPORT_FLAG void deleteStrategyFact(IHftStrategyFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
}


/**
 * @brief 高频交易策略工厂类构造函数
 * @details 初始化策略工厂实例
 */
WtHftStraFact::WtHftStraFact()
{
}


/**
 * @brief 高频交易策略工厂类析构函数
 * @details 清理工厂资源
 */
WtHftStraFact::~WtHftStraFact()
{
}

/**
 * @brief 获取工厂名称
 * @return 工厂名称
 * @details 返回定义的高频交易策略工厂名称
 */
const char* WtHftStraFact::getName()
{
	return FACT_NAME;
}

/**
 * @brief 枚举支持的策略
 * @param cb 枚举策略的回调函数
 * @details 向主框架提供工厂支持的策略列表，当前支持SimpleHft策略
 */
void WtHftStraFact::enumStrategy(FuncEnumHftStrategyCallback cb)
{
	// 注册SimpleHft策略，参数分别为：工厂名称、策略名称、是否启用
	cb(FACT_NAME, "SimpleHft", true);
}

/**
 * @brief 创建策略实例
 * @param name 策略名称
 * @param id 策略ID
 * @return 创建的策略实例指针，如果策略不存在则返回NULL
 * @details 根据策略名称和ID创建相应的高频交易策略实例
 */
HftStrategy* WtHftStraFact::createStrategy(const char* name, const char* id)
{
	// 创建 SimpleHft 策略
	if(strcmp(name, "SimpleHft") == 0)
	{
		return new WtHftStraDemo(id);
	}

	// 如果策略名称不匹配，返回 NULL
	return NULL;
}

/**
 * @brief 删除策略实例
 * @param stra 要删除的策略实例指针
 * @return 删除是否成功
 * @details 判断策略是否属于本工厂创建，如果是则删除该策略实例
 */
bool WtHftStraFact::deleteStrategy(HftStrategy* stra)
{
	// 如果要删除的策略为空，直接返回成功
	if (stra == NULL)
		return true;

	// 验证策略是否属于本工厂创建
	if (strcmp(stra->getFactName(), FACT_NAME) != 0)
		return false;

	// 删除策略实例
	delete stra;
	return true;
}
