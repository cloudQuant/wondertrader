/*!
 * \file WtExeFact.h
 * \brief 执行单元工厂类头文件
 *
 * 该文件定义了执行单元工厂类(WtExeFact)，负责创建、枚举和管理各种类型的执行单元
 * 
 * \author Wesley
 * \date 2020/03/30
 */
#pragma once
#include "../Includes/ExecuteDefs.h"

USING_NS_WTP;

/**
 * @brief 执行单元工厂类
 * @details 负责创建、枚举和管理各种类型的交易执行单元，包括普通执行单元、差量执行单元和套利执行单元
 */
class WtExeFact : public IExecuterFact
{
public:
	/**
	 * @brief 构造函数
	 */
	WtExeFact();

	/**
	 * @brief 析构函数
	 */
	virtual ~WtExeFact();

public:
	/**
	 * @brief 获取工厂名称
	 * @return 返回工厂名称
	 */
	virtual const char* getName() override;
	/**
	 * @brief 枚举所有支持的执行单元
	 * @param cb 回调函数，用于处理枚举的执行单元信息
	 */
	virtual void enumExeUnit(FuncEnumUnitCallback cb) override;

	/**
	 * @brief 创建普通执行单元
	 * @param name 执行单元名称
	 * @return 创建的执行单元指针，如果不支持该名称则返回NULL
	 */
	virtual ExecuteUnit* createExeUnit(const char* name) override;

	/**
	 * @brief 创建差量执行单元
	 * @param name 执行单元名称
	 * @return 创建的差量执行单元指针，如果不支持该名称则返回NULL
	 */
	virtual ExecuteUnit* createDiffExeUnit(const char* name) override;

	/**
	 * @brief 创建套利执行单元
	 * @param name 执行单元名称
	 * @return 创建的套利执行单元指针，如果不支持该名称则返回NULL
	 */
	virtual ExecuteUnit* createArbiExeUnit(const char* name) override;

	/**
	 * @brief 删除执行单元
	 * @param unit 要删除的执行单元指针
	 * @return 删除是否成功，如果单元为NULL或者不属于本工厂创建则返回false
	 */
	virtual bool deleteExeUnit(ExecuteUnit* unit) override;

};

