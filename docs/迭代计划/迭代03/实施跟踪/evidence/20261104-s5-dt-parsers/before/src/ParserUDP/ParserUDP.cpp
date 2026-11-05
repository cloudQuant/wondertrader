/*!
 * \file ParserUDP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserUDP.h"
#include <boost/asio/post.hpp>
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"

#include <boost/bind.hpp>
#include <atomic>
#include <cstring>
#include <stdexcept>

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	fmtutil::format_to(buffer, format, args...);

	sink->handleParserLog(ll, buffer);
}

namespace
{
	thread_local ParserUDP* g_current_udp_worker = nullptr;
#ifdef WT_PARSERUDP_TEST_HOOKS
	std::atomic<bool> g_fail_next_worker_start{false};
	std::atomic<bool> g_fail_after_receive_arm{false};
	std::atomic<bool> g_fail_next_stop_timer{false};
#endif
}

#define UDP_MSG_SUBSCRIBE	0x100
#define UDP_MSG_PUSHTICK	0x200
#define UDP_MSG_PUSHORDQUE	0x201	//委托队列
#define UDP_MSG_PUSHORDDTL	0x202	//委托明细
#define UDP_MSG_PUSHTRANS	0x203	//逐笔成交

#pragma pack(push,1)

typedef struct UDPPacketHead
{
	uint32_t		_type;
} UDPPacketHead;
//UDP请求包
typedef struct _UDPReqPacket : UDPPacketHead
{
	char			_data[1020];
} UDPReqPacket;

//UDPTick数据包
template <typename T>
struct UDPDataPacket : UDPPacketHead
{
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
typedef UDPDataPacket<WTSOrdQueStruct>	UDPOrdQuePacket;
typedef UDPDataPacket<WTSOrdDtlStruct>	UDPOrdDtlPacket;
typedef UDPDataPacket<WTSTransStruct>	UDPTransPacket;


extern "C"
{
#ifdef WT_PARSERUDP_TEST_HOOKS
	EXPORT_FLAG void parserUdpTestFailNextWorkerStart() { g_fail_next_worker_start.store(true); }
	EXPORT_FLAG void parserUdpTestFailAfterReceiveArm() { g_fail_after_receive_arm.store(true); }
	EXPORT_FLAG void parserUdpTestFailNextStopTimer() { g_fail_next_stop_timer.store(true); }
	EXPORT_FLAG bool parserUdpTestLegacyShutdown(IParserApi* api) { return static_cast<ParserUDP*>(api)->shutdown(); }
#endif
	EXPORT_FLAG IParserApi* createParser()
	{
		try { return new ParserUDP(); }
		catch (...) { return NULL; }
	}

	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			ParserUDP* udp = dynamic_cast<ParserUDP*>(parser);
			if (udp != NULL && !udp->shutdown())
				return;

			delete parser;
			parser = NULL;
		}
	}

	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL udp_preflight() { return WT_PARSER_LIFECYCLE_V2_OK; }
	static ParserUDP* udp_from_handle(void* handle) { return static_cast<ParserUDP*>(static_cast<IParserApi*>(handle)); }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL udp_request(void* handle)
	{ return handle ? udp_from_handle(handle)->requestStopV2() : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL udp_can_wait(void* handle)
	{ return handle ? udp_from_handle(handle)->canWaitV2() : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL udp_wait(void* handle, uint32_t timeout)
	{ return handle ? udp_from_handle(handle)->waitDrainedV2(timeout) : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL udp_destroy(void** handle)
	{
		if (!handle || !*handle) return WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT;
		ParserUDP* parser = udp_from_handle(*handle);
		if (!parser->prepareDestroyV2()) return WT_PARSER_LIFECYCLE_V2_ERROR;
		try { delete parser; *handle = NULL; return WT_PARSER_LIFECYCLE_V2_OK; }
		catch (...) { return WT_PARSER_LIFECYCLE_V2_ERROR; }
	}
	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		static const WtParserLifecycleApiV2 table = { sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
			WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0}, udp_preflight, udp_request, udp_can_wait, udp_wait, udp_destroy };
		return major == WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ? &table : NULL;
	}
};



ParserUDP::ParserUDP()
	: _stop_timer(_io_service)
	, _strand(boost::asio::make_strand(_io_service))
	, _b_socket(NULL)
	, _s_socket(NULL)
	, _s_inited(false)
, _sink(NULL)
	, _stopped(false)
	, _connecting(false)
	, _connection_epoch(0)
	, _close_event_emitted(true)
	, _stop_requested(false)
	{
}


ParserUDP::~ParserUDP()
{
	if (_v2_joined.load(std::memory_order_acquire)) return;
	// The exported deleteParser entry point retains the owner when this is
	// called from an SPI callback. Direct deletion from that callback cannot be
	// made safe by the legacy void IParserApi::release() contract.
	shutdown();
}

ParserUDP::ApiGuard::ApiGuard(ParserUDP* parser, bool allowClosed) noexcept
	: _parser(parser), _frame{parser, NULL}, _entered(false)
{
	uint64_t state = parser->_api_state.load(std::memory_order_acquire);
	for (;;)
	{
		if (((state & API_CLOSED) && (!allowClosed || (state & API_V2_STOP))) ||
			(state & API_COUNT_MASK) == API_COUNT_MASK) return;
		if (parser->_api_state.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel)) break;
	}
	_frame.previous = _api_stack_top;
	_api_stack_top = &_frame;
	_entered = true;
}

ParserUDP::ApiGuard::~ApiGuard()
{
	if (!_entered) return;
	_api_stack_top = _frame.previous;
	_parser->_api_state.fetch_sub(1, std::memory_order_release);
}

bool ParserUDP::is_api_thread() const noexcept
{
	for (ApiFrame* frame = _api_stack_top; frame; frame = frame->previous)
		if (frame->parser == this) return true;
	return false;
}

void ParserUDP::armStopTimer()
{
#ifdef WT_PARSERUDP_TEST_HOOKS
	if (g_fail_next_stop_timer.exchange(false)) throw std::runtime_error("injected stop timer failure");
#endif
	_stop_timer.expires_after(std::chrono::milliseconds(10));
	_stop_timer.async_wait([this](const boost::system::error_code& error) {
		std::lock_guard<std::recursive_mutex> lock(_mtx_io);
		if (error == boost::asio::error::operation_aborted) return;
		if (error)
		{
			_stop_post_failed.store(true, std::memory_order_release);
			return;
		}
		if (_v2_stop.load(std::memory_order_acquire)) { stopOnWorkerV2(); return; }
		if (_stop_requested.load(std::memory_order_acquire)) return;
		try { armStopTimer(); }
		catch (...) { _stop_post_failed.store(true, std::memory_order_release); }
	});
}

void ParserUDP::stopOnWorkerV2()
{
	std::lock_guard<std::recursive_mutex> lock(_mtx_io);
	_stopped = true;
	close_sockets();
	_work_guard.reset();
	_stop_completed.store(true, std::memory_order_release);
}

WtParserLifecycleV2Status ParserUDP::requestStopV2() noexcept
{
	static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
		"ParserUDP V2 stop requires lock-free atomic operations");
	_api_state.fetch_or(API_CLOSED | API_V2_STOP, std::memory_order_acq_rel);
	_v2_stop.store(true, std::memory_order_release);
	_stop_requested.store(true, std::memory_order_release);
	return _stop_post_failed.load(std::memory_order_acquire) ? WT_PARSER_LIFECYCLE_V2_ERROR : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserUDP::canWaitV2() const noexcept
{
	return (is_worker_thread() || is_callback_thread() || is_api_thread()) ?
		WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserUDP::waitDrainedV2(uint32_t timeoutMs) noexcept
{
	try {
	if (canWaitV2() != WT_PARSER_LIFECYCLE_V2_OK) return WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK;
	if (!_v2_stop.load(std::memory_order_acquire)) return WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT;
	const auto start = std::chrono::steady_clock::now();
	do
	{
		if (_stop_post_failed.load(std::memory_order_acquire)) return WT_PARSER_LIFECYCLE_V2_ERROR;
		if ((_api_state.load(std::memory_order_acquire) & API_COUNT_MASK) == 0)
		{
			bool exited = !_worker_created.load(std::memory_order_acquire);
			if (!exited)
			{
				if (_worker_exit.exited.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
				{
					if (!_worker_exit.exited.get()) return WT_PARSER_LIFECYCLE_V2_ERROR;
					exited = true;
				}
			}
			if (exited)
			{
				std::unique_lock<std::recursive_mutex> io(_mtx_io, std::try_to_lock);
				if (io.owns_lock())
				{
					_sink = NULL;
					_v2_drained.store(true, std::memory_order_release);
					return WT_PARSER_LIFECYCLE_V2_DRAINED;
				}
			}
		}
		if (timeoutMs == 0) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (timeoutMs == UINT32_MAX || std::chrono::steady_clock::now() - start < std::chrono::milliseconds(timeoutMs));
	return WT_PARSER_LIFECYCLE_V2_PENDING;
	} catch (...) { return WT_PARSER_LIFECYCLE_V2_ERROR; }
}

bool ParserUDP::prepareDestroyV2() noexcept
{
	if (!_v2_drained.load(std::memory_order_acquire) || canWaitV2() != WT_PARSER_LIFECYCLE_V2_OK) return false;
	try
	{
		std::unique_lock<std::mutex> lock(_mtx_lifecycle, std::try_to_lock);
		if (!lock.owns_lock()) return false;
		if (_thrd_parser && _thrd_parser->joinable()) _thrd_parser->join();
		_thrd_parser.reset();
		close_sockets();
		_v2_joined.store(true, std::memory_order_release);
		return true;
	} catch (...) { return false; }
}

bool ParserUDP::init( WTSVariant* config )
{
	ApiGuard api(this);
	if (!api) return false;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_initialized_once || _stop_requested.load(std::memory_order_acquire)) return false;
	if (config == NULL)
		return false;

	const std::string host = config->getCString("host");
	if (host.empty())
		return false;

	boost::system::error_code error;
	ip::address addr = ip::make_address(host, error);
	if (error)
		return false;

	const int bport = config->getInt32("bport");
	const int sport = config->getInt32("sport");
	uint32_t gpsize = config->getUInt32("gpsize");
	if (gpsize == 0)
		gpsize = 1000;

	_hots = host;
	_bport = bport;
	_sport = sport;
	_gpsize = gpsize;
	_server_ep = ip::udp::endpoint(addr, _sport);

	_broad_ep = ip::udp::endpoint(ip::address_v4::any(), _bport);

	_initialized_once = true;
	return true;
}

void ParserUDP::release()
{
	// The legacy interface cannot report a self-join failure. In that case the
	// exported deleter leaves the parser pointer intact so the owner can retry.
	(void)shutdown();
}

bool ParserUDP::is_worker_thread() const
{
	return g_current_udp_worker == this;
}

bool ParserUDP::is_callback_thread() const
{
	for (CallbackFrame* frame = _callback_stack_top; frame != nullptr; frame = frame->previous)
	{
		if (frame->parser == this)
			return true;
	}
	return false;
}

void ParserUDP::close_sockets()
{
	boost::system::error_code ignored;
	if (_b_socket != NULL)
	{
		_b_socket->cancel(ignored);
		_b_socket->close(ignored);
		delete _b_socket;
		_b_socket = NULL;
	}

	if (_s_socket != NULL)
	{
		_s_socket->cancel(ignored);
		_s_socket->close(ignored);
		delete _s_socket;
		_s_socket = NULL;
	}
	_s_inited = false;
}

void ParserUDP::emit_close_once()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_close_event_emitted)
		return;

	_close_event_emitted = true;
	emit_connect_event(_connection_epoch);
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Close, 0); });
}

void ParserUDP::begin_event_epoch()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	_close_event_emitted = false;
	const uint64_t epoch = _connection_epoch;
	// Queued on the strand; a queued Close of this epoch is posted later, so
	// strand order keeps Connect before Close.
	boost::asio::post(_strand, [this, epoch]() { emit_connect_event(epoch); });
}

void ParserUDP::emit_connect_event(uint64_t epoch)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (epoch == 0 || epoch <= _connect_event_epoch)
		return;
	_connect_event_epoch = epoch;
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Connect, 0); });
}

void ParserUDP::emit_login_event(uint64_t epoch)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (epoch == 0 || epoch != _connection_epoch || epoch <= _login_event_epoch || _stopped ||
		_recovery_exhausted || _close_event_emitted || _stop_requested.load(std::memory_order_acquire))
		return;
	emit_connect_event(epoch);
	if (_stopped || epoch != _connection_epoch || _stop_requested.load(std::memory_order_acquire))
		return;
	_login_event_epoch = epoch;
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Login, 0); });
}

void ParserUDP::emit_queued_close(uint64_t epoch)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	emit_connect_event(epoch);
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Close, 0); });
}

void ParserUDP::recover_from_transport_failure(uint32_t& attempts, const char* channel, uint64_t failed_epoch)
{
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		_connection_epoch != failed_epoch)
		return;

	if (attempts >= MAX_AUTO_RECOVERY_ATTEMPTS)
	{
		_recovery_exhausted = true;
		_stopped = true;
		with_spi([&](IParserSpi* sink)
		{
			write_log(sink, LL_ERROR,
				"[ParserUDP] {} channel failed after {} automatic recovery attempts; automatic recovery stopped",
				channel, MAX_AUTO_RECOVERY_ATTEMPTS);
		});
		close_sockets();
		return;
	}

	++attempts;
	with_spi([&](IParserSpi* sink)
	{
		write_log(sink, LL_WARN, "[ParserUDP] Recovering {} channel, attempt {}/{}",
			channel, attempts, MAX_AUTO_RECOVERY_ATTEMPTS);
	});
	// SPI callbacks may re-enter disconnect()/shutdown() while this recursive
	// lock is held. Re-check the failed epoch and lifecycle after returning.
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		_connection_epoch != failed_epoch)
		return;

	// One callback schedules at most one complete pair rebuild. Repeated
	// failures are bounded by MAX_AUTO_RECOVERY_ATTEMPTS; no worker sleep or
	// synchronous retry loop is used.
	const uint64_t epoch_before_reconnect = _connection_epoch;
	if (reconnect(3))
	{
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			_connection_epoch == epoch_before_reconnect || _b_socket == nullptr || !_b_socket->is_open() ||
			_s_socket == nullptr || !_s_socket->is_open())
			return;
		begin_event_epoch();
		return;
	}
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		_connection_epoch != epoch_before_reconnect)
		return;

	if (!_stop_requested.load(std::memory_order_acquire))
	{
		_recovery_exhausted = true;
		_stopped = true;
		close_sockets();
	}
}

bool ParserUDP::shutdown()
{
	ApiGuard api(this, true);
	if (!api) return false;
	_api_state.fetch_or(API_CLOSED, std::memory_order_acq_rel);
	_stop_requested.store(true, std::memory_order_release);

	if (is_worker_thread())
	{
		// The active handler already runs on the only io_context worker, so it
		// can close the sockets inline. It cannot prove the callback stack has
		// drained or join itself; keep the thread owner for an external retry.
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		close_sockets();
		try { _stop_timer.cancel(); } catch (...) { return false; }
		_work_guard.reset();
		return false;
	}
	if (is_callback_thread())
	{
		// A non-worker callback cannot wait for its own stack to drain. The stop
		// request blocks subsequent dispatch; the owner must retry after return.
		return false;
	}

	std::lock_guard<std::mutex> lifecycle_lock(_mtx_lifecycle);
	{
		// All socket initiation and completion handlers use this recursive lock.
		// Closing under it serializes cancellation against the last active
		// handler without calling into the SPI from the releasing thread.
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		close_sockets();
		try { _stop_timer.cancel(); } catch (...) { return false; }
		_work_guard.reset();
	}

	if (_thrd_parser && _thrd_parser->joinable())
	{
		try
		{
			_thrd_parser->join();
		}
		catch (...)
		{
			return false;
		}
	}
	_thrd_parser.reset();
	_worker_running.store(false, std::memory_order_release);
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_send_in_flight = false;
		_active_send_epoch = 0;
		_active_send_id = 0;
		_pending_send_buffer.reset();
	}

	{
		StdUniqueLock queue_lock(_mtx_queue);
		std::queue<std::string> empty;
		_send_queue.swap(empty);
	}

	return true;
}

bool ParserUDP::reconnect(uint32_t flag /* = 3 */, std::shared_ptr<ReceiveOperation>* broadcast_operation)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (broadcast_operation != nullptr)
		broadcast_operation->reset();
	// An epoch represents a complete broadcast/subscription socket pair. A
	// partial reconnect would stale the untouched channel's handler without
	// arming a replacement for that channel.
	if (flag != 3 || _stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted)
		return false;

	// Every reconnect establishes a new pair epoch. Any completion already
	// queued by either old socket belongs to the failed pair and must not emit
	// another close or reconnect one channel on its own.
	++_connection_epoch;
	if (_connection_epoch == 0)
		++_connection_epoch;
	const uint64_t connection_epoch = _connection_epoch;
	_send_in_flight = false;
	_active_send_epoch = 0;
	_active_send_id = 0;
	_pending_send_buffer.reset();

	if (flag & 1)
	{//建立广播通道
		if (_b_socket != NULL)
		{
			_b_socket->close();
			delete _b_socket;
			_b_socket = NULL;
		}

		_b_socket = new ip::udp::socket(_io_service);

		_b_socket->open(_broad_ep.protocol());
		_b_socket->set_option(ip::udp::socket::reuse_address(true));
		_b_socket->set_option(ip::udp::socket::broadcast(true));
#ifdef __APPLE__
		_b_socket->set_option(ip::udp::socket::receive_buffer_size(7 * 1024 * 1024));
#else
		_b_socket->set_option(ip::udp::socket::receive_buffer_size(8 * 1024 * 1024));
#endif
		_b_socket->bind(_broad_ep);

		std::shared_ptr<ReceiveOperation> operation = arm_receive(true, connection_epoch);
#ifdef WT_PARSERUDP_TEST_HOOKS
		if (g_fail_after_receive_arm.exchange(false)) throw std::runtime_error("injected partial connection failure");
#endif
		if (broadcast_operation != nullptr)
			*broadcast_operation = operation;
	}

	if (flag & 2)
	{
		std::queue<std::string> emptyQue;
		{
			StdUniqueLock lock(_mtx_queue);
			_send_queue.swap(emptyQue);

			//建立订阅通道
			if (_s_socket != NULL)
			{
				_s_socket->close();
				delete _s_socket;
				_s_socket = NULL;
			}

			_s_inited = false;
			_s_socket = new ip::udp::socket(_io_service, ip::udp::endpoint(ip::udp::v4(), 0));
		}

		subscribe();
	}

	// subscribe() may log through the SPI; a re-entrant disconnect can close
	// these sockets before it returns. Never report that partially stopped pair
	// as a successful reconnect.
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		_connection_epoch != connection_epoch || _b_socket == nullptr || !_b_socket->is_open() ||
		_s_socket == nullptr || !_s_socket->is_open())
		return false;
	return true;
}

void ParserUDP::subscribe()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire))
		return;

	std::string data;
	data.resize(sizeof(UDPReqPacket), 0);
	UDPReqPacket* req = (UDPReqPacket*)data.data();
	req->_type = UDP_MSG_SUBSCRIBE;
	std::size_t length = 0;
	std::size_t skipped_long_symbols = 0;
	const std::size_t max_payload_length = sizeof(req->_data) - 1;
	for (auto& code : _set_subs)
	{
		const std::size_t pos = code.find('.');
		const char* symbol = pos == std::string::npos ? code.data() : code.data() + pos + 1;
		const std::size_t symbol_length = pos == std::string::npos ? code.size() : code.size() - pos - 1;
		if (symbol_length == 0)
			continue;

		if (symbol_length > max_payload_length)
		{
			++skipped_long_symbols;
			continue;
		}

		const std::size_t separator_length = length == 0 ? 0 : 1;
		if (length + separator_length + symbol_length > max_payload_length)
		{
			{
				StdUniqueLock lock(_mtx_queue);
				_send_queue.push(data);
			}

			data.assign(sizeof(UDPReqPacket), 0);
			req = (UDPReqPacket*)data.data();
			req->_type = UDP_MSG_SUBSCRIBE;
			length = 0;
		}

		if (length > 0)
			req->_data[length++] = ',';
		std::memcpy(req->_data + length, symbol, symbol_length);
		length += symbol_length;
	}

	if (length > 0)
	{
		StdUniqueLock lock(_mtx_queue);
		_send_queue.push(data);
	}

	if (skipped_long_symbols > 0)
	{
		with_spi([&](IParserSpi* sink)
		{
			write_log(sink, LL_ERROR, "[ParserUDP] Skipped {} subscription code(s) that exceed packet capacity",
				static_cast<uint32_t>(skipped_long_symbols));
		});
	}

	do_send();
}

std::shared_ptr<ParserUDP::ReceiveOperation> ParserUDP::arm_receive(bool isBroad, uint64_t connection_epoch)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch)
		return nullptr;

	ip::udp::socket* socket = isBroad ? _b_socket : _s_socket;
	if (socket == NULL || !socket->is_open())
		return nullptr;

	auto operation_owner = std::make_shared<ReceiveOperation>();
	socket->async_receive_from(boost::asio::buffer(operation_owner->buffer.data(), operation_owner->buffer.size()), operation_owner->endpoint,
		[this, isBroad, connection_epoch, operation_owner](const boost::system::error_code& e, std::size_t bytes_transferred)
		{
			handle_read(e, bytes_transferred, isBroad, connection_epoch, operation_owner);
		});
	return operation_owner;
}

void ParserUDP::do_send()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted)
		return;
	const uint64_t connection_epoch = _connection_epoch;

	StdUniqueLock lock(_mtx_queue);
	if (_send_queue.empty() || _s_socket == NULL ||
		(_send_in_flight && _active_send_epoch == connection_epoch))
		return;

	with_spi([&](IParserSpi* sink) { write_log(sink, LL_INFO, "[ParserUDP] {} Ticks subscribing packets still await", _send_queue.size()); });
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch || _s_socket == NULL || !_s_socket->is_open())
		return;

	_pending_send_buffer = std::make_shared<std::string>(_send_queue.front());
	++_next_send_id;
	if (_next_send_id == 0)
		++_next_send_id;
	const uint64_t send_id = _next_send_id;
	_active_send_epoch = connection_epoch;
	_active_send_id = send_id;
	_send_in_flight = true;
	try
	{
		_s_socket->async_send_to(boost::asio::buffer(_pending_send_buffer->data(), _pending_send_buffer->size()), _server_ep,
			make_write_handler(connection_epoch, send_id, _pending_send_buffer));
	}
	catch (...)
	{
		_send_in_flight = false;
		_active_send_epoch = 0;
		_active_send_id = 0;
		_pending_send_buffer.reset();
		throw;
	}

	if(!_s_inited)
	{
		_s_inited = true;
		arm_receive(false, connection_epoch);
	}
	
}


// async_send_to completes with both an error and the transferred byte count.
// Capture the shared payload in the operation handler so reconnect can clear
// the queue/current-send slot without invalidating Asio's borrowed buffer.
std::function<void(const boost::system::error_code&, std::size_t)> ParserUDP::make_write_handler(uint64_t connection_epoch,
	uint64_t send_id, const std::shared_ptr<std::string>& buffer_owner)
{
	return [this, connection_epoch, send_id, buffer_owner](const boost::system::error_code& e, std::size_t)
	{
		handle_write(e, connection_epoch, send_id, buffer_owner);
	};
}

void ParserUDP::handle_write(const boost::system::error_code& e, uint64_t connection_epoch, uint64_t send_id,
	const std::shared_ptr<std::string>& buffer_owner)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || connection_epoch != _connection_epoch ||
		_recovery_exhausted || !_send_in_flight || _active_send_epoch != connection_epoch || _active_send_id != send_id)
		return;
	if (e == boost::asio::error::operation_aborted)
		return;

	_send_in_flight = false;
	_active_send_epoch = 0;
	_active_send_id = 0;
	_pending_send_buffer.reset();

	if (e)
	{
		emit_close_once();
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			connection_epoch != _connection_epoch)
			return;
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserUDP] Error occured while sending: {}({})", e.message().c_str(), e.value()); });
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			connection_epoch != _connection_epoch)
			return;
		recover_from_transport_failure(_send_recovery_attempts, "subscription send", connection_epoch);
	}
	else
	{
		_send_recovery_attempts = 0;
		{
			StdUniqueLock lock(_mtx_queue);
			if (!_send_queue.empty() && !_send_queue.front().compare(*buffer_owner))
				_send_queue.pop();
		}
		do_send();
	}
}

bool ParserUDP::connect()
{
	ApiGuard api(this);
	if (!api) return false;
	if (is_worker_thread() || is_callback_thread())
		return false;

	std::lock_guard<std::mutex> lifecycle_lock(_mtx_lifecycle);
	if (_stop_requested.load(std::memory_order_acquire))
		return false;

	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		if (!_initialized_once) return false;
		const bool running_pair = _b_socket != NULL && _b_socket->is_open() &&
			_s_socket != NULL && _s_socket->is_open() && !_stopped && !_recovery_exhausted &&
			!_stop_requested.load(std::memory_order_acquire) && _connection_epoch != 0 &&
			_worker_running.load(std::memory_order_acquire);
		if (running_pair)
			return true;
		if (_b_socket != NULL || _s_socket != NULL)
		{
			_stopped = true;
			close_sockets();
		}
	}

	if (!_thrd_parser)
	{
		try
		{
			_io_service.restart();
			_work_guard.reset(new WorkGuard(boost::asio::make_work_guard(_io_service)));
			_worker_running.store(true, std::memory_order_release);
#ifdef WT_PARSERUDP_TEST_HOOKS
			if (g_fail_next_worker_start.exchange(false)) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
			_thrd_parser = std::make_shared<StdThread>([this]()
			{
				g_current_udp_worker = this;
				bool clean = true;
				try { _io_service.run(); }
				catch (...)
				{
					clean = false;
					_stop_post_failed.store(true, std::memory_order_release);
					_stop_requested.store(true, std::memory_order_release);
					std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
					_stopped = true;
					close_sockets();
					_work_guard.reset();
				}
				g_current_udp_worker = nullptr;
				_worker_running.store(false, std::memory_order_release);
				try { _worker_exit.promise.set_value_at_thread_exit(clean && _stop_completed.load(std::memory_order_acquire)); }
				catch (...) { _stop_post_failed.store(true, std::memory_order_release); }
			});
			_worker_created.store(true, std::memory_order_release);
			std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
			armStopTimer();
		}
		catch (...)
		{
			const bool workerCreated = _worker_created.load(std::memory_order_acquire);
			if (workerCreated) _stop_post_failed.store(true, std::memory_order_release);
			_stop_requested.store(true, std::memory_order_release);
			{
				std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
				_work_guard.reset();
			}
			if (!workerCreated) _worker_running.store(false, std::memory_order_release);
			return false;
		}
	}
	else if (!_worker_running.load(std::memory_order_acquire)) return false;
	if (_stop_requested.load(std::memory_order_acquire)) return false;
	try
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = false;
		_connecting = false;
		_recovery_exhausted = false;
		_send_recovery_attempts = 0;
		_broadcast_recovery_attempts = 0;
		_subscription_recovery_attempts = 0;
		if (!reconnect(3))
		{
			_stopped = true;
			close_sockets();
			return false;
		}
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			_b_socket == NULL || !_b_socket->is_open() || _s_socket == NULL || !_s_socket->is_open())
		{
			_stopped = true;
			close_sockets();
			return false;
		}
		begin_event_epoch();
	}
	catch (...)
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		_close_event_emitted = true;
		close_sockets();
		return false;
	}

	return true;
}

bool ParserUDP::disconnect()
{
	ApiGuard api(this);
	if (!api) return (_api_state.load(std::memory_order_acquire) & API_V2_STOP) == 0;
	const bool on_worker = is_worker_thread();
	if (is_callback_thread() && !on_worker)
		return false;
	if (_stop_requested.load(std::memory_order_acquire))
		return on_worker ? true : shutdown();

	std::unique_lock<std::mutex> lifecycle_lock(_mtx_lifecycle, std::defer_lock);
	if (!on_worker)
		lifecycle_lock.lock();

	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		if (_stop_requested.load(std::memory_order_acquire))
			return true;
		if (_stopped)
			return true;
		_stopped = true;
		const bool had_active_connection = !_close_event_emitted;
		// Consume this connection's close token now, so a queued Close can never
		// be attributed to a later connect.
		_close_event_emitted = true;
		close_sockets();
		if (had_active_connection)
		{
			const uint64_t epoch = _connection_epoch;
			boost::asio::post(_strand, [this, epoch]() { emit_queued_close(epoch); });
		}
	}

	return true;
}

bool ParserUDP::isConnected()
{
	ApiGuard api(this);
	if (!api) return false;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	return _b_socket != NULL && _b_socket->is_open() && _s_socket != NULL && _s_socket->is_open() &&
		!_stopped && !_recovery_exhausted && !_stop_requested.load(std::memory_order_acquire) &&
		_worker_running.load(std::memory_order_acquire);
}


void ParserUDP::subscribe( const CodeSet &vecSymbols )
{
	ApiGuard api(this);
	if (!api) return;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire))
		return;

	auto cit = vecSymbols.begin();
	for(; cit != vecSymbols.end(); cit++)
	{
		const auto &code = *cit;
		if(_set_subs.find(code) == _set_subs.end())
		{
			_set_subs.insert(code);
		}
	}
}

void ParserUDP::unsubscribe(const CodeSet &/*setSymbols*/)
{
	ApiGuard api(this);
	if (!api) return;

}

void ParserUDP::registerSpi( IParserSpi* listener )
{
	ApiGuard api(this);
	if (!api) return;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire))
		return;

	const bool bReplaced = (_sink != NULL);
	_sink = listener;
	if (bReplaced && _sink)
		with_spi([](IParserSpi* sink) { write_log(sink, LL_WARN, "Listener is replaced"); });
}


void ParserUDP::handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad,
	uint64_t connection_epoch, const std::shared_ptr<ReceiveOperation>& operation_owner)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch)
		return;
	if (e == boost::asio::error::operation_aborted)
		return;

	if(e)
	{
		emit_close_once();
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			connection_epoch != _connection_epoch)
			return;

		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserUDP] Error occured while receiving from {}: {}({})", isBroad?"broad port":"subscribe port", e.message().c_str(), e.value()); });
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
			connection_epoch != _connection_epoch)
			return;

		if (!_connecting)
		{
			uint32_t& attempts = isBroad ? _broadcast_recovery_attempts : _subscription_recovery_attempts;
			recover_from_transport_failure(attempts, isBroad ? "broadcast receive" : "subscription receive", connection_epoch);
		}
		return;
	}

	if (bytes_transferred == 0)
	{
		// Empty UDP datagrams are not protocol frames. They must not reach the
		// SPI or count as a successful data receive, but the current healthy
		// channel still needs another receive operation.
		arm_receive(isBroad, connection_epoch);
		return;
	}

	if (isBroad)
		_broadcast_recovery_attempts = 0;
	else
		_subscription_recovery_attempts = 0;

	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch)
		return;

	extract_buffer(bytes_transferred, isBroad, operation_owner->buffer.data(), connection_epoch);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch)
		return;

	arm_receive(isBroad, connection_epoch);
}

void ParserUDP::extract_buffer(uint32_t length, bool isBroad, const char* buffer_data, uint64_t login_epoch)
{
	(void)isBroad;
	if (buffer_data == NULL || length < sizeof(uint32_t))
		return;

	// UDP receive storage has byte alignment only. Copy the header and payload
	// into aligned local objects instead of dereferencing packet casts into it.
	uint32_t message_type = 0;
	std::memcpy(&message_type, buffer_data, sizeof(message_type));
	const char* payload = buffer_data + sizeof(message_type);
	const uint32_t payload_length = length - sizeof(message_type);
	auto is_subscribed = [this](const char* exchg, const char* code) -> bool
	{
		if (std::memchr(exchg, '\0', MAX_EXCHANGE_LENGTH) == NULL ||
			std::memchr(code, '\0', MAX_INSTRUMENT_LENGTH) == NULL)
			return false;
		const char* fullCode = fmtutil::format("{}.{}", exchg, code);
		return _set_subs.find(fullCode) != _set_subs.end();
	};

	if (message_type == UDP_MSG_PUSHTICK || message_type == UDP_MSG_SUBSCRIBE)
	{
		if (payload_length < sizeof(WTSTickStruct))
			return;
		// A complete, recognized message is the only confirmation (card 14).
		if (login_epoch != 0)
		{
			emit_login_event(login_epoch);
			if (_stop_requested.load(std::memory_order_acquire) || _stopped)
				return;
		}
		WTSTickStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSTickData* curTick = WTSTickData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleQuote(curTick, 0); });

			curTick->release();

			static std::atomic<uint32_t> recv_cnt{0};
			const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
			if (received % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} ticks received in total", received); });
		}
	}
	else if (message_type == UDP_MSG_PUSHORDDTL)
	{
		if (payload_length < sizeof(WTSOrdDtlStruct))
			return;
		// A complete, recognized message is the only confirmation (card 14).
		if (login_epoch != 0)
		{
			emit_login_event(login_epoch);
			if (_stop_requested.load(std::memory_order_acquire) || _stopped)
				return;
		}
		WTSOrdDtlStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSOrdDtlData* curData = WTSOrdDtlData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleOrderDetail(curData); });

			curData->release();

			static std::atomic<uint32_t> recv_cnt{0};
			const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
			if (received % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} order details received in total", received); });
		}
	}
	else if (message_type == UDP_MSG_PUSHORDQUE)
	{
		if (payload_length < sizeof(WTSOrdQueStruct))
			return;
		// A complete, recognized message is the only confirmation (card 14).
		if (login_epoch != 0)
		{
			emit_login_event(login_epoch);
			if (_stop_requested.load(std::memory_order_acquire) || _stopped)
				return;
		}
		WTSOrdQueStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSOrdQueData* curData = WTSOrdQueData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleOrderQueue(curData); });

			curData->release();

			static std::atomic<uint32_t> recv_cnt{0};
			const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
			if (received % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} order queues received in total", received); });
		}
	}
	else if (message_type == UDP_MSG_PUSHTRANS)
	{
		if (payload_length < sizeof(WTSTransStruct))
			return;
		// A complete, recognized message is the only confirmation (card 14).
		if (login_epoch != 0)
		{
			emit_login_event(login_epoch);
			if (_stop_requested.load(std::memory_order_acquire) || _stopped)
				return;
		}
		WTSTransStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSTransData* curData = WTSTransData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleTransaction(curData); });

			curData->release();

			static std::atomic<uint32_t> recv_cnt{0};
			const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
			if (received % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} transactions received in total", received); });
		}
	}
	// Unknown message types are ignored; in particular, they never cause a
	// payload cast or a callback with bytes from outside the received datagram.
}

void ParserUDP::doOnConnected()
{
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Connect, 0); });
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Login, 0); });
}

void ParserUDP::doOnDisconnected()
{
	emit_close_once();
}
