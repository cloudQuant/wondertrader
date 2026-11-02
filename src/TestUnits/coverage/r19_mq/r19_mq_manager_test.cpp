/*!
 * \file r19_mq_manager_test.cpp
 * \brief lane R19：WtMsgQue 的 MQManager/MQServer/MQClient 同步路径
 *        覆盖离线 inproc 创建、失败连接清理、启动闸门和销毁；不发布非空 payload。
 *        仅成功 start/destroy 正向路径会创建一个立即 join 的本地接收线程。
 *
 * 覆盖：
 * - src/WtMsgQue/MQManager.cpp L6-18 create_server、L20-31 destroy_server、
 *   L33-44 publish_message、L46-56 log_server/log_client、L58-67 create_client、
 *   L69-80 destroy_client、L82-93 sub_topic、L95-106 start_client（未知、失败、成功分支）
 * - src/WtMsgQue/MQServer.cpp L32-48 id 生成+构造、L50-64 析构、L66-107 init（成功/绑定失败）、
 *   L109-118 publish 的 _sock<0 与空数据早退
 * - src/WtMsgQue/MQClient.cpp 构造/析构、init（成功/连接失败）、start（成功/失败闸门）
 * - src/WtMsgQue/WtMsgQue.cpp L23-65 全部 extern "C" 包装
 * 端点全部用 inproc://（进程内，不落网卡）与一个不存在的协议名（bind/connect 必失败）。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "r19_support.h"

#include "WtMsgQue/MQClient.h"
#include "WtMsgQue/MQManager.h"
#include "WtMsgQue/WtMsgQue.h"

namespace wtp {

// Narrow test seam to prove failed connect cleanup without adding a runtime API.
struct MQClientTestAccess
{
	static MQServerPtr find_server(const MQManager& manager, WtUInt32 id)
	{
		auto it = manager._servers.find(id);
		return it == manager._servers.end() ? MQServerPtr() : it->second;
	}

	static MQClientPtr find_client(const MQManager& manager, WtUInt32 id)
	{
		auto it = manager._clients.find(id);
		return it == manager._clients.end() ? MQClientPtr() : it->second;
	}

	static int socket(const MQClient& client) { return client._sock; }
	static bool ready(const MQClient& client) { return client.m_bReady; }
	static bool has_receiver_thread(const MQClient& client) { return client.m_thrdRecv != nullptr; }
};

} // namespace wtp

namespace {

std::vector<std::string> g_logs;
int g_recv_calls = 0;

void PORTER_FLAG mq_log_cb(WtUInt32 id, const char* message, bool bServer)
{
	g_logs.push_back(std::string(bServer ? "S" : "C") + std::to_string((unsigned)id) + "|" +
		(message == NULL ? std::string("<null>") : std::string(message)));
}

void PORTER_FLAG mq_msg_cb(WtUInt32 /*id*/, const char* /*topic*/, const char* /*data*/, WtUInt32 /*len*/)
{
	g_recv_calls++;
}

void reset_mq_log()
{
	regiter_callbacks(mq_log_cb);
	g_logs.clear();
}

size_t log_count() { return g_logs.size(); }

bool mq_has(const std::string& needle)
{
	for (const std::string& s : g_logs)
		if (s.find(needle) != std::string::npos)
			return true;
	return false;
}

size_t count_of(const std::string& needle)
{
	size_t n = 0;
	for (const std::string& s : g_logs)
		if (s.find(needle) != std::string::npos)
			n++;
	return n;
}

} // namespace

// 服务端：绑定成功 → 销毁 → 同名端点可以再绑（证明 nn_close 真的释放了）
TEST(r19_mq_server, bind_destroy_and_rebind_same_inproc_endpoint)
{
	reset_mq_log();
	const char* url = "inproc://r19_e1_ep";

	const WtUInt32 id1 = create_server(url, false);
	EXPECT_GT(id1, 1000u) << "makeMQSvrId 从 1001 起（MQServer.cpp L34）";
	EXPECT_TRUE(mq_has("has binded to inproc://r19_e1_ep"));
	EXPECT_TRUE(mq_has("created"));

	reset_mq_log();
	destroy_server(id1);
	EXPECT_EQ(1u, count_of("has been destroyed"));

	// 端点名没被占住 → 重新绑定成功
	const WtUInt32 id2 = create_server(url, false);
	EXPECT_GT(id2, id1) << "服务端 id 单调递增";
	EXPECT_EQ(id1 + 1u, id2) << "本用例体内没有别的 create_server";
	EXPECT_EQ(1u, count_of("has binded to")) << "同名 inproc 端点销毁后必须可以重绑";

	// confirm=true 只是把 _confirm 置起来（MQServer.cpp L71），不影响绑定
	reset_mq_log();
	const WtUInt32 id3 = create_server("inproc://r19_e1_cfm", true);
	EXPECT_GT(id3, id2);
	EXPECT_EQ(1u, count_of("has binded to inproc://r19_e1_cfm"));
	destroy_server(id3);
	destroy_server(id2);
}

// 同一 inproc endpoint 已被占用时，第二次 bind 必须失败且不登记失败对象。
TEST(r19_mq_server, duplicate_inproc_bind_returns_zero_and_does_not_register)
{
	reset_mq_log();
	MQManager manager;
	manager.regiter_callbacks(mq_log_cb);
	const std::string url = "inproc://r19_bind_conflict";

	const WtUInt32 first = manager.create_server(url.c_str(), false);
	ASSERT_GT(first, 1000u);
	ASSERT_TRUE(wtp::MQClientTestAccess::find_server(manager, first) != NULL);

	reset_mq_log();
	const WtUInt32 duplicate = manager.create_server(url.c_str(), false);
	EXPECT_EQ(0u, duplicate);
	EXPECT_TRUE(mq_has("binding url " + url));
	EXPECT_TRUE(mq_has("failed"));	// 第二个 socket 对已占用 endpoint 的真实 bind 失败
	EXPECT_TRUE(wtp::MQClientTestAccess::find_server(manager, first) != NULL);
	EXPECT_TRUE(wtp::MQClientTestAccess::find_server(manager, duplicate) == NULL);

	manager.destroy_server(first);
	const WtUInt32 rebound = manager.create_server(url.c_str(), false);
	EXPECT_GT(rebound, first);	// 首个 server 销毁后，同一 endpoint 可重新绑定
	EXPECT_TRUE(wtp::MQClientTestAccess::find_server(manager, rebound) != NULL);
	manager.destroy_server(rebound);
}

// 非法协议：失败返回 0，且不登记服务端实例
TEST(r19_mq_server, invalid_protocol_returns_zero_and_does_not_register)
{
	reset_mq_log();
	MQManager manager;
	manager.regiter_callbacks(mq_log_cb);
	const std::string url = "r19bogus://r19_ep";

	const WtUInt32 id = manager.create_server(url.c_str(), false);
	EXPECT_EQ(0u, id);
	EXPECT_TRUE(mq_has("binding url " + url)) << "MQServer.cpp L94 的失败日志";
	EXPECT_TRUE(mq_has("failed"));
	EXPECT_TRUE(wtp::MQClientTestAccess::find_server(manager, id) == NULL);

	// 0 是无效句柄；失败对象未登记，后续按 ID 调用走缺失句柄路径。
	reset_mq_log();
	manager.publish_message(id, "t", "payload", 7);
	EXPECT_EQ(1u, count_of("S0|MQServer 0 not exists"));
	EXPECT_EQ(0u, count_of("has not been initialized yet"));

	reset_mq_log();
	manager.destroy_server(id);
	EXPECT_EQ(1u, count_of("S0|MQServer 0 not exists"));
	EXPECT_EQ(0u, count_of("has been destroyed"));
}

// 空数据/NULL 数据必须在入队之前早退（否则产品侧会起广播线程）
TEST(r19_mq_server, publish_with_empty_payload_logs_nothing_and_starts_no_thread)
{
	reset_mq_log();
	const WtUInt32 server = create_server("inproc://r19_e3", false);
	ASSERT_GT(server, 1000u);

	reset_mq_log();
	publish_message(server, "topic", NULL, 0);
	publish_message(server, "topic", NULL, 8);
	publish_message(server, "topic", "", 0);
	EXPECT_EQ(0u, log_count())
		<< "MQServer.cpp L117-118：data==NULL || dataLen==0 直接 return，不记日志、不建队列、不起线程";

	// 未知 id 的一整批入口：每个都只记一条 "not exists"，且不得创建任何对象/线程
	reset_mq_log();
	destroy_server(999999u);
	destroy_client(999998u);
	subscribe_topic(999997u, "t");
	start_client(999996u);
	publish_message(999995u, "t", "x", 1);
	EXPECT_EQ(5u, count_of("not exists"));
	// 服务端/客户端两条日志通道靠 bServer 区分（MQManager.cpp L46-56）
	EXPECT_EQ(1u, count_of("S999999|MQServer 999999 not exists"));   // destroy_server
	EXPECT_EQ(1u, count_of("S999995|MQServer 999995 not exists"));   // publish_message
	EXPECT_EQ(1u, count_of("C999998|MQClient 999998 not exists"));   // destroy_client
	EXPECT_EQ(1u, count_of("C999997|MQClient 999997 not exists"));   // subscribe_topic
	EXPECT_EQ(1u, count_of("C999996|MQClient 999996 not exists"));   // start_client
	g_recv_calls = 0;
	EXPECT_EQ(0, g_recv_calls);

	destroy_server(server);
}

// 客户端：连接成功后不 start（无接收线程），订阅只改内部集合，销毁只关句柄
TEST(r19_mq_client, connect_subscribe_destroy_without_start)
{
	reset_mq_log();
	const WtUInt32 server = create_server("inproc://r19_e4", false);
	ASSERT_GT(server, 1000u);

	const WtUInt32 c1 = create_client("inproc://r19_e4", mq_msg_cb);
	EXPECT_GT(c1, 5000u) << "makeMQCientId 从 5001 起（MQClient.cpp L34）";
	EXPECT_TRUE(mq_has("has connected to inproc://r19_e4"));

	const WtUInt32 c2 = create_client("inproc://r19_e4", mq_msg_cb);
	EXPECT_EQ(c1 + 1u, c2) << "客户端 id 在同一调用序列里逐个加一";

	// sub_topic（MQManager.cpp L82-93 + MQClient.h L90-92 的 inline 插入）没有日志
	reset_mq_log();
	subscribe_topic(c1, "r19_topic");
	subscribe_topic(c1, "r19_topic2");
	EXPECT_EQ(0u, log_count()) << "订阅动作本身不该产生日志";

	// 没 start_client → 不会有接收线程 → 消息回调一次都不该被调用
	EXPECT_EQ(0, g_recv_calls);

	reset_mq_log();
	destroy_client(c1);
	destroy_client(c2);
	EXPECT_EQ(2u, count_of("has been destroyed"));

	destroy_server(server);
}

TEST(r19_mq_client, invalid_protocol_create_start_destroy_is_safe_and_logger_can_unregister)
{
	reset_mq_log();
	MQManager manager;
	manager.regiter_callbacks(mq_log_cb);
	const std::string url = "r19bogus://r19_direct";

	const WtUInt32 c = manager.create_client(url.c_str(), mq_msg_cb);
	EXPECT_EQ(0u, c);
	EXPECT_TRUE(mq_has("connecting url " + url));
	EXPECT_TRUE(mq_has("failed"));
	EXPECT_TRUE(wtp::MQClientTestAccess::find_client(manager, c) == NULL)
		<< "connect 失败后对象不登记，socket 已由 init 失败分支关闭";

	g_recv_calls = 0;
	reset_mq_log();
	manager.start_client(c);
	EXPECT_FALSE(mq_has("has started successfully"));
	EXPECT_TRUE(mq_has("C0|MQClient 0 not exists"));
	EXPECT_FALSE(mq_has("has not been initialized yet"));
	EXPECT_EQ(0, g_recv_calls);

	reset_mq_log();
	manager.destroy_client(c);
	EXPECT_TRUE(mq_has("C0|MQClient 0 not exists"));
	EXPECT_FALSE(mq_has("has been destroyed"));

	reset_mq_log();
	// 注销日志回调之后，MQManager 的 log_server/log_client（L48/L54）必须静默
	regiter_callbacks(NULL);
	start_client(889996u);
	publish_message(889995u, "t", "x", 1);
	destroy_server(889994u);
	destroy_client(889993u);
	subscribe_topic(889992u, "t");
	EXPECT_EQ(0u, log_count()) << "_cb_log 为空时一个字符都不该吐";

	// 重新挂上回调 → 同样的调用又开始记日志
	reset_mq_log();
	destroy_server(889991u);
	EXPECT_EQ(1u, count_of("not exists"));
}

TEST(r19_mq_client, start_and_destroy_successful_inproc_client)
{
	reset_mq_log();
	const WtUInt32 server = create_server("inproc://r19_started", false);
	ASSERT_GT(server, 1000u);
	const WtUInt32 c = create_client("inproc://r19_started", mq_msg_cb);
	ASSERT_GT(c, 5000u);

	reset_mq_log();
	start_client(c);
	EXPECT_TRUE(mq_has("has started successfully"));
	EXPECT_FALSE(mq_has("has not been initialized yet"));

	reset_mq_log();
	destroy_client(c);
	EXPECT_TRUE(mq_has("has been destroyed"));
	destroy_server(server);
}
