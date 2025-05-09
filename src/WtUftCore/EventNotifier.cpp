/*!
 * \file EventCaster.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 事件通知器实现文件
 *
 * 本文件实现了事件通知器(EventNotifier)类，用于将交易系统中的各类事件（如日志、订单、成交信息等）
 * 通过消息队列发送给外部系统。支持异步消息处理和JSON格式消息封装。
 */
#include "EventNotifier.h"
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
namespace rj = rapidjson;

USING_NS_WTP;

/**
 * @brief 消息队列日志回调函数
 * 
 * @param id 消息队列服务器ID
 * @param message 日志消息
 * @param bServer 是否来自服务器端
 */
void on_mq_log(unsigned long id, const char* message, bool bServer)
{
	// 可以在这里添加日志处理逻辑
}

/**
 * @brief 事件通知器构造函数
 * 
 * 初始化事件通知器对象，设置初始属性值
 */
EventNotifier::EventNotifier()
	: _mq_sid(0)      // 初始化消息队列服务器ID为0
	, _publisher(NULL) // 初始化消息发布者为空
	, _stopped(false)  // 初始化停止标志为否
{
	// 构造函数体内不需要额外操作
}


/**
 * @brief 事件通知器析构函数
 * 
 * 清理事件通知器资源，关闭工作线程并销毁消息队列连接
 */
EventNotifier::~EventNotifier()
{
	_stopped = true;  // 设置停止标志，通知工作线程结束
	if (_worker)
		_worker->join(); // 等待工作线程结束

	_asyncio.stop();  // 停止异步IO服务

	if (_remover && _mq_sid != 0)
		_remover(_mq_sid); // 销毁消息队列服务器连接
}

/**
 * @brief 初始化事件通知器
 * 
 * 根据配置加载消息队列模块，初始化消息队列服务器连接，并创建异步处理线程
 * 
 * @param cfg 配置对象，包含消息队列服务器连接参数
 * @return bool 初始化成功返回true，失败返回false
 */
bool EventNotifier::init(WTSVariant* cfg)
{
	// 如果不激活消息队列，直接返回失败
	if (!cfg->getBoolean("active"))
		return false;

	// 获取消息队列服务器URL
	_url = cfg->getCString("url");
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

	_creator = (FuncCreateMQServer)DLLHelper::get_symbol(dllInst, "create_server");
	if (_creator == NULL)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is not compatible", dllpath.c_str());
		return false;
	}

	_remover = (FuncDestroyMQServer)DLLHelper::get_symbol(dllInst, "destroy_server");
	_publisher = (FundPublishMessage)DLLHelper::get_symbol(dllInst, "publish_message");
	_register = (FuncRegCallbacks)DLLHelper::get_symbol(dllInst, "regiter_callbacks");

	//注册回调函数
	_register(on_mq_log);
	
	//创建一个MQServer
	_mq_sid = _creator(_url.c_str());

	WTSLogger::info("EventNotifier initialized with channel {}", _url.c_str());

	if (_worker == NULL)
	{
		boost::asio::io_service::work work(_asyncio);
		_worker.reset(new StdThread([this]() {
			while (!_stopped)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				_asyncio.run_one();
				//m_asyncIO.run();
			}
		}));
	}

	return true;
}

/**
 * @brief 发送日志通知
 * 
 * 将标签和日志消息作为JSON对象发送到消息队列，主题为"LOG"
 * 
 * @param tag 日志标签，标识日志来源或类型
 * @param message 日志内容
 */
void EventNotifier::notify_log(const char* tag, const char* message)
{
	// 如果消息队列服务器不可用，直接返回
	if (_mq_sid == 0)
		return;

	// 复制参数，防止异步调用时参数已被释放
	std::string strTag = tag;
	std::string strMsg = message;
	// 添加异步任务
	_asyncio.post([this, strTag, strMsg]() {
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
			_publisher(_mq_sid, "LOG", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送事件通知
 * 
 * 将事件消息作为JSON对象发送到消息队列，主题为"GRP_EVENT"
 * 
 * @param message 事件消息内容
 */
void EventNotifier::notify_event(const char* message)
{
	// 如果消息队列服务器不可用，直接返回
	if (_mq_sid == 0)
		return;

	// 复制参数，防止异步调用时参数已被释放
	std::string strMsg = message;
	// 添加异步任务
	_asyncio.post([this, strMsg]() {
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
			_publisher(_mq_sid, "GRP_EVENT", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送交易器消息通知
 * 
 * 将交易器名称和消息内容作为JSON对象发送到消息队列，主题为"TRD_NOTIFY"
 * 
 * @param trader 交易器名称
 * @param message 消息内容
 */
void EventNotifier::notify(const char* trader, const char* message)
{
	// 如果消息队列服务器不可用，直接返回
	if (_mq_sid == 0)
		return;

	// 复制参数，防止异步调用时参数已被释放
	std::string strTrader = trader;
	std::string strMsg = message;
	// 添加异步任务
	_asyncio.post([this, strTrader, strMsg]() {
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
			_publisher(_mq_sid, "TRD_NOTIFY", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送成交信息通知
 * 
 * 将成交信息转换为JSON对象发送到消息队列，主题为"TRD_TRADE"
 * 
 * @param trader 交易器名称
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param trdInfo 成交信息对象
 */
void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)
{
	// 如果成交信息为空或消息队列服务器不可用，直接返回
	if (trdInfo == NULL || _mq_sid == 0)
		return;

	// 复制参数，防止异步调用时参数已被释放
	std::string strTrader = trader;
	std::string strCode = stdCode;
	trdInfo->retain(); // 增加引用计数，防止异步调用期间被释放
	// 添加异步任务
	_asyncio.post([this, strTrader, strCode, localid, trdInfo]() {
		std::string data;
		tradeToJson(strTrader.c_str(), localid, strCode.c_str(), trdInfo, data);
		if (_publisher)
			_publisher(_mq_sid, "TRD_TRADE", data.c_str(), (unsigned long)data.size());
		trdInfo->release();
	});
}

/**
 * @brief 发送订单信息通知
 * 
 * 将订单信息转换为JSON对象发送到消息队列，主题为"TRD_ORDER"
 * 
 * @param trader 交易器名称
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param ordInfo 订单信息对象
 */
void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)
{
	// 如果订单信息为空或消息队列服务器不可用，直接返回
	if (ordInfo == NULL || _mq_sid == 0)
		return;

	// 复制参数，防止异步调用时参数已被释放
	std::string strTrader = trader;
	std::string strCode = stdCode;
	ordInfo->retain(); // 增加引用计数，防止异步调用期间被释放
	// 添加异步任务
	_asyncio.post([this, strTrader, strCode, localid, ordInfo]() {
		std::string data;
		orderToJson(strTrader.c_str(), localid, strCode.c_str(), ordInfo, data);
		if (_publisher)
			_publisher(_mq_sid, "TRD_ORDER", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 将成交信息转换为JSON格式
 * 
 * 将成交信息对象的各个字段转换为JSON格式字符串
 * 
 * @param trader 交易器名称
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param trdInfo 成交信息对象
 * @param output 输出参数，用于返回JSON字符串
 */
void EventNotifier::tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output)
{
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
		root.Accept(writer);

		output = sb.GetString();
	}
}

/**
 * @brief 将订单信息转换为JSON格式
 * 
 * 将订单信息对象的各个字段转换为JSON格式字符串
 * 
 * @param trader 交易器名称
 * @param localid 本地订单ID
 * @param stdCode 标准化合约代码
 * @param ordInfo 订单信息对象
 * @param output 输出参数，用于返回JSON字符串
 */
void EventNotifier::orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output)
{
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
		root.Accept(writer);

		output = sb.GetString();
	}
}
