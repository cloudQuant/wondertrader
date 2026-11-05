/*!
 * \file ParserXeleSkt.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserXeleSkt.h"
#include <boost/asio/post.hpp>
#include "md_struct.h"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Share/decimal.h"
#include "../Share/Converter.hpp"

#include <boost/bind.hpp>
#include <atomic>
#include <chrono>
#include <ctime>
#include <exception>
#include <memory>
#include <stdexcept>
#include <system_error>

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->handleParserLog(ll, buffer);
}

inline uint32_t strToTime(const char* strTime)
{
	static char str[10] = { 0 };
	[[maybe_unused]] const char *pos = strTime;
	int idx = 0;
	auto len = strlen(strTime);
	for (std::size_t i = 0; i < len; i++)
	{
		if (strTime[i] != ':')
		{
			str[idx] = strTime[i];
			idx++;
		}
	}
	str[idx] = '\0';

	return convert::to_uint32(str);
}

inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}

namespace
{
	thread_local ParserXeleSkt* g_current_xele_worker = nullptr;
#ifdef WT_PARSERXELE_TEST_HOOKS
	std::atomic<bool> g_fail_next_worker_start{ false };
	std::atomic<bool> g_fail_next_stop_timer{ false };
#endif

	inline bool safeLocalTime(uint64_t seconds, struct tm& out)
	{
		const time_t value = static_cast<time_t>(seconds);
#ifdef _WIN32
		return localtime_s(&out, &value) == 0;
#else
		return localtime_r(&value, &out) != NULL;
#endif
	}
}

extern "C"
{
#ifdef WT_PARSERXELE_TEST_HOOKS
	EXPORT_FLAG void parserXeleTestFailNextWorkerStart() { g_fail_next_worker_start.store(true); }
	EXPORT_FLAG void parserXeleTestFailNextStopTimer() { g_fail_next_stop_timer.store(true); }
	EXPORT_FLAG bool parserXeleTestLegacyShutdown(IParserApi* api) { return static_cast<ParserXeleSkt*>(api)->shutdown(); }
#endif
	EXPORT_FLAG IParserApi* createParser()
	{
		try { return new ParserXeleSkt(); }
		catch (...) { return NULL; }
	}

	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			// The legacy remover must not delete an owner whose worker cannot be
			// joined from this thread (for example from inside an SPI callback).
			ParserXeleSkt* xele = dynamic_cast<ParserXeleSkt*>(parser);
			if (xele != NULL && !xele->shutdown())
				return;

			delete parser;
			parser = NULL;
		}
	}

	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL xele_preflight() { return WT_PARSER_LIFECYCLE_V2_OK; }
	static ParserXeleSkt* xele_from_handle(void* handle) { return static_cast<ParserXeleSkt*>(static_cast<IParserApi*>(handle)); }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL xele_request(void* handle)
	{ return handle ? xele_from_handle(handle)->requestStopV2() : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL xele_can_wait(void* handle)
	{ return handle ? xele_from_handle(handle)->canWaitV2() : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL xele_wait(void* handle, uint32_t timeout)
	{ return handle ? xele_from_handle(handle)->waitDrainedV2(timeout) : WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT; }
	static WtParserLifecycleV2Status WT_PARSER_LIFECYCLE_V2_CALL xele_destroy(void** handle)
	{
		if (!handle || !*handle) return WT_PARSER_LIFECYCLE_V2_INVALID_ARGUMENT;
		ParserXeleSkt* parser = xele_from_handle(*handle);
		if (!parser->prepareDestroyV2()) return WT_PARSER_LIFECYCLE_V2_ERROR;
		try { delete parser; *handle = NULL; return WT_PARSER_LIFECYCLE_V2_OK; }
		catch (...) { return WT_PARSER_LIFECYCLE_V2_ERROR; }
	}
	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		static const WtParserLifecycleApiV2 table = { sizeof(WtParserLifecycleApiV2), WT_PARSER_LIFECYCLE_V2_ABI_VERSION,
			WT_PARSER_LIFECYCLE_V2_REQUIRED_CAPABILITIES, {0, 0}, xele_preflight, xele_request, xele_can_wait, xele_wait, xele_destroy };
		return major == WT_PARSER_LIFECYCLE_V2_ABI_MAJOR ? &table : NULL;
	}
};



ParserXeleSkt::ParserXeleSkt()
	: _tcp_port(0)
	, _mcast_port(0)
	, _gpsize(1000)
	, _addresses_valid(false)
	, _stop_timer(_io_service)
	, _retry_timer(_io_service)
	, _strand(boost::asio::make_strand(_io_service))
	, _udp_socket(NULL)
	, _sink(NULL)
	, _bd_mgr(NULL)
	, _stopped(false)
	, _prepared(false)
	, _connection_epoch(0)
	, _close_event_emitted(true)
	, _tick_cache(NULL)
{
}


ParserXeleSkt::~ParserXeleSkt()
{
	if (!_v2_joined.load(std::memory_order_acquire))
	{
		// The exported deleteParser entry point retains the owner when this is
		// called from the worker or an SPI callback. A direct delete from that
		// context violates the owner contract; the joinable thread member then
		// fails fast in its destructor instead of running on freed memory.
		(void)shutdown();
	}
	if (_tick_cache != NULL)
	{
		_tick_cache->release();
		_tick_cache = NULL;
	}
}

ParserXeleSkt::ApiGuard::ApiGuard(ParserXeleSkt* parser, bool allowClosed) noexcept
	: _parser(parser), _frame{ parser, NULL }, _entered(false)
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

ParserXeleSkt::ApiGuard::~ApiGuard()
{
	if (!_entered) return;
	_api_stack_top = _frame.previous;
	_parser->_api_state.fetch_sub(1, std::memory_order_release);
}

bool ParserXeleSkt::is_api_thread() const noexcept
{
	for (ApiFrame* frame = _api_stack_top; frame; frame = frame->previous)
		if (frame->parser == this) return true;
	return false;
}

bool ParserXeleSkt::is_worker_thread() const noexcept
{
	return g_current_xele_worker == this;
}

bool ParserXeleSkt::is_callback_thread() const noexcept
{
	for (CallbackFrame* frame = _callback_stack_top; frame != nullptr; frame = frame->previous)
		if (frame->parser == this) return true;
	return false;
}

void ParserXeleSkt::armStopTimer()
{
#ifdef WT_PARSERXELE_TEST_HOOKS
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

void ParserXeleSkt::stopOnWorkerV2()
{
	std::lock_guard<std::recursive_mutex> lock(_mtx_io);
	_stopped = true;
	close_sockets();
	_work_guard.reset();
	_stop_completed.store(true, std::memory_order_release);
}

WtParserLifecycleV2Status ParserXeleSkt::requestStopV2() noexcept
{
	static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free,
		"ParserXeleSkt V2 stop requires lock-free atomic operations");
	_api_state.fetch_or(API_CLOSED | API_V2_STOP, std::memory_order_acq_rel);
	_v2_stop.store(true, std::memory_order_release);
	_stop_requested.store(true, std::memory_order_release);
	return _stop_post_failed.load(std::memory_order_acquire) ? WT_PARSER_LIFECYCLE_V2_ERROR : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserXeleSkt::canWaitV2() const noexcept
{
	return (is_worker_thread() || is_callback_thread() || is_api_thread()) ?
		WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserXeleSkt::waitDrainedV2(uint32_t timeoutMs) noexcept
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
			if (!exited && _worker_exit.exited.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
			{
				if (!_worker_exit.exited.get()) return WT_PARSER_LIFECYCLE_V2_ERROR;
				exited = true;
			}
			if (exited)
			{
				std::unique_lock<std::recursive_mutex> io(_mtx_io, std::try_to_lock);
				if (io.owns_lock())
				{
					_sink = NULL;
					_bd_mgr = NULL;
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

bool ParserXeleSkt::prepareDestroyV2() noexcept
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

bool ParserXeleSkt::init( WTSVariant* config )
{
	ApiGuard api(this);
	if (!api) return false;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	// Card 9c: a successful init is final; a later call keeps the original.
	if (_initialized_once || _stop_requested.load(std::memory_order_acquire))
		return false;

	_addresses_valid = false;
	if (config == NULL)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Missing parser configuration"); });
		return false;
	}

	const std::string tcp_host = config->getCString("tcp_host");
	const std::string mcast_host = config->getCString("mcast_host");
	const std::string local_host = config->getCString("local_host");
	if (tcp_host.empty() || mcast_host.empty() || local_host.empty())
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] TCP, multicast and local addresses are required"); });
		return false;
	}

	boost::system::error_code ec;
	const ip::address tcp_addr = ip::make_address(tcp_host, ec);
	if (ec)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Invalid tcp_host address '{}': {}", tcp_host, ec.message()); });
		return false;
	}

	const ip::address_v4 mcast_addr = ip::make_address_v4(mcast_host, ec);
	if (ec || !mcast_addr.is_multicast())
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Invalid mcast_host multicast address '{}': {}",
			mcast_host, ec ? ec.message() : "address is not multicast"); });
		return false;
	}

	const ip::address_v4 local_addr = ip::make_address_v4(local_host, ec);
	if (ec)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Invalid local_host IPv4 address '{}': {}", local_host, ec.message()); });
		return false;
	}

	_tcp_host = tcp_host;
	_tcp_port = config->getInt32("tcp_port");
	_mcast_host = mcast_host;
	_mcast_port = config->getInt32("mcast_port");
	_local_host = local_host;
	_gpsize = config->getUInt32("gpsize");
	if (_gpsize == 0)
		_gpsize = 1000;

	_tcp_ep = ip::tcp::endpoint(tcp_addr, _tcp_port);
	_mcast_ep = ip::udp::endpoint(ip::address_v4::any(), _mcast_port);
	_mcast_addr = mcast_addr;
	_local_addr = local_addr;
	_addresses_valid = true;
	_initialized_once = true;

	return true;
}

void ParserXeleSkt::release()
{
	// The legacy interface cannot report a self-join failure. In that case the
	// exported deleter leaves the parser pointer intact so the owner can retry.
	(void)shutdown();
}

void ParserXeleSkt::close_sockets()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_udp_socket != NULL)
	{
		boost::system::error_code ignored;
		_udp_socket->cancel(ignored);
		_udp_socket->close(ignored);
		delete _udp_socket;
		_udp_socket = NULL;
	}
	try { _retry_timer.cancel(); } catch (...) {}
}

void ParserXeleSkt::emit_close_once()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_close_event_emitted)
		return;

	_close_event_emitted = true;
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Close, 0); });
}

bool ParserXeleSkt::shutdown()
{
	ApiGuard api(this, true);
	if (!api) return false;
	_api_state.fetch_or(API_CLOSED, std::memory_order_acq_rel);
	_stop_requested.store(true, std::memory_order_release);

	if (is_worker_thread())
	{
		// The active handler runs on the only io_context worker, so it can close
		// the channel inline, but it cannot join itself. The owner is retained.
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		close_sockets();
		try { _stop_timer.cancel(); } catch (...) { return false; }
		_work_guard.reset();
		_stop_completed.store(true, std::memory_order_release);
		return false;
	}
	if (is_callback_thread())
	{
		// A non-worker callback (for example a log during connect) cannot wait
		// for its own stack to drain; later dispatch is already blocked.
		return false;
	}

	std::lock_guard<std::mutex> lifecycle_lock(_mtx_lifecycle);
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		_stopped = true;
		close_sockets();
		try { _stop_timer.cancel(); } catch (...) { return false; }
		_work_guard.reset();
		_stop_completed.store(true, std::memory_order_release);
	}

	if (_thrd_parser && _thrd_parser->joinable())
	{
		try { _thrd_parser->join(); }
		catch (...) { return false; }
	}
	_thrd_parser.reset();
	_worker_running.store(false, std::memory_order_release);
	return true;
}

bool ParserXeleSkt::reconnect()
{
	if (!_addresses_valid)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Cannot connect before valid addresses are configured"); });
		return false;
	}

	if(!_prepared)
	{
		if (!prepare())
			return false;
		_prepared = true;
	}

	return open_channel();
}

bool ParserXeleSkt::open_channel()
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire))
		return false;

	{//建立广播通道
		if (_udp_socket != NULL)
		{
			boost::system::error_code ec;
			_udp_socket->cancel(ec);
			_udp_socket->close(ec);
			delete _udp_socket;
			_udp_socket = NULL;
		}

		++_connection_epoch;
		if (_connection_epoch == 0)
			++_connection_epoch;

		try
		{
			_udp_socket = new ip::udp::socket(_io_service);
			_udp_socket->open(_mcast_ep.protocol());
			_udp_socket->set_option(ip::udp::socket::reuse_address(true));
			_udp_socket->bind(_mcast_ep);
			_udp_socket->set_option(ip::multicast::join_group(_mcast_addr, _local_addr));

			arm_receive(_connection_epoch);
		}
		catch (const std::exception& e)
		{
			if (_udp_socket != NULL)
			{
				boost::system::error_code ec;
				_udp_socket->cancel(ec);
				_udp_socket->close(ec);
				delete _udp_socket;
				_udp_socket = NULL;
			}
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Failed to open multicast socket: {}", e.what()); });
			return false;
		}
	}
	with_spi([&](IParserSpi* sink) { write_log(sink, LL_INFO, "[ParserXeleSkt] Ready to receive from multicast tunnel {}:{}...", _mcast_host, _mcast_port); });
	// A callback above may have stopped or disconnected this parser.
	return !_stop_requested.load(std::memory_order_acquire) && _udp_socket != NULL && _udp_socket->is_open();
}

void ParserXeleSkt::arm_receive(uint64_t connection_epoch)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || connection_epoch != _connection_epoch ||
		_udp_socket == NULL || !_udp_socket->is_open())
		return;

	auto operation = std::make_shared<ReceiveOperation>();
	_udp_socket->async_receive_from(boost::asio::buffer(operation->buffer.data(), operation->buffer.size()), operation->endpoint,
		[this, connection_epoch, operation](const boost::system::error_code& e, std::size_t bytes_transferred)
		{
			handle_udp_read(e, bytes_transferred, connection_epoch, operation);
		});
}

bool ParserXeleSkt::prepare()
{
	// The blocking TCP snapshot read runs without the I/O mutex. Inputs are
	// copied under the mutex and the result is committed under it.
	IBaseDataMgr* bd_mgr = NULL;
	CodeSet subscriptions;
	ip::tcp::endpoint tcp_ep;
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		bd_mgr = _bd_mgr;
		subscriptions = _set_subs;
		tcp_ep = _tcp_ep;
	}

	boost::asio::io_context io_context;
	ip::tcp::socket s(io_context);
	boost::system::error_code ec;
	s.connect(tcp_ep, ec);
	if(ec)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Preparing snapshots failed, connecting tcp tunnel {}:{} error: {}", _tcp_host, _tcp_port, ec.message()); });
		return false;
	}

	auto cacheReleaser = [](TickCache* cache) {
		if (cache != NULL)
			cache->release();
	};
	std::unique_ptr<TickCache, decltype(cacheReleaser)> candidateCache(
		TickCache::create(), cacheReleaser);
	if (!candidateCache)
	{
		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Failed to allocate snapshot cache"); });
		s.close(ec);
		return false;
	}
	wt_hashmap<int, double> candidatePriceScales;

	with_spi([&](IParserSpi* sink) { write_log(sink, LL_INFO, "[ParserXeleSkt] Preparing snapshots via tcp tunnel {}:{}...", _tcp_host, _tcp_port); });

	boost::array<char, 4096> buffer;
	std::string content;
	auto snap_size = sizeof(CXeleShfeSnapShot) + sizeof(CXeleShfeMarketHead);
	for (;;)
	{
		if (_stop_requested.load(std::memory_order_acquire))
		{
			s.close(ec);
			return false;
		}
		std::size_t reply_length = boost::asio::read(s, boost::asio::buffer(buffer.data(), buffer.size()), ec);
		const bool readEof = ec == boost::asio::error::eof;
		const bool readError = static_cast<bool>(ec) && !readEof;
		xele_skt_detail::SnapshotReadStatus readStatus;
		try
		{
			readStatus = xele_skt_detail::processSnapshotReadResult(
				content, buffer.data(), reply_length, readEof, readError, snap_size,
				[&](const char* snapshot, std::size_t) {
					const CXeleShfeMarketHead* mh = reinterpret_cast<const CXeleShfeMarketHead*>(snapshot);
					if (mh->Version != XELE_MD_DATA_VERSION || mh->Type != MESSAGE_SNAP_SHOT)
						return false;

					const CXeleShfeSnapShot* p = reinterpret_cast<const CXeleShfeSnapShot*>(
						snapshot + sizeof(CXeleShfeMarketHead));
					const int instrumentNo = p->InstrumentNo;
					std::string instrumentCode;
					if (!xele_skt_detail::tryGetInstrumentCode(p->InstrumentID,
						sizeof(p->InstrumentID), instrumentCode))
						return true;

					if (bd_mgr == NULL)
						return true;
					WTSContractInfo* ct = bd_mgr->getContract(instrumentCode.c_str());
					if (ct == NULL)
						return true;

					auto it = subscriptions.find(ct->getFullCode());
					if (it == subscriptions.end())
						return true;

					candidatePriceScales[instrumentNo] = p->PriceTick;
					WTSTickData* tick = WTSTickData::create(instrumentCode.c_str());
					if (tick == NULL)
						return true;
					auto tickReleaser = [](WTSTickData* item) {
						if (item != NULL)
							item->release();
					};
					std::unique_ptr<WTSTickData, decltype(tickReleaser)> tickGuard(tick, tickReleaser);
					tick->setContractInfo(ct);

					WTSTickStruct& quote = tick->getTickStruct();
					wt_strcpy(quote.exchg, ct->getExchg());

					quote.action_date = strToTime(p->ActionDay);
					quote.action_time = strToTime(p->UpdateTime) * 1000 + p->UpdateMilliSec;
					quote.price = checkValid(p->LastPrice);
					quote.open = checkValid(p->OpenPrice);
					quote.high = checkValid(p->HighestPrice);
					quote.low = checkValid(p->LowestPrice);
					quote.total_volume = p->Volume;
					quote.trading_date = 0;
					quote.settle_price = checkValid(p->SettlementPrice);
					quote.total_turnover = p->Turnover;
					quote.open_interest = p->OpenInterest;
					quote.upper_limit = p->UpperLimitPrice;
					quote.lower_limit = p->LowerLimitPrice;
					quote.pre_close = p->PreClosePrice;
					quote.pre_settle = p->PreSettlementPrice;
					quote.pre_interest = p->PreOpenInterest;
					quote.ask_prices[0] = p->AskPrice;
					quote.bid_prices[0] = p->BidPrice;
					quote.ask_qty[0] = p->AskVolume;
					quote.bid_qty[0] = p->BidVolume;

					candidateCache->add(instrumentNo, tick, false);
					tickGuard.release();
					return true;
				});
		}
		catch (const std::exception& e)
		{
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Processing TCP snapshot stream failed: {}", e.what()); });
			s.close(ec);
			return false;
		}
		catch (...)
		{
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Processing TCP snapshot stream failed with an unknown exception"); });
			s.close(ec);
			return false;
		}
		if (readStatus == xele_skt_detail::SnapshotReadStatus::continue_reading)
			continue;
		if (readStatus == xele_skt_detail::SnapshotReadStatus::complete)
			break;

		const boost::system::error_code readEc = ec;
		if (readStatus == xele_skt_detail::SnapshotReadStatus::invalid_frame)
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Invalid snapshot header in TCP snapshot stream"); });
		else if (readStatus == xele_skt_detail::SnapshotReadStatus::io_error)
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Reading TCP snapshot stream failed: {}", readEc.message()); });
		else
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] TCP snapshot stream ended with an incomplete snapshot"); });

		s.close(ec);
		return false;
	}

	s.close(ec);
	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		TickCache* oldCache = _tick_cache;
		_price_scales.swap(candidatePriceScales);
		_tick_cache = candidateCache.release();
		if (oldCache != NULL)
			oldCache->release();
	}

	with_spi([&](IParserSpi* sink) { write_log(sink, LL_INFO, "[ParserXeleSkt] All snapshots synced"); });

	return true;
}

bool ParserXeleSkt::ensure_worker()
{
	if (_thrd_parser)
		return _worker_running.load(std::memory_order_acquire);

	try
	{
		_io_service.restart();
		{
			std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
			_work_guard.reset(new WorkGuard(boost::asio::make_work_guard(_io_service)));
		}
		_worker_running.store(true, std::memory_order_release);
#ifdef WT_PARSERXELE_TEST_HOOKS
		if (g_fail_next_worker_start.exchange(false)) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
#endif
		_thrd_parser = std::make_shared<StdThread>([this]()
		{
			g_current_xele_worker = this;
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
			g_current_xele_worker = nullptr;
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
	return true;
}

bool ParserXeleSkt::connect()
{
	ApiGuard api(this);
	if (!api) return false;
	// Card 9d: lifecycle re-entry from the worker or an SPI callback is busy.
	if (is_worker_thread() || is_callback_thread())
		return false;

	std::lock_guard<std::mutex> lifecycle_lock(_mtx_lifecycle);
	if (_stop_requested.load(std::memory_order_acquire))
		return false;

	{
		std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
		if (!_initialized_once || !_addresses_valid)
			return false;
		if (_udp_socket != NULL && _udp_socket->is_open() && !_stopped &&
			_worker_running.load(std::memory_order_acquire))
			return true;
	}

	// The snapshot is synchronized before any worker exists; a failed snapshot
	// leaves no asynchronous work behind.
	if (!_prepared)
	{
		if (!prepare())
			return false;
		_prepared = true;
	}

	if (!ensure_worker())
		return false;
	if (_stop_requested.load(std::memory_order_acquire))
		return false;

	return open_on_worker();
}

bool ParserXeleSkt::open_on_worker()
{
	// The socket is opened (and registered with the reactor) on the worker
	// that runs the reactor; registering it from this thread while the worker
	// is inside the reactor has no ordering visible to the worker other than
	// the kernel event queue. The caller waits only while the worker lives.
	auto result = std::make_shared<std::promise<bool>>();
	std::future<bool> opened = result->get_future();
	try
	{
		boost::asio::post(_strand, [this, result]() {
			bool ok = false;
			try
			{
				std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
				if (!_stop_requested.load(std::memory_order_acquire))
				{
					_stopped = false;
					ok = reconnect();
					if (ok)
						_close_event_emitted = false;
					else
					{
						_stopped = true;
						close_sockets();
					}
				}
			}
			catch (...)
			{
				std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
				_stopped = true;
				_close_event_emitted = true;
				close_sockets();
				ok = false;
			}
			result->set_value(ok);
		});
	}
	catch (...)
	{
		return false;
	}

	while (opened.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready)
	{
		if (!_worker_running.load(std::memory_order_acquire))
			return false;
	}
	try { return opened.get(); }
	catch (...) { return false; }
}

bool ParserXeleSkt::disconnect()
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
		_stopped = true;
		const bool had_active_connection = !_close_event_emitted;
		// Consume this connection's close token now, so a queued Close can never
		// be attributed to a later connect.
		_close_event_emitted = true;
		close_sockets();
		if (had_active_connection)
			boost::asio::post(_strand, [this]() { doOnDisconnected(); });
	}

	return true;
}

bool ParserXeleSkt::isConnected()
{
	ApiGuard api(this);
	if (!api) return false;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	return _udp_socket != NULL && _udp_socket->is_open() && !_stopped &&
		!_stop_requested.load(std::memory_order_acquire) && _worker_running.load(std::memory_order_acquire);
}


void ParserXeleSkt::subscribe( const CodeSet &vecSymbols )
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

void ParserXeleSkt::unsubscribe(const CodeSet &/*setSymbols*/)
{
	ApiGuard api(this);
	if (!api) return;
}

void ParserXeleSkt::registerSpi( IParserSpi* listener )
{
	ApiGuard api(this);
	if (!api) return;
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire))
		return;

	bool bReplaced = (_sink!=NULL);
	_sink = listener;
	if(bReplaced && _sink)
	{
		with_spi([](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserXeleSkt] Listener is replaced"); });
	}

	if (_sink)
	{
		IBaseDataMgr* manager = NULL;
		with_spi([&](IParserSpi* sink) { manager = sink->getBaseDataMgr(); });
		if (_stop_requested.load(std::memory_order_acquire) || _sink != listener)
			return;
		_bd_mgr = manager;
	}
}

void ParserXeleSkt::schedule_retry(uint64_t failed_epoch)
{
	// Replaces the former 2s sleep on the worker: the delay is a cancellable
	// timer, so stop/disconnect are observed without waiting for it.
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || failed_epoch != _connection_epoch)
		return;
	_retry_timer.expires_after(std::chrono::seconds(2));
	_retry_timer.async_wait([this, failed_epoch](const boost::system::error_code& error) {
		std::lock_guard<std::recursive_mutex> lock(_mtx_io);
		if (error || _stop_requested.load(std::memory_order_acquire) || _stopped ||
			failed_epoch != _connection_epoch)
			return;
		if (open_channel())
			_close_event_emitted = false;
	});
}

void ParserXeleSkt::handle_udp_read(const boost::system::error_code& e, std::size_t bytes_transferred,
	uint64_t connection_epoch, const std::shared_ptr<ReceiveOperation>& operation)
{
	std::lock_guard<std::recursive_mutex> io_lock(_mtx_io);
	if (_stop_requested.load(std::memory_order_acquire) || _stopped || connection_epoch != _connection_epoch)
		return;
	if (e == boost::asio::error::operation_aborted)
		return;

	if(e)
	{
		emit_close_once();
		if (_stop_requested.load(std::memory_order_acquire) || _stopped || connection_epoch != _connection_epoch)
			return;

		with_spi([&](IParserSpi* sink) { write_log(sink, LL_ERROR, "[ParserXeleSkt] Error occured while receiving: {}({})", e.message().c_str(), e.value()); });
		schedule_retry(connection_epoch);
		return;
	}

	if (bytes_transferred > 0)
		extract_buffer(static_cast<uint32_t>(bytes_transferred), operation->buffer.data());

	if (_stop_requested.load(std::memory_order_acquire) || _stopped || connection_epoch != _connection_epoch)
		return;
	arm_receive(connection_epoch);
}

#define XELE_MD_EPOCH 1546272000 // unix timestamp of 2019/1/1 0:0:0
#define FOUR_BIT 4
#define FIVE_HUNDRED_MS 500
#define ZERO_MS 0
#define XELE_MD_MS_MASK 0x000f

void ParserXeleSkt::extract_buffer(uint32_t length, const char* buffer_data)
{
	// Called on the worker with _mtx_io held. Every frame must fit inside the
	// received datagram; an unknown type stops parsing instead of looping.
	if (buffer_data == NULL || _tick_cache == NULL)
		return;
	std::size_t len = 0;
	while (length > len)
	{
		if (length - len < sizeof(CXeleShfeMarketHead))
			return;
		const char * data = buffer_data + len;
		const CXeleShfeMarketHead *mh = reinterpret_cast<const CXeleShfeMarketHead *>(data);
		int8_t version = mh->Version;
		if (version != XELE_MD_DATA_VERSION)
			return;

		int8_t type = mh->Type;
		if (type == MESSAGE_MARKET_DATA)
		{
			const CXeleShfeMarketData *p = reinterpret_cast<const CXeleShfeMarketData *>(data + sizeof(CXeleShfeMarketHead));
			constexpr std::size_t packLen = sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeMarketData);
			if (length - len < packLen)
				return;
			len += packLen;

			//write_log(_sink, LL_DEBUG, "[ParserXeleSkt] Receiving market data, length: {}", packLen);

			WTSTickData* tick = (WTSTickData*)_tick_cache->get(p->InstrumentNo);
			if (tick != NULL && _bd_mgr != NULL && tick->getContractInfo() != NULL)
			{
				uint64_t SnapTime = ((p->SnapDateTime) >> FOUR_BIT) + XELE_MD_EPOCH;
				struct tm tmNow;
				if (!safeLocalTime(SnapTime, tmNow))
					continue;
				const struct tm* tNow = &tmNow;
				uint32_t SnapMillisec = (p->SnapDateTime & XELE_MD_MS_MASK) ? FIVE_HUNDRED_MS : ZERO_MS;
				uint32_t actDate = (tNow->tm_year + 1900) * 10000 + (tNow->tm_mon + 1) * 100 + tNow->tm_mday;
				uint32_t actTime = tNow->tm_hour * 10000 + tNow->tm_min * 100 + tNow->tm_sec;
				actTime = actTime * 1000 + SnapMillisec;

				double scale = _price_scales[p->InstrumentNo];
				WTSTickStruct& quote = tick->getTickStruct();
				quote.action_date = actDate;
				quote.action_time = actTime;
				if(quote.trading_date == 0)
				{
					quote.trading_date = _bd_mgr->calcTradingDate(tick->getContractInfo()->getFullPid(), actDate, actTime / 100000);
				}

				quote.price = p->LastPrice*scale;
				quote.high = std::max(quote.high, p->LastPrice*scale);
				quote.low = std::min(quote.low, p->LastPrice*scale);
				if (decimal::eq(quote.open, 0))
					quote.open = quote.price;

				quote.total_volume = p->Volume;
				quote.total_turnover = (double)p->Turnover;
				quote.open_interest = (double)p->OpenInterest;

				quote.volume = 0;
				quote.turn_over = 0;
				quote.diff_interest = 0;

				//委卖价格
				quote.ask_prices[0] = p->AskPrice*scale;
				//委买价格
				quote.bid_prices[0] = p->BidPrice*scale;
				//委卖量
				quote.ask_qty[0] = p->AskVolume;
				//委买量
				quote.bid_qty[0] = p->BidVolume;

				with_spi([&](IParserSpi* sink) { sink->handleQuote(tick, 1); });

				static std::atomic<uint32_t> recv_cnt{0};
				const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
				if (received % _gpsize == 0)
					with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserXeleSkt] {} ticks received in total", received); });
				if (_stop_requested.load(std::memory_order_acquire) || _stopped)
					return;
			}
		}
		else if (type == MESSAGE_DEPTH)
		{
			const CXeleShfeDepthMarketData *p = reinterpret_cast<const CXeleShfeDepthMarketData *>(data + sizeof(CXeleShfeMarketHead));
			constexpr std::size_t packLen = sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeDepthMarketData);
			if (length - len < packLen)
				return;
			len += packLen;

			//write_log(_sink, LL_DEBUG, "[ParserXeleSkt] Receiving depth market data of {}, length: {}", p->InstrumentNo, packLen);

			WTSTickData* tick = (WTSTickData*)_tick_cache->get(p->InstrumentNo);
			if (tick != NULL && _bd_mgr != NULL && tick->getContractInfo() != NULL)
			{
				uint64_t SnapTime = ((p->SnapDateTime) >> FOUR_BIT) + XELE_MD_EPOCH;
				struct tm tmNow;
				if (!safeLocalTime(SnapTime, tmNow))
					continue;
				const struct tm* tNow = &tmNow;
				uint32_t SnapMillisec = (p->SnapDateTime & XELE_MD_MS_MASK) ? FIVE_HUNDRED_MS : ZERO_MS;
				uint32_t actDate = (tNow->tm_year + 1900) * 10000 + (tNow->tm_mon + 1) * 100 + tNow->tm_mday;
				uint32_t actTime = tNow->tm_hour * 10000 + tNow->tm_min * 100 + tNow->tm_sec;
				actTime = actTime * 1000 + SnapMillisec;

				double scale = _price_scales[p->InstrumentNo];
				WTSTickStruct& quote = tick->getTickStruct();
				quote.action_date = actDate;
				quote.action_time = actTime;
				if (quote.trading_date == 0)
				{
					quote.trading_date = _bd_mgr->calcTradingDate(tick->getContractInfo()->getFullPid(), actDate, actTime / 100000);
				}
				quote.price = p->LastPrice*scale;
				quote.high = std::max(quote.high, p->LastPrice*scale);
				quote.low = std::min(quote.low, p->LastPrice*scale);
				if (decimal::eq(quote.open, 0))
					quote.open = quote.price;

				quote.total_volume = p->Volume;
				quote.total_turnover = (double)p->Turnover;
				quote.open_interest = (double)p->OpenInterest;

				quote.volume = 0;
				quote.turn_over = 0;
				quote.diff_interest = 0;

				//委卖价格
				quote.ask_prices[0] = p->AskPrice1*scale;
				quote.ask_prices[1] = p->AskPrice2*scale;
				quote.ask_prices[2] = p->AskPrice3*scale;
				quote.ask_prices[3] = p->AskPrice4*scale;
				quote.ask_prices[4] = p->AskPrice5*scale;

				//委买价格
				quote.bid_prices[0] = p->BidPrice1*scale;
				quote.bid_prices[1] = p->BidPrice2*scale;
				quote.bid_prices[2] = p->BidPrice3*scale;
				quote.bid_prices[3] = p->BidPrice4*scale;
				quote.bid_prices[4] = p->BidPrice5*scale;

				//委卖量
				quote.ask_qty[0] = p->AskVolume1;
				quote.ask_qty[1] = p->AskVolume2;
				quote.ask_qty[2] = p->AskVolume3;
				quote.ask_qty[3] = p->AskVolume4;
				quote.ask_qty[4] = p->AskVolume5;

				//委买量
				quote.bid_qty[0] = p->BidVolume1;
				quote.bid_qty[1] = p->BidVolume2;
				quote.bid_qty[2] = p->BidVolume3;
				quote.bid_qty[3] = p->BidVolume4;
				quote.bid_qty[4] = p->BidVolume5;

				with_spi([&](IParserSpi* sink) { sink->handleQuote(tick, 1); });

				static std::atomic<uint32_t> recv_cnt{0};
				const uint32_t received = recv_cnt.fetch_add(1, std::memory_order_relaxed) + 1;
				if (received % _gpsize == 0)
					with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserXeleSkt] {} depth ticks received in total", received); });
				if (_stop_requested.load(std::memory_order_acquire) || _stopped)
					return;
			}
		}
		else if (type == MESSAGE_TYPE_HEART_BEAT) {
			constexpr std::size_t packLen = sizeof(CXeleShfeMarketHead) + sizeof(CXeleShfeHeartBeat);
			if (length - len < packLen)
				return;
			len += packLen;
			with_spi([&](IParserSpi* sink) { write_log(sink, LL_DEBUG, "[ParserXeleSkt] Receiving heartbeat packet, length: {}\n", packLen); });
		}
		else
		{
			return;
		}
	}
}

void ParserXeleSkt::doOnConnected()
{
	// Not called: the Connect/Login event contract (R76-03) is undecided.
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Connect, 0); });
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Login, 0); });
}

void ParserXeleSkt::doOnDisconnected()
{
	// Queued by disconnect(), which already consumed the close token.
	with_spi([](IParserSpi* sink) { sink->handleEvent(WPE_Close, 0); });
}
