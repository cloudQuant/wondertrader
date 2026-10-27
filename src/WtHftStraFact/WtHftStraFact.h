/*!
 * @file WtHftStraFact.h
 * @author wondertrader
 *
 * @brief 高频交易策略工厂类定义
 * @details 定义了用于创建和管理高频交易策略的工厂类
 */

#pragma once
#include "../Includes/HftStrategyDefs.h"

USING_NS_WTP;

/**
 * @brief 高频交易策略工厂类
 * @details 继承自IHftStrategyFact接口，实现高频交易策略的创建、枚举和删除功能
 */
class WtHftStraFact : public IHftStrategyFact
{
public:
	/**
	 * @brief 构造函数
	 * @details 创建一个高频交易策略工厂实例
	 */
	WtHftStraFact();

	/**
	 * @brief 析构函数
	 * @details 清理高频交易策略工厂资源
	 */
	virtual ~WtHftStraFact();

public:
	/**
	 * @brief 获取工厂名称
	 * @return 工厂名称
	 * @details 返回定义的高频交易策略工厂名称
	 */
	virtual const char* getName() override;

	/**
	 * @brief 枚举支持的策略
	 * @param cb 枚举策略的回调函数
	 * @details 通过回调函数返回工厂支持的所有策略
	 */
	virtual void enumStrategy(FuncEnumHftStrategyCallback cb) override;

	/**
	 * @brief 创建策略实例
	 * @param name 策略名称
	 * @param id 策略ID
	 * @return 创建的策略实例指针，如果策略不存在则返回NULL
	 * @details 根据策略名称和ID创建相应的高频交易策略实例
	 */
	virtual HftStrategy* createStrategy(const char* name, const char* id) override;

	/**
	 * @brief 删除策略实例
	 * @param stra 要删除的策略实例指针
	 * @return 删除是否成功
	 * @details 判断策略是否属于本工厂创建，如果是则删除该策略实例
	 */
	virtual bool deleteStrategy(HftStrategy* stra) override;
};

