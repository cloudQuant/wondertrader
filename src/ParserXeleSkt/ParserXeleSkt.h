/*!
 * \file ParserXeleSkt.h
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
#include "../Includes/DateMappingBinding.h"
#include "../Includes/WTSCollection.hpp"
#include "../Share/StdUtils.hpp"

#include <atomic>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

namespace xele_skt_detail
{
inline bool tryGetInstrumentCode(const char* instrumentId, std::size_t capacity, std::string& code)
{
	if (instrumentId == nullptr)
		return false;

	std::size_t length = 0;
	while (length < capacity && instrumentId[length] != '\0')
		++length;
	if (length == capacity)
		return false;

	code.assign(instrumentId, length);
	return true;
}

// Append received bytes and visit every complete fixed-size snapshot. Any
// incomplete suffix stays in pending for the next socket read.
enum class SnapshotReadStatus
{
	continue_reading,
	complete,
	invalid_frame,
	io_error,
	truncated_frame
};

template <typename Callback>
bool consumeCompleteSnapshots(std::string& pending, const char* bytes,
	std::size_t byteCount, std::size_t snapshotSize, Callback&& callback)
{
	if (bytes != nullptr && byteCount != 0)
		pending.append(bytes, byteCount);
	if (snapshotSize == 0)
		return false;

	while (pending.size() >= snapshotSize)
	{
		const bool continueParsing = callback(pending.data(), snapshotSize);
		pending.erase(0, snapshotSize);
		if (!continueParsing)
		{
			pending.clear();
			return false;
		}
	}
	return true;
}

// A synchronous read can return payload bytes together with EOF/error. Consume
// those bytes before telling the caller whether another read is possible.
template <typename Callback>
SnapshotReadStatus processSnapshotReadResult(std::string& pending, const char* bytes,
	std::size_t byteCount, bool readEof, bool readError, std::size_t snapshotSize,
	Callback&& callback)
{
	if (!consumeCompleteSnapshots(pending, bytes, byteCount, snapshotSize, callback))
		return SnapshotReadStatus::invalid_frame;
	if (readError)
		return SnapshotReadStatus::io_error;
	if (readEof)
		return pending.empty() ? SnapshotReadStatus::complete : SnapshotReadStatus::truncated_frame;
	return SnapshotReadStatus::continue_reading;
}
}

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/executor_work_guard.hpp>

USING_NS_WTP;
using namespace boost::asio;

class ParserXeleSkt : public IParserApi
{
friend struct ParserXeleSktTestAccess;

public:
	ParserXeleSkt();
	~ParserXeleSkt();

	//IQuoteParser ½Ó¿Ú
public:
	// A second init after one successful init is rejected and keeps the
	// original configuration.
	/// K31=A：宿主注入 DateMappingV1 表（可选导出 bindParserDateMappingV1 调用；须在 init 前，只接受一次）。
	bool bindDateMapping(const wtp::DateMappingV1* table) noexcept { return _date_binding.bind(table); }

	virtual bool init(WTSVariant* config) override;

	// Legacy void interface: external calls stop and join synchronously; a
	// worker/SPI callback thread can only request stop. Use shutdown() or the
	// module deleter to observe whether the owner may be destroyed.
	virtual void release() override;

	// Synchronously stop the worker and drain all callbacks. Returns false when
	// called from the io worker or an SPI callback; the owner must retain this
	// parser and retry from another thread before destroying it.
	bool shutdown();
	WtParserLifecycleV2Status requestStopV2() noexcept;
	WtParserLifecycleV2Status canWaitV2() const noexcept;
	WtParserLifecycleV2Status waitDrainedV2(uint32_t timeoutMs) noexcept;
	bool prepareDestroyV2() noexcept;

	// Reusable after disconnect(); rejected after release()/shutdown()/V2 stop
	// and from the worker or an SPI callback (busy, retry at a safe point).
	virtual bool connect() override;

	// Closes the multicast channel; the worker stays alive for a later connect.
	// From the worker the close runs inline and WPE_Close is queued.
	virtual bool disconnect() override;

	virtual bool isConnected() override;

	virtual void subscribe(const CodeSet &vecSymbols) override;
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	virtual void registerSpi(IParserSpi* listener) override;


private:
	struct CallbackFrame { ParserXeleSkt* parser; CallbackFrame* previous; };
	struct ApiFrame { ParserXeleSkt* parser; ApiFrame* previous; };
	class ApiGuard
	{
	public:
		explicit ApiGuard(ParserXeleSkt* parser, bool allowClosed = false) noexcept;
		~ApiGuard();
		explicit operator bool() const noexcept { return _entered; }
	private:
		ParserXeleSkt* _parser;
		ApiFrame _frame;
		bool _entered;
	};
	struct WorkerExitProof
	{
		std::promise<bool> promise;
		std::shared_future<bool> exited;
		WorkerExitProof() : exited(promise.get_future().share()) {}
	};
	using ReceiveBuffer = boost::array<char, 4096>;
	struct ReceiveOperation
	{
		ReceiveBuffer buffer;
		ip::udp::endpoint endpoint;
	};

	template<typename Fn>
	void with_spi(Fn&& fn)
	{
		// Every SPI entry is serialized by the recursive I/O mutex and recorded
		// on a thread-local callback stack so lifecycle calls can detect it.
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		if (_stop_requested.load(std::memory_order_acquire) || _sink == NULL)
			return;
		IParserSpi* sink = _sink;
		CallbackFrame frame{ this, _callback_stack_top };
		_callback_stack_top = &frame;
		try
		{
			fn(sink);
		}
		catch (...)
		{
			_callback_stack_top = frame.previous;
			throw;
		}
		_callback_stack_top = frame.previous;
	}

	bool	is_api_thread() const noexcept;
	bool	is_worker_thread() const noexcept;
	bool	is_callback_thread() const noexcept;
	void	armStopTimer();
	void	stopOnWorkerV2();
	bool	ensure_worker();
	void	close_sockets();
	void	emit_close_once();
	// Card 14a=C event contract, all guarded by _mtx_io:
	// WPE_Connect = transport established for an epoch (snapshot synced and
	// multicast receive armed); WPE_Login = confirmed by the first valid feed
	// frame of that epoch (fail-closed: never inferred from local success).
	void	begin_event_epoch();
	void	emit_connect_event(uint64_t epoch);
	void	emit_login_event(uint64_t epoch);
	void	emit_queued_close(uint64_t epoch);
	void	schedule_retry(uint64_t failed_epoch);

	void	arm_receive(uint64_t connection_epoch);
	void	handle_udp_read(const boost::system::error_code& e, std::size_t bytes_transferred,
		uint64_t connection_epoch, const std::shared_ptr<ReceiveOperation>& operation);

	bool	prepare();

	bool	reconnect();
	bool	open_channel();
	bool	open_on_worker();

	void	extract_buffer(uint32_t length, const char* buffer_data, uint64_t login_epoch = 0);

private:
	void	doOnConnected();
	void	doOnDisconnected();

private:
	std::string	_tcp_host;
	int			_tcp_port;
	std::string	_mcast_host;
	int			_mcast_port;
	std::string	_local_host;
	uint32_t	_gpsize;
	ip::address_v4	_mcast_addr;
	ip::address_v4	_local_addr;
	bool		_addresses_valid;

	ip::udp::endpoint	_mcast_ep;
	ip::tcp::endpoint	_tcp_ep;
	io_context			_io_service;
	boost::asio::steady_timer _stop_timer;
	boost::asio::steady_timer _retry_timer;

	boost::asio::strand<boost::asio::io_context::executor_type>	_strand;

	ip::udp::socket*	_udp_socket;

	IParserSpi*		_sink;
	IBaseDataMgr*	_bd_mgr;
	wtp::DateMappingBinding	_date_binding;	// K31=A：已注入时走路径 A，否则路径 C
	bool			_stopped;
	bool			_prepared;
	// Incremented for every opened multicast channel; completions from an
	// older channel are ignored. Guarded by _mtx_io.
	uint64_t		_connection_epoch;
	// Guarded by _mtx_io; reset for each successful connect.
	bool			_close_event_emitted;
	// Highest epochs for which WPE_Connect / WPE_Login were emitted.
	uint64_t		_connect_event_epoch = 0;
	uint64_t		_login_event_epoch = 0;

	std::atomic<bool>	_stop_requested{ false };
	std::atomic<bool>	_worker_running{ false };
	static constexpr uint64_t API_CLOSED = UINT64_C(1) << 63;
	static constexpr uint64_t API_V2_STOP = UINT64_C(1) << 62;
	static constexpr uint64_t API_COUNT_MASK = API_V2_STOP - 1;
	std::atomic<uint64_t> _api_state{ 0 };
	std::atomic<bool> _v2_stop{ false };
	std::atomic<bool> _v2_drained{ false };
	std::atomic<bool> _v2_joined{ false };
	std::atomic<bool> _worker_created{ false };
	std::atomic<bool> _stop_post_failed{ false };
	std::atomic<bool> _stop_completed{ false };
	bool _initialized_once = false;
	WorkerExitProof _worker_exit;
	using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	std::unique_ptr<WorkGuard> _work_guard;
	std::recursive_mutex _mtx_io;
	std::mutex			_mtx_lifecycle;
	inline static thread_local CallbackFrame* _callback_stack_top = nullptr;
	inline static thread_local ApiFrame* _api_stack_top = nullptr;

	CodeSet			_set_subs;
	StdThreadPtr	_thrd_parser;

	typedef WTSHashMap<int>	TickCache;
	TickCache*		_tick_cache;

	wt_hashmap<int, double> _price_scales;
};
