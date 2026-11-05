/*!
 * \file EventNotifier.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 事件通知器对象定义，用于回测过程中的事件和数据广播
 */
#pragma once

#include <queue>
#include <condition_variable>
#include <mutex>

#include "../Includes/WTSMarcos.h"
#include "../Includes/WTSObject.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/DLLHelper.hpp"

/**
 * @brief 创建消息队列服务器的函数类型
 * @param 服务器URL地址
 * @param 是否为服务器模式
 * @return 服务器ID
 */
typedef WtUInt32(PORTER_FLAG *FuncCreateMQServer)(const char*, bool);

/**
 * @brief 销毁消息队列服务器的函数类型
 * @param 服务器ID
 */
typedef void(PORTER_FLAG *FuncDestroyMQServer)(WtUInt32);

/**
 * @brief 发布消息的函数类型
 * @param 服务器ID
 * @param 主题
 * @param 消息内容
 * @param 消息长度
 */
typedef void(PORTER_FLAG *FundPublishMessage)(WtUInt32, const char*, const char*, WtUInt32);

/**
 * @brief 日志回调函数类型
 * @param 服务器ID
 * @param 日志消息
 * @param 是否为服务器
 */
typedef void(PORTER_FLAG *FuncLogCallback)(WtUInt32, const char*, bool);

/**
 * @brief 注册回调函数的函数类型
 * @param 日志回调函数
 */
typedef void(PORTER_FLAG *FuncRegCallbacks)(FuncLogCallback);


NS_WTP_BEGIN
class WTSVariant;

/**
 * @brief 事件通知器类，用于回测过程中的事件、数据和资金信息广播
 * @details 该类封装了消息队列服务器的创建、销毁和消息发布功能，便于其他模块获取回测过程中的信息
 */
class EventNotifier
{
public:
	/**
	 * @brief 构造函数
	 */
	EventNotifier();

	/**
	 * @brief 析构函数
	 */
	~EventNotifier();

public:
	/**
	 * @brief 初始化事件通知器
	 * @param cfg 配置项，包含消息队列服务器的URL等信息
	 * @return 是否初始化成功
	 */
	bool	init(WTSVariant* cfg);

	/**
	 * @brief 通知事件
	 * @param evtType 事件类型
	 */
	void	notifyEvent(const char* evtType);

	/**
	 * @brief 通知数据
	 * @param topic 主题
	 * @param data 数据内容
	 * @param dataLen 数据长度
	 */
	void	notifyData(const char* topic, void* data , uint32_t dataLen);

	/**
	 * @brief 通知资金信息
	 * @param topic 主题
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param total_profit 总盈亏
	 * @param dynprofit 动态盈亏
	 * @param dynbalance 动态余额
	 * @param total_fee 总手续费
	 */
	void	notifyFund(const char* topic, uint32_t uDate, double total_profit, double dynprofit, double dynbalance, double total_fee);

	/**
	 * @brief 释放事件通知器（幂等，R20_wtbtcore#3 / 卡9）
	 * @details 先关闭发布准入并等待在途发布全部返回，再恰好一次销毁非零 server id，
	 *          最后清空函数指针与 id。之后的 notify* 为空操作、init 被拒绝（9c=A）。
	 *          WtMsgQue 未提供版本化回调静默能力，模块无法证明已无内部线程/回调，
	 *          按 9a=A 保持常驻到进程退出，不调用 free_library。
	 *          调用方须在回放线程停止并 join 之后调用（WtBtRunner::finalizeRelease）；
	 *          唯一宿主回调 on_mq_log 为空操作，不存在发布内同步重入 release 的路径。
	 *          析构复用本函数。
	 */
	void	release();

private:
	/**
	 * @brief 在途发布：持有准入期间调用插件 publish，返回后递减计数
	 */
	void	publish(const char* topic, const char* data, WtUInt32 dataLen);

private:
	std::string		m_strURL;        ///< 消息队列服务器的URL地址
	uint32_t		_mq_sid;         ///< 消息队列服务器ID
	FuncCreateMQServer	_creator;    ///< 创建消息队列服务器的函数指针
	FuncDestroyMQServer	_remover;    ///< 销毁消息队列服务器的函数指针
	FundPublishMessage	_publisher;  ///< 发布消息的函数指针
	FuncRegCallbacks	_register;   ///< 注册回调函数的函数指针

	std::mutex		_lifecycle_mtx;	///< 串行化 init/release
	std::mutex		_state_mtx;		///< 保护函数指针、server id、准入与在途计数
	std::condition_variable	_drained;	///< 在途发布归零通知
	uint32_t		_inflight;		///< 在途发布数
	bool			_released;		///< 已释放：拒绝新的发布与 init
	DllHandle		_module;		///< 已加载的 MQ 模块（9a=A：常驻到进程退出）
};

NS_WTP_END
