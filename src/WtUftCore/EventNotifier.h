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

#include <boost/asio/io_context.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <memory>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/DLLHelper.hpp"
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
	// The owner must stop external notify callers before destruction. Destruction
	// must not be initiated from this notifier's publish callback: self-join and
	// preserving the current object's lifetime cannot be made safe here.
	~EventNotifier();

private:
	void	stopWorker();
	void	tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output);
	void	orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output);

public:
	bool	init(WTSVariant* cfg);

	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	void	notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);
	void	notify(const char* trader, const char* message);

	void	notify_log(const char* tag, const char* message);

	void	notify_event(const char* message);

private:
	std::string		_url;
	DllHandle		_dll_handle; // successful MQ modules remain pinned without an ABI quiescence guarantee
	WtUInt32		_mq_sid;
	FuncCreateMQServer	_creator;
	FuncDestroyMQServer	_remover;
	FundPublishMessage	_publisher;
	FuncRegCallbacks	_register;

	boost::asio::io_context		_asyncio;
	std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> _work_guard;
	StdThreadPtr				_worker;
};

NS_WTP_END
