#pragma once
#include <atomic>
#include <mutex>
#include <random>
#include <vector>

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSCollection.hpp"


NS_WTP_BEGIN
	class WTSTickData;
NS_WTP_END

USING_NS_WTP;

/*
 *	仿真交易器
 */
class TraderMocker : public ITraderApi
{
public:
	TraderMocker();
	~TraderMocker();

private:
	friend struct TraderMockerRngTestAccess;
	std::vector<uint32_t> split_volume(uint32_t vol, uint32_t minQty, uint32_t maxQty);
	std::mt19937 _split_rng;
	std::mutex _split_rng_mutex;

	/*
	*	撮合 定时器
	*/
	int32_t		match_once();

	uint32_t	makeTradeID();
	uint32_t	makeOrderID();
	// BUG-R60-001: once stop has been requested (release()/disconnect(), from any
	// thread) this instance delivers nothing more to its listener. Every delivery
	// re-reads the listener through this gate, and all deliveries are serialized
	// by _mtx_awaits, so a stop requested inside one callback isolates every later
	// callback of the same (still alive) instance from the host and from any new
	// session object that reuses the same listener.
	ITraderSpi*	get_listener() const
	{
		if (_delivery_closed.load(std::memory_order_acquire))
			return NULL;
		return _listener.load(std::memory_order_acquire);
	}

	void		load_positions();
	void		save_positions();


private:
	StdThreadPtr	_thrd_match;
	std::atomic<bool>	_terminated;
	// Serializes stop admission with each request's retain and post. Once
	// released, this instance cannot be restarted until old work has an
	// ownership-safe generation boundary.
	std::mutex		_request_mutex;
	bool			_requests_closed = false;
	// BUG-T6-009 / card 9c: a successfully initialized instance rejects init().
	bool			_initialized = false;

	// BUG-T6-009 / card 9b,9d: final stop/join is serialized by this mutex and
	// only performed from a thread that is not one of this adapter's own
	// workers. A release()/disconnect() issued from a callback on the io worker
	// or the matcher closes admission and requests stop, and the join is
	// completed later by an external release(), the destructor, or never (the
	// factory remover quarantines an instance it cannot prove has stopped).
	std::mutex		_release_mutex;
	bool			_released = false;
	std::atomic<bool>	_match_started{ false };
	// BUG-R60-001: closed together with request admission; see get_listener().
	std::atomic<bool>	_delivery_closed{ false };

	bool		on_own_thread() const;
	void		request_stop_from_callback();
	void		join_matcher_locked();

public:
	// Factory-remover support (card 9a/9b): true when the caller runs on this
	// instance's io worker or matcher thread.
	bool		is_callback_thread() const { return on_own_thread(); }
	// Quarantine support: detach this instance's threads (they exit on their own
	// after the stop request) so nothing is left unjoined; the object is never freed.
	void		detach_for_quarantine();
private:

	StdUniqueMutex		_mutex_api;

	std::atomic<uint32_t>	_auto_trade_id;
	std::atomic<uint32_t>	_auto_order_id;
	std::atomic<uint32_t>	_auto_entrust_id;

	std::atomic<ITraderSpi*> _listener;
	IBaseDataMgr*		_bd_mgr;

	StdThreadPtr		_thrd_worker;

	uint32_t		_millisecs;
	uint32_t		_mocker_id;
	bool			_use_newpx;
	double			_max_qty;
	double			_min_qty;

	WTSArray*		_orders;
	WTSArray*		_trades;
	typedef WTSHashMap<std::string> TickCache;
	TickCache*		_ticks;

	typedef WTSHashMap<std::string> OrderCache;
	OrderCache*			_awaits;
	StdUniqueMutex	_mtx_awaits;

	wt_hashset<std::string>	_codes;

	uint64_t		_max_tick_time;
	uint64_t		_last_match_time;

	typedef struct _PosUnit
	{
		double	_volume;
		double	_frozen;
	} PosUnit;

	typedef struct _PosItem
	{
		char	_exchg[MAX_INSTRUMENT_LENGTH];
		char	_code[MAX_INSTRUMENT_LENGTH];

		PosUnit		_long;
		PosUnit		_short;

		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}
	} PosItem;

	wt_hashmap<std::string, PosItem> _positions;
	std::string		_pos_file;

private:
	int			_udp_port;

	boost::asio::ip::udp::endpoint	_broad_ep;
	boost::asio::io_context			_io_service;

	boost::asio::ip::udp::socket*	_b_socket;

	boost::array<char, 1024> _b_buffer;

	void	handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad);

	void	extract_buffer(uint32_t length, bool isBroad);

	void	reconn_udp();

//////////////////////////////////////////////////////////////////////////
//ITraderApi
public:
	virtual bool init(WTSVariant *params) override;

	virtual void release() override;

	virtual void registerSpi(ITraderSpi *listener) override;

	virtual void connect() override;

	virtual void disconnect() override;

	virtual bool isConnected() override;

	virtual bool makeEntrustID(char* buffer, int length) override;

	virtual int login(const char* user, const char* pass, const char* productInfo) override;

	virtual int logout() override;

	virtual int orderInsert(WTSEntrust* eutrust) override;

	virtual int orderAction(WTSEntrustAction* action) override;

	virtual int queryAccount() override;

	virtual int queryPositions() override;

	virtual int queryOrders() override;

	virtual int queryTrades() override;
};
