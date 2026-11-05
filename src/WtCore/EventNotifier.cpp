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
#include <chrono>
#include <vector>
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
#include <cstdio>
#include <exception>
#include <memory>
#include <utility>
namespace rj = rapidjson;

USING_NS_WTP;

void PORTER_FLAG on_mq_log(WtUInt32 /*id*/, const char* /*message*/, bool /*bServer*/)
{

}

EventNotifier::EventNotifier()
	: _mq_sid(0)
	, _creator(NULL)
	, _remover(NULL)
	, _publisher(NULL)
	, _register(NULL)
	, _accepting(false)
	, _stopped(false)
	, _core(std::make_shared<Core>())
{
	
}


EventNotifier::~EventNotifier()
{
	stopAccepting();
	// Decision K24=E1 (2026-11-04): wait for the queue to drain with a bound. A
	// worker still busy (e.g. a publisher blocked by the MQ module) is detached
	// and its core is retained until process exit; the MQ server is then not
	// destroyed because a handler may still be using it.
	std::shared_ptr<Core> core = _core;
	bool drained = true;
	if (core->worker)
	{
		std::unique_lock<std::mutex> lock(core->mtx);
		drained = core->cv.wait_for(lock, std::chrono::milliseconds(_stop_timeout_ms),
			[&core]() { return core->finished; });
		lock.unlock();
		if (drained)
		{
			if (core->worker->joinable())
				core->worker->join();
		}
		else
		{
			WTSLogger::error("EventNotifier worker did not drain within {} ms; it is detached and its MQ server is retained until process exit",
				_stop_timeout_ms);
			core->worker->detach();
			static std::mutex* retainedMutex = new std::mutex();
			static std::vector<std::shared_ptr<Core>>* retained = new std::vector<std::shared_ptr<Core>>();
			std::lock_guard<std::mutex> retainLock(*retainedMutex);
			retained->push_back(core);
		}
	}
	if (!drained)
		return;

	core->io.stop();

	if (_remover && _mq_sid != 0)
		_remover(_mq_sid);
}

void EventNotifier::stopAccepting()
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	_accepting = false;
	_stopped.store(true, std::memory_order_relaxed);
	if (_core->work)
		_core->work->reset();
}

// The caller holds _state_mutex until postHandler has returned. This makes an
// accepted post happen before stopAccepting releases the work guard.
bool EventNotifier::canAccept(const char* topic) const
{
	if (_accepting && _mq_sid != 0)
		return true;
	if (_stopped.load(std::memory_order_relaxed))
		std::fprintf(stderr, "EventNotifier rejected %s: stopping\n", topic);
	return false;
}

template<typename Handler>
void EventNotifier::postHandler(const char* topic, Handler&& handler)
{
	try
	{
		boost::asio::post(_core->io, [topic, task = std::forward<Handler>(handler)]() mutable {
			try { task(); }
			catch (const std::exception& ex) {
				std::fprintf(stderr, "EventNotifier %s handler failed: %s\n", topic, ex.what());
			}
			catch (...) {
				std::fprintf(stderr, "EventNotifier %s handler failed: unknown exception\n", topic);
			}
		});
	}
	catch (...)
	{
		std::fprintf(stderr, "EventNotifier failed to queue %s\n", topic);
		throw;
	}
}

bool EventNotifier::init(WTSVariant* cfg)
{
	if (!cfg->getBoolean("active"))
		return false;

	const std::string url = cfg->getCString("url");
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
	if (creator == NULL)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is not compatible", dllpath.c_str());
		return false;
	}

	FuncDestroyMQServer remover = (FuncDestroyMQServer)DLLHelper::get_symbol(dllInst, "destroy_server");
	FundPublishMessage publisher = (FundPublishMessage)DLLHelper::get_symbol(dllInst, "publish_message");
	FuncRegCallbacks registerCallbacks = (FuncRegCallbacks)DLLHelper::get_symbol(dllInst, "regiter_callbacks");
	if (registerCallbacks == NULL)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is missing regiter_callbacks", dllpath.c_str());
		return false;
	}

	//注册回调函数
	registerCallbacks(on_mq_log);

	//创建一个MQServer
	// This configuration has no confirm option. Match the MQ module's default
	// behavior explicitly so the plugin ABI receives the complete argument list.
	// I03-NEW-065: id 0 means no server was created; report failure, keep the
	// existing notifier state and start no worker.
	const WtUInt32 sid = creator(url.c_str(), false);
	if (sid == 0)
	{
		WTSLogger::error("MQ module {} failed to create a server for {}", dllpath.c_str(), url.c_str());
		return false;
	}

	// Keep the existing notifier intact until the candidate module has all
	// required exports and has created its server.
	_url = url;
	_creator = creator;
	_remover = remover;
	_publisher = publisher;
	_register = registerCallbacks;
	_mq_sid = sid;
	_stop_timeout_ms = cfg->has("stop_timeout_ms") ? cfg->getUInt32("stop_timeout_ms") : 5000;
	_core->publisher = publisher;
	_core->sid = sid;

	if (_core->worker == NULL)
	{
		std::lock_guard<std::mutex> lock(_state_mutex);
		_core->work.reset(new WorkGuard(_core->io.get_executor()));
		try
		{
			std::shared_ptr<Core> core = _core;
			_core->worker.reset(new StdThread([core]() {
				core->io.run();
				{
					std::lock_guard<std::mutex> doneLock(core->mtx);
					core->finished = true;
				}
				core->cv.notify_all();
			}));
			_accepting = true;
		}
		catch (...)
		{
			_core->work.reset();
			throw;
		}
	}
	WTSLogger::info("EventNotifier initialized with channel {}", _url.c_str());

	return true;
}

void EventNotifier::notify_log(const char* tag, const char* message)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("LOG"))
		return;

	std::string strTag = tag;
	std::string strMsg = message;
	postHandler("LOG", [weakCore = std::weak_ptr<Core>(_core), strTag, strMsg]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
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

		core->publish("LOG", data);
	});
}

void EventNotifier::notify_event(const char* message)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("GRP_EVENT"))
		return;

	std::string strMsg = message;
	postHandler("GRP_EVENT", [weakCore = std::weak_ptr<Core>(_core), strMsg]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
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
		core->publish("GRP_EVENT", data);
	});
}

void EventNotifier::notify(const char* trader, const char* message)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("TRD_NOTIFY"))
		return;

	std::string strTrader = trader;
	std::string strMsg = message;
	postHandler("TRD_NOTIFY", [weakCore = std::weak_ptr<Core>(_core), strTrader, strMsg]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
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
		core->publish("TRD_NOTIFY", data);
	});
}

void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (trdInfo == NULL || !canAccept("TRD_TRADE"))
		return;

	std::string strTrader = trader;
	std::string strCode = stdCode;
	// The shared_ptr deleter balances retain even if allocating its control block fails.
	trdInfo->retain();
	std::shared_ptr<WTSTradeInfo> retainedTrade(trdInfo, [](WTSTradeInfo* p) {
		p->release();
	});
	postHandler("TRD_TRADE", [weakCore = std::weak_ptr<Core>(_core), strTrader, strCode, localid, retainedTrade]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
		std::string data;
		tradeToJson(strTrader.c_str(), localid, strCode.c_str(), retainedTrade.get(), data);
		if (data.empty())
			WTSLogger::error("EventNotifier failed to serialize TRD_TRADE JSON");
		else core->publish("TRD_TRADE", data);
	});
}

void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (ordInfo == NULL || !canAccept("TRD_ORDER"))
		return;

	std::string strTrader = trader;
	std::string strCode = stdCode;
	ordInfo->retain();
	std::shared_ptr<WTSOrderInfo> retainedOrder(ordInfo, [](WTSOrderInfo* p) {
		p->release();
	});
	postHandler("TRD_ORDER", [weakCore = std::weak_ptr<Core>(_core), strTrader, strCode, localid, retainedOrder]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
		std::string data;
		orderToJson(strTrader.c_str(), localid, strCode.c_str(), retainedOrder.get(), data);
		if (data.empty())
			WTSLogger::error("EventNotifier failed to serialize TRD_ORDER JSON");
		else core->publish("TRD_ORDER", data);
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

void EventNotifier::notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("CHART_INDEX"))
		return;

	std::string sid = straId;
	std::string iname = idxName;
	std::string lname = lineName;
	postHandler("CHART_INDEX", [weakCore = std::weak_ptr<Core>(_core), time, sid, iname, lname, val]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("index_name", rj::Value(iname.c_str(), allocator), allocator);
			root.AddMember("line_name", rj::Value(lname.c_str(), allocator), allocator);
			root.AddMember("time", time, allocator);
			root.AddMember("value", val, allocator);

			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		core->publish("CHART_INDEX", data);
	});
}

void EventNotifier::notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("CHART_MARKER"))
		return;

	std::string sid = straId;
	std::string sIcon = icon;
	std::string sTag = tag;
	postHandler("CHART_MARKER", [weakCore = std::weak_ptr<Core>(_core), time, sid, sIcon, sTag, price]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("icon", rj::Value(sIcon.c_str(), allocator), allocator);
			root.AddMember("tag", rj::Value(sTag.c_str(), allocator), allocator);
			root.AddMember("time", time, allocator);
			root.AddMember("price", price, allocator);

			rj::StringBuffer sb;
			rj::Writer<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		core->publish("CHART_MARKER", data);
	});
}

void EventNotifier::notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag)
{
	std::lock_guard<std::mutex> lock(_state_mutex);
	if (!canAccept("STRA_TRADE"))
		return;

	std::string sid = straId;
	std::string code = stdCode;
	std::string tag = userTag;
	postHandler("STRA_TRADE", [weakCore = std::weak_ptr<Core>(_core), sid, code, tag, isLong, isOpen, curTime, price]() {
		// K24: a queued handler must not own the core that owns its io_context (that
		// cycle leaked discarded handlers and their retained trade/order references).
		const std::shared_ptr<Core> core = weakCore.lock();
		if (!core)
			return;
		std::string data;
		{
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("code", rj::Value(code.c_str(), allocator), allocator);
			root.AddMember("tag", rj::Value(tag.c_str(), allocator), allocator);
			root.AddMember("long", isLong, allocator);
			root.AddMember("open", isOpen, allocator);
			root.AddMember("time", curTime, allocator);
			root.AddMember("price", price, allocator);

			rj::StringBuffer sb;
			rj::Writer<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		core->publish("STRA_TRADE", data);
	});
}
