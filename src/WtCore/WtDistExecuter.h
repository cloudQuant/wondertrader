/*!
 * \file WtDistExecuter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 分布式执行器头文件
 * \details 分布式执行器用于在分布式环境下广播目标仓位信息，
 *          与差量执行器不同，它不直接执行交易，而是将目标仓位广播给其他节点
 */
#pragma once
#include "IExecCommand.h"
#include "ExecCommandQuiesce.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>

NS_WTP_BEGIN
class WTSVariant;

/**
 * \brief 分布式执行器类
 * \details 用于在分布式系统中管理和广播目标仓位信息，但不直接执行交易操作
 *          继承自IExecCommand接口，实现了执行命令的基本功能。该类没有线程池、
 *          插件工厂或插件执行单元；quiesce 只关闭同步入口并排空正在运行的调用。
 */
class WtDistExecuter : public IExecCommand,
	public IExecCommandQuiesce, public IExecCommandQuiesceStages
{
public:
	/**
	 * \brief 构造函数
	 * \param name 执行器名称
	 */
	WtDistExecuter(const char* name);

	/**
	 * \brief 析构函数
	 */
	virtual ~WtDistExecuter();

public:
	/**
	 * \brief 初始化执行器
	 * \param params 初始化参数集合，包含如scale等参数
	 * \return 是否初始化成功；每个实例仅允许一次成功初始化
	 */
	bool init(WTSVariant* params);

	void requestQuiesce() override;
	bool canWaitQuiescedOnCurrentThread() const override;
	bool waitQuiesced() override;
	bool preflightQuiesceOnCurrentThread() const override;
	bool drainHostQuiesced() override;
	bool snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const override;
	bool completeQuiesceAfterGlobalPluginWait() override;
	WtExecuterFactory* quiesceFactory() const override;


public:
	//////////////////////////////////////////////////////////////////////////
	// IExecCommand接口实现
	/**
	 * \brief 设置目标仓位
	 * \details 批量设置多个标的仓位，并广播这些仓位信息
	 * \param targets 目标仓位映射表，键为标的代码，值为目标仓位
	 */
	virtual void set_position(const wt_hashmap<std::string, double>& targets) override;

	/**
	 * \brief 单个品种目标仓位变化通知
	 * \details 当单个品种的目标仓位发生变化时触发，用于广播目标仓位变化
	 * \param stdCode 品种代码
	 * \param targetPos 目标仓位
	 */
	virtual void on_position_changed(const char* stdCode, double targetPos) override;

	/**
	 * \brief 行情数据回调
	 * \details 在分布式执行器中不处理行情数据
	 * \param stdCode 品种代码
	 * \param newTick 最新的行情数据
	 */
	virtual void on_tick(const char* stdCode, WTSTickData* newTick) override;

private:
	struct OperationMarker;
	class OperationLease;
	bool enterOperation(OperationMarker& marker);
	void leaveOperation(OperationMarker& marker);
	bool isCurrentThreadInOperation() const;

	mutable std::mutex		_quiesce_mtx;
	std::condition_variable	_quiesce_cv;
	bool					_quiesce_requested;
	uint32_t				_active_operations;

	std::mutex				_init_mtx;
	std::mutex				_target_pos_mtx;
	static thread_local OperationMarker* _current_operation;

	/**
	 * \brief 配置项
	 * \details 存储分布式执行器的配置参数
	 */
	WTSVariant*			_config;
	bool				_initialized;

	/**
	 * \brief 仓位缩放比例
	 * \details 用于将输入的仓位数量按照比例放大或缩小
	 */
	std::atomic<uint32_t>	_scale;

	/**
	 * \brief 目标仓位映射表
	 * \details 存储所有品种的目标仓位情况，键为品种代码，值为目标仓位
	 */
	wt_hashmap<std::string, double> _target_pos;
};
NS_WTP_END

