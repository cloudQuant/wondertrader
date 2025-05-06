/**
 * @file EventNotifier.cpp
 * @project	WonderTrader
 *
 * @author Wesley
 * @date 2020/03/30
 * 
 * @brief 事件通知器实现
 * @details 实现了事件通知器类，负责处理并发送各类交易相关的事件和日志
 *          通过消息队列模块异步实现通知功能
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
 * @param id 服务器ID
 * @param message 日志消息
 * @param bServer 是否服务器日志
 */
void on_mq_log(unsigned long id, const char* message, bool bServer)
{
    // 空实现，可以根据需要处理消息队列的日志
}

/**
 * @brief 构造函数
 * @details 初始化事件通知器对象，设置默认属性值
 */
EventNotifier::EventNotifier()
	: _mq_sid(0)
	, _publisher(NULL)
	, _stopped(false)
{
	// 构造函数中只进行初始化，不进行复杂操作
}


/**
 * @brief 析构函数
 * @details 清理事件通知器资源，停止工作线程并销毁消息队列服务器
 */
EventNotifier::~EventNotifier()
{
	// 停止工作线程
	_stopped = true;
	if (_worker)
		_worker->join();

	// 停止异步IO服务
	_asyncio.stop();

	// 销毁消息队列服务器
	if (_remover && _mq_sid != 0)
		_remover(_mq_sid);
}

/**
 * @brief 初始化事件通知器
 * @details 加载消息队列模块，初始化消息队列服务并启动工作线程
 * @param cfg 配置对象
 * @return 是否初始化成功
 */
bool EventNotifier::init(WTSVariant* cfg)
{
	// 检查是否启用事件通知器
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

	// 加载消息队列动态库
	DllHandle dllInst = DLLHelper::load_library(dllpath.c_str());
	if (dllInst == NULL)
	{
		WTSLogger::error("MQ module {} loading failed", dllpath.c_str());
		return false;
	}

	// 获取创建服务器函数
	_creator = (FuncCreateMQServer)DLLHelper::get_symbol(dllInst, "create_server");
	if (_creator == NULL)
	{
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is not compatible", dllpath.c_str());
		return false;
	}

	// 获取其他函数指针
	_remover = (FuncDestroyMQServer)DLLHelper::get_symbol(dllInst, "destroy_server");
	_publisher = (FundPublishMessage)DLLHelper::get_symbol(dllInst, "publish_message");
	_register = (FuncRegCallbacks)DLLHelper::get_symbol(dllInst, "regiter_callbacks");

	// 注册日志回调函数
	_register(on_mq_log);
	
	// 创建消息队列服务器
	_mq_sid = _creator(_url.c_str());

	WTSLogger::info("EventNotifier initialized with channel {}", _url.c_str());

	// 创建并启动工作线程
	if (_worker == NULL)
	{
		// 创建一个work对象保证io_service不会因为没有任务而退出
		boost::asio::io_service::work work(_asyncio);
		_worker.reset(new StdThread([this]() {
			while (!_stopped)
			{
				// 定期运行一个异步任务
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
 * @details 将日志消息异步发送到消息队列，使用JSON格式封装
 * @param tag 日志标签
 * @param message 日志消息内容
 */
void EventNotifier::notify_log(const char* tag, const char* message)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string strTag = tag;
	std::string strMsg = message;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, strTag, strMsg]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加标签、时间和消息内容
			root.AddMember("tag", rj::Value(strTag.c_str(), allocator), allocator);
			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			// 将JSON对象转换为格式化的字符串
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}

		// 发送到消息队列，主题为"LOG"
		if (_publisher)
			_publisher(_mq_sid, "LOG", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送事件通知
 * @details 将一般事件消息异步发送到消息队列，使用JSON格式封装
 * @param message 事件消息内容
 */
void EventNotifier::notify_event(const char* message)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string strMsg = message;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, strMsg]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加时间和消息内容
			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			// 将JSON对象转换为格式化的字符串
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		// 发送到消息队列，主题为"GRP_EVENT"
		if (_publisher)
			_publisher(_mq_sid, "GRP_EVENT", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送交易者消息通知
 * @details 将交易者的消息异步发送到消息队列，使用JSON格式封装
 * @param trader 交易者标识
 * @param message 消息内容
 */
void EventNotifier::notify(const char* trader, const char* message)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string strTrader = trader;
	std::string strMsg = message;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, strTrader, strMsg]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加交易者、时间和消息内容
			root.AddMember("trader", rj::Value(strTrader.c_str(), allocator), allocator);
			root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
			root.AddMember("message", rj::Value(strMsg.c_str(), allocator), allocator);

			// 将JSON对象转换为格式化的字符串
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		// 发送到消息队列，主题为"TRD_NOTIFY"
		if (_publisher)
			_publisher(_mq_sid, "TRD_NOTIFY", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送成交事件通知
 * @details 将成交信息转换为JSON并异步发送到消息队列
 * @param trader 交易者标识
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param trdInfo 成交信息对象
 */
void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo)
{
	// 检查成交信息和消息队列是否初始化
	if (trdInfo == NULL || _mq_sid == 0)
		return;

	// 复制字符串，并增加对成交信息对象的引用计数
	std::string strTrader = trader;
	std::string strCode = stdCode;
	trdInfo->retain();
	// 将任务添加到异步IO服务中
	_asyncio.post([this, strTrader, strCode, localid, trdInfo]() {
		std::string data;
		// 将成交信息转换为JSON格式
		tradeToJson(strTrader.c_str(), localid, strCode.c_str(), trdInfo, data);
		// 发送到消息队列，主题为"TRD_TRADE"
		if (_publisher)
			_publisher(_mq_sid, "TRD_TRADE", data.c_str(), (unsigned long)data.size());
		// 释放成交信息对象
		trdInfo->release();
	});
}

/**
 * @brief 发送订单事件通知
 * @details 将订单信息转换为JSON并异步发送到消息队列
 * @param trader 交易者标识
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param ordInfo 订单信息对象
 */
void EventNotifier::notify(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo)
{
	// 检查订单信息和消息队列是否初始化
	if (ordInfo == NULL || _mq_sid == 0)
		return;

	// 复制字符串，并增加对订单信息对象的引用计数
	std::string strTrader = trader;
	std::string strCode = stdCode;
	ordInfo->retain();
	// 将任务添加到异步IO服务中
	_asyncio.post([this, strTrader, strCode, localid, ordInfo]() {
		std::string data;
		// 将订单信息转换为JSON格式
		orderToJson(strTrader.c_str(), localid, strCode.c_str(), ordInfo, data);
		// 发送到消息队列，主题为"TRD_ORDER"
		if (_publisher)
			_publisher(_mq_sid, "TRD_ORDER", data.c_str(), (unsigned long)data.size());
		// 释放订单信息对象
		ordInfo->release();
	});
}

/**
 * @brief 将成交信息转换为JSON格式
 * @details 根据成交信息对象的各个字段构造JSON对象
 * @param trader 交易者标识
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param trdInfo 成交信息对象
 * @param output 输出的JSON字符串
 */
void EventNotifier::tradeToJson(const char* trader, uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo, std::string& output)
{
	// 检查成交信息是否为空
	if(trdInfo == NULL)
	{
		output = "{}";
		return;
	}

	// 解析成交方向和开平仓标记
	bool isLong = (trdInfo->getDirection() == WDT_LONG);
	bool isOpen = (trdInfo->getOffsetType() == WOT_OPEN);
	bool isToday = (trdInfo->getOffsetType() == WOT_CLOSETODAY);

	{
		// 创建JSON对象
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		// 添加交易者和基本信息
		root.AddMember("trader", rj::Value(trader, allocator), allocator);
		root.AddMember("time", (uint64_t)trdInfo->getTradeTime(), allocator);
		root.AddMember("localid", localid, allocator);
		root.AddMember("code", rj::Value(stdCode, allocator), allocator);
		root.AddMember("islong", isLong, allocator);
		root.AddMember("isopen", isOpen, allocator);
		root.AddMember("istoday", isToday, allocator);

		// 添加成交量价信息
		root.AddMember("volume", trdInfo->getVolume(), allocator);
		root.AddMember("price", trdInfo->getPrice(), allocator);

		// 将JSON对象转换为格式化的字符串
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);

		output = sb.GetString();
	}
}

/**
 * @brief 将订单信息转换为JSON格式
 * @details 根据订单信息对象的各个字段构造JSON对象
 * @param trader 交易者标识
 * @param localid 本地订单ID
 * @param stdCode 标准合约代码
 * @param ordInfo 订单信息对象
 * @param output 输出的JSON字符串
 */
void EventNotifier::orderToJson(const char* trader, uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo, std::string& output)
{
	// 检查订单信息是否为空
	if (ordInfo == NULL)
	{
		output = "{}";
		return;
	}

	// 解析订单方向和状态信息
	bool isLong = (ordInfo->getDirection() == WDT_LONG);
	bool isOpen = (ordInfo->getOffsetType() == WOT_OPEN);
	bool isToday = (ordInfo->getOffsetType() == WOT_CLOSETODAY);
	bool isCanceled = (ordInfo->getOrderState() == WOS_Canceled);

	{
		// 创建JSON对象
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		// 添加交易者和基本信息
		root.AddMember("trader", rj::Value(trader, allocator), allocator);
		root.AddMember("time", TimeUtils::getLocalTimeNow(), allocator);
		root.AddMember("localid", localid, allocator);
		root.AddMember("code", rj::Value(stdCode, allocator), allocator);
		root.AddMember("islong", isLong, allocator);
		root.AddMember("isopen", isOpen, allocator);
		root.AddMember("istoday", isToday, allocator);
		root.AddMember("canceled", isCanceled, allocator);

		// 添加订单量价和状态信息
		root.AddMember("total", ordInfo->getVolume(), allocator);
		root.AddMember("left", ordInfo->getVolLeft(), allocator);
		root.AddMember("traded", ordInfo->getVolTraded(), allocator);
		root.AddMember("price", ordInfo->getPrice(), allocator);
		root.AddMember("state", rj::Value(ordInfo->getStateMsg(), allocator), allocator);

		// 将JSON对象转换为格式化的字符串
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);

		output = sb.GetString();
	}
}

/**
 * @brief 发送图表指标通知
 * @details 将图表指标数据异步发送到消息队列，使用JSON格式封装
 * @param time 时间戳
 * @param straId 策略ID
 * @param idxName 指标名称
 * @param lineName 线条名称
 * @param val 指标值
 */
void EventNotifier::notify_chart_index(uint64_t time, const char* straId, const char* idxName, const char* lineName, double val)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string sid = straId;
	std::string iname = idxName;
	std::string lname = lineName;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, time, sid, iname, lname, val]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加策略、指标、线条、时间和值信息
			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("index_name", rj::Value(iname.c_str(), allocator), allocator);
			root.AddMember("line_name", rj::Value(lname.c_str(), allocator), allocator);
			root.AddMember("time", time, allocator);
			root.AddMember("value", val, allocator);

			// 将JSON对象转换为格式化的字符串
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		// 发送到消息队列，主题为"CHART_INDEX"
		if (_publisher)
			_publisher(_mq_sid, "CHART_INDEX", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送图表标记通知
 * @details 将图表上的标记点数据异步发送到消息队列，使用JSON格式封装
 * @param time 时间戳
 * @param straId 策略ID
 * @param price 价格
 * @param icon 图标
 * @param tag 标签
 */
void EventNotifier::notify_chart_marker(uint64_t time, const char* straId, double price, const char* icon, const char* tag)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string sid = straId;
	std::string sIcon = icon;
	std::string sTag = tag;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, time, sid, sIcon, sTag, price]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加策略、图标、标签、时间和价格信息
			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("icon", rj::Value(sIcon.c_str(), allocator), allocator);
			root.AddMember("tag", rj::Value(sTag.c_str(), allocator), allocator);
			root.AddMember("time", time, allocator);
			root.AddMember("price", price, allocator);

			// 将JSON对象转换为字符串（注意这里使用Writer而非PrettyWriter）
			rj::StringBuffer sb;
			rj::Writer<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		// 发送到消息队列，主题为"CHART_MARKER"
		if (_publisher)
			_publisher(_mq_sid, "CHART_MARKER", data.c_str(), (unsigned long)data.size());
	});
}

/**
 * @brief 发送策略交易信号通知
 * @details 将策略交易信号数据异步发送到消息队列，使用JSON格式封装
 * @param straId 策略ID
 * @param stdCode 标准合约代码
 * @param isLong 是否做多
 * @param isOpen 是否开仓
 * @param curTime 当前时间戳
 * @param price 交易价格
 * @param userTag 用户标记
 */
void EventNotifier::notify_trade(const char* straId, const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, const char* userTag)
{
	// 检查消息队列服务器是否初始化
	if (_mq_sid == 0)
		return;

	// 复制字符串，避免异步调用中的生命周期问题
	std::string sid = straId;
	std::string code = stdCode;
	std::string tag = userTag;
	// 将任务添加到异步IO服务中
	_asyncio.post([this, sid, code, tag, isLong, isOpen, curTime, price]() {
		std::string data;
		{
			// 创建JSON对象
			rj::Document root(rj::kObjectType);
			rj::Document::AllocatorType &allocator = root.GetAllocator();

			// 添加策略、合约、标签、交易方向、时间和价格信息
			root.AddMember("strategy", rj::Value(sid.c_str(), allocator), allocator);
			root.AddMember("code", rj::Value(code.c_str(), allocator), allocator);
			root.AddMember("tag", rj::Value(tag.c_str(), allocator), allocator);
			root.AddMember("long", isLong, allocator);
			root.AddMember("open", isOpen, allocator);
			root.AddMember("time", curTime, allocator);
			root.AddMember("price", price, allocator);

			// 将JSON对象转换为字符串
			rj::StringBuffer sb;
			rj::Writer<rj::StringBuffer> writer(sb);
			root.Accept(writer);

			data = sb.GetString();
		}
		// 发送到消息队列，主题为"STRA_TRADE"
		if (_publisher)
			_publisher(_mq_sid, "STRA_TRADE", data.c_str(), (unsigned long)data.size());
	});
}