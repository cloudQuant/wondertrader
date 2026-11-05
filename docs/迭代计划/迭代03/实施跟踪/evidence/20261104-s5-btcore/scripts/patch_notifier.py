import sys
root=sys.argv[1]
h=root+'/src/WtBtCore/EventNotifier.h'
s=open(h).read()
old_inc='#include <queue>\n'
new_inc='#include <queue>\n#include <condition_variable>\n#include <mutex>\n'
assert old_inc in s; s=s.replace(old_inc,new_inc,1)
old_api='''	void	notifyFund(const char* topic, uint32_t uDate, double total_profit, double dynprofit, double dynbalance, double total_fee);
	

private:'''
new_api='''	void	notifyFund(const char* topic, uint32_t uDate, double total_profit, double dynprofit, double dynbalance, double total_fee);

	/**
	 * @brief 释放事件通知器（幂等，R20_wtbtcore#3 / 卡9）
	 * @details 先关闭发布准入并等待在途发布全部返回，再恰好一次销毁非零 server id，
	 *          最后清空函数指针与 id。之后的 notify* 为空操作、init 被拒绝（9c=A）。
	 *          WtMsgQue 未提供版本化回调静默能力，模块无法证明已无内部线程/回调，
	 *          按 9a=A 保持常驻到进程退出，不调用 free_library。
	 *          调用方须在回放线程停止并 join 之后调用（WtBtRunner::finalizeRelease）；
	 *          唯一宿主回调 on_mq_log 为空操作，不存在发布内同步重入 release 的路径。
	 *          析构复用本函数。
	 */
	void	release();

private:
	/**
	 * @brief 在途发布：持有准入期间调用插件 publish，返回后递减计数
	 */
	void	publish(const char* topic, const char* data, WtUInt32 dataLen);

private:'''
assert old_api in s; s=s.replace(old_api,new_api,1)
old_mem='''	FuncRegCallbacks	_register;   ///< 注册回调函数的函数指针
};'''
new_mem='''	FuncRegCallbacks	_register;   ///< 注册回调函数的函数指针

	std::mutex		_lifecycle_mtx;	///< 串行化 init/release
	std::mutex		_state_mtx;		///< 保护函数指针、server id、准入与在途计数
	std::condition_variable	_drained;	///< 在途发布归零通知
	uint32_t		_inflight;		///< 在途发布数
	bool			_released;		///< 已释放：拒绝新的发布与 init
	DllHandle		_module;		///< 已加载的 MQ 模块（9a=A：常驻到进程退出）
};'''
assert old_mem in s; s=s.replace(old_mem,new_mem,1)
old_inc2='#include "../Share/StdUtils.hpp"\n'
new_inc2='#include "../Share/StdUtils.hpp"\n#include "../Share/DLLHelper.hpp"\n'
assert old_inc2 in s; s=s.replace(old_inc2,new_inc2,1)
open(h,'w').write(s)

c=root+'/src/WtBtCore/EventNotifier.cpp'
s=open(c).read()
old_ctor='''	, _register(nullptr)
{
	// init 前通知函数为空操作，且析构不会误调用未初始化的函数指针。
}'''
new_ctor='''	, _register(nullptr)
	, _inflight(0)
	, _released(false)
	, _module(NULL)
{
	// init 前通知函数为空操作，且析构不会误调用未初始化的函数指针。
}'''
assert old_ctor in s; s=s.replace(old_ctor,new_ctor,1)
old_dtor='''EventNotifier::~EventNotifier()
{
	// 如果移除器函数存在且服务器ID有效，则销毁服务器
	if (_remover && _mq_sid != 0)
		_remover(_mq_sid);
}'''
new_dtor='''EventNotifier::~EventNotifier()
{
	// 复用幂等 release：已显式释放时不会重复销毁服务器
	release();
}

void EventNotifier::release()
{
	std::lock_guard<std::mutex> lifecycle(_lifecycle_mtx);
	FuncDestroyMQServer remover = nullptr;
	WtUInt32 sid = 0;
	bool hadModule = false;
	{
		std::unique_lock<std::mutex> lock(_state_mtx);
		// 先关闭准入：此后 notify* 不再进入插件
		_released = true;
		// 等待已进入插件的发布全部返回，再销毁服务器（卡9 9b=A）
		_drained.wait(lock, [this]() { return _inflight == 0; });
		remover = _remover;
		sid = _mq_sid;
		hadModule = _module != NULL;
		_creator = nullptr;
		_remover = nullptr;
		_publisher = nullptr;
		_register = nullptr;
		_mq_sid = 0;
	}

	if (remover && sid != 0)
	{
		remover(sid);
		WTSLogger::info("EventNotifier released channel {}", m_strURL.c_str());
	}
	if (hadModule)
		WTSLogger::debug("MQ module is retained until process exit: no versioned callback-quiescence capability");
}

void EventNotifier::publish(const char* topic, const char* data, WtUInt32 dataLen)
{
	FundPublishMessage publisher = nullptr;
	WtUInt32 sid = 0;
	{
		std::lock_guard<std::mutex> lock(_state_mtx);
		if (_released || _publisher == nullptr || _mq_sid == 0)
			return;
		publisher = _publisher;
		sid = _mq_sid;
		++_inflight;
	}

	struct InflightGuard
	{
		EventNotifier* self;
		~InflightGuard()
		{
			std::lock_guard<std::mutex> lock(self->_state_mtx);
			if (--self->_inflight == 0)
				self->_drained.notify_all();
		}
	} guard{ this };
	publisher(sid, topic, data, dataLen);
}'''
assert old_dtor in s; s=s.replace(old_dtor,new_dtor,1)
old_init_head='''	// 检查是否启用了事件通知器
	if (!cfg || !cfg->getBoolean("active") || _mq_sid != 0)
		return false;
'''
new_init_head='''	// 检查是否启用了事件通知器
	if (!cfg || !cfg->getBoolean("active"))
		return false;

	std::lock_guard<std::mutex> lifecycle(_lifecycle_mtx);
	{
		// 卡9 9c=A：已初始化或已释放时拒绝，保留原实例
		std::lock_guard<std::mutex> lock(_state_mtx);
		if (_released || _mq_sid != 0)
			return false;
	}
'''
assert old_init_head in s; s=s.replace(old_init_head,new_init_head,1)
old_pub='''	_creator = creator;
	_remover = remover;
	_publisher = publisher;
	_register = registerCallbacks;
	_mq_sid = sid;
'''
new_pub='''	{
		std::lock_guard<std::mutex> lock(_state_mtx);
		_creator = creator;
		_remover = remover;
		_publisher = publisher;
		_register = registerCallbacks;
		_mq_sid = sid;
		_module = dllInst;
	}
'''
assert old_pub in s; s=s.replace(old_pub,new_pub,1)
# notify functions
old_ne='''	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, "BT_EVENT", evtType, (WtUInt32)strlen(evtType));'''
new_ne='''	// 如果发布者函数存在，则发布消息（释放后为空操作）
	if (evtType == nullptr)
		return;
	publish("BT_EVENT", evtType, (WtUInt32)strlen(evtType));'''
assert old_ne in s; s=s.replace(old_ne,new_ne,1)
old_nd='''	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, topic, (const char*)data, dataLen);'''
new_nd='''	// 如果发布者函数存在，则发布消息（释放后为空操作）
	publish(topic, (const char*)data, dataLen);'''
assert old_nd in s; s=s.replace(old_nd,new_nd,1)
old_nf='''	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, topic, (const char*)output.c_str(), (WtUInt32)output.size());'''
new_nf='''	// 如果发布者函数存在，则发布消息（释放后为空操作）
	publish(topic, (const char*)output.c_str(), (WtUInt32)output.size());'''
assert old_nf in s; s=s.replace(old_nf,new_nf,1)
open(c,'w').write(s)
print("notifier patched")
