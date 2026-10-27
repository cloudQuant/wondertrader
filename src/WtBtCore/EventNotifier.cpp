/*!
 * \file EventNotifier.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 事件通知器实现，用于回测过程中的事件和数据广播
 */
#include "EventNotifier.h"
#include "WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/DLLHelper.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

USING_NS_WTP;

/**
 * @brief 消息队列日志回调函数
 * @param id 消息队列服务器ID
 * @param message 日志信息
 * @param bServer 是否为服务器模式
 * @details 该函数用于处理消息队列服务器的日志信息，当前实现为空
 */
void on_mq_log(unsigned long id, const char* message, bool bServer)
{
	// 当前实现为空，可以根据需要添加日志处理逻辑
}

/**
 * @brief 事件通知器构造函数
 * @details 初始化成员变量，将消息队列服务器ID初始化为0
 */
EventNotifier::EventNotifier()
	: _mq_sid(0)  // 初始化消息队列服务器ID为0
{
	// 构造函数中无其他初始化操作，主要在init方法中完成
}


/**
 * @brief 事件通知器析构函数
 * @details 如果有消息队列服务器ID并且有移除器函数，则调用移除器函数销毁服务器
 */
EventNotifier::~EventNotifier()
{
	// 如果移除器函数存在且服务器ID有效，则销毁服务器
	if (_remover && _mq_sid != 0)
		_remover(_mq_sid);
}

/**
 * @brief 初始化事件通知器
 * @param cfg 配置项，包含消息队列服务器相关设置
 * @return 是否初始化成功
 * @details 该方法完成事件通知器的初始化，包括加载消息队列模块动态库，
 *          获取必要的函数指针，注册回调函数，并创建消息队列服务器
 */
bool EventNotifier::init(WTSVariant* cfg)
{
	// 检查是否启用了事件通知器
	if (!cfg->getBoolean("active"))
		return false;

	// 获取服务器URL
	m_strURL = cfg->getCString("url");
	// 构造消息队列模块的文件名，根据不同系统进行包装
	std::string module = DLLHelper::wrap_module("WtMsgQue", "lib");
	// 先在当前工作目录下查找模块
	std::string dllpath = WtHelper::getCWD() + module;
	// 如果没有，则在安装目录下查找
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getInstDir() + module;

	// 加载消息队列模块动态库
	DllHandle dllInst = DLLHelper::load_library(dllpath.c_str());
	if (dllInst == NULL)
	{
		// 加载失败，记录错误日志
		WTSLogger::error("MQ module %{} loading failed", dllpath.c_str());
		return false;
	}

	// 获取创建服务器的函数指针
	_creator = (FuncCreateMQServer)DLLHelper::get_symbol(dllInst, "create_server");
	if (_creator == NULL)
	{
		// 获取函数指针失败，释放动态库并记录错误日志
		DLLHelper::free_library(dllInst);
		WTSLogger::error("MQ module {} is not compatible", dllpath.c_str());
		return false;
	}

	// 获取其他必要的函数指针
	_remover = (FuncDestroyMQServer)DLLHelper::get_symbol(dllInst, "destroy_server");
	_publisher = (FundPublishMessage)DLLHelper::get_symbol(dllInst, "publish_message");
	_register = (FuncRegCallbacks)DLLHelper::get_symbol(dllInst, "regiter_callbacks");

	// 注册日志回调函数
	_register(on_mq_log);
	
	// 创建消息队列服务器，true表示服务器模式
	_mq_sid = _creator(m_strURL.c_str(), true);

	// 记录初始化成功的日志
	WTSLogger::info("EventNotifier initialized with channel {}", m_strURL.c_str());

	return true;
}

/**
 * @brief 通知回测系统事件
 * @param evtType 事件类型
 * @details 将指定类型的事件通过消息队列广播出去，主题固定为"BT_EVENT"
 *          如果消息发布函数不存在，则不进行操作
 */
void EventNotifier::notifyEvent(const char* evtType)
{
	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, "BT_EVENT", evtType, (unsigned long)strlen(evtType));
}

/**
 * @brief 通知自定义数据
 * @param topic 消息主题
 * @param data 数据内容指针
 * @param dataLen 数据长度
 * @details 将指定的数据内容通过消息队列以指定的主题广播出去
 *          如果消息发布函数不存在，则不进行操作
 */
void EventNotifier::notifyData(const char* topic, void* data , uint32_t dataLen)
{
	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, topic, (const char*)data, dataLen);
}

/**
 * @brief 通知资金信息
 * @param topic 消息主题
 * @param uDate 日期，格式为YYYYMMDD
 * @param total_profit 总盈亏
 * @param dynprofit 动态盈亏
 * @param dynbalance 动态余额
 * @param total_fee 总手续费
 * @details 将指定的资金信息通过消息队列广播出去
 *          资金信息会被格式化为JSON字符串，然后作为消息内容发送
 */
void EventNotifier::notifyFund(const char* topic, uint32_t uDate, double total_profit, double dynprofit, double dynbalance, double total_fee)
{
	// 创建输出字符串
	std::string output;
	{
		// 创建 JSON 对象
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		// 添加各个资金信息字段
		root.AddMember("date", uDate, allocator);
		root.AddMember("total_profit", total_profit, allocator);
		root.AddMember("dynprofit", dynprofit, allocator);
		root.AddMember("dynbalance", dynbalance, allocator);
		root.AddMember("total_fee", total_fee, allocator);

		// 将 JSON 对象转换为格式化的字符串
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);

		// 获取最终的 JSON 字符串
		output = sb.GetString();
	}

	// 如果发布者函数存在，则发布消息
	if (_publisher)
		_publisher(_mq_sid, topic, (const char*)output.c_str(), (unsigned long)output.size());
}
