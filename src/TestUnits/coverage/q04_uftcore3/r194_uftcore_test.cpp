/*
 * r194_uftcore_test.cpp  (wave27 · lane R194)
 *
 * WtUftCore 本轮目标（TraderAdapter 65 / WtUftEngine 34 / UftStraContext 22 /
 * ParserAdapter 13）经逐行复核后，可用公开 API 安全触达的面只剩“桩函数返回值”
 * 与“缺模块早退”：
 *  - WtUftEngine::get_kline_slice/get_tick_slice 在 HEAD 上无条件 return NULL，
 *    其后的订阅表/重采样代码是死代码（见 bugs/R194.md BUG-R194-001），
 *    本文件锁定该桩行为与 on_bar 空订阅不崩。
 *  - ParserAdapter::init 的 module 缺失/装载失败早退（不加载任何真实插件）。
 *  - UftStraContext 的 5 个数值 watch_param 重载在 ShareManager 未初始化时
 *    对 allocate_value 返回值直接解引用（BUG-R11-002，本 lane 复核仍成立），
 *    保留 DISABLED_ 复现占位，启用即段错误。
 *
 * 边界：只构造引擎/上下文，不 run、不建 shm、不加载插件、不写文件。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

/*
 * 生产: WtUftEngine.cpp:179-183、192-195、209-243（两处 return NULL 之后的死代码）
 * 公开切片入口在无数据管理器时也必须返回 NULL；on_bar 在订阅表为空时是空循环。
 */
TEST(R194UftEngine, SliceEntriesAreNullStubsAndOnBarIsInert)
{
	WtUftEngine engine;
	engine.init(nullptr, nullptr, nullptr, nullptr);

	EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SHFE.rb.2510", "d", 5));
	EXPECT_EQ(nullptr, engine.get_tick_slice(1, "SHFE.rb.2510", 5));
	EXPECT_EQ(nullptr, engine.get_order_detail_slice(1, "SHFE.rb.2510", 5));
	EXPECT_EQ(nullptr, engine.get_order_queue_slice(1, "SHFE.rb.2510", 5));
	EXPECT_EQ(nullptr, engine.get_transaction_slice(1, "SHFE.rb.2510", 5));

	engine.on_bar("SHFE.rb.2510", "d", 1, nullptr);
}

/*
 * 生产: WtUftCore/ParserAdapter.cpp:57-100（module 缺失/装载失败早退）
 * 不存在的 module 只触发 load_library 失败；早退前不使用 stub/bgMgr，故可传空。
 */
TEST(R194UftParserAdapter, MissingModuleIsRejected)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r194_missing_parser_module");

	ParserAdapter pa;
	EXPECT_FALSE(pa.init("r194", cfg, nullptr, nullptr));
	EXPECT_STREQ("r194", pa.id());

	cfg->release();
}

/*
 * 生产: WtUftCore/UftStraContext.cpp:704-727（5 个数值 watch_param 重载）
 * 它们在 ShareManager::self().allocate_value 返回 nullptr 时直接解引用，
 * 无共享内存段即段错误（BUG-R11-002 复核）。保留复现占位，禁止启用。
 */
TEST(R194UftContext, DISABLED_NumericWatchParamDereferencesNullWhenShareManagerAbsent)
{
	WtUftEngine engine;
	engine.init(nullptr, nullptr, nullptr, nullptr);
	UftStraContext ctx(&engine, "r194_ctx");

	EXPECT_EQ(1, ctx.watch_param("r194_key", (int32_t)1));
	EXPECT_EQ(2, ctx.watch_param("r194_key", (int64_t)2));
	EXPECT_EQ(3U, ctx.watch_param("r194_key", (uint32_t)3));
	EXPECT_EQ(4U, ctx.watch_param("r194_key", (uint64_t)4));
	EXPECT_DOUBLE_EQ(5.5, ctx.watch_param("r194_key", 5.5));
}
