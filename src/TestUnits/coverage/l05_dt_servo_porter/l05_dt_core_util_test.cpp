/*
 * L05 - WtDtCore 无 socket / 无 DLL 依赖的纯内存工具
 *
 * 覆盖 src/WtDtCore/WtHelper.cpp（get_cwd）、src/WtDtCore/WtHelper.h（模块目录读写）、
 * src/WtDtCore/StatHelper.hpp（updateStatInfo / getStatInfo / 溢出分支）、
 * src/WtDtCore/IDataCaster.h（未覆写的默认空实现）。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 */
#include <gtest/gtest.h>

#include <mutex>
#include <shared_mutex>

/*
 * WtDtCore/StatHelper.hpp 依赖 BoostRWMutex / BoostWriteLock / BoostReadLock 三个别名，
 * 但全仓库没有任何地方定义它们（见缺陷 L05-004），单独包含该头文件必然编译失败。
 * 这里在测试侧用 std 的读写锁补齐别名，只为让 StatHelper.hpp 的行能被编译覆盖，
 * 不改动任何生产代码。
 */
typedef std::shared_mutex BoostRWMutex;
typedef std::unique_lock<std::shared_mutex> BoostWriteLock;
typedef std::shared_lock<std::shared_mutex> BoostReadLock;

#include "WtDtCore/WtHelper.h"
#if defined(__clang__)
// StatHelper.hpp::updateStatInfo 里 flag 计算后未使用（见缺陷 L05-004），
// 在 -Werror 门禁下只能由测试侧局部抑制，不改生产代码。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "WtDtCore/StatHelper.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "WtDtCore/IDataCaster.h"

#include <cstring>
#include <filesystem>
#include <string>

USING_NS_WTP;

namespace {

	// 只实现纯虚的 tick 广播，其余三类广播走 IDataCaster 的默认空实现
	class L05MinimalCaster : public IDataCaster
	{
	public:
		std::string _lastTickCode;
		uint32_t _ordQueCalls = 0;
		uint32_t _ordDtlCalls = 0;
		uint32_t _transCalls = 0;

		void broadcast(WTSTickData* curTick) override
		{
			if (curTick == NULL)
				_lastTickCode = "<null>";
		}

		void broadcast(WTSOrdQueData* curOrdQue) override
		{
			if (curOrdQue == NULL)
				_ordQueCalls++;
		}

		void broadcast(WTSOrdDtlData* curOrdDtl) override
		{
			if (curOrdDtl == NULL)
				_ordDtlCalls++;
		}

		void broadcast(WTSTransData* curTrans) override
		{
			if (curTrans == NULL)
				_transCalls++;
		}
	};

	// 完全不覆写默认实现，直接落到 IDataCaster.h 里的空函数体
	class L05BareCaster : public IDataCaster
	{
	public:
		uint32_t _tickCalls = 0;

		void broadcast(WTSTickData* curTick) override
		{
			_tickCalls++;
			(void)curTick;
		}
	};

} // namespace

TEST(L05DtCoreHelper, CwdIsStandardised)
{
	const char* cwd = WtHelper::get_cwd();
	ASSERT_TRUE(cwd != NULL);
	const std::string strCwd(cwd);
	EXPECT_FALSE(strCwd.empty());
	// standardisePath 会在末尾补上目录分隔符
	const char lastChar = strCwd[strCwd.size() - 1];
	EXPECT_TRUE(lastChar == '/' || lastChar == '\\');
	// 第二次调用走缓存分支
	EXPECT_STREQ(WtHelper::get_cwd(), cwd);
}

TEST(L05DtCoreHelper, ModuleDirRoundtrip)
{
	WtHelper::set_module_dir("/tmp/l05_module_dir");
	EXPECT_STREQ(WtHelper::get_module_dir(), "/tmp/l05_module_dir");
	WtHelper::set_module_dir("");
	EXPECT_STREQ(WtHelper::get_module_dir(), "");
}

TEST(L05DtCoreStatHelper, AccumulatesCounters)
{
	StatHelper& helper = StatHelper::one();

	StatHelper::StatInfo before = helper.getStatInfo(StatHelper::ST_BROADCAST);
	helper.updateStatInfo(StatHelper::ST_BROADCAST, 3, 5, 1024);
	StatHelper::StatInfo after = helper.getStatInfo(StatHelper::ST_BROADCAST);

	EXPECT_EQ(after._recv_packs, before._recv_packs + 3);
	EXPECT_EQ(after._send_packs, before._send_packs + 5);
	EXPECT_EQ(after._send_bytes, before._send_bytes + 1024);

	// 全 0 更新：既不走接收分支也不走发送分支
	helper.updateStatInfo(StatHelper::ST_BROADCAST, 0, 0, 0);
	StatHelper::StatInfo same = helper.getStatInfo(StatHelper::ST_BROADCAST);
	EXPECT_EQ(same._recv_packs, after._recv_packs);
	EXPECT_EQ(same._send_packs, after._send_packs);
	EXPECT_EQ(same._send_bytes, after._send_bytes);

	// 单例引用稳定
	EXPECT_EQ(&StatHelper::one(), &helper);
}

TEST(L05DtCoreStatHelper, HandlesSendBytesOverflow)
{
	StatHelper& helper = StatHelper::one();
	// 先把计数器推到接近 UINT64_MAX，命中溢出保护分支（直接置为新值）
	helper.updateStatInfo(StatHelper::ST_BROADCAST, 0, 0, UINT64_MAX - 1);
	StatHelper::StatInfo full = helper.getStatInfo(StatHelper::ST_BROADCAST);
	EXPECT_EQ(full._send_bytes, UINT64_MAX - 1);

	helper.updateStatInfo(StatHelper::ST_BROADCAST, 1, 0, 100);
	StatHelper::StatInfo wrapped = helper.getStatInfo(StatHelper::ST_BROADCAST);
	EXPECT_EQ(wrapped._send_bytes, 100u);
}

TEST(L05DtCoreStatHelper, DefaultStatInfoIsZeroed)
{
	StatHelper::StatInfo info;
	EXPECT_EQ(info._recv_packs, 0u);
	EXPECT_EQ(info._send_packs, 0u);
	EXPECT_EQ(info._send_bytes, 0u);
}

TEST(L05DtCoreCaster, OverriddenHooksAreCalled)
{
	L05MinimalCaster caster;
	IDataCaster* base = &caster;

	base->broadcast((WTSTickData*)NULL);
	base->broadcast((WTSOrdQueData*)NULL);
	base->broadcast((WTSOrdDtlData*)NULL);
	base->broadcast((WTSTransData*)NULL);

	EXPECT_EQ(caster._lastTickCode, std::string("<null>"));
	EXPECT_EQ(caster._ordQueCalls, 1u);
	EXPECT_EQ(caster._ordDtlCalls, 1u);
	EXPECT_EQ(caster._transCalls, 1u);
}

TEST(L05DtCoreCaster, DefaultImplsAreNoOps)
{
	L05BareCaster caster;
	IDataCaster* base = &caster;

	// 未覆写的三个重载走 IDataCaster.h 中的默认空实现，调用不应产生副作用
	base->broadcast((WTSOrdQueData*)NULL);
	base->broadcast((WTSOrdDtlData*)NULL);
	base->broadcast((WTSTransData*)NULL);
	base->broadcast((WTSTickData*)NULL);
	EXPECT_EQ(caster._tickCalls, 1u);
}
