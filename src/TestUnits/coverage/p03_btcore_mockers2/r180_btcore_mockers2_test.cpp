/**
 * r180_btcore_mockers2_test.cpp  (lane R180 · wave26)
 *
 * 靶子：WtBtCore 剩余未覆盖行中，不依赖策略插件、hook 线程与私有注入的公开面。
 * 每个用例注释标明生产文件:行段；全部只走公开 API，独立、确定性，无长线程/墙钟。
 * 临时文件只落系统临时目录，由夹具 TearDown 清理。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/ExecMocker.h>
#include <WtBtCore/HftMocker.h>
#include <WtBtCore/HisDataReplayer.h>

#include "r180_common.h"

namespace
{
	const char* const kCu = "SHFE.cu.2406";
	const char* const kStk1 = "SSE.STK.000001";
	const char* const kStk1Qfq = "SSE.STK.000001+";
	const char* const kFut1 = "DCE.i.2409";

	using R180LoaderFixture = r72::Fixture;
	using R180ReplayFixture = r161::ReplayFixture;

	// r72::Loader 的 finalCalls 是全局计数；本夹具额外按合约记录，
	// 用来证明被 check_cache_days 淘汰的缓存确实重新走了 loader。
	class CountingLoader : public r72::Loader
	{
	public:
		std::map<std::string, uint32_t> calls;

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period,
			FuncReadBars cb) override
		{
			calls[stdCode]++;
			return r72::Loader::loadFinalHisBars(obj, stdCode, period, cb);
		}
	};

	WTSTickData* make_cu_tick(double price)
	{
		return r141::make_tick(kCu, 20200102u, 93200000u,
			price, price - 10.0, price + 10.0);
	}
}

// 生产: ExecMocker.cpp:71-85（工厂装载成功臂）、140-161（handle_init）
// 公开 init 指向本机构建产出的 libWtExeFact.dylib，executer 选用真实注册的
// WtTWapExeUnit；未经过任何 tick 就 handle_init，set_position 在空 _last_tick
// 处安全返回。handle_replay_done 必须落出交易日志表头。
TEST_F(R180LoaderFixture, R180Exec_InitRealExecuterAndHandleInit)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r180::make_exec_cfg(_root, _base, kCu, 0, "r180_exec_init");
	ASSERT_TRUE(rp.init(cfg));

	ExecMocker mocker(&rp);
	ASSERT_TRUE(mocker.init(cfg));
	cfg->release();

	mocker.handle_init();
	EXPECT_DOUBLE_EQ(0.0, mocker.getPosition(kCu));
	EXPECT_DOUBLE_EQ(0.0, mocker.getUndoneQty(kCu));

	mocker.handle_replay_done();
	const std::string log = r161::read_text(_out / "exec" / "trades_r180_exec_init.csv");
	EXPECT_NE(std::string::npos, log.find("localid,signaltime"));
}

// 生产: ExecMocker.cpp:168-192（handle_schedule 三个 volmode 分支与信号计数）
//       ExecMocker.cpp:286-335（handle_entrust/handle_order/handle_trade 全回调）
// 不调用 handle_init，执行单元 _channel_ready 保持 false：TWap 的 on_tick/
// set_position 都不会走到 do_calc 的尾部（避免它 release _last_tick 后的悬垂指针），
// 但 ExecMocker 自己的调度、撤单日志、成交持仓更新全部照常执行。
TEST_F(R180LoaderFixture, R180Exec_ScheduleAndDirectCallbacks)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r180::make_exec_cfg(_root, _base, kCu, 0, "r180_exec_sched");
	ASSERT_TRUE(rp.init(cfg));

	ExecMocker mocker(&rp);
	ASSERT_TRUE(mocker.init(cfg));
	cfg->release();

	WTSTickData* tick = make_cu_tick(70000.0);
	mocker.handle_tick(kCu, tick, 0);
	tick->release();

	// 仓位 0：volmode=0 走 _target=_volunit
	mocker.handle_schedule(20200102u, 1000u);

	const uint64_t ordTime = 20200102ull * 1000000000ull + 100000000ull;
	mocker.handle_entrust(1u, kCu, true, "ok", ordTime);
	mocker.handle_order(2u, kCu, true, 1.0, 70000.0, false, ordTime);
	mocker.handle_order(3u, kCu, false, 1.0, 70000.0, true, ordTime);
	mocker.handle_trade(4u, kCu, true, 1.0, 70000.0, 70000.0, ordTime);
	EXPECT_DOUBLE_EQ(1.0, mocker.getPosition(kCu));

	// 仓位 >0：volmode=0 走 _target=-_volunit
	mocker.handle_schedule(20200102u, 1001u);

	mocker.handle_trade(5u, kCu, false, 2.0, 70000.0, 70000.0, ordTime);
	EXPECT_DOUBLE_EQ(-1.0, mocker.getPosition(kCu));

	mocker.handle_replay_done();
	const std::string log = r161::read_text(_out / "exec" / "trades_r180_exec_sched.csv");
	EXPECT_NE(std::string::npos, log.find(",true"));
	EXPECT_NE(std::string::npos, log.find(",false"));
}

// 生产: ExecMocker.cpp:270-274（cancel(stdCode,isBuy,qty) 的撤单回调）
// 先 handle_tick 让撮合引擎有最新价；限价远低于卖一，订单不会被成交；
// 第二次 tick 激活订单（state=1）后再按合约撤单，回调才会累计撤单量。
TEST_F(R180LoaderFixture, R180Exec_CancelByCodeRunsCallback)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r180::make_exec_cfg(_root, _base, kCu, 0, "r180_exec_cancel");
	ASSERT_TRUE(rp.init(cfg));

	ExecMocker mocker(&rp);
	ASSERT_TRUE(mocker.init(cfg));
	cfg->release();

	WTSTickData* t1 = make_cu_tick(70000.0);
	mocker.handle_tick(kCu, t1, 0);
	t1->release();

	OrderIDs ids = mocker.buy(kCu, 69000.0, 1.0);
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(1.0, mocker.getUndoneQty(kCu));

	WTSTickData* t2 = make_cu_tick(70000.0);
	mocker.handle_tick(kCu, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(1.0, mocker.getUndoneQty(kCu));

	OrderIDs canceled = mocker.cancel(kCu, true, 0.0);
	EXPECT_EQ(1u, canceled.size());
	EXPECT_DOUBLE_EQ(0.0, mocker.getUndoneQty(kCu));
}

// 生产: ExecMocker.cpp:180-187（volmode=-1/+1 的累加分支）
// 与上一条相同，不调用 handle_init 以保持计算线程关闭；两个方向各起一个
// 独立 ExecMocker，handle_schedule 后仓位仍为 0（只更新目标仓位，不撮合）。
TEST_F(R180LoaderFixture, R180Exec_ScheduleDirectionalVolumeModes)
{
	HisDataReplayer rp;
	WTSVariant* rcfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(rcfg));
	rcfg->release();

	const int32_t modes[2] = { -1, 1 };
	const char* ids[2] = { "r180_exec_sell", "r180_exec_buy" };
	for (int i = 0; i < 2; i++)
	{
		WTSVariant* cfg = r180::make_exec_cfg(_root, _base, kCu, modes[i], ids[i]);
		ExecMocker mocker(&rp);
		ASSERT_TRUE(mocker.init(cfg));
		cfg->release();

		WTSTickData* tick = make_cu_tick(70000.0);
		mocker.handle_tick(kCu, tick, 0);
		tick->release();

		mocker.handle_schedule(20200102u, 1000u);
		EXPECT_DOUBLE_EQ(0.0, mocker.getPosition(kCu));
	}
}

// BUG-R180-001（待审核）：WtTWapExeUnit::do_calc 在发单臂末尾 release()
// _last_tick 却不置空（WtTWapExeUnit.cpp:713-767）；第二个 tick 会在
// on_tick 的 339-341 行再次 release，悬垂指针导致 UAF。启用即崩溃，故禁用；
// 保留为缺陷复现占位，不放宽断言。
TEST_F(R180LoaderFixture, DISABLED_R180Exec_TwapSecondTickReleasesDanglingLastTick)
{
	HisDataReplayer rp;
	WTSVariant* cfg = r180::make_exec_cfg(_root, _base, kCu, 0, "r180_exec_uaf");
	ASSERT_TRUE(rp.init(cfg));

	ExecMocker mocker(&rp);
	ASSERT_TRUE(mocker.init(cfg));
	cfg->release();

	mocker.handle_init();	// _channel_ready=true，第一个 tick 会走到 do_calc 尾部

	WTSTickData* t1 = make_cu_tick(70000.0);
	mocker.handle_tick(kCu, t1, 0);
	t1->release();

	WTSTickData* t2 = make_cu_tick(70010.0);
	mocker.handle_tick(kCu, t2, 0);
	t2->release();

	FAIL() << "BUG-R180-001: WtTWapExeUnit::do_calc releases _last_tick without clearing it";
}

// 生产: HisDataReplayer.cpp:3710（loader 自动转储 KP_Minute5 的 BlockType）
//       3634-3666（转储文件命中后直接读取）
// autoTrans=true 的 loader 首次加载 12 根 m5：写 his/min5/SSE/000001.dsb；
// 第二次查询同一 key 命中 _bars_cache，不再回调 loader。
TEST_F(R180LoaderFixture, R180His_LoaderM5AutoTransThenDsbReuse)
{
	r72::Loader loader;
	loader.autoTrans = true;
	loader.finalBars[kStk1] = r180::make_m5_bars(20200102u, 12, 10.0);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r141::NullSink sink;
	rp.register_sink(&sink, "r180_m5");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* k = rp.get_kline_slice(kStk1, "m", 5, 5);
	ASSERT_NE(nullptr, k);
	EXPECT_GT(k->size(), 0u);
	k->release();
	EXPECT_EQ(1u, loader.finalCalls);
	EXPECT_TRUE(r180::fs::exists(_base / "his" / "min5" / "SSE" / "000001.dsb"));

	WTSKlineSlice* k2 = rp.get_kline_slice(kStk1, "m", 6, 5);
	ASSERT_NE(nullptr, k2);
	EXPECT_GT(k2->size(), 0u);
	k2->release();
	EXPECT_EQ(1u, loader.finalCalls);
}

// 生产: HisDataReplayer.cpp:3639-3642（自动转储 dsb 尺寸检查失败）
// 预置 3 字节的 his/day/SSE/000001.dsb；get_kline_slice 先命中该文件，
// sizecheck 失败后回落到 loader，并把合法的 dsb 重新写回。
TEST_F(R180LoaderFixture, R180His_CorruptAutoTransDsbRebuiltFromLoader)
{
	r72::Loader loader;
	loader.autoTrans = true;
	loader.finalBars[kStk1] = r180::make_daily_bars(10.0);

	const r180::fs::path dsb = _base / "his" / "day" / "SSE" / "000001.dsb";
	ASSERT_TRUE(r72::write_text(dsb, std::string("bad")));

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, true);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r141::NullSink sink;
	rp.register_sink(&sink, "r180_corrupt");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* k = rp.get_kline_slice(kStk1, "d", 1);
	ASSERT_NE(nullptr, k);
	EXPECT_GT(k->size(), 0u);
	k->release();
	EXPECT_EQ(1u, loader.finalCalls);
	EXPECT_GT(r180::fs::file_size(dsb), static_cast<uintmax_t>(20));
}

// 生产: HisDataReplayer.cpp:3651-3652（bSubbed=false 写入 _unbars_cache）
//       3704-3731（autoTrans 转储 m5）
// tick=false + 复权代码 isMain 查询把 _main_period 写成 "m#5"、把原始代码放进
// _unsubbed_in_need；prepare 的 checkUnbars 以 bSubbed=false 调 loader，
// 加载 12 根 m5 并落盘 his/min5/SSE/000001.dsb。
TEST_F(R180LoaderFixture, R180His_CheckUnbarsMinute5UsesUnbarsCache)
{
	r72::Loader loader;
	loader.autoTrans = true;
	loader.finalBars[kStk1] = r180::make_m5_bars(20200102u, 12, 10.0);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001021500ull, false);
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r141::NullSink sink;
	rp.register_sink(&sink, "r180_unbars");

	// 复权代码：loader 没有该 key -> 空切片，但 _main_period / _unsubbed_in_need 已写入
	EXPECT_EQ(nullptr, rp.get_kline_slice(kStk1Qfq, "m", 2, 5, true));
	ASSERT_TRUE(rp.prepare());
	EXPECT_EQ(1u, sink.initCalls);
	EXPECT_EQ(2u, loader.finalCalls);
	EXPECT_TRUE(r180::fs::exists(_base / "his" / "min5" / "SSE" / "000001.dsb"));
}

// 生产: HisDataReplayer.cpp:4624-4627（check_cache_days 的 codes 逗号拼接）
// 主 K 线之外再缓存两只合约，cache_clear_days=2：第二个交易日同时淘汰两者，
// 第二个 key 进入 codes 非空分支。淘汰后再次查询 A 必须重新走 loader。
TEST_F(R180LoaderFixture, R180His_CacheClearJoinsMultipleCodes)
{
	const char* const kExtraA = "DCE.i.2409";
	const char* const kExtraB = "DCE.j.2409";

	CountingLoader loader;
	loader.autoTrans = false;
	loader.finalBars[kStk1] = r180::make_daily_bars(10.0);
	loader.finalBars[kExtraA] = r180::make_daily_bars(800.0);
	loader.finalBars[kExtraB] = r180::make_daily_bars(900.0);

	HisDataReplayer rp;
	WTSVariant* cfg = r72::make_cfg(_root, _base, 202001020930ull, 202001081500ull, true);
	cfg->append("cache_clear_days", static_cast<uint32_t>(2));
	ASSERT_TRUE(rp.init(cfg, nullptr, &loader));
	cfg->release();

	r141::NullSink sink;
	rp.register_sink(&sink, "r180_clear");
	ASSERT_TRUE(rp.prepare());

	WTSKlineSlice* main = rp.get_kline_slice(kStk1, "d", 5, 1, true);
	ASSERT_NE(nullptr, main);
	main->release();
	WTSKlineSlice* a = rp.get_kline_slice(kExtraA, "d", 5, 1);
	ASSERT_NE(nullptr, a);
	a->release();
	WTSKlineSlice* b = rp.get_kline_slice(kExtraB, "d", 5, 1);
	ASSERT_NE(nullptr, b);
	b->release();
	EXPECT_EQ(1u, loader.calls[kExtraA]);
	EXPECT_EQ(1u, loader.calls[kExtraB]);

	rp.run(false);

	// 两只缓存都已在第二个交易日被淘汰；再查询 A 会重新回调 loader。
	// run 结束后 _cur_tdate 已越过 A 的最后一根日线，切片可能为 NULL，
	// 但 loader 调用次数必须 +1。
	WTSKlineSlice* reload = rp.get_kline_slice(kExtraA, "d", 5, 1);
	if (reload != nullptr)
		reload->release();
	EXPECT_EQ(2u, loader.calls[kExtraA]);
}

// 生产: HftMocker.cpp:561（stra_cancel(localid) 队列任务把 _left 置 0）
// 买单限价 700 远低于卖一，第一次 tick 只激活订单不成交；stra_cancel 入队后
// 第二次 tick 执行任务：订单被移除，未完成量归零且不触发已知的同 tick 崩溃点。
TEST_F(R180ReplayFixture, R180Hft_CancelLocalOrderZeroesLeft)
{
	HftMocker hft(&_rp, "r180_hft_cancel");

	OrderIDs ids = hft.stra_buy(kFut1, 700.0, 1.0, "r180");
	ASSERT_EQ(1u, ids.size());
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_undone(kFut1));

	WTSTickData* t1 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	hft.handle_tick(kFut1, t1, 0);
	t1->release();
	EXPECT_DOUBLE_EQ(1.0, hft.stra_get_undone(kFut1));

	ASSERT_TRUE(hft.stra_cancel(ids[0]));

	WTSTickData* t2 = r141::make_tick(kFut1, 20200102u, 93200000u, 800.0, 799.0, 801.0);
	hft.handle_tick(kFut1, t2, 0);
	t2->release();
	EXPECT_DOUBLE_EQ(0.0, hft.stra_get_undone(kFut1));
}
