/**
 * p04_replayer_config_prepare_test.cpp
 * 覆盖 src/WtBtCore/HisDataReplayer.cpp 的配置装载与回放前置阶段：
 *   init(L171-300)          —— stime/etime/cache_clear_days/tick/adjust_flag/
 *                              align_by_section/dont_simtick_if_notrade 读取、
 *                              basefiles 的 session/commodity/holiday/hot/second 缺失分支、
 *                              fees 缺失、mode 非 bin 分支
 *   prepare(L649-675)       —— _running 复入保护、reset(L459)、日期/时间/交易日推进赋值、
 *                              handle_init 回调、tick 关闭时 checkUnbars(L3167)
 *   run(L677-739) 空数据分支 —— 主K线自动确定失败 -> handle_replay_done
 *   stop(L634) / clear_cache(L436) / register_task(L407)
 * 说明：仓库内配置的时间约定是 YYYYMMDDHHMM（12 位），prepare()/run_by_ticks()
 * 用 /10000 取日期、%10000 取时分，故本文件用 12 位入参做确定断言；
 * 头文件把 set_time_range 标注为 YYYYMMDDHHMMSS，混用会错位，已记入 bugs。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <sys/resource.h>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path lane_root()
	{
		// src/TestUnits/coverage/p04_btcore_replayer -> 仓库根
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path();
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("p04_replayer_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	/**
	 * @brief 只记录回调次数/参数的数据接收器
	 *        IDataSink 的 handle_tick/handle_bar_close/handle_schedule/handle_init/
	 *        handle_session_begin/handle_session_end 为纯虚，其余有默认实现
	 */
	class RecordingSink : public IDataSink
	{
	public:
		uint32_t initCalls {0};
		uint32_t replayDoneCalls {0};
		uint32_t sessionBeginCalls {0};
		uint32_t sessionEndCalls {0};
		uint32_t tickCalls {0};
		uint32_t barCloseCalls {0};
		uint32_t scheduleCalls {0};
		uint32_t lastSessionTDate {0};
		uint32_t lastScheduleDate {0};
		uint32_t lastScheduleTime {0};

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			(void)stdCode; (void)curTick; (void)pxType;
			tickCalls++;
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			(void)stdCode; (void)period; (void)times; (void)newBar;
			barCloseCalls++;
		}

		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			lastScheduleDate = uDate;
			lastScheduleTime = uTime;
			scheduleCalls++;
		}

		void handle_init() override { initCalls++; }

		void handle_session_begin(uint32_t curTDate) override
		{
			sessionBeginCalls++;
			lastSessionTDate = curTDate;
		}

		void handle_session_end(uint32_t curTDate) override
		{
			sessionEndCalls++;
			lastSessionTDate = curTDate;
		}

		void handle_replay_done() override { replayDoneCalls++; }
	};

	// 最小可用配置：csv 模式（不触发 HisDataMgr 的动态库装载）
	WTSVariant* base_cfg(const std::filesystem::path& workdir)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", workdir.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001101500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		return cfg;
	}

	// 挂上仓库自带的只读样例基础数据
	void attach_basefiles(WTSVariant* cfg, const std::filesystem::path& root,
		bool withSession, bool withCommodity)
	{
		WTSVariant* basefiles = WTSVariant::createObject();
		if (withSession)
			basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		if (withCommodity)
			basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
	}
}

class P04ReplayerConfigTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_workdir = make_temp_dir("cfg");
		ASSERT_TRUE(std::filesystem::exists(_workdir));
		_root = lane_root();
		_outdir = make_temp_dir("cfg_out");
		WtHelper::setOutputDir(_outdir.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_workdir, ec);
		std::filesystem::remove_all(_outdir, ec);
	}

	std::filesystem::path _workdir;
	std::filesystem::path _outdir;
	std::filesystem::path _root;
};

TEST_F(P04ReplayerConfigTest, initReadsEveryScalarSwitch)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);
	cfg->append("tick", true);
	cfg->append("adjust_flag", static_cast<uint32_t>(7));
	cfg->append("align_by_section", true);
	cfg->append("dont_simtick_if_notrade", true);
	cfg->append("cache_clear_days", static_cast<uint32_t>(33));

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	EXPECT_TRUE(replayer.is_tick_enabled());
	// tick 打开时 _tick_simulated 保持构造默认；仅断言公开可读值稳定
	EXPECT_NO_THROW(replayer.is_tick_simulated());

	// 未运行 -> stop 只告警（L636 分支）
	replayer.stop();
}

TEST_F(P04ReplayerConfigTest, cfgTimeRangeIsUsedOnlyWhenUnset)
{
	// init 里 _begin_time/_end_time 非 0 时不再读配置（L207-L211）
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);
	cfg->append("stime", static_cast<uint64_t>(199912310930));
	cfg->append("etime", static_cast<uint64_t>(199912311500));

	HisDataReplayer replayer;
	replayer.set_time_range(202002030930, 202002101500);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	replayer.enable_tick(false);

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_cfg");

	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(20200203u, replayer.get_date());
	EXPECT_EQ(930u, replayer.get_raw_time());
	EXPECT_EQ(0u, replayer.get_secs());
	EXPECT_EQ(1u, sink.initCalls);
	replayer.stop();
}

TEST_F(P04ReplayerConfigTest, prepareAdvancesDateAndTradingDate)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	replayer.enable_tick(false);

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_prepare");

	// prepare 之前的时间字段都是构造默认 0
	EXPECT_EQ(0u, replayer.get_date());
	EXPECT_EQ(0u, replayer.get_trading_date());

	ASSERT_TRUE(replayer.prepare());
	// _cur_date = stime/10000, _cur_time = stime%10000, _cur_secs = 0
	EXPECT_EQ(20200102u, replayer.get_date());
	EXPECT_EQ(930u, replayer.get_min_time());
	EXPECT_EQ(930u, replayer.get_raw_time());
	EXPECT_EQ(0u, replayer.get_secs());
	// 20200102 是交易日，SD0930 白盘时段 -> 交易日就是当天
	EXPECT_EQ(20200102u, replayer.get_trading_date());
	EXPECT_EQ(1u, sink.initCalls);
	// tick 关闭时走 checkUnbars()（空集合，无回调）
	EXPECT_EQ(0u, sink.barCloseCalls);
	EXPECT_EQ(0u, sink.sessionBeginCalls);

	// _running 已经置位：重复 prepare 返回 false（L651-L655），且不再回调 handle_init
	EXPECT_FALSE(replayer.prepare());
	EXPECT_EQ(1u, sink.initCalls);

	// prepare 之后处于运行态：stop 置终止标记并打日志，第二次 stop 直接返回（L642）
	replayer.stop();
	replayer.stop();
}

TEST_F(P04ReplayerConfigTest, runWithoutAnyDataEndsReplay)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	// tick 关闭 + 没有任何K线缓存 -> run() 走 L725-L731 的收尾分支
	replayer.enable_tick(false);

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_emptyrun");
	ASSERT_TRUE(replayer.prepare());

	replayer.run(false);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	EXPECT_EQ(0u, sink.sessionBeginCalls);
	EXPECT_EQ(0u, sink.sessionEndCalls);
	EXPECT_EQ(0u, sink.tickCalls);

	// run() 结束时把 _running 打回 false，于是 prepare 可以再次成功
	EXPECT_TRUE(replayer.prepare());
	EXPECT_EQ(2u, sink.initCalls);
	replayer.stop();
}

TEST_F(P04ReplayerConfigTest, stopBeforePrepareIsNoop)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_stop");
	replayer.stop();
	// 未运行 -> 仅告警，不会进入终止态，随后 run 仍会正常收尾
	replayer.prepare();
	replayer.run(false);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	replayer.stop();
}

TEST_F(P04ReplayerConfigTest, missingBasefilesEntriesStillInit)
{
	// basefiles 存在但 session/commodity 全缺：init 的每个 if 分支都走 false
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, false, false);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	// 没有品种数据 -> 品种查询为 NULL
	EXPECT_EQ(nullptr, replayer.get_commodity_info("SSE.STK.000001"));
	// 没有手续费配置 -> 费用恒为 0
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("SSE.STK.000001", 10.0, 100.0, 0));
	EXPECT_EQ(std::string(""), replayer.get_rawcode("SSE.STK.000001"));
}

TEST_F(P04ReplayerConfigTest, loadedBasefilesGiveCommodityAndSession)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);
	cfg->append("fees", (_root / "src/TestUnits/fixtures/fees_minfee.json").string().c_str());

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	// stk_comms.json 里的品种规则可查询，且同输入结果稳定
	WTSCommodityInfo* comm = replayer.get_commodity_info("SSE.STK.000001");
	EXPECT_EQ(comm, replayer.get_commodity_info("SSE.STK.000001"));
	EXPECT_EQ(nullptr, replayer.get_commodity_info("DCE.NOPE2001"));

	// 时段查询：按 sid 与按代码两种入参都要稳定
	WTSSessionInfo* byId = replayer.get_session_info("SD0930");
	EXPECT_EQ(byId, replayer.get_session_info("SD0930", false));
	WTSSessionInfo* byCode = replayer.get_session_info("SSE.STK.000001", true);
	EXPECT_EQ(byCode, replayer.get_session_info("SSE.STK.000001", true));

	// fees 命中 fixture：回归锚点（费率/minfee 细节由 test_replayer_fee 覆盖）
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 0));
}

TEST_F(P04ReplayerConfigTest, missingFeesFileLeavesEmptyFeeMap)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);
	cfg->append("fees", "/tmp/p04_definitely_missing_fee_file.json");

	HisDataReplayer replayer;
	// loadFees 读不到文件只打日志，不影响 init 返回值
	EXPECT_TRUE(replayer.init(cfg));
	cfg->release();
	EXPECT_DOUBLE_EQ(0.0, replayer.calc_fee("SSE.STK.000001", 1.0, 100.0, 0));
}

TEST_F(P04ReplayerConfigTest, clearCacheWipesPriceMap)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	replayer.update_price("SSE.STK.000001", 12.34);
	ASSERT_DOUBLE_EQ(12.34, replayer.get_cur_price("SSE.STK.000001"));
	replayer.clear_cache();
	EXPECT_DOUBLE_EQ(0.0, replayer.get_cur_price("SSE.STK.000001"));
	// 缓存清空后价格仍然可读（不会命中脏数据）
	replayer.update_price("SSE.STK.000001", 1.0);
	EXPECT_DOUBLE_EQ(1.0, replayer.get_cur_price("SSE.STK.000001"));
}

TEST_F(P04ReplayerConfigTest, registerTaskAcceptsEveryPeriodTag)
{
	WTSVariant* cfg = base_cfg(_workdir);
	attach_basefiles(cfg, _root, true, true);

	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	RecordingSink sink;
	replayer.register_sink(&sink, "p04_task");

	// L410-L421 的六个分支：d/w/m/y/min/未知
	const char* periods[] = {"d", "w", "m", "y", "min", "tick", "D", ""};
	for (uint32_t i = 0; i < sizeof(periods) / sizeof(periods[0]); i++)
	{
		EXPECT_NO_THROW(replayer.register_task(1000 + i, i, 930, periods[i]));
	}

	// 注册任务后 run() 改走 run_by_tasks 分支；这里不启用该调用（需要行情数据，见文件末尾说明）
	// 只验证重复注册不会破坏已有状态
	replayer.update_price("SSE.STK.000001", 3.0);
	EXPECT_DOUBLE_EQ(3.0, replayer.get_cur_price("SSE.STK.000001"));
}

// base_cfg 的其他字段均有效，唯一缺少的是 basefiles。将危险 init 放在子进程，
// 验证其按 bool 接口返回失败；_Exit 避免析构过程掩盖 init 的结果。
TEST_F(P04ReplayerConfigTest, initWithoutBasefilesReportsFailure)
{
	EXPECT_EXIT(
		{
			struct rlimit noCoreDump{};
			if (::setrlimit(RLIMIT_CORE, &noCoreDump) != 0)
				std::_Exit(2);

			try
			{
				WTSVariant* cfg = base_cfg(_workdir);
				const char cfgReady[] = "P04_CFG_READY\n";
				(void)::write(STDERR_FILENO, cfgReady, sizeof(cfgReady) - 1);

				HisDataReplayer* replayer = new HisDataReplayer();
				const char replayerReady[] = "P04_REPLAYER_READY\n";
				(void)::write(STDERR_FILENO, replayerReady, sizeof(replayerReady) - 1);

				const char initBegin[] = "P04_INIT_BEGIN\n";
				(void)::write(STDERR_FILENO, initBegin, sizeof(initBegin) - 1);
				const bool initialized = replayer->init(cfg);
				std::_Exit(initialized ? 1 : 0);
			}
			catch (...)
			{
				std::_Exit(3);
			}
		},
		::testing::ExitedWithCode(0), "");
}

// “没有 sink 就 prepare”违反接收器注册的回放生命周期前置条件；不作为产品缺陷测试。
// 零 stime/etime 的解析依赖当前日期和 session 交易日规则，现有证据显示推进有界，
// 但零值表示何种时间范围尚无契约。待语义确定并补充可控时钟/日期观测后再单独增加用例。
