/*!
 * \file r5_udp_caster_test.cpp
 * \brief WtDtCore/UDPCaster 覆盖用例（迭代01 第七波 R5）
 *
 * 覆盖目标（全部为纯内存/纯逻辑分支，不建 socket、不 bind、不起线程、不 sleep）：
 *  - UDPCaster 构造/析构（L49-60）
 *  - init() 的 active 开关分支（L62-68）
 *  - addBRecver() 的地址解析与 type 分桶 + catch 分支（L222-241）
 *  - addMRecver() 的非法地址（socket 创建之前抛出）分支（L244-249、L260-265）
 *  - 4 个 broadcast() 重载 -> do_broadcast() 的 m_sktBroadcast==NULL 前置守卫（L268-291）
 *  - stop() 在未 start 时的空线程守卫（L133-143）
 *
 * 说明：do_receive()/start()/do_broadcast() 的组包广播循环、handle_send_broad/
 * handle_send_multi 都需要真实 socket 或后台线程（且后两者是死代码），本轮留白，
 * 见 docs/迭代计划/迭代01/bugs/R5_wtdtcore.md。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/UDPCaster.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

namespace fs = std::filesystem;

namespace {

TEST(R5UDPCaster, CtorYieldsSelfConsistentEmptyState)
{
	// 构造只置三个标量（L49-55），没有任何 socket/线程句柄
	UDPCaster caster;
	// 未 start 时 4 张接收方列表都为空，因此 do_broadcast 必须走 L290 的守卫直接返回
	WTSTickData* tick = WTSTickData::create("rb2001");
	ASSERT_NE(tick, nullptr);
	EXPECT_EQ(tick->retainCount(), 1u);
	caster.broadcast(tick);
	// 若守卫失效进了 m_dataQue，CastData 拷贝会 retain 成 2
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

TEST(R5UDPCaster, AllFourBroadcastOverloadsAreNoopWithoutBroadcastSocket)
{
	UDPCaster caster;
	WTSTickData* tick = WTSTickData::create("rb2001");
	WTSOrdQueData* ordQue = WTSOrdQueData::create("rb2001");
	WTSOrdDtlData* ordDtl = WTSOrdDtlData::create("rb2001");
	WTSTransData* trans = WTSTransData::create("rb2001");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(ordQue, nullptr);
	ASSERT_NE(ordDtl, nullptr);
	ASSERT_NE(trans, nullptr);

	caster.broadcast(tick);
	caster.broadcast(ordQue);
	caster.broadcast(ordDtl);
	caster.broadcast(trans);

	// 四个重载都转发到同一个守卫（L268-286 -> L288-291），一个对象都没被接管
	EXPECT_TRUE(tick->isSingleRefs());
	EXPECT_TRUE(ordQue->isSingleRefs());
	EXPECT_TRUE(ordDtl->isSingleRefs());
	EXPECT_TRUE(trans->isSingleRefs());

	tick->release();
	ordQue->release();
	ordDtl->release();
	trans->release();
}

TEST(R5UDPCaster, BroadcastOfNullObjectIsRejectedByGuard)
{
	UDPCaster caster;
	// data==NULL 分支：不能崩、不能进队列
	caster.broadcast(static_cast<WTSTickData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdQueData*>(nullptr));
	caster.broadcast(static_cast<WTSOrdDtlData*>(nullptr));
	caster.broadcast(static_cast<WTSTransData*>(nullptr));
	// 走到这里说明 L290 的 data==NULL 判定生效
	WTSTickData* tick = WTSTickData::create("DCE.c2009");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

TEST(R5UDPCaster, StopBeforeStartIsSafeAndTerminatesCaster)
{
	UDPCaster caster;
	// m_thrdIO / m_thrdCast 均为空指针 -> L137-142 的两个 if 都走 false 分支
	caster.stop();
	caster.stop();

	// stop() 之后 m_bTerminated=true，broadcast 走第三个守卫条件
	WTSTickData* tick = WTSTickData::create("rb2001");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();

	// 终止态不影响接收方地址登记（纯内存列表操作）
	EXPECT_TRUE(caster.addBRecver("239.255.0.1", 9009, 0));
}

TEST(R5UDPCaster, AddBRecverAcceptsValidIPv4IntoTypeBucket)
{
	UDPCaster caster;
	// type==0 -> m_listFlatRecver（L228-229）
	EXPECT_TRUE(caster.addBRecver("239.255.0.1", 8888));
	EXPECT_TRUE(caster.addBRecver("255.255.255.255", 1));
	// type==1 -> m_listJsonRecver（L230-231）
	EXPECT_TRUE(caster.addBRecver("127.0.0.1", 9999, 1));
	// type==2 -> m_listRawRecver（L232-233）
	EXPECT_TRUE(caster.addBRecver("10.0.0.1", 65535, 2));
	// 未知 type：三个 if 都不命中，仍然返回 true（静默丢弃，见 BUG 记录）
	EXPECT_TRUE(caster.addBRecver("172.16.0.1", 7000, 3));
	EXPECT_TRUE(caster.addBRecver("172.16.0.1", 7000, 99));
}

TEST(R5UDPCaster, AddBRecverRejectsUnparsableAddressViaCatch)
{
	UDPCaster caster;
	// make_address_v4 抛异常 -> catch(...) -> return false（L235-238）
	EXPECT_FALSE(caster.addBRecver("", 8888));
	EXPECT_FALSE(caster.addBRecver("not-an-ip", 8888));
	EXPECT_FALSE(caster.addBRecver("256.1.1.1", 8888));
	EXPECT_FALSE(caster.addBRecver("1.1.1", 8888));
	// IPv6 字面量不是 v4 地址，同样落在 catch 分支
	EXPECT_FALSE(caster.addBRecver("::1", 8888));
	EXPECT_FALSE(caster.addBRecver("2001:db8::1", 8888, 1));
	// 失败路径不污染列表：随后合法地址仍可登记
	EXPECT_TRUE(caster.addBRecver("239.1.2.3", 7777, 1));
}

TEST(R5UDPCaster, AddBRecverAcceptsPortBoundariesAndDuplicates)
{
	UDPCaster caster;
	// port 是 int，直接进 boost::asio endpoint 的 uint16 端口位；不校验范围也不该抛
	EXPECT_TRUE(caster.addBRecver("239.255.1.1", 0));
	EXPECT_TRUE(caster.addBRecver("239.255.1.2", 65535, 1));
	EXPECT_TRUE(caster.addBRecver("239.255.1.3", -1, 2));
	// 同一 host:port 重复登记：实现里没有任何去重，两次都返回 true（列表增长）
	EXPECT_TRUE(caster.addBRecver("239.255.1.1", 0));
	EXPECT_TRUE(caster.addBRecver("239.255.1.1", 0, 0));
	// 登记了接收方但仍然没有广播 socket -> 广播依然被 L290 守卫拦下
	WTSTickData* tick = WTSTickData::create("IF2001");
	ASSERT_NE(tick, nullptr);
	caster.broadcast(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

TEST(R5UDPCaster, RegisteredReceiversAloneNeverEnableBroadcastPath)
{
	UDPCaster caster;
	ASSERT_TRUE(caster.addBRecver("239.255.9.9", 8888, 2));
	WTSOrdDtlData* dtl = WTSOrdDtlData::create("IF2001");
	WTSTransData* trans = WTSTransData::create("IF2001");
	ASSERT_NE(dtl, nullptr);
	ASSERT_NE(trans, nullptr);
	// 组包/发送循环（L300-395）只有在 m_thrdCast 被创建后才会跑，而它要求 m_sktBroadcast 非空
	caster.broadcast(dtl);
	caster.broadcast(trans);
	EXPECT_TRUE(dtl->isSingleRefs());
	EXPECT_TRUE(trans->isSingleRefs());
	dtl->release();
	trans->release();
}

TEST(R5UDPCaster, BaseClassPointerDispatchesToUdpBroadcastNoop)
{
	UDPCaster caster;
	IDataCaster* base = &caster;
	WTSTickData* tick = WTSTickData::create("rb2005");
	WTSOrdQueData* ordQue = WTSOrdQueData::create("rb2005");
	ASSERT_NE(tick, nullptr);
	ASSERT_NE(ordQue, nullptr);
	// 虚表分派到 UDPCaster 的 4 个重载（L268-286），全部转发到同一个守卫
	base->broadcast(tick);
	base->broadcast(ordQue);
	base->broadcast(static_cast<WTSOrdDtlData*>(nullptr));
	base->broadcast(static_cast<WTSTransData*>(nullptr));
	EXPECT_EQ(tick->retainCount(), 1u);
	EXPECT_EQ(ordQue->retainCount(), 1u);
	tick->release();
	ordQue->release();
}

TEST(R5UDPCaster, AddMRecverRejectsBadAddressBeforeAnySocket)
{
	UDPCaster caster;
	// L248 make_address_v4 抛出，早于 L250 的 UDPSocket 构造，故不会 bind
	EXPECT_FALSE(caster.addMRecver("", 8000, 8001, 0));
	EXPECT_FALSE(caster.addMRecver("bad.host.name", 8000, 8001, 1));
	EXPECT_FALSE(caster.addMRecver("999.999.999.999", 8000, 8001, 2));
	EXPECT_FALSE(caster.addMRecver("192.168.1.256", 8000, 8001));
}

// 合法地址的 addMRecver 会 new UDPSocket(ioservice, endpoint(v4, sendport))，
// 也就是真的 bind 一个端口并 join_group —— 违反本轮“不建 socket/不 bind”约束，
// 需要注入式 socket 接缝才能覆盖 L249-259，留给主控裁定。
TEST(R5UDPCaster, DISABLED_AddMRecverWithValidAddressNeedsRealSocket)
{
	UDPCaster caster;
	EXPECT_TRUE(caster.addMRecver("239.255.0.7", 8000, 8001, 0));
}

TEST(R5UDPCaster, InitReturnsFalseWhenInactiveAndTouchesNothing)
{
	// WTSCfgLoader::load_from_json 是 private，只能走文件入口
	static std::atomic<int> seq { 0 };
	fs::path dir = fs::temp_directory_path()
		/ ("r5_udp_cfg_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(dir);
	const fs::path cfgFile = dir / "caster.json";

	UDPCaster caster;
	{
		std::ofstream ofs(cfgFile.string());
		ofs << R"JSON({"active": false, "sport": 39999, "bport": 39998})JSON";
	}
	WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgFile.string().c_str());
	ASSERT_NE(cfg, nullptr);
	// L67-68：active 为 false 直接 return false，不解析 broadcast/multicast、不 start()
	EXPECT_FALSE(caster.init(cfg, nullptr, nullptr));
	cfg->release();

	// 缺省 active 也当作 false 处理（getBoolean 的 defVal=false），
	// 因此 broadcast 列表里的接收方不会被登记，start() 也不会被调用
	{
		std::ofstream ofs(cfgFile.string());
		ofs << R"JSON({"sport": 39999, "broadcast": [{"host": "239.255.0.1", "port": 8888, "type": 0}]})JSON";
	}
	WTSVariant* cfg2 = WTSCfgLoader::load_from_file(cfgFile.string().c_str());
	ASSERT_NE(cfg2, nullptr);
	EXPECT_FALSE(caster.init(cfg2, nullptr, nullptr));
	cfg2->release();

	std::error_code ec;
	fs::remove_all(dir, ec);
}

}  // namespace
