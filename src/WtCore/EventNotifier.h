/*!
 * \file EventCaster.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UDP广播对象定义
 */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"

typedef WtUInt32(*FuncCreateMQServer)(const char*, bool);
typedef void(*FuncDestroyMQServer)(WtUInt32);
typedef void(*FundPublishMessage)(WtUInt32, const char*, const char*, WtUInt32);

typedef void(PORTER_FLAG *FuncLogCallback)(WtUInt32, const char*, bool);
typedef void(*FuncRegCallbacks)(FuncLogCallback);


NS_WTP_BEGIN
class WTSTradeInfo;
class WTSOrderInfo;
class WTSVariant;

class EventNotifier
{
public:
	EventNotifier();
	~EventNotifier();


private:
	void	tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output);
	void	orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output);
	void	stopAccepting();
	bool	canAccept(const char* topic) const;
	template<typename Handler> void postHandler(const char* topic, Handler&& handler);

public:
	bool	init(WTSVariant* cfg);

	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);
	void	notify(const char* trader, const char* message);

	void	notify_log(const char* tag, const char* message);

	void	notify_event(const char* message);

	void	notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag);
	void	notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val);
	void	notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag);

private:
	std::string		_url;
	WtUInt32		_mq_sid;
	FuncCreateMQServer	_creator;
	FuncDestroyMQServer	_remover;
	FundPublishMessage	_publisher;
	FuncRegCallbacks	_register;

	mutable std::mutex		_state_mutex;
	bool			_accepting;
	std::atomic<bool>		_stopped;
	boost::asio::io_context		_asyncio;
	using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	std::unique_ptr<WorkGuard>	_work;
	StdThreadPtr				_worker;
};

NS_WTP_END
