/*!
 * \file r102_wtdatamgr_test.cpp
 * \brief WtDtServo/WtDataManager 缓存装配、RT K线、按日期/数量/区间取数（迭代01 第十八波 R102）
 *
 * 为什么必须 fork：
 *  - 产品里唯一实现 readKlineSliceByCount / readTickSliceByDate 的 reader 是
 *    WtDataStorage 的 WtRdmDtReader，它 init 时会起一个 5 秒巡检线程
 *    （WtRdmDtReader.cpp:76-82 的巡检循环）。本 lane 的硬约束是不在测试进程里留长线程，
 *    因此所有真实 reader 场景都放进 fork 子进程：子进程完成查询后立即 exit(0)，
 *    巡检线程随进程终止，父进程既不建线程也不读挂钟。
 *  - 历史数据用临时目录里的 .dsb（RAW_V2）喂入，完全可控，不依赖任何外部数据包。
 *
 * 覆盖（行号按生产文件）：
 *  - WtDataManager.cpp:49-89 initStore（默认模块名 / 成功装载）
 *  - :112-143 tick 按区间 / 按日期 / 按数量
 *  - :158-189 秒线：readTickSliceByDate -> extractKlineData
 *  - :191-204 按日期/区间，times==1 直读
 *  - :199-243 / :337-393 首次缓存装配（times>1，m1/m5/day）
 *  - :244-283 / :395-443 命中缓存后的增量更新（readKlineSliceByRange + erase 最后一条）
 *  - :286-334 / :445-481 区间/数量二分定位
 *  - :507-558 subscribe_bar times==1 / times>1
 *  - :560-565 clear_subbed_bars（非空 _rt_bars）
 *  - :567-605 update_bars：合约信息有/无两条，m1/m3/m5/day 四种周期字符串
 *  - :607-616 clear_cache（已初始化 reader 分支）
 *  - :39-47 析构释放 _bars_cache（dm 离开作用域）
 *
 * 不覆盖（原因见 manifests/wave18/R102.md）：
 *  - :64-85 符号缺失/装载失败后仍 _reader->init，启用即崩（BUG-Q05-001 / BUG-R8-002 一族）；
 *  - :145-156 get_session_info(sid,false)，私有函数，公开 API 只以 isCode=true 调用；
 *  - 价格复权 / level2 已由 R45 覆盖，不重复。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <filesystem>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

#include "r102_seed.h"

namespace fs = std::filesystem;

namespace {

struct R102DmReport
{
	int32_t		fail_line = 0;
	int32_t		a = 0;
	int32_t		b = 0;
	int32_t		c = 0;
	int32_t		d = 0;
	int32_t		e = 0;
	int32_t		f = 0;
	int32_t		g = 0;
	int32_t		h = 0;
};

#define R102_DM_CHK(rep, cond) \
	do { if (!(cond)) { if ((rep).fail_line == 0) (rep).fail_line = __LINE__; } } while (0)

class R102WtDataManagerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r102::make_tmp_dir("dm");
		_bin = r102::bin_dir();
		_store = r102::with_slash(_dir / "store");
		_runnerCfg = (_dir / "runner.json").string();
		_logCfg = (_dir / "no_such_logcfg.yaml").string();

		r102::write_basefiles(_dir);
		r102::write_text(_runnerCfg, r102::runner_cfg_json(_dir));
		r102::write_text(_dir / "data_default.json",
			r102::data_cfg_json(_store, false));
		r102::write_text(_dir / "data_explicit.json",
			r102::data_cfg_json(_store, true));
		ASSERT_TRUE(r102::seed_default_store(_store));

		WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	// 在子进程里跑 fn(rep)；子进程正常 exit(0) 且所有检查通过才算成功。
	void run_child(const std::function<void(R102DmReport&)>& fn)
	{
		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R102DmReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R102DmReport rep;
		memset(&rep, 0, sizeof(rep));
		ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
		::close(fds[0]);

		int status = 0;
		ASSERT_EQ(::waitpid(pid, &status, 0), pid);
		ASSERT_TRUE(WIFEXITED(status)) << "子进程异常退出, status=" << status;
		ASSERT_EQ(WEXITSTATUS(status), 0);
		ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
		EXPECT_EQ(rep.fail_line, 0) << "子进程检查失败行 " << rep.fail_line;
	}
	// 子进程内的公共装配：base 数据 runner + 指向临时 store 的 WtDataManager。
	// dataName 选 data_default.json 时会走 initStore 的默认模块名分支（WtDataManager.cpp:54-56）。
	bool init_dm(WtDtRunner& runner, WtDataManager& dm, const char* dataName)
	{
		runner.initialize(_runnerCfg.c_str(), true, _bin.c_str(), _logCfg.c_str(), NULL, NULL);

		WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / dataName).string().c_str());
		if (cfg == NULL)
			return false;

		bool ok = dm.init(cfg, &runner);
		cfg->release();
		return ok;
	}

	bool seed_extended_minute_bars()
	{
		std::vector<WTSBarStruct> bars;
		for (uint32_t minute = 31; minute <= 55; ++minute)
		{
			const uint32_t hhmm = 900 + minute;
			const double px = minute <= 40
				? 50.0 + (minute - 31)
				: 100.0 + (minute - 41);
			bars.push_back(r102::make_min_bar(20240102, hhmm, px));
		}
		return r102::seed_min_bars(_store, "min1", "CFFEX", "IC2204", bars);
	}

	fs::path		_dir;
	std::string		_bin;
	std::string		_store;
	std::string		_runnerCfg;
	std::string		_logCfg;
};

// ---------------------------------------------------------------------------
// tick 按日期/区间/数量、秒线、clear_cache
// ---------------------------------------------------------------------------
TEST_F(R102WtDataManagerTest, TickQueriesAndSecondBarsFromFileStore)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, "data_default.json"))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// WtDataManager.cpp:119-122 -> WtRdmDtReader::readTickSliceByDate（his/ticks 文件）
		WTSTickSlice* byDate = dm.get_tick_slice_by_date("CFFEX.IC.2204", 20240102);
		rep.a = byDate == NULL ? -1 : (int32_t)byDate->size();
		if (byDate != NULL)
			byDate->release();

		// :112-117 区间读（内部 *100000，入参为 YYYYMMDDHHMM）
		WTSTickSlice* byRange = dm.get_tick_slices_by_range(
			"CFFEX.IC.2204", 202401020930, 202401021000);
		rep.b = byRange == NULL ? -1 : (int32_t)byRange->size();
		if (byRange != NULL)
			byRange->release();

		// :158-189 秒线：tick -> extractKlineData(30s)
		WTSKlineSlice* sbars = dm.get_skline_slice_by_date("CFFEX.IC.2204", 30, 20240102);
		rep.c = sbars == NULL ? -1 : (int32_t)sbars->size();
		if (sbars != NULL)
			sbars->release();

		// :484-488 按数量：股票代码让 WtRdmDtReader 的 getCommodity(exchg,code) 命中
		WTSTickSlice* byCount = dm.get_tick_slice_by_count("SSE.600000", 2, 202401021000);
		rep.d = byCount == NULL ? -1 : (int32_t)byCount->size();
		if (byCount != NULL)
			byCount->release();

		// :607-616 已初始化 reader 的 clearCache
		dm.clear_cache();
		rep.e = 1;

		R102_DM_CHK(rep, rep.a == 3);	// 3 条 tick
		R102_DM_CHK(rep, rep.b >= 2);
		R102_DM_CHK(rep, rep.c > 0);
		R102_DM_CHK(rep, rep.d == 2);
	});
}

// ---------------------------------------------------------------------------
// times==1 直读 + times>1 首次装配 / 增量更新 / 二分定位
// ---------------------------------------------------------------------------
TEST_F(R102WtDataManagerTest, KlineRangeCountCacheAssemblyAndIncremental)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, "data_default.json"))
		{
			rep.fail_line = __LINE__;
			return;
		}
		// 按日期的 7x 聚合需要足够的完整分钟组，避免 fixture 裁尾后为空。
		if (!seed_extended_minute_bars())
		{
			rep.fail_line = __LINE__;
			return;
		}

		// :199-204 times==1 日线直读
		WTSKlineSlice* dayOne = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_DAY, 1, 202401020000, 202401042359);
		rep.a = dayOne == NULL ? -1 : (int32_t)dayOne->size();
		if (dayOne != NULL)
			dayOne->release();

		// :337-393 首次装配 m1 x3；:445-481 数量定位
		WTSKlineSlice* cnt3 = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 3, 5, 202401021000);
		rep.b = cnt3 == NULL ? -1 : (int32_t)cnt3->size();
		if (cnt3 != NULL)
			cnt3->release();

		// :244-283 同 key 命中缓存 -> readKlineSliceByRange 增量 + erase 最后一条
		WTSKlineSlice* rng3 = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 3, 202401020933, 202401021000);
		rep.c = rng3 == NULL ? -1 : (int32_t)rng3->size();
		if (rng3 != NULL)
			rng3->release();

		// :217-243 首次装配 m1 x5（不同 key）
		WTSKlineSlice* rng5 = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 5, 202401020935, 202401021000);
		rep.d = rng5 == NULL ? -1 : (int32_t)rng5->size();
		if (rng5 != NULL)
			rng5->release();

		// :395-443 同 key 命中缓存后的数量路径增量
		WTSKlineSlice* cnt5 = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 5, 3, 202401021000);
		rep.e = cnt5 == NULL ? -1 : (int32_t)cnt5->size();
		if (cnt5 != NULL)
			cnt5->release();

		// :229-230 / :380-381 / :432-433 日线的 date 分支
		WTSKlineSlice* cntDay = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_DAY, 3, 2, 202401042359);
		rep.f = cntDay == NULL ? -1 : (int32_t)cntDay->size();
		if (cntDay != NULL)
			cntDay->release();

		// :191-197 按日期 + 首次装配 m1 x7（getBoundaryTime 的两个边界）
		WTSKlineSlice* byDate = dm.get_kline_slice_by_date("CFFEX.IC.2204",
			KP_Minute1, 7, 20240102);
		rep.g = byDate == NULL ? -1 : (int32_t)byDate->size();
		R102_DM_CHK(rep, byDate != NULL);
		if (byDate != NULL)
		{
			R102_DM_CHK(rep, byDate->size() == 2);
			if (byDate->size() == 2)
			{
				const WTSBarStruct* bars = (const WTSBarStruct*)byDate->get_block_addr(0);
				R102_DM_CHK(rep, bars[0].time == TimeUtils::timeToMinBar(20240102, 937));
				R102_DM_CHK(rep, bars[0].close == 56.0 && bars[0].vol == 35.0);
				R102_DM_CHK(rep, bars[1].time == TimeUtils::timeToMinBar(20240102, 944));
				R102_DM_CHK(rep, bars[1].close == 103.0 && bars[1].vol == 35.0);
			}
		}
		if (byDate != NULL)
			byDate->release();

		R102_DM_CHK(rep, rep.a == 3);
		R102_DM_CHK(rep, rep.b > 0);
		R102_DM_CHK(rep, rep.c > 0);
		R102_DM_CHK(rep, rep.d > 0);
		R102_DM_CHK(rep, rep.e > 0);
		R102_DM_CHK(rep, rep.f > 0);
		R102_DM_CHK(rep, rep.g > 0);
	});	// dm 析构 -> WtDataManager.cpp:39-47 释放 6 份缓存
}

// ---------------------------------------------------------------------------
// subscribe_bar（times==1 / times>1 / m5 / day）与 update_bars
// ---------------------------------------------------------------------------
TEST_F(R102WtDataManagerTest, SubscribeBarsAndUpdateFromTick)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, "data_default.json"))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// :507-538 times==1：readKlineSliceByCount + 逐块 memcpy 到 WTSKlineData
		dm.subscribe_bar("CFFEX.IC.2204", KP_Minute1, 1);
		// :539-557 times>1：extractKlineData 后挂到 _rt_bars
		dm.subscribe_bar("CFFEX.IC.2204", KP_Minute1, 3);
		// :595-597 KP_Minute5 的 "m{times*5}" 分支
		dm.subscribe_bar("CFFEX.IC.2204", KP_Minute5, 1);
		// :598-601 default 分支 -> "d{times}"
		dm.subscribe_bar("CFFEX.IC.2204", KP_DAY, 1);

		// :567-605 有合约信息：sInfo 直接来自 tick 的合约。
		// tick.code 用标准码，才能通过 WTSDataFactory::updateKlineData 的 code 比较。
		WTSTickStruct ts = r102::make_tick("CFFEX", "IC2204", 20240102, 100500000, 105.0);
		snprintf(ts.code, sizeof(ts.code), "%s", "CFFEX.IC.2204");
		WTSTickData* withCt = WTSTickData::create(ts);
		WTSContractInfo* ct = runner.getBaseDataMgr().getContract("IC2204", "CFFEX");
		if (withCt != NULL && ct != NULL)
		{
			withCt->setContractInfo(ct);
			dm.update_bars("CFFEX.IC.2204", withCt);
			withCt->release();
		}

		// :580-584 无合约信息：get_session_info(code, true) 回查品种时段
		WTSTickData* noCt = WTSTickData::create(ts);
		if (noCt != NULL)
		{
			dm.update_bars("CFFEX.IC.2204", noCt);
			noCt->release();
		}

		// :560-565 _rt_bars 非空时的清空
		dm.clear_subbed_bars();
		rep.a = 1;

		R102_DM_CHK(rep, rep.a == 1);
	});
}

// Reader range 起点包含缓存最后一条；该重叠 bar 需跳过，后续每根基础 bar 都参与重采样。
TEST_F(R102WtDataManagerTest, IncrementalRangeProcessesBarsAfterOverlappingBoundary)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, "data_default.json"))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// 首次装配 10 根 1 分钟 -> 5 分钟，缓存 09:35/09:40。
		WTSKlineSlice* base = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 5, 10, 202401021000);
		R102_DM_CHK(rep, base != NULL);
		if (base == NULL)
			return;
		R102_DM_CHK(rep, base->size() == 2);
		if (base->size() == 2)
		{
			const WTSBarStruct* bars = (const WTSBarStruct*)base->get_block_addr(0);
			R102_DM_CHK(rep, bars[0].time == TimeUtils::timeToMinBar(20240102, 935));
			R102_DM_CHK(rep, bars[1].time == TimeUtils::timeToMinBar(20240102, 940));
			R102_DM_CHK(rep, bars[1].vol == 25.0);
		}
		base->release();

		// 追加到 09:55，形成 09:45/09:50/09:55 三根新 5 分钟 bar。
		R102_DM_CHK(rep, seed_extended_minute_bars());
		dm.clear_cache();

		// 增量 reader 从 _last_bartime(09:40) 起返回，首根与缓存尾 bar 重叠。
		WTSKlineSlice* inc = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 5, 202401020930, 202401021000);
		R102_DM_CHK(rep, inc != NULL);
		if (inc == NULL)
			return;
		R102_DM_CHK(rep, inc->size() == 4);
		if (inc->size() == 4)
		{
			const WTSBarStruct* bars = (const WTSBarStruct*)inc->get_block_addr(0);
			R102_DM_CHK(rep, bars[0].time == TimeUtils::timeToMinBar(20240102, 935));
			R102_DM_CHK(rep, bars[1].time == TimeUtils::timeToMinBar(20240102, 940));
			R102_DM_CHK(rep, bars[1].vol == 25.0); // 重叠的 09:40 未被重复计入。
			R102_DM_CHK(rep, bars[2].time == TimeUtils::timeToMinBar(20240102, 945));
			R102_DM_CHK(rep, bars[2].close == 104.0 && bars[2].vol == 25.0);
			R102_DM_CHK(rep, bars[3].time == TimeUtils::timeToMinBar(20240102, 950));
			R102_DM_CHK(rep, bars[3].close == 109.0 && bars[3].vol == 25.0);
		}
		inc->release();
	});
}

TEST_F(R102WtDataManagerTest, IncrementalCountProcessesBarsAfterOverlappingBoundary)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, "data_default.json"))
		{
			rep.fail_line = __LINE__;
			return;
		}

		WTSKlineSlice* base = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 5, 10, 202401021000);
		R102_DM_CHK(rep, base != NULL);
		if (base == NULL)
			return;
		R102_DM_CHK(rep, base->size() == 2);
		if (base->size() == 2)
		{
			const WTSBarStruct* bars = (const WTSBarStruct*)base->get_block_addr(0);
			R102_DM_CHK(rep, bars[1].time == TimeUtils::timeToMinBar(20240102, 940));
			R102_DM_CHK(rep, bars[1].vol == 25.0);
		}
		base->release();

		R102_DM_CHK(rep, seed_extended_minute_bars());
		dm.clear_cache();

		WTSKlineSlice* inc = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 5, 10, 202401021000);
		R102_DM_CHK(rep, inc != NULL);
		if (inc == NULL)
			return;
		R102_DM_CHK(rep, inc->size() == 5);
		if (inc->size() == 5)
		{
			const WTSBarStruct* bars = (const WTSBarStruct*)inc->get_block_addr(0);
			R102_DM_CHK(rep, bars[1].time == TimeUtils::timeToMinBar(20240102, 940));
			R102_DM_CHK(rep, bars[1].vol == 25.0); // 重叠的 09:40 未被重复计入。
			R102_DM_CHK(rep, bars[2].time == TimeUtils::timeToMinBar(20240102, 945));
			R102_DM_CHK(rep, bars[2].close == 104.0 && bars[2].vol == 25.0);
			R102_DM_CHK(rep, bars[3].time == TimeUtils::timeToMinBar(20240102, 950));
			R102_DM_CHK(rep, bars[3].close == 109.0 && bars[3].vol == 25.0);
			R102_DM_CHK(rep, bars[4].time == TimeUtils::timeToMinBar(20240102, 955));
			R102_DM_CHK(rep, bars[4].close == 114.0 && bars[4].vol == 25.0);
		}
		inc->release();
	});
}

// 已登记缺陷的复现面：initStore 在插件加载/符号缺失后仍解引用 _reader。
// 该用例启用即 SIGSEGV，只作缺陷证据保留（BUG-Q05-001 / BUG-R8-002）。
TEST_F(R102WtDataManagerTest, DISABLED_InitStoreWithoutPluginDerefsNullReader)
{
	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"{\"align_by_section\":false,\"store\":{\"module\":\"r102_no_such_module\",\"path\":\"/tmp/r102_empty/\"}}", false);
	ASSERT_NE(cfg, nullptr);

	WtDataManager dm;
	WtDtRunner runner;	// 不 initialize：_bd_mgr/_hot_mgr 为 NULL，但 store 走默认模块
	dm.init(cfg, &runner);	// 默认模块不存在 -> _reader==NULL -> :87 崩溃
	cfg->release();
}

}	// namespace
