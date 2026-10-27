/*!
 * \file WtExeFact.cpp
 * \brief 执行单元工厂类实现文件
 *
 * 该文件实现了执行单元工厂类(WtExeFact)，负责创建、枚举和管理各种类型的执行单元
 * 
 * \author Wesley
 * \date 2020/03/30
 */
#include "WtExeFact.h"

#include "WtTWapExeUnit.h"
#include "WtMinImpactExeUnit.h"
#include "WtDiffMinImpactExeUnit.h"
#include "WtStockMinImpactExeUnit.h"
#include "WtVWapExeUnit.h"
#include "WtStockVWapExeUnit.h"
/**
 * @brief 工厂名称常量
 */
const char* FACT_NAME = "WtExeFact";

/**
 * @brief C接口函数区
 */
extern "C"
{
	/**
	 * @brief 创建执行单元工厂的导出函数
	 * @return 新创建的执行单元工厂实例
	 */
	EXPORT_FLAG IExecuterFact* createExecFact()
	{
		IExecuterFact* fact = new WtExeFact();
		return fact;
	}

	/**
	 * @brief 删除执行单元工厂的导出函数
	 * @param fact 要删除的执行单元工厂实例
	 */
	EXPORT_FLAG void deleteExecFact(IExecuterFact* fact)
	{
		if (fact != NULL)
			delete fact;
	}
};


/**
 * @brief 构造函数
 */
WtExeFact::WtExeFact()
{
}


/**
 * @brief 析构函数
 */
WtExeFact::~WtExeFact()
{
}

/**
 * @brief 获取工厂名称
 * @return 返回工厂名称常量
 */
const char* WtExeFact::getName()
{
	return FACT_NAME;
}

/**
 * @brief 枚举所有支持的执行单元
 * @details 通过回调函数将所支持的执行单元列表提供给上层应用
 * @param cb 回调函数，包含工厂名称、单元名称和是否是特殊单元信息
 */
void WtExeFact::enumExeUnit(FuncEnumUnitCallback cb)
{
	cb(FACT_NAME, "WtTWapExeUnit", false);
	cb(FACT_NAME, "WtMinImpactExeUnit", true);
}

/**
 * @brief 创建普通执行单元
 * @details 根据指定的名称创建相应类型的执行单元实例
 * @param name 执行单元名称，支持WtTWapExeUnit, WtMinImpactExeUnit等
 * @return 创建的执行单元指针，如果不支持该名称则返回NULL
 */
ExecuteUnit* WtExeFact::createExeUnit(const char* name)
{
	if (strcmp(name, "WtTWapExeUnit") == 0)
		return new WtTWapExeUnit();
	else if (strcmp(name, "WtMinImpactExeUnit") == 0)
		return new WtMinImpactExeUnit();
	else if (strcmp(name, "WtStockMinImpactExeUnit") == 0)
		return new WtStockMinImpactExeUnit();
	else if (strcmp(name, "WtVWapExeUnit") == 0)
		return  new WtVWapExeUnit();
	else if (strcmp(name, "WtStockVWapExeUnit") == 0)
		return new WtStockVWapExeUnit();
	return NULL;
}

/**
 * @brief 创建差量执行单元
 * @details 根据指定的名称创建差量执行单元实例，当前仅支持WtDiffMinImpactExeUnit
 * @param name 执行单元名称
 * @return 创建的差量执行单元指针，如果不支持该名称则返回NULL
 */
ExecuteUnit* WtExeFact::createDiffExeUnit(const char* name)
{
	if (strcmp(name, "WtDiffMinImpactExeUnit") == 0)
		return new WtDiffMinImpactExeUnit();

	return NULL;
}

/**
 * @brief 创建套利执行单元
 * @details 根据指定的名称创建套利执行单元实例，当前暂不支持任何套利执行单元
 * @param name 执行单元名称
 * @return 创建的套利执行单元指针，当前始终返回NULL
 */
ExecuteUnit* WtExeFact::createArbiExeUnit(const char* name)
{
	return NULL;
}

/**
 * @brief 删除执行单元
 * @details 对指定的执行单元进行安全删除，首先检查单元是否为NULL和是否由本工厂创建
 * @param unit 要删除的执行单元指针
 * @return 删除是否成功：true-成功删除或单元为NULL，false-单元不属于本工厂创建
 */
bool WtExeFact::deleteExeUnit(ExecuteUnit* unit)
{
	if (unit == NULL)
		return true;

	if (strcmp(unit->getFactName(), FACT_NAME) != 0)
		return false;

	delete unit;
	return true;
}
