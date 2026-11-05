#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>

#include "ParserUDP/ParserUDP.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

extern "C" void deleteParser(IParserApi*& parser);

struct ParserUDPSpiReentryState
{
	std::mutex mutex;
	std::condition_variable condition;
	bool callback_entered = false;
	bool allow_callback_return = false;
	bool callback_completed = false;
	bool connected = false;
	bool replacement_attempted = false;
	bool replacement_returned = false;
	bool io_was_available = false;
	bool second_probe_done = false;
	bool second_io_was_available = false;
	uint32_t callback_entries = 0;
	uint32_t callbacks_active = 0;
	uint32_t max_callbacks_active = 0;
	bool replacement_warning_entered = false;
};

struct ParserUDPNestedCallbackState
{
	bool outer_parser_recognized = false;
	bool nested_replacement_completed = false;
	bool nested_shutdown_result = true;
};

/// Narrow test seam for queue/completion state. The send lifetime regression
/// also exercises real socket operations without starting a worker thread.
struct ParserUDPTestPeer
{
	using ReceiveOperation = ParserUDP::ReceiveOperation;

	static void enqueue(ParserUDP& parser, const std::string& packet)
	{
		StdUniqueLock lock(parser._mtx_queue);
		parser._send_queue.push(packet);
	}

	static void complete_successfully(ParserUDP& parser)
	{
		auto completion = stage_write_handler(parser);
		completion(boost::system::error_code(), 0);
	}

	static std::function<void(const boost::system::error_code&, std::size_t)> stage_write_handler(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		StdUniqueLock queue_lock(parser._mtx_queue);
		if (parser._send_queue.empty())
			return {};
		parser._pending_send_buffer = std::make_shared<std::string>(parser._send_queue.front());
		++parser._next_send_id;
		if (parser._next_send_id == 0)
			++parser._next_send_id;
		parser._active_send_epoch = parser._connection_epoch;
		parser._active_send_id = parser._next_send_id;
		parser._send_in_flight = true;
		return parser.make_write_handler(parser._active_send_epoch, parser._active_send_id, parser._pending_send_buffer);
	}

	static std::function<void(const boost::system::error_code&, std::size_t)> capture_current_write_handler(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		if (!parser._send_in_flight || !parser._pending_send_buffer)
			return {};
		return parser.make_write_handler(parser._active_send_epoch, parser._active_send_id, parser._pending_send_buffer);
	}

	static std::function<void(const boost::system::error_code&, std::size_t)> reconnect_with_new_pending_send(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		parser.reconnect(3);
		enqueue(parser, "duplicate packet");
		auto old_completion = stage_write_handler(parser);
		parser.reconnect(3);
		enqueue(parser, "duplicate packet");
		parser.do_send();
		return old_completion;
	}

	static std::size_t queued_packets(ParserUDP& parser)
	{
		StdUniqueLock lock(parser._mtx_queue);
		return parser._send_queue.size();
	}

	static std::vector<std::string> capture_subscription_packets(ParserUDP& parser, const CodeSet& symbols)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		parser._set_subs.clear();
		for (const std::string& symbol : symbols)
			parser._set_subs.insert(symbol);
		{
			StdUniqueLock queue_lock(parser._mtx_queue);
			std::queue<std::string> empty;
			parser._send_queue.swap(empty);
		}

		// With no subscription socket, do_send leaves the serialized packets in
		// the queue for this test seam to inspect.
		parser.subscribe();

		std::vector<std::string> packets;
		StdUniqueLock queue_lock(parser._mtx_queue);
		while (!parser._send_queue.empty())
		{
			packets.push_back(parser._send_queue.front());
			parser._send_queue.pop();
		}
		return packets;
	}

	static void post(ParserUDP& parser, std::function<void()> task)
	{
		boost::asio::post(parser._io_service, std::move(task));
	}

	static void post_connected_callback(ParserUDP& parser)
	{
		post(parser, [&parser]() { parser.doOnConnected(); });
	}

	static void post_synthetic_send_error(ParserUDP& parser, const boost::system::error_code& error)
	{
		post(parser, [&parser, error]()
		{
			std::function<void(const boost::system::error_code&, std::size_t)> completion;
			{
				std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
				if (parser._stop_requested.load(std::memory_order_acquire) || parser._stopped)
					return;
				enqueue(parser, "synthetic failed send");
				completion = stage_write_handler(parser);
			}
			if (completion)
				completion(error, 0);
		});
	}

	static void dispatch_connected_callback(ParserUDP& parser)
	{
		parser.doOnConnected();
	}

	static bool stop_requested(const ParserUDP& parser)
	{
		return parser._stop_requested.load(std::memory_order_acquire);
	}

	static void dispatch_parser_log(ParserUDP& parser)
	{
		parser.with_spi([](IParserSpi* sink) { sink->handleParserLog(LL_INFO, "test callback"); });
	}

	static bool try_lock_io(ParserUDP& parser)
	{
		std::unique_lock<std::recursive_mutex> io_probe(parser._mtx_io, std::try_to_lock);
		return io_probe.owns_lock();
	}

	static bool is_callback_thread(const ParserUDP& parser)
	{
		return parser.is_callback_thread();
	}

	static void cancel_broadcast_read(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		if (parser._b_socket != NULL)
		{
			boost::system::error_code ignored;
			parser._b_socket->cancel(ignored);
		}
	}

	static uint64_t connection_epoch(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser._connection_epoch;
	}

	static uint64_t reconnect(ParserUDP& parser,
		std::shared_ptr<ReceiveOperation>* broadcast_operation = nullptr)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		if (parser.reconnect(3, broadcast_operation))
			parser._close_event_emitted = false;
		return parser._connection_epoch;
	}

	static uint16_t broadcast_port(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		if (parser._b_socket == nullptr || !parser._b_socket->is_open())
			return 0;
		boost::system::error_code error;
		const auto endpoint = parser._b_socket->local_endpoint(error);
		return error ? 0 : endpoint.port();
	}

	static void add_subscription(ParserUDP& parser, const std::string& symbol)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		parser._set_subs.insert(symbol);
	}

	static bool reconnect_with_flag(ParserUDP& parser, uint32_t flag)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser.reconnect(flag);
	}

	static void dispatch_read_success(ParserUDP& parser, bool is_broad, uint64_t epoch,
		const std::shared_ptr<ReceiveOperation>& operation, std::size_t bytes = 1)
	{
		parser.handle_read(boost::system::error_code(), bytes, is_broad, epoch, operation);
	}

	static void dispatch_read_error(ParserUDP& parser, bool is_broad, uint64_t epoch,
		const boost::system::error_code& error = boost::asio::error::connection_reset)
	{
		auto operation = std::make_shared<ReceiveOperation>();
		parser.handle_read(error, 0, is_broad, epoch, operation);
	}

	static bool stopped(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser._stopped;
	}

	static uint32_t max_auto_recovery_attempts()
	{
		return ParserUDP::MAX_AUTO_RECOVERY_ATTEMPTS;
	}

	static uint32_t send_recovery_attempts(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser._send_recovery_attempts;
	}

	static uint32_t broadcast_recovery_attempts(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser._broadcast_recovery_attempts;
	}

	static bool recovery_exhausted(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		return parser._recovery_exhausted;
	}

	static std::weak_ptr<std::string> start_send_and_reconnect(ParserUDP& parser)
	{
		std::lock_guard<std::recursive_mutex> io_lock(parser._mtx_io);
		parser.reconnect(3);
		enqueue(parser, "old epoch packet");
		parser.do_send();
		std::weak_ptr<std::string> old_owner = parser._pending_send_buffer;
		parser.reconnect(3);
		enqueue(parser, "new epoch packet");
		return old_owner;
	}

	static std::string queued_front(ParserUDP& parser)
	{
		StdUniqueLock queue_lock(parser._mtx_queue);
		return parser._send_queue.empty() ? std::string() : parser._send_queue.front();
	}

	static std::string pending_send_data(const ParserUDP& parser)
	{
		return parser._pending_send_buffer ? *parser._pending_send_buffer : std::string();
	}

	static std::size_t poll_one(ParserUDP& parser)
	{
		return parser._io_service.poll_one();
	}

	static void poll_until_send_owner_released(ParserUDP& parser, const std::weak_ptr<std::string>& owner)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!owner.expired() && std::chrono::steady_clock::now() < deadline)
		{
			if (poll_one(parser) == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	static bool poll_until_receive_owner_released(ParserUDP& parser,
		const std::weak_ptr<ReceiveOperation>& owner)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (!owner.expired() && std::chrono::steady_clock::now() < deadline)
		{
			if (poll_one(parser) == 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return owner.expired();
	}
};

namespace
{
	#pragma pack(push, 1)
	struct TestUDPSubscribePacket
	{
		uint32_t type;
		char data[1020];
	};
	#pragma pack(pop)

	static_assert(sizeof(TestUDPSubscribePacket) == 1024, "UDP subscribe request packet ABI");
	constexpr uint32_t kUdpSubscribeMessage = 0x100;

	std::vector<std::string> parse_like_udp_caster(const std::string& packet)
	{
		if (packet.size() != sizeof(TestUDPSubscribePacket))
		{
			ADD_FAILURE() << "subscription packet has unexpected size " << packet.size();
			return {};
		}

		TestUDPSubscribePacket request{};
		std::memcpy(&request, packet.data(), sizeof(request));
		EXPECT_EQ(kUdpSubscribeMessage, request.type);

		const char* terminator = static_cast<const char*>(
			std::memchr(request.data, '\0', sizeof(request.data)));
		if (terminator == nullptr)
		{
			ADD_FAILURE() << "receiver cannot find the required NUL terminator";
			return {};
		}

		const std::string payload(request.data, static_cast<std::size_t>(terminator - request.data));
		std::vector<std::string> codes;
		std::string exchange;
		std::string code;
		std::size_t begin = 0;
		while (begin <= payload.size())
		{
			const std::size_t comma = payload.find(',', begin);
			const std::string full_code = payload.substr(begin,
				comma == std::string::npos ? std::string::npos : comma - begin);
			const std::size_t dot = full_code.find('.');
			if (dot == std::string::npos)
				code = full_code;
			else
			{
				code = full_code.substr(dot + 1);
				exchange = full_code.substr(0, dot);
			}
			EXPECT_FALSE(code.empty());
			// ParserUDP sends instrument suffixes, which UDPCaster resolves by code
			// through getContract(code, "").
			EXPECT_TRUE(exchange.empty());
			codes.push_back(code);
			if (comma == std::string::npos)
				break;
			begin = comma + 1;
		}
		return codes;
	}

	CodeSet make_exact_capacity_symbols(std::set<std::string>& expected_codes)
	{
		CodeSet symbols;
		for (uint32_t index = 0; index < 31; ++index)
		{
			const std::string number = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
			const std::string code = "C" + number + std::string(28, 'x');
			symbols.insert("SHFE." + code);
			expected_codes.insert(code);
		}
		const std::string short_code = "Z31" + std::string(24, 'y');
		symbols.insert("DCE." + short_code);
		expected_codes.insert(short_code);
		return symbols;
	}

	WTSVariant* make_udp_config(int sport = 0)
	{
		WTSVariant* config = WTSVariant::createObject();
		config->append("host", "127.0.0.1");
		config->append("bport", 0);
		config->append("sport", sport);
		config->append("gpsize", 100);
		return config;
	}

	class BlockingUdpSink : public IParserSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleParserLog(WTSLogLevel, const char*) override {}

		void handleEvent(WTSParserEvent, int32_t) override
		{
			std::unique_lock<std::mutex> lock(_mutex);
			++_events;
			_entered = true;
			_condition.notify_all();
			_condition.wait(lock, [this]() { return _continue; });
		}

		bool wait_until_entered()
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _condition.wait_for(lock, std::chrono::seconds(3), [this]() { return _entered; });
		}

		void continue_callback()
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_continue = true;
			_condition.notify_all();
		}

		uint32_t events() const { return _events.load(); }

	private:
		std::mutex _mutex;
		std::condition_variable _condition;
		bool _entered = false;
		bool _continue = false;
		std::atomic<uint32_t> _events{0};
	};

	class SelfReleasingUdpSink : public IParserSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleParserLog(WTSLogLevel, const char*) override {}
		void handleEvent(WTSParserEvent, int32_t) override
		{
			// I03-NEW-115（测试侧 TSan）：事件线程可能在 registerSpi 之后、set_on_event 期间回调；
			// 回调对象的读写都经 _on_event_mtx，调用在锁外进行（回调内会释放 parser）。
			std::function<void()> callback;
			{
				std::lock_guard<std::mutex> lock(_on_event_mtx);
				callback = _on_event;
			}
			if (callback)
				callback();
		}

		void set_on_event(std::function<void()> callback)
		{
			std::lock_guard<std::mutex> lock(_on_event_mtx);
			_on_event = std::move(callback);
		}

	private:
		std::mutex _on_event_mtx;
		std::function<void()> _on_event;
	};

	class CountingUdpSink : public IParserSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override
		{
			std::lock_guard<std::mutex> lock(_mutex);
			++_quote_count;
			_condition.notify_all();
		}
		void handleParserLog(WTSLogLevel, const char*) override {}

		void handleEvent(WTSParserEvent event, int32_t) override
		{
			if (event != WPE_Close)
				return;
			std::lock_guard<std::mutex> lock(_mutex);
			++_close_count;
			_condition.notify_all();
		}

		bool wait_for_close_count(uint32_t count)
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _condition.wait_for(lock, std::chrono::seconds(3), [this, count]() { return _close_count >= count; });
		}

		uint32_t close_count() const
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _close_count;
		}

		uint32_t quote_count() const
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _quote_count;
		}

	private:
		mutable std::mutex _mutex;
		std::condition_variable _condition;
		uint32_t _close_count = 0;
		uint32_t _quote_count = 0;
	};

	class DisconnectOnRecoveryLogSink : public IParserSpi
	{
	public:
		explicit DisconnectOnRecoveryLogSink(ParserUDP& parser) : _parser(parser) {}
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}

		void handleParserLog(WTSLogLevel level, const char* message) override
		{
			if (level != LL_WARN || message == nullptr || std::string(message).find("Recovering") == std::string::npos)
				return;

			const bool disconnected = _parser.disconnect();
			{
				std::lock_guard<std::mutex> lock(_mutex);
				_disconnect_succeeded = disconnected;
				_recovery_log_seen = true;
			}
			_condition.notify_all();
		}

		bool wait_for_recovery_log()
		{
			std::unique_lock<std::mutex> lock(_mutex);
			return _condition.wait_for(lock, std::chrono::seconds(3), [this]() { return _recovery_log_seen; });
		}

		bool disconnect_succeeded() const
		{
			std::lock_guard<std::mutex> lock(_mutex);
			return _disconnect_succeeded;
		}

	private:
		ParserUDP& _parser;
		mutable std::mutex _mutex;
		std::condition_variable _condition;
		bool _recovery_log_seen = false;
		bool _disconnect_succeeded = false;
	};

	class BlockingReentrantIoSink : public IParserSpi
	{
	public:
		BlockingReentrantIoSink(ParserUDP& parser, ParserUDPSpiReentryState& state)
			: _parser(parser), _state(state) {}

		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}

		void handleParserLog(WTSLogLevel, const char*) override
		{
			{
				std::unique_lock<std::mutex> lock(_state.mutex);
				_state.callback_entered = true;
				++_state.callback_entries;
				++_state.callbacks_active;
				_state.max_callbacks_active = std::max(_state.max_callbacks_active, _state.callbacks_active);
				_state.condition.notify_all();
				_state.condition.wait(lock, [this]() { return _state.allow_callback_return; });
			}

			const bool connected = _parser.isConnected();
			_parser.subscribe(CodeSet{});

			{
				std::lock_guard<std::mutex> lock(_state.mutex);
				_state.connected = connected;
				_state.callback_completed = true;
				--_state.callbacks_active;
				_state.condition.notify_all();
			}
		}

	private:
		ParserUDP& _parser;
		ParserUDPSpiReentryState& _state;
	};

	class TrackedLogSink : public IParserSpi
	{
	public:
		explicit TrackedLogSink(ParserUDPSpiReentryState& state) : _state(state) {}
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}

		void handleParserLog(WTSLogLevel, const char*) override
		{
			std::lock_guard<std::mutex> lock(_state.mutex);
			++_state.callback_entries;
			++_state.callbacks_active;
			_state.max_callbacks_active = std::max(_state.max_callbacks_active, _state.callbacks_active);
			_state.replacement_warning_entered = true;
			--_state.callbacks_active;
			_state.condition.notify_all();
		}

	private:
		ParserUDPSpiReentryState& _state;
	};

	class NestedParserASink : public IParserSpi
	{
	public:
		explicit NestedParserASink(ParserUDP& parser_b) : _parser_b(parser_b) {}
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}
		void handleParserLog(WTSLogLevel, const char*) override
		{
			ParserUDPTestPeer::dispatch_parser_log(_parser_b);
		}
	private:
		ParserUDP& _parser_b;
	};

	class NestedParserBSink : public IParserSpi
	{
	public:
		NestedParserBSink(ParserUDP& parser_a, IParserSpi& replacement, ParserUDPNestedCallbackState& state)
			: _parser_a(parser_a), _replacement(replacement), _state(state) {}
		IBaseDataMgr* getBaseDataMgr() override { return nullptr; }
		void handleSymbolList(const WTSArray*) override {}
		void handleQuote(WTSTickData*, uint32_t) override {}
		void handleEvent(WTSParserEvent, int32_t) override {}
		void handleParserLog(WTSLogLevel, const char*) override
		{
			_state.outer_parser_recognized = ParserUDPTestPeer::is_callback_thread(_parser_a);
			if (!_state.outer_parser_recognized)
				return;

			_parser_a.registerSpi(&_replacement);
			_state.nested_replacement_completed = true;
			_state.nested_shutdown_result = _parser_a.shutdown();
		}
	private:
		ParserUDP& _parser_a;
		IParserSpi& _replacement;
		ParserUDPNestedCallbackState& _state;
	};
}

TEST(ParserUDPSubscribeTest, MultiContractPacketIsReadableByUdpCaster)
{
	ParserUDP parser;
	CodeSet symbols;
	symbols.insert("SHFE.ag2512");
	symbols.insert("DCE.i2509");
	symbols.insert("CZCE.MA701");

	const std::vector<std::string> packets = ParserUDPTestPeer::capture_subscription_packets(parser, symbols);
	ASSERT_EQ(1u, packets.size());
	const std::vector<std::string> decoded = parse_like_udp_caster(packets.front());
	const std::set<std::string> actual(decoded.begin(), decoded.end());
	EXPECT_EQ((std::set<std::string>{ "ag2512", "i2509", "MA701" }), actual);
}

TEST(ParserUDPSubscribeTest, ExactlyFullReceiverFieldIsNulTerminated)
{
	ParserUDP parser;
	std::set<std::string> expected_codes;
	const CodeSet symbols = make_exact_capacity_symbols(expected_codes);
	const std::vector<std::string> packets = ParserUDPTestPeer::capture_subscription_packets(parser, symbols);
	ASSERT_EQ(1u, packets.size());
	ASSERT_EQ(sizeof(TestUDPSubscribePacket), packets.front().size());

	TestUDPSubscribePacket request{};
	std::memcpy(&request, packets.front().data(), sizeof(request));
	const char* terminator = static_cast<const char*>(
		std::memchr(request.data, '\0', sizeof(request.data)));
	ASSERT_NE(nullptr, terminator);
	EXPECT_EQ(request.data + sizeof(request.data) - 1, terminator);
	const std::vector<std::string> decoded = parse_like_udp_caster(packets.front());
	const std::set<std::string> actual(decoded.begin(), decoded.end());
	EXPECT_EQ(expected_codes, actual);
}

TEST(ParserUDPSubscribeTest, OverCapacityContractsAreChunkedWithoutLoss)
{
	ParserUDP parser;
	std::set<std::string> expected_codes;
	CodeSet symbols = make_exact_capacity_symbols(expected_codes);
	symbols.insert("DCE.EXTRA");
	expected_codes.insert("EXTRA");

	const std::vector<std::string> packets = ParserUDPTestPeer::capture_subscription_packets(parser, symbols);
	ASSERT_EQ(2u, packets.size());
	std::set<std::string> actual;
	for (const std::string& packet : packets)
	{
		const std::vector<std::string> decoded = parse_like_udp_caster(packet);
		for (const std::string& code : decoded)
			EXPECT_TRUE(actual.insert(code).second) << "duplicate code across packets: " << code;
	}
	EXPECT_EQ(expected_codes, actual);
}

TEST(ParserUDPSubscribeTest, UnencodableSingleTokenDoesNotCorruptValidSubscriptions)
{
	ParserUDP parser;
	CodeSet symbols;
	symbols.insert("SHFE.rb");
	symbols.insert("DCE." + std::string(1020, 'z'));

	const std::vector<std::string> packets = ParserUDPTestPeer::capture_subscription_packets(parser, symbols);
	ASSERT_EQ(1u, packets.size());
	const std::vector<std::string> decoded = parse_like_udp_caster(packets.front());
	EXPECT_EQ((std::set<std::string>{ "rb" }), std::set<std::string>(decoded.begin(), decoded.end()));
}

TEST(ParserUDPQueueLockTest, SuccessfulWriteReleasesQueueLockBeforeSchedulingNextSend)
{
	ParserUDP parser;
	ParserUDPTestPeer::enqueue(parser, "queued packet");

	// handle_write pops the completed packet, unlocks, then calls do_send().
	// do_send takes the same queue mutex even when no socket/packet remains.
	EXPECT_NO_THROW(ParserUDPTestPeer::complete_successfully(parser));
	EXPECT_EQ(0u, ParserUDPTestPeer::queued_packets(parser));
}

TEST(ParserUDPQueueLockTest, StaleWriteCompletionCannotConsumeNextEpochPacket)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	auto stale_completion = ParserUDPTestPeer::reconnect_with_new_pending_send(parser);
	ASSERT_TRUE(static_cast<bool>(stale_completion));
	ASSERT_EQ(1u, ParserUDPTestPeer::queued_packets(parser));
	ASSERT_EQ("duplicate packet", ParserUDPTestPeer::queued_front(parser));
	ASSERT_EQ("duplicate packet", ParserUDPTestPeer::pending_send_data(parser));

	// A successful completion from the prior request/epoch must not pop or
	// clear the current request, even if it is dispatched after the new send.
	stale_completion(boost::system::error_code(), sizeof(uint32_t));
	EXPECT_EQ(1u, ParserUDPTestPeer::queued_packets(parser));
	EXPECT_EQ("duplicate packet", ParserUDPTestPeer::queued_front(parser));
	EXPECT_EQ("duplicate packet", ParserUDPTestPeer::pending_send_data(parser));
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPQueueLockTest, AsyncSendBufferLivesUntilItsCancellationCompletion)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	const std::weak_ptr<std::string> old_owner = ParserUDPTestPeer::start_send_and_reconnect(parser);
	ASSERT_FALSE(old_owner.expired());
	ASSERT_EQ("new epoch packet", ParserUDPTestPeer::queued_front(parser));

	// Reconnect closed the old socket and removed the queue's string, but the
	// real Asio operation still borrows its bytes until its cancellation handler
	// has run and released the handler-captured shared owner.
	ParserUDPTestPeer::poll_until_send_owner_released(parser, old_owner);
	EXPECT_TRUE(old_owner.expired());
	EXPECT_EQ("new epoch packet", ParserUDPTestPeer::queued_front(parser));
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, CurrentSendErrorRecoversPairOnceAndIgnoresStaleOrAbortedCompletions)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	CodeSet symbols;
	symbols.insert("SHFE.rb");
	parser.subscribe(symbols);

	const uint64_t failed_epoch = ParserUDPTestPeer::reconnect(parser);
	ASSERT_FALSE(ParserUDPTestPeer::pending_send_data(parser).empty());
	EXPECT_FALSE(ParserUDPTestPeer::reconnect_with_flag(parser, 1));
	EXPECT_EQ(failed_epoch, ParserUDPTestPeer::connection_epoch(parser));
	auto failed_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(failed_completion));

	failed_completion(boost::asio::error::connection_reset, 0);
	const uint64_t recovered_epoch = ParserUDPTestPeer::connection_epoch(parser);
	ASSERT_NE(failed_epoch, recovered_epoch);
	EXPECT_EQ(1u, sink.close_count());
	EXPECT_EQ(1u, ParserUDPTestPeer::queued_packets(parser));
	EXPECT_FALSE(ParserUDPTestPeer::pending_send_data(parser).empty());

	// The old request's late error must not start another recovery cycle.
	failed_completion(boost::asio::error::connection_reset, 0);
	EXPECT_EQ(recovered_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_EQ(1u, sink.close_count());

	// A second error from the new current request is actionable once.
	auto recovered_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(recovered_completion));
	recovered_completion(boost::asio::error::connection_reset, 0);
	const uint64_t second_recovered_epoch = ParserUDPTestPeer::connection_epoch(parser);
	ASSERT_NE(recovered_epoch, second_recovered_epoch);
	EXPECT_EQ(2u, sink.close_count());

	// Closing the parser cancels the current send; its aborted completion must
	// not start another reconnect or close cycle.
	auto aborted_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(aborted_completion));
	const uint64_t before_abort = ParserUDPTestPeer::connection_epoch(parser);
	aborted_completion(boost::asio::error::operation_aborted, 0);
	EXPECT_EQ(before_abort, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_EQ(2u, sink.close_count());
	ASSERT_TRUE(parser.disconnect());
	const uint64_t disconnected_epoch = ParserUDPTestPeer::connection_epoch(parser);
	aborted_completion(boost::asio::error::operation_aborted, 0);
	EXPECT_TRUE(ParserUDPTestPeer::stopped(parser));
	EXPECT_EQ(disconnected_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_EQ(2u, sink.close_count());
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, ReceiveOperationsOwnDistinctBuffersAndPartialReconnectIsRejected)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	std::shared_ptr<ParserUDPTestPeer::ReceiveOperation> old_operation;
	const uint64_t old_epoch = ParserUDPTestPeer::reconnect(parser, &old_operation);
	ASSERT_TRUE(old_operation);
	old_operation->buffer[0] = 'o';

	EXPECT_FALSE(ParserUDPTestPeer::reconnect_with_flag(parser, 2));
	EXPECT_EQ(old_epoch, ParserUDPTestPeer::connection_epoch(parser));

	std::shared_ptr<ParserUDPTestPeer::ReceiveOperation> new_operation;
	const uint64_t new_epoch = ParserUDPTestPeer::reconnect(parser, &new_operation);
	ASSERT_NE(old_epoch, new_epoch);
	ASSERT_TRUE(new_operation);
	new_operation->buffer[0] = 'n';
	EXPECT_NE(old_operation->buffer.data(), new_operation->buffer.data());
	EXPECT_NE(&old_operation->endpoint, &new_operation->endpoint);

	// Model an already-completed old receive dispatching after reconnect. It
	// must not parse or alter the active epoch's separately owned receive area.
	ParserUDPTestPeer::dispatch_read_success(parser, true, old_epoch, old_operation);
	EXPECT_EQ('n', new_operation->buffer[0]);
	EXPECT_EQ(new_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, ExternalReleaseWaitsForActiveSpiCallbackAndBlocksLaterDispatch)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	BlockingUdpSink sink;
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.connect());
	ParserUDPTestPeer::post_connected_callback(parser);
	if (!sink.wait_until_entered())
	{
		parser.release();
		FAIL() << "The posted worker callback did not start";
		return;
	}

	std::atomic<bool> release_returned{false};
	std::thread releaser([&]()
	{
		parser.release();
		release_returned.store(true);
	});

	const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (!ParserUDPTestPeer::stop_requested(parser) && std::chrono::steady_clock::now() < stop_deadline)
		std::this_thread::yield();
	const bool stop_was_requested = ParserUDPTestPeer::stop_requested(parser);
	const bool release_returned_while_callback_active = release_returned.load();
	sink.continue_callback();
	releaser.join();

	EXPECT_TRUE(stop_was_requested);
	EXPECT_FALSE(release_returned_while_callback_active);
	EXPECT_TRUE(release_returned.load());
	EXPECT_EQ(1u, sink.events()); // the Login event is suppressed after stop

	ParserUDPTestPeer::dispatch_connected_callback(parser);
	EXPECT_EQ(1u, sink.events()); // successful release is a callback drain barrier
}

TEST(ParserUDPShutdownTest, SelfReleaseAndDeleteRetainOwnerForExternalRetry)
{
	std::unique_ptr<ParserUDP> owner(new ParserUDP());
	ParserUDP* udp = owner.get();
	WTSVariant* config = make_udp_config();
	if (!udp->init(config))
	{
		config->release();
		FAIL() << "UDP initialization failed";
		return;
	}
	config->release();
	if (!udp->connect())
	{
		FAIL() << "UDP connect failed";
		return;
	}
	IParserApi* api = udp;
	SelfReleasingUdpSink sink;
	udp->registerSpi(&sink);

	std::mutex result_mutex;
	std::condition_variable result_condition;
	bool callback_finished = false;
	bool owner_retained = false;
	sink.set_on_event([&]()
	{
		api->release();
		deleteParser(api);
		{
			std::lock_guard<std::mutex> lock(result_mutex);
			owner_retained = api != nullptr;
			callback_finished = true;
		}
		result_condition.notify_one();
	});
	ParserUDPTestPeer::post_connected_callback(*udp);

	bool completed = false;
	{
		std::unique_lock<std::mutex> lock(result_mutex);
		completed = result_condition.wait_for(lock, std::chrono::seconds(3), [&]() { return callback_finished; });
	}
	EXPECT_TRUE(completed);
	if (!completed)
	{
		udp->shutdown();
		return;
	}

	EXPECT_TRUE(owner_retained);
	EXPECT_NE(nullptr, api);
	const bool drained = udp->shutdown();
	EXPECT_TRUE(drained);
	if (!drained)
		return;
	owner.release();
	deleteParser(api);
	EXPECT_EQ(nullptr, api);
}

TEST(ParserUDPShutdownTest, ActiveUdpDisconnectEmitsOneCloseAndReconnectStartsNewEventEpoch)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.connect()); // opens a real local UDP receive socket
	ASSERT_TRUE(parser.isConnected());
	ASSERT_TRUE(parser.disconnect()); // closes it and cancels its pending receive
	ASSERT_TRUE(sink.wait_for_close_count(1));

	// connect joins the previous io worker before reopening sockets, so all
	// cancellation completions from the first connection have drained here.
	ASSERT_TRUE(parser.connect());
	EXPECT_TRUE(parser.isConnected());
	EXPECT_EQ(1u, sink.close_count());
	ASSERT_TRUE(parser.disconnect());
	ASSERT_TRUE(sink.wait_for_close_count(2));

	// Rejoining the cancelled receive before this reconnect proves its late
	// completion did not report a second close for the same connection.
	ASSERT_TRUE(parser.connect());
	EXPECT_EQ(2u, sink.close_count());
	parser.release();
}

TEST(ParserUDPSpiLockTest, CallbacksSerializeReenterIoAndReplacementWaitsForTheOldSink)
{
	ParserUDP parser;
	ParserUDPSpiReentryState state;
	BlockingReentrantIoSink old_sink(parser, state);
	TrackedLogSink replacement_sink(state);
	parser.registerSpi(&old_sink);

	std::thread first_dispatch([&]() { ParserUDPTestPeer::dispatch_parser_log(parser); });
	{
		std::unique_lock<std::mutex> lock(state.mutex);
		if (!state.condition.wait_for(lock, std::chrono::seconds(3), [&]() { return state.callback_entered; }))
		{
			state.allow_callback_return = true;
			lock.unlock();
			state.condition.notify_all();
			first_dispatch.join();
			FAIL() << "The controlled SPI callback did not start";
			return;
		}
	}

	std::thread second_dispatch([&]()
	{
		const bool io_available = ParserUDPTestPeer::try_lock_io(parser);
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.second_io_was_available = io_available;
			state.second_probe_done = true;
			state.condition.notify_all();
		}
		ParserUDPTestPeer::dispatch_parser_log(parser);
	});
	std::thread registrar([&]()
	{
		const bool io_available = ParserUDPTestPeer::try_lock_io(parser);
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.io_was_available = io_available;
			state.replacement_attempted = true;
			state.condition.notify_all();
		}
		parser.registerSpi(&replacement_sink);
		{
			std::lock_guard<std::mutex> lock(state.mutex);
			state.replacement_returned = true;
			state.condition.notify_all();
		}
	});

	{
		std::unique_lock<std::mutex> lock(state.mutex);
		const bool waiting_ready = state.condition.wait_for(lock, std::chrono::seconds(3), [&]()
		{
			return state.second_probe_done && state.replacement_attempted;
		});
		if (!waiting_ready)
		{
			state.allow_callback_return = true;
			lock.unlock();
			state.condition.notify_all();
			first_dispatch.join();
			second_dispatch.join();
			registrar.join();
			FAIL() << "The competing I/O operations did not reach their synchronization points";
			return;
		}
	}

	{
		std::lock_guard<std::mutex> lock(state.mutex);
		state.allow_callback_return = true;
	}
	state.condition.notify_all();
	first_dispatch.join();
	second_dispatch.join();
	registrar.join();

	EXPECT_TRUE(state.callback_completed);
	EXPECT_FALSE(state.io_was_available);
	EXPECT_FALSE(state.second_io_was_available);
	EXPECT_TRUE(state.replacement_attempted);
	EXPECT_TRUE(state.replacement_returned);
	EXPECT_TRUE(state.replacement_warning_entered);
	EXPECT_EQ(1u, state.max_callbacks_active);
	EXPECT_FALSE(state.connected);
}

TEST(ParserUDPSpiLockTest, NestedParserCallbacksRetainOuterParserIdentity)
{
	ParserUDP parser_a;
	ParserUDP parser_b;
	ParserUDPNestedCallbackState state;
	CountingUdpSink replacement_sink;
	NestedParserBSink sink_b(parser_a, replacement_sink, state);
	NestedParserASink sink_a(parser_b);
	parser_a.registerSpi(&sink_a);
	parser_b.registerSpi(&sink_b);

	ParserUDPTestPeer::dispatch_parser_log(parser_a);

	EXPECT_TRUE(state.outer_parser_recognized);
	EXPECT_TRUE(state.nested_replacement_completed);
	EXPECT_FALSE(state.nested_shutdown_result);
}

TEST(ParserUDPShutdownTest, AutomaticReconnectStartsANewCloseEventCycle)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.connect());
	const uint64_t failed_epoch = ParserUDPTestPeer::connection_epoch(parser);
	ParserUDPTestPeer::dispatch_read_error(parser, true, failed_epoch);
	ASSERT_TRUE(sink.wait_for_close_count(1));
	EXPECT_NE(failed_epoch, ParserUDPTestPeer::connection_epoch(parser));

	// A successful reconnect has completed before disconnect can acquire the
	// I/O mutex, so disconnect belongs to the new event cycle.
	ASSERT_TRUE(parser.disconnect());
	ASSERT_TRUE(sink.wait_for_close_count(2));
	ASSERT_TRUE(parser.connect()); // joins the cancelled receive callbacks
	EXPECT_EQ(2u, sink.close_count());
	parser.release();
}

TEST(ParserUDPShutdownTest, ExpectedOperationAbortedDoesNotReportOrReconnect)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	const uint64_t epoch = ParserUDPTestPeer::reconnect(parser);
	ParserUDPTestPeer::cancel_broadcast_read(parser);
	while (ParserUDPTestPeer::poll_one(parser) != 0)
	{
	}
	EXPECT_EQ(epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_EQ(0u, sink.close_count());
	EXPECT_EQ(0u, ParserUDPTestPeer::broadcast_recovery_attempts(parser));
	EXPECT_FALSE(ParserUDPTestPeer::recovery_exhausted(parser));
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, ReentrantRecoveryLogDisconnectPreventsReconnectAndAllowsControlThreadRestart)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	DisconnectOnRecoveryLogSink sink(parser);
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.connect());
	const uint64_t disconnected_epoch = ParserUDPTestPeer::connection_epoch(parser);

	ParserUDPTestPeer::post_synthetic_send_error(parser, boost::asio::error::connection_reset);
	ASSERT_TRUE(sink.wait_for_recovery_log());
	EXPECT_TRUE(sink.disconnect_succeeded());
	EXPECT_EQ(disconnected_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_TRUE(ParserUDPTestPeer::stopped(parser));
	EXPECT_FALSE(parser.isConnected());

	// A control-thread connect joins the stopped worker and starts a fresh pair.
	ASSERT_TRUE(parser.connect());
	EXPECT_NE(disconnected_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_TRUE(parser.isConnected());
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, PersistentSendErrorsStopAfterBoundedRecoveryAttempts)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	CodeSet symbols;
	symbols.insert("SHFE.rb");
	parser.subscribe(symbols);
	ParserUDPTestPeer::reconnect(parser);

	const uint32_t max_attempts = ParserUDPTestPeer::max_auto_recovery_attempts();
	std::function<void(const boost::system::error_code&, std::size_t)> oldest_completion;
	for (uint32_t attempt = 0; attempt < max_attempts; ++attempt)
	{
		const uint64_t failed_epoch = ParserUDPTestPeer::connection_epoch(parser);
		auto completion = ParserUDPTestPeer::capture_current_write_handler(parser);
		ASSERT_TRUE(static_cast<bool>(completion));
		if (attempt == 0)
			oldest_completion = completion;
		completion(boost::asio::error::connection_reset, 0);
		EXPECT_NE(failed_epoch, ParserUDPTestPeer::connection_epoch(parser));
		EXPECT_EQ(attempt + 1, ParserUDPTestPeer::send_recovery_attempts(parser));
		EXPECT_EQ(attempt + 1, sink.close_count());
	}

	EXPECT_FALSE(ParserUDPTestPeer::recovery_exhausted(parser));
	const uint64_t terminal_epoch = ParserUDPTestPeer::connection_epoch(parser);
	auto terminal_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(terminal_completion));
	terminal_completion(boost::asio::error::connection_reset, 0);
	EXPECT_EQ(terminal_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_TRUE(ParserUDPTestPeer::recovery_exhausted(parser));
	EXPECT_TRUE(ParserUDPTestPeer::stopped(parser));
	EXPECT_FALSE(parser.isConnected());
	EXPECT_EQ(max_attempts + 1, sink.close_count());

	// A completion from the first failed epoch stays inert after exhaustion.
	oldest_completion(boost::asio::error::connection_reset, 0);
	EXPECT_EQ(terminal_epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_EQ(max_attempts + 1, sink.close_count());
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, PersistentReadErrorsStopAfterBoundedRecoveryAttempts)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	uint64_t epoch = ParserUDPTestPeer::reconnect(parser);
	const uint32_t max_attempts = ParserUDPTestPeer::max_auto_recovery_attempts();
	for (uint32_t attempt = 0; attempt < max_attempts; ++attempt)
	{
		ParserUDPTestPeer::dispatch_read_error(parser, true, epoch);
		const uint64_t recovered_epoch = ParserUDPTestPeer::connection_epoch(parser);
		ASSERT_NE(epoch, recovered_epoch);
		EXPECT_EQ(attempt + 1, ParserUDPTestPeer::broadcast_recovery_attempts(parser));
		EXPECT_EQ(attempt + 1, sink.close_count());
		epoch = recovered_epoch;
	}

	ParserUDPTestPeer::dispatch_read_error(parser, true, epoch);
	EXPECT_EQ(epoch, ParserUDPTestPeer::connection_epoch(parser));
	EXPECT_TRUE(ParserUDPTestPeer::recovery_exhausted(parser));
	EXPECT_TRUE(ParserUDPTestPeer::stopped(parser));
	EXPECT_EQ(max_attempts + 1, sink.close_count());
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPShutdownTest, SuccessfulCurrentSendResetsItsRecoveryBudget)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config(9000);
	ASSERT_TRUE(parser.init(config));
	config->release();

	CodeSet symbols;
	symbols.insert("SHFE.rb");
	parser.subscribe(symbols);
	ParserUDPTestPeer::reconnect(parser);
	auto failed_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(failed_completion));
	failed_completion(boost::asio::error::connection_reset, 0);
	EXPECT_EQ(1u, ParserUDPTestPeer::send_recovery_attempts(parser));

	auto successful_completion = ParserUDPTestPeer::capture_current_write_handler(parser);
	ASSERT_TRUE(static_cast<bool>(successful_completion));
	successful_completion(boost::system::error_code(), sizeof(uint32_t));
	EXPECT_EQ(0u, ParserUDPTestPeer::send_recovery_attempts(parser));
	EXPECT_EQ(0u, ParserUDPTestPeer::queued_packets(parser));
	EXPECT_TRUE(parser.shutdown());
}

// I03-NEW-012: this case runs the real worker. With sport=0 the subscription
// packet goes to 127.0.0.1:0; macOS fails that async_send_to with
// EADDRNOTAVAIL on the worker, which is a genuine failure of the recovered
// epoch and races with the injected-error assertions below (a second Close).
// Give the subscription channel a bound local receiver so only the injected
// read errors can end an epoch.
class LocalUdpReceiver
{
public:
	LocalUdpReceiver() : _socket(_io, boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)) {}
	int port() const { return static_cast<int>(_socket.local_endpoint().port()); }

private:
	boost::asio::io_context _io;
	boost::asio::ip::udp::socket _socket;
};

TEST(ParserUDPShutdownTest, OldTwoChannelReadCompletionsBelongToOneReconnectEpoch)
{
	for (bool broadcast_error_first : { true, false })
	{
		LocalUdpReceiver receiver;
		ParserUDP parser;
		WTSVariant* config = make_udp_config(receiver.port());
		ASSERT_TRUE(parser.init(config));
		config->release();

		CountingUdpSink sink;
		parser.registerSpi(&sink);
		CodeSet symbols;
		symbols.insert("SHFE.rb");
		parser.subscribe(symbols); // Arm the subscription receive as well as broadcast.
		ASSERT_TRUE(parser.connect());
		const uint64_t failed_epoch = ParserUDPTestPeer::connection_epoch(parser);

		ParserUDPTestPeer::dispatch_read_error(parser, broadcast_error_first, failed_epoch);
		ASSERT_EQ(1u, sink.close_count());
		const uint64_t recovered_epoch = ParserUDPTestPeer::connection_epoch(parser);
		ASSERT_NE(failed_epoch, recovered_epoch);

		// The other channel's already-queued error belongs to the failed pair.
		ParserUDPTestPeer::dispatch_read_error(parser, !broadcast_error_first, failed_epoch);
		EXPECT_EQ(1u, sink.close_count());
		EXPECT_EQ(recovered_epoch, ParserUDPTestPeer::connection_epoch(parser));

		// A fault from the newly armed pair starts the next close cycle.
		ParserUDPTestPeer::dispatch_read_error(parser, !broadcast_error_first, recovered_epoch);
		EXPECT_EQ(2u, sink.close_count());
		EXPECT_NE(recovered_epoch, ParserUDPTestPeer::connection_epoch(parser));
		EXPECT_TRUE(parser.shutdown());
	}
}

TEST(ParserUDPShutdownTest, BroadcastOnlyRecoveryStartsOneNewCloseCycle)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	ASSERT_TRUE(parser.connect()); // No symbols means no subscription receive is armed.
	const uint64_t failed_epoch = ParserUDPTestPeer::connection_epoch(parser);
	ParserUDPTestPeer::dispatch_read_error(parser, true, failed_epoch);
	ASSERT_EQ(1u, sink.close_count());
	const uint64_t recovered_epoch = ParserUDPTestPeer::connection_epoch(parser);
	ASSERT_NE(failed_epoch, recovered_epoch);

	// An inactive/late subscription completion from the prior pair is stale.
	ParserUDPTestPeer::dispatch_read_error(parser, false, failed_epoch);
	EXPECT_EQ(1u, sink.close_count());
	ParserUDPTestPeer::dispatch_read_error(parser, true, recovered_epoch);
	EXPECT_EQ(2u, sink.close_count());
	EXPECT_TRUE(parser.shutdown());
}

TEST(ParserUDPReceiveTest, EmptyBroadcastDatagramRearmsBeforeTheNextValidTick)
{
	ParserUDP parser;
	WTSVariant* config = make_udp_config();
	ASSERT_TRUE(parser.init(config));
	config->release();

	CountingUdpSink sink;
	parser.registerSpi(&sink);
	std::shared_ptr<ParserUDPTestPeer::ReceiveOperation> first_receive;
	const uint64_t epoch = ParserUDPTestPeer::reconnect(parser, &first_receive);
	ASSERT_NE(0u, epoch);
	ASSERT_TRUE(first_receive != nullptr);
	const uint16_t port = ParserUDPTestPeer::broadcast_port(parser);
	ASSERT_NE(0u, port);
	ParserUDPTestPeer::add_subscription(parser, "SHFE.rb");

	const std::weak_ptr<ParserUDPTestPeer::ReceiveOperation> first_receive_owner = first_receive;
	first_receive.reset();

	boost::asio::io_context sender_context;
	boost::asio::ip::udp::socket sender(sender_context,
		boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(), 0));
	const boost::asio::ip::udp::endpoint target(boost::asio::ip::address_v4::loopback(), port);
	const char empty_payload = 0;
	sender.send_to(boost::asio::buffer(&empty_payload, 0), target);

	// The old receive owner's release proves the real zero-byte completion ran.
	// The next valid datagram then proves that this channel was re-armed.
	ASSERT_TRUE(ParserUDPTestPeer::poll_until_receive_owner_released(parser, first_receive_owner));
	EXPECT_EQ(0u, sink.quote_count());
	EXPECT_EQ(0u, sink.close_count());

	const uint32_t push_tick_type = 0x200;
	WTSTickStruct tick{};
	std::memcpy(tick.exchg, "SHFE", sizeof("SHFE"));
	std::memcpy(tick.code, "rb", sizeof("rb"));
	std::vector<char> valid_packet(sizeof(push_tick_type) + sizeof(tick), 0);
	std::memcpy(valid_packet.data(), &push_tick_type, sizeof(push_tick_type));
	std::memcpy(valid_packet.data() + sizeof(push_tick_type), &tick, sizeof(tick));
	sender.send_to(boost::asio::buffer(valid_packet), target);

	const auto quote_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	while (sink.quote_count() == 0 && std::chrono::steady_clock::now() < quote_deadline)
	{
		if (ParserUDPTestPeer::poll_one(parser) == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(1u, sink.quote_count());
	EXPECT_EQ(0u, sink.close_count());
	EXPECT_TRUE(parser.shutdown());
}
