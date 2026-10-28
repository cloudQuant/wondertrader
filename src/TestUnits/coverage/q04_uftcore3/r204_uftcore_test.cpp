/*
 * r204_uftcore_test.cpp  (wave28 · lane R204)
 *
 * WtUftCore 四靶经逐行复核后，本进程内可安全执行的面只剩切片桩与缺模块早退：
 *  - WtUftEngine.cpp:179-243/592-602  get_kline_slice/get_tick_slice 首行即
 *    return NULL，bar 订阅表无公开写入点，切片后续与 on_bar 循环全是死代码
 *    （BUG-R194-001）；本文件锁定公开切片入口的 NULL 返回值，并确认上下文
 *    包装层不会转去订阅。
 *  - UftStraContext.cpp:705-727 数值 watch_param 在无共享内存段时解引用 NULL
 *    （BUG-R11-002），保持不驱动；944-982 需要引擎返回非空切片，而引擎四个
 *    切片入口当前全是 NULL 桩。
 *  - WtUftCore/ParserAdapter.cpp:90-93、153-161 需插件 createParser 返回空
 *    或 api->init 返回 false，不装载真实行情插件。
 *  - TraderAdapter.cpp:1908-1917 的 isTradeEnabled 在头文件声明为 inline、
 *    定义只在 .cpp 且编译器不产出外部符号，测试 TU 无法链接（BUG-R204-001），
 *    本轮不为其写会链接失败的用例。
 *
 * 边界：只构造内存对象；不建 shm/socket，不加载任何动态库，不 run 引擎。
 */
#include <gtest/gtest.h>

#include <string>

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/WTSVariant.hpp"

using namespace wtp;

/*
 * 生产: WtUftCore/WtUftEngine.cpp:179-183、209-211
 *        WtUftCore/UftStraContext.cpp:933-956
 * 两个引擎切片入口是首行 return NULL 桩；上下文包装层拿不到切片时直接返回
 * NULL，不进入 sub_tick 分支，也不触碰未初始化的 _data_mgr。
 */
TEST(R204UftContext, SliceStubsReturnNullThroughContext)
{
	WtUftEngine engine;
	engine.init(nullptr, nullptr, nullptr, nullptr);
	UftStraContext ctx(&engine, "r204_ctx");

	EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SHFE.rb2510", "m", 5));
	EXPECT_EQ(nullptr, engine.get_tick_slice(1, "SHFE.rb2510", 5));

	EXPECT_EQ(nullptr, ctx.stra_get_bars("SHFE.rb2510", "m5", 5));
	EXPECT_EQ(nullptr, ctx.stra_get_bars("SHFE.rb2510", "d", 5));
	EXPECT_EQ(nullptr, ctx.stra_get_ticks("SHFE.rb2510", 5));
}

/*
 * 生产: WtUftCore/ParserAdapter.cpp:57-100
 * module 缺失/装载失败在 registerSpi/init 之前早退；stub 与 bdMgr 可传空。
 */
TEST(R204UftParserAdapter, MissingModuleIsRejected)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "r204_missing_parser_module");

	ParserAdapter pa;
	EXPECT_FALSE(pa.init("r204_missing", cfg, nullptr, nullptr));
	EXPECT_STREQ("r204_missing", pa.id());

	cfg->release();
}
