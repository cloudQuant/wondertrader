/*
 * r213_uftcore_test.cpp  (wave29 · lane R213)
 *
 * WtUftCore 四靶（TraderAdapter 65 / WtUftEngine 34 / UftStraContext 22 /
 * ParserAdapter 13）已由 R194/R204 逐行裁定；本轮复核后新增的可执行面只有
 * WtUftEngine.h 的三个时间取值 inline 函数（get_raw_time/get_secs/
 * get_trading_date，此前 0 覆盖），其余仍为不可达或崩溃接缝：
 *  - WtUftEngine.cpp:182、213-243 在两处首行 return NULL 之后；592-602 的
 *    bar 订阅表无公开写入点（BUG-R194-001）。
 *  - UftStraContext.cpp:705-727 数值 watch_param 在 ShareManager 离线时解引用
 *    NULL（BUG-R11-002）；944-982 需要引擎返回非空切片，而四个切片入口都是
 *    NULL 桩。
 *  - WtUftCore/ParserAdapter.cpp:90-93、153-161 需要插件 createParser 返回空
 *    或 api->init 返回 false；不装载真实行情插件。
 *  - TraderAdapter.cpp:1908-1917 头文件声明 inline、定义只在 .cpp，测试 TU
 *    无法链接（BUG-R204-001）；1878-1965 的时间窗写入点全部被注释（BUG-Q04-004）。
 *
 * 边界：只构造内存对象；不 run、不建 shm/socket、不加载任何动态库。
 */
#include <gtest/gtest.h>

#include <string>

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

/*
 * 生产: WtUftCore/WtUftEngine.h:67-73（get_raw_time/get_secs/get_trading_date）
 * set_date_time/set_trading_date 写入后，三个此前从未被调用的取值函数必须
 * 原样返回；日期/时间字段互不串写。on_bar 在空订阅表下是空循环。
 */
TEST(R213UftEngine, TimeAccessorsAndInertOnBar)
{
	WtUftEngine engine;
	engine.init(nullptr, nullptr, nullptr, nullptr);

	engine.set_date_time(20261028, 931, 15, 93115);
	EXPECT_EQ(20261028u, engine.get_date());
	EXPECT_EQ(931u, engine.get_min_time());
	EXPECT_EQ(93115u, engine.get_raw_time());
	EXPECT_EQ(15u, engine.get_secs());

	engine.set_trading_date(20261028);
	EXPECT_EQ(20261028u, engine.get_trading_date());

	engine.on_bar("SSE.600000", "m5", 5, nullptr);
	engine.on_bar("SSE.600000", "d", 1, nullptr);
}

/*
 * 生产: WtUftCore/ParserAdapter.cpp:55-100、150-161
 * NULL 配置、空 module、缺模块、二次 init、initExt 空 API、无 API run
 * 都必须返回 false；首次 init 的 id 在模块检查之前写入，不对失败重入保留旧 id 作假设；
 * release 在无 api 时是空操作。
 */
TEST(R213UftParserAdapter, GuardsBeforeModuleLoad)
{
	ParserAdapter pa;
	EXPECT_FALSE(pa.init("r213_uft_null", nullptr, nullptr, nullptr));

	WTSVariant* cfgEmpty = WTSVariant::createObject();
	EXPECT_FALSE(pa.init("r213_uft_empty", cfgEmpty, nullptr, nullptr));
	EXPECT_STREQ("r213_uft_empty", pa.id());
	EXPECT_FALSE(pa.init("r213_uft_second", cfgEmpty, nullptr, nullptr));
	cfgEmpty->release();

	WTSVariant* cfgMissing = WTSVariant::createObject();
	cfgMissing->append("module", "r213_no_such_uft_parser_module");
	ParserAdapter pb;
	EXPECT_FALSE(pb.init("r213_uft_missing", cfgMissing, nullptr, nullptr));
	EXPECT_STREQ("r213_uft_missing", pb.id());
	cfgMissing->release();

	ParserAdapter pc;
	EXPECT_FALSE(pc.initExt("r213_uft_ext", nullptr, nullptr, nullptr));
	EXPECT_FALSE(pc.run());
	pc.release();
}

/*
 * 生产: WtUftCore/WtUftEngine.cpp:179-183、209-211
 *        WtUftCore/UftStraContext.cpp:694-702、730-770、933-956
 * 两个引擎切片入口是首行 return NULL 桩；上下文包装层拿不到切片就直接返回
 * NULL，不进入 sub_tick 分支；字符串 watch_param/read_param 在离线时不解引用。
 */
TEST(R213UftContext, OfflineParamAndSliceStubs)
{
	WtUftEngine engine;
	engine.init(nullptr, nullptr, nullptr, nullptr);
	UftStraContext ctx(&engine, "r213_ctx");

	EXPECT_EQ(nullptr, engine.get_kline_slice(1, "SSE.600000", "m", 5));
	EXPECT_EQ(nullptr, engine.get_tick_slice(1, "SSE.600000", 5));

	EXPECT_EQ(nullptr, ctx.stra_get_bars("SSE.600000", "d", 5));
	EXPECT_EQ(nullptr, ctx.stra_get_bars("SSE.600000", "m5", 5));
	EXPECT_EQ(nullptr, ctx.stra_get_ticks("SSE.600000", 5));

	EXPECT_EQ(nullptr, ctx.watch_param("r213_str", "v"));
	EXPECT_STREQ("dflt", ctx.read_param("r213_read", "dflt"));
	EXPECT_EQ(nullptr, ctx.sync_param("r213_sync", 1.0));
	ctx.commit_param_watcher();
}
