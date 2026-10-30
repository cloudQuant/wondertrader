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
	EXPORT_FLAG IParserApi* createParser()
	{
		ParserUDP* parser = new ParserUDP();
		return parser;
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
};



ParserUDP::ParserUDP()
	: _strand(boost::asio::make_strand(_io_service))
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
	// The exported deleteParser entry point retains the owner when this is
	// called from an SPI callback. Direct deletion from that callback cannot be
	// made safe by the legacy void IParserApi::release() contract.
	shutdown();
}

bool ParserUDP::init( WTSVariant* config )
{
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
		_close_event_emitted = false;
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
	_stop_requested.store(true, std::memory_order_release);

	if (is_worker_thread())
	{
		// The active handler already runs on the only io_context worker, so it
		// can close the sockets inline. It cannot prove the callback stack has
		// drained or join itself; keep the thread owner for an external retry.
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		close_sockets();
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
	uint32_t length = 0;
	for (auto& code : _set_subs)
	{
		if (length > 0)
		{
			req->_data[length] = ',';
			length++;
		}

		std::size_t pos = code.find('.');
		if (pos != std::string::npos)
			strcpy(req->_data + length, (char*)code.c_str() + pos + 1);
		else
			strcpy(req->_data + length, code.c_str());

		length += code.size();

		if (length > 1000)
		{
			StdUniqueLock lock(_mtx_queue);
			_send_queue.push(data);

			data.resize(sizeof(UDPReqPacket), 0);
			req = (UDPReqPacket*)data.data();
			req->_type = UDP_MSG_SUBSCRIBE;
			length = 0;
		}
	}

    if (length > 0) {
        StdUniqueLock lock(_mtx_queue);
        _send_queue.push(data);
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
	if (is_worker_thread() || is_callback_thread())
		return false;

	std::lock_guard<std::mutex> lifecycle_lock(_mtx_lifecycle);
	if (_stop_requested.load(std::memory_order_acquire))
		return false;

	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
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
		_thrd_parser.reset();
	}

	_io_service.restart();
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
		_close_event_emitted = false;
		_worker_running.store(true, std::memory_order_release);

		_thrd_parser.reset(new StdThread([this]()
		{
			g_current_udp_worker = this;
			try
			{
				_io_service.run();
			}
			catch (...)
			{
				_stop_requested.store(true, std::memory_order_release);
				std::lock_guard<std::recursive_mutex> worker_io_lock(_mtx_io);
				_stopped = true;
				close_sockets();
			}
			g_current_udp_worker = nullptr;
			_worker_running.store(false, std::memory_order_release);
		}));
	}
	catch (...)
	{
		_worker_running.store(false, std::memory_order_release);
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
		close_sockets();
		if (had_active_connection)
			boost::asio::post(_strand, boost::bind(&ParserUDP::doOnDisconnected, this));
	}

	return true;
}

bool ParserUDP::isConnected()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	return _b_socket != NULL && _b_socket->is_open() && _s_socket != NULL && _s_socket->is_open() &&
		!_stopped && !_recovery_exhausted && !_stop_requested.load(std::memory_order_acquire) &&
		_worker_running.load(std::memory_order_acquire);
}


void ParserUDP::subscribe( const CodeSet &vecSymbols )
{
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

}

void ParserUDP::registerSpi( IParserSpi* listener )
{
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

	extract_buffer(bytes_transferred, isBroad, operation_owner->buffer.data());
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || _recovery_exhausted ||
		connection_epoch != _connection_epoch)
		return;

	arm_receive(isBroad, connection_epoch);
}

void ParserUDP::extract_buffer(uint32_t length, bool isBroad, const char* buffer_data)
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
		WTSTickStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSTickData* curTick = WTSTickData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleQuote(curTick, 0); });

			curTick->release();

			static uint32_t recv_cnt = 0;
			recv_cnt++;
			if (recv_cnt % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} ticks received in total", recv_cnt); });
		}
	}
	else if (message_type == UDP_MSG_PUSHORDDTL)
	{
		if (payload_length < sizeof(WTSOrdDtlStruct))
			return;
		WTSOrdDtlStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSOrdDtlData* curData = WTSOrdDtlData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleOrderDetail(curData); });

			curData->release();

			static uint32_t recv_cnt = 0;
			recv_cnt++;
			if (recv_cnt % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} order details received in total", recv_cnt); });
		}
	}
	else if (message_type == UDP_MSG_PUSHORDQUE)
	{
		if (payload_length < sizeof(WTSOrdQueStruct))
			return;
		WTSOrdQueStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSOrdQueData* curData = WTSOrdQueData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleOrderQueue(curData); });

			curData->release();

			static uint32_t recv_cnt = 0;
			recv_cnt++;
			if (recv_cnt % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} order queues received in total", recv_cnt); });
		}
	}
	else if (message_type == UDP_MSG_PUSHTRANS)
	{
		if (payload_length < sizeof(WTSTransStruct))
			return;
		WTSTransStruct data;
		std::memcpy(&data, payload, sizeof(data));
		if (is_subscribed(data.exchg, data.code))
		{
			WTSTransData* curData = WTSTransData::create(data);
			with_spi([&](IParserSpi* sink) { sink->handleTransaction(curData); });

			curData->release();

			static uint32_t recv_cnt = 0;
			recv_cnt++;
			if (recv_cnt % _gpsize == 0)
				with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserUDP] {} transactions received in total", recv_cnt); });
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
