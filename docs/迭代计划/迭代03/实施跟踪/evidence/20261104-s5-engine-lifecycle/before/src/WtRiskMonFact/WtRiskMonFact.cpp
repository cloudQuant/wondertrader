/*!
 * \file WtRiskMonFact.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 风控模块工厂实现文件
 * \details 该文件实现了WonderTrader的风控模块工厂类，用于创建和管理风控模块。
 *          包括工厂类的实现以及导出的C接口函数。
 */
#include "WtRiskMonFact.h"
#include "WtSimpRiskMon.h"

/**
 * @brief 风控模块工厂名称
 * @details 定义了风控模块工厂的名称常量，用于标识该工厂
 */
const char* FACT_NAME = "WtRiskMonFact";

/**
 * @brief C接口函数定义
 * @details 定义了导出的C接口函数，用于动态加载风控模块工厂
 */
extern "C"
{
	/**
	 * @brief 创建风控模块工厂的导出函数
	 * @details 创建并返回一个新的风控模块工厂实例
	 * @return 新创建的风控模块工厂接口指针
	 */
	EXPORT_FLAG IRiskMonitorFact* createRiskMonFact()
	{
		IRiskMonitorFact* fact = new WtRiskMonFact();
		return fact;
	}

	/**
	 * @brief 删除风控模块工厂的导出函数
	 * @details 删除并释放指定的风控模块工厂实例
	 * @param fact 要删除的风控模块工厂接口指针
	 */
	EXPORT_FLAG void deleteRiskMonFact(IRiskMonitorFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
}


/**
 * @brief 风控模块工厂类的构造函数
 * @details 初始化风控模块工厂对象
 */
WtRiskMonFact::WtRiskMonFact()
{
	// 构造函数不需要额外的初始化操作
}


/**
 * @brief 风控模块工厂类的析构函数
 * @details 清理风控模块工厂对象的资源
 */
WtRiskMonFact::~WtRiskMonFact()
{
	// 析构函数不需要额外的清理操作
}

/**
 * @brief 获取工厂名称
 * @details 返回风控模块工厂的名称，用于标识工厂
 * @return 工厂名称字符串
 */
const char* WtRiskMonFact::getName()
{
	// 返回工厂名称常量
	return FACT_NAME;
}

/**
 * @brief 枚举风控模块
 * @details 通过回调函数枚举当前工厂支持的所有风控模块
 * @param cb 枚举回调函数，用于接收枚举结果
 */
void WtRiskMonFact::enumRiskMonitors(FuncEnumRiskMonCallback cb)
{
	// 注释掉的代码是不再使用的风控模块
	//cb(FACT_NAME, "WtSimpExeUnit", false);
	
	// 调用回调函数返回当前支持的风控模块
	// 参数为：工厂名称、模块名称、是否是最后一个模块
	cb(FACT_NAME, "SimpleRiskMon", true);
}

/**
 * @brief 创建风控模块
 * @details 根据指定的名称创建一个新的风控模块实例
 * @param name 风控模块名称
 * @return 新创建的风控模块对象指针，如果创建失败则返回NULL
 */
WtRiskMonitor* WtRiskMonFact::createRiskMonotor(const char* name)
{
	// 判断要创建的风控模块类型
	if (strcmp(name, "SimpleRiskMon") == 0)
		// 创建简单风控模块
		return new WtSimpleRiskMon();
	
	// 如果模块名称不匹配，返回NULL
	return NULL;
}

/**
 * @brief 删除风控模块
 * @details 删除并释放指定的风控模块实例
 * @param unit 要删除的风控模块对象指针
 * @return 删除是否成功，成功返回true，失败返回false
 */
bool WtRiskMonFact::deleteRiskMonotor(WtRiskMonitor* unit)
{
	// 如果传入的指针为空，直接返回true
	if (unit == NULL)
		return true;

	// 检查风控模块是否属于当前工厂
	// 如果不属于当前工厂，返回false
	if (strcmp(unit->getFactName(), FACT_NAME) != 0)
		return false;

	// 删除风控模块对象
	delete unit;
	return true;
}
