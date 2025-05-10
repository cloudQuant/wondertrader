/**
 * @file WtSelStraFact.h
 * @brief 选股策略工厂类头文件
 * 
 * @details 该文件定义了选股策略工厂类，负责创建、管理和枚举选股策略
 */

#pragma once
#include "../Includes/SelStrategyDefs.h"

USING_NS_WTP;

/**
 * @brief 选股策略工厂类
 * 
 * @details 实现了ISelStrategyFact接口，负责选股策略的创建、枚举和删除
 * 当前支持的策略：DualThrustSelection
 */
class WtSelStraFact : public ISelStrategyFact
{
public:
	/**
	 * @brief 构造函数
	 */
	WtSelStraFact();

	/**
	 * @brief 析构函数
	 */
	virtual ~WtSelStraFact();

public:
	/**
	 * @brief 获取工厂名称
	 * @return 返回工厂名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 创建选股策略
	 * @param name 策略名称
	 * @param id 策略ID
	 * @return 返回创建的策略对象指针，如果策略名称不支持则返回NULL
	 */
	virtual SelStrategy* createStrategy(const char* name, const char* id) override;

	/**
	 * @brief 枚举支持的所有策略
	 * @param cb 回调函数，用于接收策略信息
	 */
	virtual void enumStrategy(FuncEnumSelStrategyCallback cb) override;

	/**
	 * @brief 删除策略
	 * @param stra 要删除的策略对象指针
	 * @return 是否成功删除策略
	 */
	virtual bool deleteStrategy(SelStrategy* stra) override;
};

