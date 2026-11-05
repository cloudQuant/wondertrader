#pragma once
#include "PorterDefs.h"
#include "../Share/StdUtils.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include "../TraderCTP/VendorTdLifecycle.hpp"

class Dumper
{

public:
	Dumper(): 
		_cb_account(nullptr), _cb_order(nullptr), _cb_trade(nullptr), 
		_cb_position(nullptr), _refresh_span(10), _stopped(false), _configured(false){}
	~Dumper();

	// T3-012 / 卡9：void run()/release() 的可观察结果
	enum LifecycleState : uint32_t
	{
		DS_IDLE = 0,			// 未运行
		DS_RUNNING = 1,			// run(false) worker 或 run(true) 循环在运行
		DS_STOP_DEFERRED = 2,	// 在 worker/回调内请求释放：已关闭准入，join 延后到安全点
		DS_RELEASED = 3,		// worker 已 join、回调已排空，之后不再向用户回调
		DS_DRAIN_UNCERTAIN = 4	// 有界等待超时：对象保留，不得视为已释放
	};
	LifecycleState lifecycle_state() const { return static_cast<LifecycleState>(_state.load(std::memory_order_acquire)); }

	// T3-012：release() 完成排空后，用户显式重新登记回调即重新开放回调准入与 run；
	// 停止尚未完成（延后/不确定）时只更新指针，回调准入保持关闭。
	void register_callbacks(FuncOnAccount cbAccount, FuncOnOrder cbOrder, FuncOnTrade cbTrade, FuncOnPosition cbPosition);

	// 有界停止上限（毫秒），测试可调小
	void set_stop_bound(uint32_t ms) { _stop_bound_ms = ms; }

	void init(const char* logProfile);

	bool config(const char* cfgfile, bool isFile, const char* modDir);

	void run(bool bOnce = true);

	void release();

	void on_account(const char* channelid, uint32_t curTDate, const char* currency, double prebalance, double balance,
		double dynbalance, double closeprofit, double dynprofit, double fee, double margin, double deposit, double withdraw, double available, bool isLast);

	void on_order(const char* channelid, const char* exchg, const char* code, uint32_t curTDate,
		const char* orderid, uint32_t direct, uint32_t offset, double volume, double leftover, double traded, double price, uint32_t ordertype,
		uint32_t pricetype, WtUInt64 ordertime, uint32_t state, const char* statemsg, bool isLast);

	void on_trade(const char* channelid, const char* exchg, const char* code, uint32_t curTDate,
		const char* tradeid, const char* orderid, uint32_t direct, uint32_t offset, double volume, double price,
		double amount, uint32_t ordertype, uint32_t tradetype, WtUInt64 tradetime, bool isLast);

	void on_position(const char* channelid, const char* exchg, const char* code, uint32_t curTDate, uint32_t direct,
		double volume, double newvol, double cost, double margin, double avgpx, double dynprofit, uint32_t volscale, bool isLast);

private:
	bool			complete_release_locked();

private:
	std::atomic<FuncOnAccount>	_cb_account;
	std::atomic<FuncOnOrder>	_cb_order;
	std::atomic<FuncOnTrade>	_cb_trade;
	std::atomic<FuncOnPosition>	_cb_position;

	// 卡9：用户回调准入闸门（排空后不再回调）、worker 标记与停止结果
	wt_vendor_td::Gate		_callbacks;
	std::atomic<uint32_t>	_state{DS_IDLE};
	std::atomic<bool>		_worker_running{false};
	std::atomic<uint32_t>	_run_once_active{0};
	std::mutex				_release_mutex;
	uint32_t				_stop_bound_ms = wt_vendor_td::DEFAULT_STOP_BOUND_MS;

	StdThreadPtr	_worker;
	std::mutex		_worker_mutex;
	std::condition_variable _worker_cv;

	uint32_t		_refresh_span;
	bool			_stopped;
	bool			_configured;
};

