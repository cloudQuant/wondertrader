/*!
 * \file ParserUDP.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include "../Includes/IParserApi.h"
#include "../Share/StdUtils.hpp"

#include <queue>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>

USING_NS_WTP;
using namespace boost::asio;

class ParserUDP : public IParserApi
{
public:
	ParserUDP();
	~ParserUDP();

	//IQuoteParser ½Ó¿Ú
public:
	virtual bool init(WTSVariant* config) override;

	// Legacy void interface: external calls drain synchronously; a callback
	// thread can only request stop. Use shutdown() or the module deleter to
	// observe whether the owner may be destroyed.
	virtual void release() override;

	// Synchronously stop the worker and drain all callbacks. Returns false when
	// called from an io/SPI callback thread; the owner must retain this parser
	// and retry from another thread before destroying it.
	bool shutdown();

	virtual bool connect() override;

	virtual bool disconnect() override;

	virtual bool isConnected() override;

	virtual void subscribe(const CodeSet &vecSymbols) override;
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	virtual void registerSpi(IParserSpi* listener) override;


private:
	friend struct ParserUDPTestPeer;
	friend struct ParserUDPExtractBufferTestPeer;
	struct CallbackFrame
	{
		ParserUDP* parser;
		CallbackFrame* previous;
	};

	template<typename Fn>
	void with_spi(Fn&& fn)
	{
		// Every SPI entry, including listener-replacement logs, is serialized by
		// the recursive I/O mutex. It also establishes a single lock order for
		// callbacks which re-enter isConnected()/subscribe().
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

	using ReceiveBuffer = boost::array<char, 1024>;
	using ReceiveEndpoint = ip::udp::endpoint;
	struct ReceiveOperation
	{
		ReceiveBuffer buffer;
		ReceiveEndpoint endpoint;
	};
	std::shared_ptr<ReceiveOperation> arm_receive(bool isBroad, uint64_t connection_epoch);
	void	handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad,
		uint64_t connection_epoch, const std::shared_ptr<ReceiveOperation>& operation_owner);
	void	handle_write(const boost::system::error_code& e, uint64_t connection_epoch, uint64_t send_id,
		const std::shared_ptr<std::string>& buffer_owner);
	std::function<void(const boost::system::error_code&, std::size_t)> make_write_handler(uint64_t connection_epoch,
		uint64_t send_id, const std::shared_ptr<std::string>& buffer_owner);
	void	close_sockets();
	// Caller may already hold _mtx_io; only the first close path reports WPE_Close.
	void	emit_close_once();
	void	recover_from_transport_failure(uint32_t& attempts, const char* channel, uint64_t failed_epoch);
	bool	is_worker_thread() const;
	bool	is_callback_thread() const;

	bool	reconnect(uint32_t flag = 3, std::shared_ptr<ReceiveOperation>* broadcast_operation = nullptr);

	void	subscribe();

	void	extract_buffer(uint32_t length, bool isBroad, const char* buffer_data);

private:
	void	doOnConnected();
	void	doOnDisconnected();

	void	do_send();

private:
	std::string	_hots;
	int			_bport;
	int			_sport;
	uint32_t	_gpsize;

	ip::udp::endpoint	_broad_ep;
	ip::udp::endpoint	_server_ep;
	io_context			_io_service;

	boost::asio::strand<boost::asio::io_context::executor_type>	_strand;

	ip::udp::socket*	_b_socket;
	ip::udp::socket*	_s_socket;
	bool				_s_inited;

	IParserSpi*				_sink;
	bool					_stopped;
	bool					_connecting;
	// Incremented whenever the complete channel pair is rebuilt. Completions
	// from an older pair are ignored after a reconnect starts.
	uint64_t			_connection_epoch;
	// Guarded by _mtx_io; reset for each successful connect.
	bool					_close_event_emitted;
	inline static constexpr uint32_t MAX_AUTO_RECOVERY_ATTEMPTS = 3;
	uint32_t				_send_recovery_attempts = 0;
	uint32_t				_broadcast_recovery_attempts = 0;
	uint32_t				_subscription_recovery_attempts = 0;
	bool					_recovery_exhausted = false;
	std::atomic<bool>	_stop_requested;
	// True only while the io_context worker is expected to process completions.
	std::atomic<bool>	_worker_running{ false };
	std::recursive_mutex _mtx_io;
	std::mutex			_mtx_lifecycle;
	inline static thread_local CallbackFrame* _callback_stack_top = nullptr;

	CodeSet					_set_subs;

	StdThreadPtr			_thrd_parser;

	StdUniqueMutex			_mtx_queue;
	std::queue<std::string>	_send_queue;
	bool					_send_in_flight = false;
	uint64_t				_next_send_id = 0;
	uint64_t				_active_send_epoch = 0;
	uint64_t				_active_send_id = 0;
	std::shared_ptr<std::string> _pending_send_buffer;
};

