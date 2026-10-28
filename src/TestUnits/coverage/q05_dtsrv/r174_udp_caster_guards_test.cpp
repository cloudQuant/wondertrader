/*!
 * \file r174_udp_caster_guards_test.cpp
 * \brief UDPCaster 在「不建真实 socket」前提下的剩余公开守卫
 *        （迭代01 第二十五波 R174）
 *
 * 最新 profdata 中 UDPCaster.cpp 剩余 65 行全部落在真实 socket 路径
 * （start 的 bind/catch、do_receive 回调、订阅回包、组播发送、dead code），
 * 与本 lane「不启动真实 socket」的硬约束冲突。本文件只锁住无需 socket 的公开面：
 *   UDPCaster.cpp:67-68   active=false 直接返回，broadcast/multicast 数组不解析
 *   UDPCaster.cpp:222-241 addBRecver 只解析地址，坏地址 catch 返回 false
 *   UDPCaster.cpp:244-263 addMRecver 在地址解析失败时先于 socket 构造返回 false
 *   UDPCaster.cpp:288-291 m_sktBroadcast==NULL / m_bTerminated 的广播守卫
 *   UDPCaster.cpp:133-143 stop() 在未 start 时只置位并唤醒条件变量
 * 对象全程没有 socket/线程，析构不会触发 BUG-R164-001。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <WtDtCore/UDPCaster.h>
#include <Includes/WTSDataDef.hpp>

#include "r174_test_util.h"

namespace {

class R174UdpCasterGuardTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r174::make_tmp_dir("udp_guard"); }

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::filesystem::path _dir;
};

/*
 * UDPCaster.cpp:67-68、222-241、244-263、288-291、133-143
 * active=false 时 init 在任何 socket 之前返回；接收方注册只做 address_v4 解析；
 * 未 start 的 m_sktBroadcast 为 NULL，四类 broadcast 都在入队前返回。
 * stop() 在无 io/cast 线程时安全；终止标记让 stop 后的 broadcast 也直接返回。
 */
TEST_F(R174UdpCasterGuardTest, InactiveInitAndPreStartBroadcastStayOffline)
{
	WTSVariant* cfg = r174::load_cfg(_dir, "inactive.json", R"JSON({
		"active": false,
		"sport": 1,
		"broadcast": [{"host": "127.0.0.1", "port": 9000, "type": 0}],
		"multicast": [{"host": "239.255.0.99", "port": 9001, "sendport": 0, "type": 2}]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	UDPCaster caster;
	EXPECT_FALSE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 9100, 0));
	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 9101, 1));
	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 9102, 2));
	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 9103, 3));
	EXPECT_FALSE(caster.addBRecver("not-an-address", 9104, 0));
	// 地址解析先失败，不会走到 new UDPSocket；不产生 socket。
	EXPECT_FALSE(caster.addMRecver("not-an-address", 9105, 0, 0));

	WTSTickData* tick = WTSTickData::create("600000");
	WTSOrdQueData* que = WTSOrdQueData::create("600000");
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("600000");
	WTSTransData* trans = WTSTransData::create("600000");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(que, nullptr);
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster.broadcast(tick);
	caster.broadcast(que);
	caster.broadcast(dtl);
	caster.broadcast(trans);
	caster.broadcast(static_cast<WTSTickData*>(nullptr));
	EXPECT_EQ(tick->retainCount(), 1u);
	EXPECT_EQ(que->retainCount(), 1u);
	EXPECT_EQ(dtl->retainCount(), 1u);
	EXPECT_EQ(trans->retainCount(), 1u);

	caster.stop();
	// m_bTerminated 之后的广播同样在入队前返回，引用计数不变。
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);

	tick->release();
	que->release();
	dtl->release();
	trans->release();
}

}	// namespace
