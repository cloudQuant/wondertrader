/*!
 * \file EventCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "EventNotifier.h"

#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include "WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSCollection.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>
#include <exception>
#include <memory>
namespace rj = rapidjson;

USING_NS_WTP;

namespace
{
	template<typename T>
	std::shared_ptr<T> retainForAsync(T* object)
	{
		object->retain();
		return std::shared_ptr<T>(object, [](T* retained) {
			retained->release();
		});
	}
}

void PORTER_FLAG on_mq_log(WtUInt32 /*id*/, const char* /*message*/, bool /*bServer*/)
{

}

EventNotifier::EventNotifier()
	: _dll_handle(NULL)
	, _mq_sid(0)
	, _creator(NULL)
	, _remover(NULL)
	, _publisher(NULL)
	, _register(NULL)
{
	
}


EventNotifier::~EventNotifier()
{
	stopWorker();
	_asyncio.stop();

	if (_remover && _mq_sid != 0)
	{
		_remover(_mq_sid);
		_mq_sid = 0;
	}

	// The MQ ABI has no callback-quiescence contract. Keep a successful module
	// load pinned until process exit rather than risking unloading code that a
	// plugin thread may still call through a registered callback pointer.
}

void EventNotifier::stopWorker()
{
	// Keep the io_context alive until release starts, then let run() drain all
	// already-posted notifications before the worker is joined.
	if (_work_guard)
		_work_guard->reset();

	if (_worker && _worker->joinable())
		_worker->join();
}

bool EventNotifier::init(WTSVariant* cfg)
{
	if (cfg == NULL || !cfg->getBoolean("active"))
		return false;

	const char* url = cfg->getCString("url");
	if (url == NULL)
		return false;

	// Repeated init with the same active URL is harmless; changing the URL on an
	// already active notifier is rejected instead of leaking another MQ server.
	if (_dll_handle != NULL || _worker != NULL || _mq_sid != 0)
		return _dll_handle != NULL && _worker != NULL && _worker->joinable()
			&& _mq_sid != 0 && _url == url;

	_url = url;
	std::string module = DLLHelper::wrap_module("WtMsgQue", "lib");
	//先看工作目录下是否有对应模块
	std::string dllpath = WtHelper::getCWD() + module;
	//如果没有,则再看模块目录,即dll同目录下
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getInstDir() + module;

	DllHandle dllInst = DLLHelper::load_library(dllpath.c_str());
	if (dllInst == NULL)
	{
		WTSLogger::error("MQ module {} loading failed", dllpath.c_str());
		return false;
	}

	FuncCreateMQServer creator = (FuncCreateMQServer)DLLHelper::get_symbol(dllInst, "create_server");
	FuncDestroyMQServer remover = (FuncDestroyMQServer)DLLHelper::get_symbol(dllInst, "destroy_server");
	FundPublishMessage publisher = (FundPublishMessage)DLLHelper::get_symbol(dllInst, "publish_message");
	FuncRegCallbacks registerCallbacks = (FuncRegCallbacks)DLLHelper::get_symbol(dllInst, "regiter_callbacks");
	if (creator == NULL || remover == NULL || publisher == NULL || registerCallbacks == NULL)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is not compatible", dllpath.c_str());
		return false;
	}

	//创建一个MQServer。注册全局日志回调前先确认所有必需符号和服务端 ID。
	WtUInt32 mqSid = creator(_url.c_str(), false);
	if (mqSid == 0)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} failed to create a server", dllpath.c_str());
		return false;
	}

	registerCallbacks(on_mq_log);
	_dll_handle = dllInst;
	_creator = creator;
	_remover = remover;
	_publisher = publisher;
	_register = registerCallbacks;
	_mq_sid = mqSid;

	WTSLogger::info("EventNotifier initialized with channel {}", _url.c_str());

	if (_worker == NULL)
	{
		_work_guard.reset(new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(
			boost::asio::make_work_guard(_asyncio)));
		_worker.reset(new StdThread([this]() {
			for (;;)
			{
				try
				{
					_asyncio.run();
					return;
				}
				catch (const std::exception& e)
				{
					try { WTSLogger::error("EventNotifier async handler failed: {}", e.what()); }
					catch (...) {}
				}
				catch (...)
				{
					try { WTSLogger::error("EventNotifier async handler failed with an unknown exception"); }
					catch (...) {}
				}
			}
		}));
	}

	return true;
}

void EventNotifier::notify_log(const char* tag, const char* message)
{
	if (_mq_sid == 0)
		return;

	std::string strTag = tag;
	std::string strMsg = message;
	boost::asio::post(_asyncio, [this, strTag, strMsg]() {
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("tag", rj::Value(strTag.c_str(), allocator), allocator);
			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}

		if (_publisher)
			_publisher(_mq_sid, "LOG", data.c_str(), (WtUInt32)data.size());
	});
}

void EventNotifier::notify_event(const char* message)
{
	if (_mq_sid == 0)
		return;

	std::string strMsg = message;
	boost::asio::post(_asyncio, [this, strMsg]() {
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		if (_publisher)
			_publisher(_mq_sid, "GRP_EVENT", data.c_str(), (WtUInt32)data.size());
	});
}

void EventNotifier::notify(const char* trader, const char* message)
{
	if (_mq_sid == 0)
		return;

	std::string strTrader = trader;
	std::string strMsg = message;
	boost::asio::post(_asyncio, [this, strTrader, strMsg]() {
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("trader", rj::Value(strTrader.c_str(), allocator), allocator);
			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		if (_publisher)
			_publisher(_mq_sid, "TRD_NOTIFY", data.c_str(), (WtUInt32)data.size());
	});
}

void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)
{
	if (trdInfo == NULL || _mq_sid == 0)
		return;

	std::string strTrader = trader;
	std::string strCode = stdCode;
	std::shared_ptr<WTSTradeInfo> retained = retainForAsync(trdInfo);
	boost::asio::post(_asyncio, [this, strTrader, strCode, localid, retained]() {
		std::string data;
		tradeToJson(strTrader.c_str(), localid, strCode.c_str(), retained.get(), data);
		if (data.empty())
			WTSLogger::error("UFT EventNotifier failed to serialize TRD_TRADE JSON");
		else if (_publisher)
			_publisher(_mq_sid, "TRD_TRADE", data.c_str(), (WtUInt32)data.size());
	});
}

void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)
{
	if (ordInfo == NULL || _mq_sid == 0)
		return;

	std::string strTrader = trader;
	std::string strCode = stdCode;
	std::shared_ptr<WTSOrderInfo> retained = retainForAsync(ordInfo);
	boost::asio::post(_asyncio, [this, strTrader, strCode, localid, retained]() {
		std::string data;
		orderToJson(strTrader.c_str(), localid, strCode.c_str(), retained.get(), data);
		if (data.empty())
			WTSLogger::error("UFT EventNotifier failed to serialize TRD_ORDER JSON");
		else if (_publisher)
			_publisher(_mq_sid, "TRD_ORDER", data.c_str(), (WtUInt32)data.size());
	});
}

void EventNotifier::tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output)
{
	output.clear();
	if(trdInfo == NULL)
	{
		output = "{}";
		return;
	}

	bool isLong = (trdInfo->getDirection() == WDT_LONG);
	bool isOpen = (trdInfo->getOffsetType() == WOT_OPEN);
	bool isToday = (trdInfo->getOffsetType() == WOT_CLOSETODAY);

	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		root.AddMember("trader", rj::Value(trader, allocator), allocator);
		root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
		root.AddMember("localid", localid, allocator);
		root.AddMember("code", rj::Value(stdCode, allocator), allocator);
		root.AddMember("islong", isLong, allocator);
		root.AddMember("isopen", isOpen, allocator);
		root.AddMember("istoday", isToday, allocator);

		root.AddMember("volume", trdInfo->getVolume(), allocator);
		root.AddMember("price", trdInfo->getPrice(), allocator);

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		if (!root.Accept(writer) || !writer.IsComplete())
			return;

		output.assign(sb.GetString(), sb.GetSize());
	}
}

void EventNotifier::orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output)
{
	output.clear();
	if (ordInfo == NULL)
	{
		output = "{}";
		return;
	}

	bool isLong = (ordInfo->getDirection() == WDT_LONG);
	bool isOpen = (ordInfo->getOffsetType() == WOT_OPEN);
	bool isToday = (ordInfo->getOffsetType() == WOT_CLOSETODAY);
	bool isCanceled = (ordInfo->getOrderState() == WOS_Canceled);

	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		root.AddMember("trader", rj::Value(trader, allocator), allocator);
		root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
		root.AddMember("localid", localid, allocator);
		root.AddMember("code", rj::Value(stdCode, allocator), allocator);
		root.AddMember("islong", isLong, allocator);
		root.AddMember("isopen", isOpen, allocator);
		root.AddMember("istoday", isToday, allocator);
		root.AddMember("canceled", isCanceled, allocator);

		root.AddMember("total", ordInfo->getVolume(), allocator);
		root.AddMember("left", ordInfo->getVolLeft(), allocator);
		root.AddMember("traded", ordInfo->getVolTraded(), allocator);
		root.AddMember("price", ordInfo->getPrice(), allocator);
		root.AddMember("state", rj::Value(ordInfo->getStateMsg(), allocator), allocator);

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		if (!root.Accept(writer) || !writer.IsComplete())
			return;

		output.assign(sb.GetString(), sb.GetSize());
	}
}
