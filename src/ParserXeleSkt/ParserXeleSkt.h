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
#include "../Includes/WTSCollection.hpp"
#include "../Share/StdUtils.hpp"

#include <cstddef>
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
	virtual bool init(WTSVariant* config) override;

	virtual void release() override;

	virtual bool connect() override;

	virtual bool disconnect() override;

	virtual bool isConnected() override;

	virtual void subscribe(const CodeSet &vecSymbols) override;
	virtual void unsubscribe(const CodeSet &vecSymbols) override;

	virtual void registerSpi(IParserSpi* listener) override;


private:
	void	handle_udp_read(const boost::system::error_code& e, std::size_t bytes_transferred);

	bool	prepare();

	bool	reconnect();

	void	extract_buffer(uint32_t length);

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
	ip::udp::endpoint	_udp_ep;
	ip::tcp::endpoint	_tcp_ep;
	io_context			_io_service;

	io_context::strand	_strand;

	ip::udp::socket*	_udp_socket;

	boost::array<char, 4096> _udp_buffer;

	IParserSpi*		_sink;
	IBaseDataMgr*	_bd_mgr;
	bool			_stopped;
	bool			_prepared;

	CodeSet			_set_subs;
	StdThreadPtr	_thrd_parser;

	typedef WTSHashMap<int>	TickCache;
	TickCache*		_tick_cache;

	wt_hashmap<int, double> _price_scales;
};

