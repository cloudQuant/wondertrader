/*!
 * \file WtCtaStraFact.cpp
 * \brief CTA策略工厂实现文件
 * 
 * 该文件实现了CTA策略工厂类，包括策略的创建、枚举和删除功能
 * 并提供了工厂的动态加载和卸载接口
 * 
 * \author Wesley
 */

#include "WtCtaStraFact.h"
#include "WtStraDualThrust.h"

#include <string.h>
#include <boost/config.hpp>

const char* FACT_NAME = "WtCtaStraFact";


/**
 * \brief 外部C接口函数，用于动态库导出
 * 
 * 这些函数提供了工厂的创建和删除接口，供外部程序动态加载使用
 */
extern "C"
{
	/**
	 * \brief 创建策略工厂实例
	 * 
	 * 该函数用于创建WtStraFact工厂实例，作为动态库导出函数
	 * 外部程序可以通过该函数获取工厂实例，然后创建具体的策略
	 * 
	 * \return 返回创建的策略工厂接口指针
	 */
	EXPORT_FLAG ICtaStrategyFact* createStrategyFact()
	{
		ICtaStrategyFact* fact = new WtStraFact();
		return fact;
	}

	/**
	 * \brief 删除策略工厂实例
	 * 
	 * 该函数用于删除策略工厂实例，释放相关资源
	 * 外部程序在不再需要工厂时调用该函数进行清理
	 * 
	 * \param fact 要删除的策略工厂接口指针
	 */
	EXPORT_FLAG void deleteStrategyFact(ICtaStrategyFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
};


/**
 * \brief WtStraFact类的构造函数
 * 
 * 初始化策略工厂实例
 * 当前实现为空，因为不需要特殊的初始化操作
 */
WtStraFact::WtStraFact()
{
}


/**
 * \brief WtStraFact类的析构函数
 * 
 * 清理策略工厂实例的资源
 * 当前实现为空，因为没有需要特殊清理的资源
 */
WtStraFact::~WtStraFact()
{
}

/**
 * \brief 创建策略实例
 * 
 * 根据策略名称和ID创建相应的策略实例
 * 当前支持的策略只有DualThrust
 * 
 * \param name 策略名称，如"DualThrust"
 * \param id 策略实例的唯一标识
 * \return 返回创建的策略实例指针，如果策略名称不支持则返回NULL
 */
CtaStrategy* WtStraFact::createStrategy(const char* name, const char* id)
{
	if (strcmp(name, "DualThrust") == 0)
		return new WtStraDualThrust(id);

	return NULL;
}

/**
 * \brief 删除策略实例
 * 
 * 删除指定的策略实例，释放相关资源
 * 只能删除由当前工厂创建的策略实例
 * 
 * \param stra 要删除的策略实例指针
 * \return 删除是否成功，true表示成功，false表示失败
 */
bool WtStraFact::deleteStrategy(CtaStrategy* stra)
{
	// 如果策略指针为空，直接返回成功
	if (stra == NULL)
		return true;

	// 检查策略是否由当前工厂创建
	if (strcmp(stra->getFactName(), FACT_NAME) != 0)
		return false;

	// 删除策略实例并返回成功
	delete stra;
	return true;
}

/**
 * \brief 枚举支持的策略
 * 
 * 通过回调函数返回工厂支持的所有策略名称
 * 当前工厂只支持DualThrust策略
 * 
 * \param cb 枚举策略的回调函数，用于接收策略信息
 */
void WtStraFact::enumStrategy(FuncEnumStrategyCallback cb)
{
	// 调用回调函数，传入工厂名称、策略名称和是否是最后一个策略
	cb(FACT_NAME, "DualThrust", true);
}

/**
 * \brief 获取工厂名称
 * 
 * 返回当前策略工厂的名称，用于标识该工厂
 * 
 * \return 工厂名称字符串，固定为"WtCtaStraFact"
 */
const char* WtStraFact::getName()
{
	return FACT_NAME;
}