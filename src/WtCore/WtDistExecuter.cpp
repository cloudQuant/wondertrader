/*!
 * \file WtDistExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 分布式执行器实现
 * \details 实现分布式执行器的核心功能，包括初始化、目标仓位设置和通知
 */

#include "WtDistExecuter.h"

#include "../Includes/WTSVariant.hpp"

#include "../Share/decimal.h"
#include "../WTSTools/WTSLogger.h"
#include <exception>

USING_NS_WTP;

struct WtDistExecuter::OperationMarker
{
	const WtDistExecuter* owner;
	OperationMarker* previous;
};

thread_local WtDistExecuter::OperationMarker* WtDistExecuter::_current_operation = NULL;

class WtDistExecuter::OperationLease
{
public:
	explicit OperationLease(WtDistExecuter* owner)
		: _owner(owner), _entered(owner->enterOperation(_marker))
	{}

	OperationLease(const OperationLease&) = delete;
	OperationLease& operator=(const OperationLease&) = delete;

	~OperationLease()
	{
		if (_entered)
			_owner->leaveOperation(_marker);
	}

	bool entered() const { return _entered; }

private:
	WtDistExecuter* _owner;
	OperationMarker _marker;
	bool _entered;
};

/**
 * \brief 分布式执行器构造函数
 * \details 初始化分布式执行器，调用父类构造函数
 * \param name 执行器名称
 */
WtDistExecuter::WtDistExecuter(const char* name)
	: IExecCommand(name)
	, _quiesce_requested(false)
	, _active_operations(0)
	, _config(NULL)
	, _initialized(false)
	, _scale(0)
{

}

/**
 * \brief 分布式执行器析构函数
 * \details 资源清理
 */
WtDistExecuter::~WtDistExecuter()
{
	requestQuiesce();
	if (!waitQuiesced())
		std::terminate();

	if (_config)
	{
		_config->release();
		_config = NULL;
	}
}

/**
 * \brief 初始化分布式执行器
 * \details 从配置参数中读取如缩放比例等设置
 * \param params 配置参数集合
 * \return 是否初始化成功
 */
bool WtDistExecuter::init(WTSVariant* params)
{
	OperationLease operation(this);
	if (!operation.entered())
		return false;
	std::lock_guard<std::mutex> initLock(_init_mtx);

	// Initialization is one-shot. Reject duplicate calls without changing the
	// retained configuration or scale, so callers can safely retry only after a
	// failed (for example, null-parameter) attempt.
	if (params == NULL || _initialized)
		return false;

	const uint32_t scale = params->getUInt32("scale");
	params->retain();
	_config = params;
	_scale.store(scale, std::memory_order_release);
	_initialized = true;

	return true;
}

void WtDistExecuter::requestQuiesce()
{
	std::lock_guard<std::mutex> lock(_quiesce_mtx);
	_quiesce_requested = true;
	_quiesce_cv.notify_all();
}

bool WtDistExecuter::isCurrentThreadInOperation() const
{
	for (OperationMarker* marker = _current_operation; marker != NULL; marker = marker->previous)
	{
		if (marker->owner == this)
			return true;
	}
	return false;
}

bool WtDistExecuter::enterOperation(OperationMarker& marker)
{
	const bool reentrant = isCurrentThreadInOperation();
	std::lock_guard<std::mutex> lock(_quiesce_mtx);
	if (_quiesce_requested && !reentrant)
		return false;

	++_active_operations;
	marker.owner = this;
	marker.previous = _current_operation;
	_current_operation = &marker;
	return true;
}

void WtDistExecuter::leaveOperation(OperationMarker& marker)
{
	_current_operation = marker.previous;
	std::lock_guard<std::mutex> lock(_quiesce_mtx);
	if (_active_operations == 0)
		std::terminate();
	--_active_operations;
	if (_active_operations == 0)
		_quiesce_cv.notify_all();
}

bool WtDistExecuter::canWaitQuiescedOnCurrentThread() const
{
	return !isCurrentThreadInOperation();
}

bool WtDistExecuter::waitQuiesced()
{
	if (!canWaitQuiescedOnCurrentThread())
		return false;

	std::unique_lock<std::mutex> lock(_quiesce_mtx);
	if (!_quiesce_requested)
		return false;
	_quiesce_cv.wait(lock, [this]() { return _active_operations == 0; });
	return true;
}

bool WtDistExecuter::preflightQuiesceOnCurrentThread() const
{
	return canWaitQuiescedOnCurrentThread();
}

bool WtDistExecuter::drainHostQuiesced()
{
	if (!preflightQuiesceOnCurrentThread())
		return false;

	std::unique_lock<std::mutex> lock(_quiesce_mtx);
	if (!_quiesce_requested)
		return false;
	_quiesce_cv.wait(lock, [this]() { return _active_operations == 0; });
	return true;
}

bool WtDistExecuter::snapshotQuiescePluginOwners(std::vector<ExecQuiesceOwnerPtr>& out) const
{
	out.clear();
	std::lock_guard<std::mutex> lock(_quiesce_mtx);
	return _quiesce_requested && _active_operations == 0;
}

bool WtDistExecuter::completeQuiesceAfterGlobalPluginWait()
{
	std::lock_guard<std::mutex> lock(_quiesce_mtx);
	if (!_quiesce_requested || _active_operations != 0)
		return false;
	return true;
}

WtExecuterFactory* WtDistExecuter::quiesceFactory() const
{
	return NULL;
}

/**
 * \brief 设置目标仓位
 * \details 批量更新多个品种的目标仓位，并广播变化
 * \param targets 目标仓位映射表，键为品种代码，值为目标仓位
 */
void WtDistExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	OperationLease operation(this);
	if (!operation.entered())
		return;
	const uint32_t scale = _scale.load(std::memory_order_acquire);
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;

		// 应用仓位缩放比例
		newVol *= scale;
		double oldVol;
		{
			std::lock_guard<std::mutex> targetLock(_target_pos_mtx);
			oldVol = _target_pos[stdCode];
			_target_pos[stdCode] = newVol;
		}
		if (!decimal::eq(oldVol, newVol))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}]{}目标仓位更新: {} -> {}", _name.c_str(), stdCode, oldVol, newVol);
		}

		// TODO: 这里广播目标仓位到其他节点
	}
}

/**
 * \brief 处理单个品种的目标仓位变化
 * \details 更新单个品种的目标仓位，并广播到其他节点
 * \param stdCode 品种代码
 * \param targetPos 目标仓位
 */
void WtDistExecuter::on_position_changed(const char* stdCode, double targetPos)
{
	OperationLease operation(this);
	if (!operation.entered())
		return;
	// 应用仓位缩放比例
	targetPos *= _scale.load(std::memory_order_acquire);
	double oldVol;
	{
		std::lock_guard<std::mutex> targetLock(_target_pos_mtx);
		oldVol = _target_pos[stdCode];
		_target_pos[stdCode] = targetPos;
	}

	if (!decimal::eq(oldVol, targetPos))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}]{}目标仓位更新: {} -> {}", _name.c_str(), stdCode, oldVol, targetPos);
	}

	// TODO: 这里广播目标仓位到其他节点
}

/**
 * \brief 处理行情数据回调
 * \details 分布式执行器不需要直接处理行情数据，这个方法是接口实现的空方法
 * \param stdCode 品种代码
 * \param newTick 最新的行情数据
 */
void WtDistExecuter::on_tick(const char* /*stdCode*/, WTSTickData* /*newTick*/)
{
	OperationLease operation(this);
	if (!operation.entered())
		return;
	// 分布式执行器不需要处理ontick
}
