/**
 * @file WtUftStraFact.h
 * @brief UFT策略工厂头文件
 * @author Wesley
 * @date 未指定
 * 
 * @details 定义UFT策略工厂类，用于创建和管理UFT策略实例
 */

#pragma once
#include "../Includes/UftStrategyDefs.h"

USING_NS_WTP;

/**
 * @brief UFT策略工厂类
 * @details 实现IUftStrategyFact接口，提供UFT策略的创建、删除、枚举等功能
 */
class WtUftStraFact : public IUftStrategyFact
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化策略工厂实例
	 */
	WtUftStraFact();

	/**
	 * @brief 虚构函数
	 * @details 清理策略工厂实例的资源
	 */
	virtual ~WtUftStraFact();

public:
	/**
	 * @brief 获取工厂名称
	 * @return 工厂名称字符串
	 * @details 返回工厂类的全局唯一标识符
	 */
	virtual const char* getName() override;

	/**
	 * @brief 枚举工厂支持的所有策略
	 * @param cb 枚举回调函数
	 * @details 调用回调函数，传入工厂名称、策略名称和状态参数，用于向调用者披露支持的策略
	 */
	virtual void enumStrategy(FuncEnumUftStrategyCallback cb) override;

	/**
	 * @brief 创建策略实例
	 * @param name 策略名称
	 * @param id 策略ID
	 * @return 创建的策略对象指针，如果策略名称不支持则返回NULL
	 * @details 根据策略名称创建相应的策略对象实例
	 */
	virtual UftStrategy* createStrategy(const char* name, const char* id) override;

	/**
	 * @brief 删除策略实例
	 * @param stra 策略对象指针
	 * @return 删除是否成功，true表示成功，false表示失败
	 * @details 安全删除策略对象并释放内存
	 */
	virtual bool deleteStrategy(UftStrategy* stra) override;
};

