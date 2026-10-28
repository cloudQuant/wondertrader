/*!
 * \file r164_udp_caster_active_test.cpp
 * \brief UDPCaster 的 active 装配、回环广播与接收器注册（迭代01 第二十四波 R164）
 *
 * 与既有用例的分工（只读，不修改）：
 *  - r5/r68 覆盖 active=false、坏地址、接收方分桶与未 start 的 broadcast 守卫；
 *  - r154 的三个 active 用例在 ~UDPCaster 崩溃（BUG-R164-001），实际只落到构造/守卫；
 *  - 本文件补 UDPCaster.cpp:62-143（init/start/stop）、145-220（do_receive）、
 *    244-266（addMRecver）、268-400（四个 broadcast 与 do_broadcast 组包/发送）。
 *
 * 网络约束：接收方与客户端都只走 127.0.0.1；配置里的组播项只做 join_group，
 * 本文件的 broadcast 用例不配置 multicast，do_broadcast 不会向组播列表发送。
 * 产品对象的析构会崩（socket 晚于 io_context 销毁），因此用 LeakyCaster 只 stop 不 delete；
 * 每个用例由 case_runner 独立进程执行，进程退出即回收。
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <Includes/WTSDataDef.hpp>

#include "r164_test_util.h"

namespace {

class R164UdpCasterActiveTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r164::make_tmp_dir("udp_active"); }

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
};

/*
 * UDPCaster.cpp:62-99、101-131、133-138、268-290
 * active=true 且没有任何有效 broadcast 接收方：
 *  - broadcast 数组里只有解析失败的地址，addBRecver 走 catch 返回 false，三个列表为空；
 *  - multicast 数组同时给非法地址和三个合法组地址，驱动 addMRecver 的地址解析/分桶
 *    （join_group 成功与否由本机网卡决定，不向组播地址发送任何数据）；
 *  - start() 只建订阅 socket 与 io 线程，m_sktBroadcast 保持 NULL；
 *  - 四个 broadcast 在入队前被 m_sktBroadcast==NULL 守卫拦下，引用计数不增加；
 *  - stop() 停 io_context 并 join m_thrdIO（L138），m_thrdCast 为空不 join。
 */
TEST_F(R164UdpCasterActiveTest, InitActiveWithoutReceiversStartsSubscribeThenStops)
{
	WTSVariant* cfg = r164::load_cfg(_dir, "no_receiver.json", R"JSON({
		"active": true,
		"sport": 0,
		"broadcast": [
			{"host": "bad.host.name", "port": 9000, "type": 2}
		],
		"multicast": [
			{"host": "bad.host.name", "port": 9001, "sendport": 0, "type": 0},
			{"host": "239.255.0.10", "port": 9010, "sendport": 0, "type": 0},
			{"host": "239.255.0.11", "port": 9011, "sendport": 0, "type": 1},
			{"host": "239.255.0.12", "port": 9012, "sendport": 0, "type": 2}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	r164::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, nullptr, nullptr));
	cfg->release();
	caster.arm();

	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster->broadcast(tick);
	caster->broadcast(que);
	caster->broadcast(dtl);
	caster->broadcast(trans);
	caster->broadcast(static_cast<WTSTickData*>(nullptr));

	EXPECT_EQ(tick->retainCount(), 1u);
	EXPECT_EQ(que->retainCount(), 1u);
	EXPECT_EQ(dtl->retainCount(), 1u);
	EXPECT_EQ(trans->retainCount(), 1u);

	caster.stop_once();

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

/*
 * UDPCaster.cpp:93-96、101-131、145-154、203-216
 * 配置只给 bport 不给 sport：init 把 sport 回落到 bport（L93-96），start() 用该端口
 * bind 0.0.0.0 的订阅 socket。向该端口发 4 字节短包，do_receive 走
 * bytes_recvd != sizeof(UDPReqPacket) 分支（L203-216）并在回环上收到错误文本，
 * 证明 fallback 端口确实被绑定且接收回调在运行。
 */
TEST_F(R164UdpCasterActiveTest, BportFallbackBindsSubscribePort)
{
	const int port = r164::pick_free_udp_port();
	ASSERT_NE(port, 0);

	WTSVariant* cfg = r164::load_cfg(_dir, "bport_fallback.json",
		std::string(R"JSON({"active": true, "bport": )JSON")
		+ std::to_string(port) + R"JSON(})JSON");
	ASSERT_NE(cfg, nullptr);

	r164::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, nullptr, nullptr));
	cfg->release();
	caster.arm();

	const int clientFd = r164::bind_udp_loopback(nullptr);
	ASSERT_GE(clientFd, 0);

	const uint32_t junk = 0x12345678u;
	ASSERT_TRUE(r164::send_datagram(clientFd, static_cast<uint16_t>(port),
		&junk, sizeof(junk)));

	std::vector<char> reply;
	ASSERT_GT(r164::recv_datagram(clientFd, reply, 3000), 0);
	EXPECT_EQ(std::string(reply.begin(), reply.end()), r164::cannot_identify_text());

	caster.stop_once();
	::close(clientFd);
}

/*
 * UDPCaster.cpp:101-108、268-400、UDPCaster.h:107-134
 * active=true + raw 单播接收方（127.0.0.1 临时端口）：
 *  - start() 因 m_listRawRecver 非空创建广播 socket 并 set_option(broadcast)；
 *  - 四个虚重载分别把 0x200/0x201/0x202/0x203 入队，CastData 在入队/出队时
 *    retain/copy/release（头文件 107-134）；
 *  - m_thrdCast 按 dataType 组包，逐个 send_to 到 raw 接收方；
 *  - broadcast(NULL) 在 socket 非空时仍被 data==NULL 守卫拦下（L290）；
 *  - stop() 先置位再 notify，join m_thrdCast（L142）；终止后新广播被
 *    m_bTerminated 守卫拦下。
 * 不配置 multicast，m_listRawGroup 为空，L379-388 的组播发送不会执行。
 */
TEST_F(R164UdpCasterActiveTest, ActiveWithRawReceiverBroadcastsAllFourTypes)
{
	uint16_t recvPort = 0;
	const int recvFd = r164::bind_udp_loopback(&recvPort);
	ASSERT_GE(recvFd, 0);

	const std::string recv = std::to_string(recvPort);
	WTSVariant* cfg = r164::load_cfg(_dir, "raw_receiver.json",
		std::string(R"JSON({
		"active": true,
		"sport": 0,
		"broadcast": [
			{"host": "127.0.0.1", "port": )JSON") + recv + R"JSON(, "type": 2},
			{"host": "127.0.0.1", "port": )JSON" + recv + R"JSON(, "type": 1},
			{"host": "127.0.0.1", "port": )JSON" + recv + R"JSON(, "type": 0},
			{"host": "not-an-ip", "port": )JSON" + recv + R"JSON(, "type": 2}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	r164::LeakyCaster caster;
	ASSERT_TRUE(caster->init(cfg, nullptr, nullptr));
	cfg->release();
	caster.arm();

	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster->broadcast(tick);
	caster->broadcast(que);
	caster->broadcast(dtl);
	caster->broadcast(trans);
	caster->broadcast(static_cast<WTSTickData*>(nullptr));

	ASSERT_TRUE(r164::wait_until([&]() {
		return tick->retainCount() == 1u && que->retainCount() == 1u
			&& dtl->retainCount() == 1u && trans->retainCount() == 1u;
	}, 3000)) << "broadcast 队列未被消费完";

	const std::size_t expectLen[4] = {
		4u + sizeof(WTSTickStruct),
		4u + sizeof(WTSOrdQueStruct),
		4u + sizeof(WTSOrdDtlStruct),
		4u + sizeof(WTSTransStruct)
	};
	const uint32_t expectType[4] = { 0x200u, 0x201u, 0x202u, 0x203u };
	for (int i = 0; i < 4; i++)
	{
		std::vector<char> pkt;
		const int n = r164::recv_datagram(recvFd, pkt, 3000);
		ASSERT_EQ(n, static_cast<int>(expectLen[i])) << "第 " << i << " 个数据报长度不符";
		uint32_t type = 0;
		memcpy(&type, pkt.data(), sizeof(type));
		EXPECT_EQ(type, expectType[i]);
	}

	// 等 m_thrdCast 回到 cond.wait，再入队一条并立即 stop：队列非空时线程不会
	// 卡在 wait 上，stop 的 terminated 置位与 notify 都不会丢。
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	WTSTickData* tail = WTSTickData::create("600001");
	ASSERT_NE(tail, nullptr);
	caster->broadcast(tail);
	caster.stop_once();

	WTSTickData* afterStop = WTSTickData::create("600002");
	ASSERT_NE(afterStop, nullptr);
	caster->broadcast(afterStop);
	caster->broadcast(static_cast<WTSTickData*>(nullptr));
	EXPECT_EQ(afterStop->retainCount(), 1u);

	::close(recvFd);

	tick->release();
	que->release();
	dtl->release();
	trans->release();
	afterStop->release();
	// tail 可能仍在泄漏的 m_dataQue 里，引用计数归 1 与否取决于线程退出时机，
	// 这里只释放调用方持有的那一份，不做断言。
	tail->release();
}

/*
 * UDPCaster.cpp:222-266
 * 注册表的增删面：产品只提供 addBRecver/addMRecver，没有删除接口。
 *  - addBRecver 对坏地址返回 false；合法地址按 type=0/1/2 入桶并返回 true；
 *    未知 type=7 也返回 true 但不入桶（分桶 if 链没有 else）。
 *  - addMRecver 对坏地址在 make_address_v4 处抛异常并被 catch 转 false；
 *    合法组地址会 new UDPSocket 并 join_group，join 成功与否依赖本机网卡，
 *    用例只断言“不抛异常”，不把环境差异写成失败。
 * 不调用 init/start，也没有任何报文发送。
 */
TEST_F(R164UdpCasterActiveTest, RegistrationBucketsAndRejectInvalid)
{
	r164::LeakyCaster caster;

	EXPECT_FALSE(caster->addBRecver("not-an-ip", 9000, 0));
	EXPECT_TRUE(caster->addBRecver("127.0.0.1", 9000, 0));
	EXPECT_TRUE(caster->addBRecver("127.0.0.1", 9000, 1));
	EXPECT_TRUE(caster->addBRecver("127.0.0.1", 9000, 2));
	EXPECT_TRUE(caster->addBRecver("127.0.0.1", 9000, 7));

	EXPECT_FALSE(caster->addMRecver("not-an-ip", 9001, 0, 0));
	EXPECT_NO_THROW(caster->addMRecver("239.255.0.20", 9002, 0, 0));
	EXPECT_NO_THROW(caster->addMRecver("239.255.0.21", 9003, 0, 1));
	EXPECT_NO_THROW(caster->addMRecver("239.255.0.22", 9004, 0, 2));
	EXPECT_NO_THROW(caster->addMRecver("239.255.0.23", 9005, 0, 7));

	// 没有 broadcast socket，四个重载都在 L290 的首个条件处返回。
	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	caster->broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

/*
 * 触发条件（禁用原因）：UDPCaster 的成员声明顺序（UDPCaster.h:88-96）使
 * m_ioservice 先于三个 socket 容器析构；active init 创建 socket 后对象析构，
 * socket 析构访问已销毁的 reactor，SIGSEGV。任何“起过 socket 的 UDPCaster”
 * 只要走到析构就会崩，与是否 stop 无关。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R164.md BUG-R164-001。
 */
TEST_F(R164UdpCasterActiveTest, DISABLED_ActiveStartThenDestructorCrashes)
{
	const int port = r164::pick_free_udp_port();
	ASSERT_NE(port, 0);

	WTSVariant* cfg = r164::load_cfg(_dir, "destruct_crash.json",
		std::string(R"JSON({"active": true, "sport": )JSON")
		+ std::to_string(port) + R"JSON(})JSON");
	ASSERT_NE(cfg, nullptr);

	{
		UDPCaster caster;
		ASSERT_TRUE(caster.init(cfg, nullptr, nullptr));
		cfg->release();
		caster.stop();
	}	// ~UDPCaster: m_ioservice 已析构，m_sktSubscribe 析构时 SIGSEGV
}

}	// namespace
