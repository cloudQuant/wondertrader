/*!
 * \file PorterDefs.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 消息队列模块的定义文件
 * \details 定义了消息队列模块中使用的回调函数类型和其他相关定义
 */
#pragma once
#include "../Includes/WTSMarcos.h"

/**
 * @brief 消息队列数据接收回调函数类型
 * @details 客户端接收到消息时的回调函数类型，用于处理收到的消息
 * @param id 客户端ID
 * @param topic 消息主题
 * @param data 消息数据指针
 * @param dataLen 消息数据长度
 */
typedef void(PORTER_FLAG *FuncMQCallback)(WtUInt32 id, const char* topic, const char* data, WtUInt32 dataLen);

/**
 * @brief 日志回调函数类型
 * @details 用于记录服务端和客户端的日志信息
 * @param id 服务端或客户端ID
 * @param message 日志消息内容
 * @param bServer 是否为服务端日志，true表示服务端，false表示客户端
 */
typedef void(PORTER_FLAG *FuncLogCallback)(WtUInt32 id, const char* message, bool bServer);