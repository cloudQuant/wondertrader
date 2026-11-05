/*!
 * \file ParserShm.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include "../Includes/IParserApi.h"
#include "../Includes/IParserLifecycleV2.h"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSStruct.h"
#include "../Share/BoostMappingFile.hpp"

#include <boost/asio.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <atomic>
#include <future>
#include <mutex>

USING_NS_WTP;
using namespace boost::asio;

class ParserShm : public IParserApi
{
public:
	ParserShm();
	~ParserShm();

#pragma pack(push, 8)
	typedef struct _DataItem
	{
		uint32_t	_type;	//数据类型， 0-tick,1-委托队列,2-逐笔委托,3-逐笔成交
		union
		{
			WTSTickStruct	_tick;
			WTSOrdQueStruct _queue;
			WTSOrdDtlStruct	_order;
			WTSTransStruct	_trans;
		};

		_DataItem() { memset(this, 0, sizeof(_DataItem)); }
	} DataItem;

	template <int N = 8 * 1024>
	struct _DataQueue
	{
		uint64_t	_capacity = N;
		volatile uint64_t	_readable;
		volatile uint64_t	_writable;
		uint32_t	_pid;
		DataItem	_items[N];

		_DataQueue() :_readable(UINT64_MAX), _writable(0), _pid(0) {}
	};

	typedef _DataQueue<8 * 1024>	CastQueue;

#pragma pack(pop)

public:
	virtual bool init(WTSVariant* config) override;

	virtual void release() override;

	virtual bool connect() override;

	virtual bool disconnect() override;

	virtual bool isConnected() override;

	virtual void subscribe(const CodeSet &vecSymbols) override;
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	virtual void registerSpi(IParserSpi* listener) override;

	WtParserLifecycleV2Status requestStopV2() noexcept;
	WtParserLifecycleV2Status canWaitV2() const noexcept;
	WtParserLifecycleV2Status waitDrainedV2(uint32_t timeoutMs) noexcept;
	bool prepareDestroyV2() noexcept;
	bool shutdownLegacy();
#ifdef WT_PARSERSHM_TEST_HOOKS
	bool testWorkerFailed() const noexcept { return _worker_failed.load(std::memory_order_acquire); }
#endif

private:
	struct ApiFrame { ParserShm* parser; ApiFrame* previous; };
	class ApiGuard {
	public:
		explicit ApiGuard(ParserShm* parser, bool allowClosed = false) noexcept;
		~ApiGuard();
		explicit operator bool() const noexcept { return _entered; }
	private:
		ParserShm* _parser;
		ApiFrame _frame;
		bool _entered;
	};
	struct CallbackFrame { ParserShm* parser; CallbackFrame* previous; };
	template<typename Fn> void withSpi(Fn&& fn)
	{
		std::lock_guard<std::recursive_mutex> lock(_state_mutex);
		if (_stopped.load(std::memory_order_acquire) || !_sink) return;
		CallbackFrame frame{this, _callback_stack_top};
		_callback_stack_top = &frame;
		try { fn(_sink); } catch (...) { _callback_stack_top = frame.previous; throw; }
		_callback_stack_top = frame.previous;
	}
	bool isApiThread() const noexcept;
	bool isCallbackThread() const noexcept;
	void sleepInterruptibly(uint64_t micros) const;
	static constexpr uint64_t API_CLOSED = UINT64_C(1) << 63;
	static constexpr uint64_t API_V2_STOP = UINT64_C(1) << 62;
	static constexpr uint64_t API_COUNT_MASK = API_V2_STOP - 1;
	std::atomic<uint64_t> _api_state{0};
	// K51 (BUG-R18-011, user decision 2026-11-06): items dropped because this consumer fell
	// more than one queue capacity behind the producer (reported with a WARN each time).
	std::atomic<uint64_t> _lag_dropped{0};
	std::atomic<bool> _v2_stop{false}, _v2_drained{false}, _v2_joined{false};
	std::atomic<bool> _worker_created{false}, _worker_failed{false}, _connected{false};
	std::promise<bool> _exit_promise;
	std::shared_future<bool> _exit_future;
	std::recursive_mutex _state_mutex;
	std::mutex _lifecycle_mutex;
	bool _initialized_once = false;
	inline static thread_local ApiFrame* _api_stack_top = nullptr;
	inline static thread_local CallbackFrame* _callback_stack_top = nullptr;
	std::string		_path;
	typedef std::shared_ptr<BoostMappingFile> MappedFilePtr;
	MappedFilePtr	_mapfile;
	CastQueue*		_queue;
	uint32_t		_gpsize;
	uint32_t		_check_span;
	uint32_t		_cpu;

	IParserSpi*		_sink;
	std::atomic<bool>	_stopped;

	std::vector<bool> _subbed;

	IBaseDataMgr*	_bd_mgr;

	StdThreadPtr	_thrd_parser;
};

