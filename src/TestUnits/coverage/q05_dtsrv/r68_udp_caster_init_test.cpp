/*!
 * \file r68_udp_caster_init_test.cpp
 * \brief WtDtCore/UDPCaster::init 的配置形态与拒绝分支补测（迭代01 第十三波 R68）
 *
 * 与既有 r5_udp_caster_test.cpp 的分工（只读复用，不修改）：
 *  - R5 已覆盖 active=false / 缺省 active、addBRecver/addMRecver 的地址解析与
 *    type 分桶、四个 broadcast 的空指针与未 start 守卫、stop 的未 start 守卫；
 *  - 本文件只补 init 的另外三种布尔写法（字符串 "false"、数字 0）以及
 *    「拒绝时不会去解析畸形 broadcast/multicast 项」的时序，并新增
 *    cfg==NULL 的阻塞性用例（BUG-R68-001）。
 *
 * 不创建 socket、不 bind、不起 io/广播线程；active=false 的 init 在 L67-68 就返回。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/UDPCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

#include "r68_test_util.h"

namespace fs = std::filesystem;

namespace {

class R68UdpCasterInitTest : public ::testing::Test
{
protected:
	void SetUp() override { _dir = r68::make_tmp_dir("udp"); }

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		r68::write_file(p, body);
		return WTSCfgLoader::load_from_file(p.string().c_str());
	}

	fs::path _dir;
};

/*
 * 触发条件（禁用原因）：UDPCaster::init（UDPCaster.cpp:62-68）第一句可用代码就是
 * `cfg->getBoolean("active")`，对 cfg 没有判空。dt_srv 的 caster 配置段缺失时若仍调用
 * init(NULL, ...)，直接空指针解引用 -> SIGSEGV。
 * 缺陷登记：docs/迭代计划/迭代01/bugs/R68.md BUG-R68-001。
 */
TEST_F(R68UdpCasterInitTest, DISABLED_InitCrashesWhenConfigIsNull)
{
	UDPCaster caster;
	caster.init(nullptr, nullptr, nullptr);	// 崩在 UDPCaster.cpp:67
}

// UDPCaster.cpp:67-68 —— active 写成字符串 "false" 时 asBoolean 返回 false，
// 在解析 broadcast/multicast 之前就返回，不会误建接收方、也不会 start() 真 socket。
TEST_F(R68UdpCasterInitTest, InitRejectsStringFalseActive)
{
	WTSVariant* cfg = loadCfg("strfalse.json", R"JSON({
		"active": "false",
		"sport": 39999,
		"broadcast": [{"host": "239.255.0.1", "port": 8888, "type": 0}]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	UDPCaster caster;
	EXPECT_FALSE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	// 拒绝后广播仍由 m_sktBroadcast==NULL 守卫拦下
	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

// UDPCaster.cpp:67-68 + 70-88 —— active 写成数字 0 同样返回 false；
// 即使 broadcast/multicast 数组里混入非对象项，也不会进入 L73-88 的解析循环。
TEST_F(R68UdpCasterInitTest, InitRejectsNumericZeroActiveBeforeParsingSections)
{
	WTSVariant* cfg = loadCfg("zero.json", R"JSON({
		"active": 0,
		"broadcast": [
			1,
			"not-an-object",
			{"host": "", "port": 0, "type": 99}
		],
		"multicast": [
			true,
			{"host": "bad.host.name", "port": -1, "sendport": 70000, "type": 3}
		]
	})JSON");
	ASSERT_NE(cfg, nullptr);

	UDPCaster caster;
	EXPECT_FALSE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	// 四个重载在未 start 的实例上都是 no-op
	WTSOrdQueData* oq = WTSOrdQueData::create("600000");
	WTSOrdDtlData* od = WTSOrdDtlData::create("600000");
	WTSTransData* tr = WTSTransData::create("600000");
	ASSERT_NE(oq, nullptr);
	ASSERT_NE(od, nullptr);
	ASSERT_NE(tr, nullptr);
	caster.broadcast(oq);
	caster.broadcast(od);
	caster.broadcast(tr);
	EXPECT_TRUE(oq->isSingleRefs());
	EXPECT_TRUE(od->isSingleRefs());
	EXPECT_TRUE(tr->isSingleRefs());
	oq->release();
	od->release();
	tr->release();
}

// UDPCaster.cpp:101-108 的反面：init 被拒后没有广播 socket，但接收方地址表仍可
// 通过 addBRecver 登记；随后 stop() 在未 start 时安全。列表只增不减（R5 已登记）。
TEST_F(R68UdpCasterInitTest, InactiveInitKeepsReceiverRegistrationUsable)
{
	WTSVariant* cfg = loadCfg("inactive.json", R"JSON({
		"active": false,
		"bport": 39998
	})JSON");
	ASSERT_NE(cfg, nullptr);

	UDPCaster caster;
	EXPECT_FALSE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	EXPECT_TRUE(caster.addBRecver("239.255.7.7", 9009, 0));
	EXPECT_TRUE(caster.addBRecver("239.255.7.8", 9010, 1));
	EXPECT_TRUE(caster.addBRecver("239.255.7.9", 9011, 2));

	caster.stop();
	caster.stop();

	WTSTickData* tick = WTSTickData::create("600000");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

}	// namespace
