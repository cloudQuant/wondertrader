/**
 * @file WtUftStraFact.cpp
 * @brief UFT策略工厂实现文件
 * @author Wesley
 * @date 未指定
 * 
 * @details 实现UFT策略工厂类，负责建立和管理UFT策略对象
 */

#include "WtUftStraFact.h"
#include "WtUftStraDemo.h"

#include <string.h>

/**
 * @brief 工厂名称常量
 * @details 定义工厂的唯一标识名称，用于策略实例的注册和查找
 */
const char* FACT_NAME = "WtUftStraFact";

/**
 * @brief 导出的C接口函数块
 * @details 对外导出工厂的创建和删除函数，供动态加载库使用
 */
extern "C"
{
	/**
	 * @brief 创建策略工厂实例的导出函数
	 * @return 新创建的策略工厂接口指针
	 * @details 为动态加载库提供的工厂创建函数，返回接口指针
	 */
	EXPORT_FLAG IUftStrategyFact* createStrategyFact()
	{
		IUftStrategyFact* fact = new WtUftStraFact();
		return fact;
	}

	/**
	 * @brief 删除策略工厂实例的导出函数
	 * @param fact 要删除的工厂实例指针
	 * @details 为动态加载库提供的工厂删除函数，释放工厂实例占用的内存
	 */
	EXPORT_FLAG void deleteStrategyFact(IUftStrategyFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
}


/**
 * @brief 策略工厂类的构造函数
 * @details 初始化策略工厂实例
 */
WtUftStraFact::WtUftStraFact()
{
	// 当前无需在构造函数中执行特定的初始化操作
}


/**
 * @brief 策略工厂类的析构函数
 * @details 清理策略工厂实例的资源
 */
WtUftStraFact::~WtUftStraFact()
{
	// 当前无需在析构函数中执行特定的清理操作
}

/**
 * @brief 获取工厂名称
 * @return 工厂名称字符串
 * @details 返回工厂类的全局唯一标识符
 */
const char* WtUftStraFact::getName()
{
	// 返回工厂的静态名称
	return FACT_NAME;
}

/**
 * @brief 枚举工厂支持的所有策略
 * @param cb 枚举回调函数
 * @details 调用回调函数，传入工厂名称、策略名称和状态参数，用于向调用者披露支持的策略
 */
void WtUftStraFact::enumStrategy(FuncEnumUftStrategyCallback cb)
{
	// 调用回调函数，传入工厂名称、策略名称、状态
	// 参数含义：工厂名称、策略名称、策略是否可用
	cb(FACT_NAME, "SimpleUft", true);
}

/**
 * @brief 创建策略实例
 * @param name 策略名称
 * @param id 策略ID
 * @return 创建的策略对象指针，如果策略名称不支持则返回NULL
 * @details 根据策略名称创建相应的策略对象实例，当前仅支持"SimpleUft"策略
 */
UftStrategy* WtUftStraFact::createStrategy(const char* name, const char* id)
{
	// 如果策略名称是"SimpleUft"，创建一个WtUftStraDemo实例
	if(strcmp(name, "SimpleUft") == 0)
	{
		return new WtUftStraDemo(id);
	}

	// 如果策略名称不支持，返回NULL
	return NULL;
}

/**
 * @brief 删除策略实例
 * @param stra 策略对象指针
 * @return 删除是否成功，true表示成功，false表示失败
 * @details 判断策略对象是否为空或者工厂名称是否匹配，然后安全删除对象
 */
bool WtUftStraFact::deleteStrategy(UftStrategy* stra)
{
	// 如果策略对象为空，直接返回成功
	if (stra == NULL)
		return true;

	// 检查策略对象的工厂名称是否与当前工厂匹配
	if (strcmp(stra->getFactName(), FACT_NAME) != 0)
		return false;

	// 删除策略对象并释放内存
	delete stra;
	return true;
}
