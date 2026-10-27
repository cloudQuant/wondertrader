/*!
 * \file WtRiskMonFact.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 *
 * \brief 风控模块工厂头文件
 * \details 该文件定义了WonderTrader的风控模块工厂类，用于创建和管理风控模块。
 *          风控模块负责监控交易风险，实现资金风控等功能。
 */
#pragma once
#include "../Includes/RiskMonDefs.h"

USING_NS_WTP;

/**
 * @brief 风控模块工厂类
 * @details 实现了IRiskMonitorFact接口，用于创建和管理风控模块。
 *          该工厂类负责实例化具体的风控模块，并管理其生命周期。
 */
class WtRiskMonFact : public IRiskMonitorFact
{
public:
	/**
	 * @brief 构造函数
	 * @details 初始化风控模块工厂对象
	 */
	WtRiskMonFact();

	/**
	 * @brief 析构函数
	 * @details 清理风控模块工厂对象的资源
	 */
	virtual ~WtRiskMonFact();

public:
	/**
	 * @brief 获取工厂名称
	 * @details 返回风控模块工厂的名称，用于标识工厂
	 * @return 工厂名称字符串
	 */
	virtual const char* getName() override;

	/**
	 * @brief 枚举风控模块
	 * @details 通过回调函数枚举当前工厂支持的所有风控模块
	 * @param cb 枚举回调函数，用于接收枚举结果
	 */
	virtual void enumRiskMonitors(FuncEnumRiskMonCallback cb) override;

	/**
	 * @brief 创建风控模块
	 * @details 根据指定的名称创建一个新的风控模块实例
	 * @param name 风控模块名称
	 * @return 新创建的风控模块对象指针，如果创建失败则返回NULL
	 */
	virtual WtRiskMonitor* createRiskMonotor(const char* name) override;

	/**
	 * @brief 删除风控模块
	 * @details 删除并释放指定的风控模块实例
	 * @param unit 要删除的风控模块对象指针
	 * @return 删除是否成功，成功返回true，失败返回false
	 */
	virtual bool deleteRiskMonotor(WtRiskMonitor* unit) override;

};

