/*!
 * \file WtCtaStraFact.h
 * \brief CTA策略工厂定义文件
 * 
 * 该文件定义了CTA策略工厂类，用于创建、枚举和删除CTA策略
 * 工厂模式的实现，用于策略的动态加载和管理
 * 
 * \author Wesley
 */

#pragma once
#include "../Includes/CtaStrategyDefs.h"

USING_NS_WTP;

/**
 * \brief CTA策略工厂类
 * 
 * 实现ICtaStrategyFact接口，用于创建、枚举和删除CTA策略
 * 该工厂类包含多个策略，如DualThrust策略
 */
class WtStraFact : public ICtaStrategyFact
{
public:
	/**
	 * \brief 构造函数
	 */
	WtStraFact();

	/**
	 * \brief 析构函数
	 */
	virtual ~WtStraFact();

public:
	/**
	 * \brief 获取工厂名称
	 * 
	 * 返回当前策略工厂的名称，用于标识该工厂
	 * 
	 * \return 工厂名称字符串，固定为"WtCtaStraFact"
	 */
	virtual const char* getName() override;

	/**
	 * \brief 创建策略
	 * 
	 * 根据策略名称和ID创建相应的策略实例
	 * 当前支持的策略包括：DualThrust
	 * 
	 * \param name 策略名称，如"DualThrust"
	 * \param id 要创建的策略对象的ID，用于在系统中唯一标识该策略实例
	 * \return 返回创建的策略对象指针，如果策略名称不支持则返回NULL
	 */
	virtual CtaStrategy* createStrategy(const char* name, const char* id) override;

	/**
	 * \brief 枚举策略名称
	 * 
	 * 通过回调函数返回工厂支持的所有策略名称
	 * 
	 * \param cb 枚举策略名称的回调函数，用于接收策略信息
	 */
	virtual void enumStrategy(FuncEnumStrategyCallback cb) override;

	/**
	 * \brief 删除策略
	 * 
	 * 删除指定的策略对象，释放相关资源
	 * 只能删除由当前工厂创建的策略对象
	 * 
	 * \param stra 要删除的策略对象指针
	 * \return 删除是否成功，true表示成功，false表示失败
	 */
	virtual bool deleteStrategy(CtaStrategy* stra) override;	
};

