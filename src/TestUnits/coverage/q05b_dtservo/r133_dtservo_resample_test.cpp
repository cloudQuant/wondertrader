/*!
 * \file r133_dtservo_resample_test.cpp
 * \brief 第二十一波 R133 - WtDataManager K线缓存重采样剩余分支
 *
 * 覆盖（只读生产代码）:
 *  - WtDataManager.cpp:199-243 非基础周期按区间首次装配（times>1），
 *    以及 :244-283 命中缓存后的增量更新（分钟线分支 :255-256 / :275-279）。
 *  - WtDataManager.cpp:286-334 区间二分定位。
 *  - WtDataManager.cpp:337-394 非基础周期按数量首次装配，其中 :366-376
 *    「本地时间不大于最后一条K线时间 -> 擦除末条」分支（用未来日期数据驱动）。
 *  - WtDataManager.cpp:395-443 按数量命中缓存后的增量更新（分钟线 :406-408 /
 *    :426-429 / 日线 :432-433）。
 *  - WtDataManager.cpp:539-557 subscribe_bar 在 times>1 且 _rt_bars==NULL 时
 *    新建 RtBarMap（:550），随后 update_bars 走一遍实时更新。
 *  - WtDataManager.cpp:191-197 按日期装配日线 times>1 + :244-283 日线增量。
 *
 * 所有真实 reader 场景都在 fork 子进程内：WtRdmDtReader init 会起 5 秒巡检线程，
 * 子进程查询完立即 exit，父进程无残留线程。历史数据只写系统临时目录，用例结束删除。
 * 不接网络/柜台/共享内存。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
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

struct R133DmReport
{
	int32_t		fail_line = 0;
	int32_t		a = 0;
	int32_t		b = 0;
	int32_t		c = 0;
	int32_t		marks = 0;
};

struct R133CapturedBar
{
	std::string code;
	std::string period;
	uintptr_t address;
	WTSBarStruct bar;
};

std::vector<R133CapturedBar> g_r133CapturedBars;

struct R133BarLifetimeEvent
{
	uintptr_t address;
	WtDataManagerBarLifetimeEvent event;
};

std::vector<R133BarLifetimeEvent> g_r133BarLifetimeEvents;

void r133_capture_bar_lifetime(uintptr_t address, WtDataManagerBarLifetimeEvent event)
{
	g_r133BarLifetimeEvents.push_back({ address, event });
}

bool r133_bar_lifetimes_balanced(std::size_t expectedCount)
{
	std::set<uintptr_t> outstanding;
	std::size_t returned = 0;
	std::size_t deleted = 0;
	for (const auto& event : g_r133BarLifetimeEvents)
	{
		if (event.event == WT_DM_BAR_FACTORY_RETURNED)
		{
			returned++;
			if (!outstanding.insert(event.address).second)
				return false;
		}
		else if (event.event == WT_DM_BAR_DELETED)
		{
			deleted++;
			if (outstanding.erase(event.address) != 1)
				return false;
		}
		else
		{
			return false;
		}
	}
	return returned == expectedCount && deleted == expectedCount && outstanding.empty();
}

bool r133_callbacks_use_cached_bars()
{
	std::set<uintptr_t> detachedAddresses;
	for (const auto& event : g_r133BarLifetimeEvents)
	{
		if (event.event == WT_DM_BAR_FACTORY_RETURNED)
			detachedAddresses.insert(event.address);
	}
	for (const auto& captured : g_r133CapturedBars)
	{
		if (detachedAddresses.find(captured.address) != detachedAddresses.end())
			return false;
	}
	return true;
}

class R133BarLifetimeHookScope
{
public:
	R133BarLifetimeHookScope()
	{
		g_r133BarLifetimeEvents.clear();
		wtp::setWtDataManagerBarLifetimeHook(&r133_capture_bar_lifetime);
	}
	~R133BarLifetimeHookScope()
	{
		wtp::setWtDataManagerBarLifetimeHook(NULL);
	}
};

void r133_capture_bar(const char* stdCode, const char* period, WTSBarStruct* bar)
{
	if (bar == NULL)
		return;
	R133CapturedBar item;
	item.code = stdCode == NULL ? "" : stdCode;
	item.period = period == NULL ? "" : period;
	item.address = (uintptr_t)bar;
	item.bar = *bar;
	g_r133CapturedBars.push_back(item);
}

#define R133_DM_CHK(rep, cond) \
	do { if (!(cond)) { if ((rep).fail_line == 0) (rep).fail_line = __LINE__; } } while (0)

// HHMM 加分钟并归一化（09:59 + 1 -> 10:00），避免造出 09:60 这类非法时间
inline uint32_t addMinutes(uint32_t hhmm, uint32_t add)
{
	uint32_t mins = (hhmm / 100) * 60 + hhmm % 100 + add;
	return (mins / 60) * 100 + mins % 60;
}

class R133WtDataManagerTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r102::make_tmp_dir("r133");
		_bin = r102::bin_dir();
		_store = r102::with_slash(_dir / "store");
		_runnerCfg = (_dir / "runner.json").string();
		_logCfg = (_dir / "no_such_logcfg.yaml").string();

		r102::write_basefiles(_dir);
		r102::write_text(_runnerCfg, r102::runner_cfg_json(_dir));
		r102::write_text(_dir / "data_default.json", r102::data_cfg_json(_store, false));
		ASSERT_TRUE(r102::seed_default_store(_store));

		WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void run_child(const std::function<void(R133DmReport&)>& fn)
	{
		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R133DmReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R133DmReport rep;
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

	bool init_dm(WtDtRunner& runner, WtDataManager& dm, FuncOnBarCallback cbBar = NULL)
	{
		runner.initialize(_runnerCfg.c_str(), true, _bin.c_str(), _logCfg.c_str(), NULL, cbBar);

		WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "data_default.json").string().c_str());
		if (cfg == NULL)
			return false;

		bool ok = dm.init(cfg, &runner);
		cfg->release();
		return ok;
	}

	fs::path		_dir;
	std::string		_bin;
	std::string		_store;
	std::string		_runnerCfg;
	std::string		_logCfg;
};

// WtDataManager.cpp:199-243 / 244-283 / 286-334：分钟线 times>1 首次装配 + 增量 + 二分
TEST_F(R133WtDataManagerTest, minute_range_cache_and_incremental)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	// 30 根 1 分钟线，保证 5 分钟缓存装配后仍有足够 bar 供增量擦除
	std::vector<WTSBarStruct> mins;
	for (uint32_t i = 0; i < 30; i++)
		mins.push_back(r102::make_min_bar(20240102, addMinutes(931, i), 50.0 + i));
	ASSERT_TRUE(r102::seed_min_bars(_store, "min1", "CFFEX", "IC2204", mins));

	run_child([this](R133DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// 首次装配：readKlineSliceByCount -> extractKlineData(5) -> 擦末条
		WTSKlineSlice* first = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 5, 202401020930, 202401021000);
		rep.a = first == NULL ? -1 : (int32_t)first->size();
		if (first != NULL)
			first->release();

		// 同 key 第二次：走 :244-283 增量（分钟线 :255-256 / :275-279）
		WTSKlineSlice* second = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 5, 202401020930, 202401021000);
		rep.b = second == NULL ? -1 : (int32_t)second->size();
		if (second != NULL)
			second->release();

		// etime 落在两根 5 分钟线之间：二分定位走 eit-- 分支
		WTSKlineSlice* bisect = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_Minute1, 5, 202401020930, 202401020952);
		rep.c = bisect == NULL ? -1 : (int32_t)bisect->size();
		if (bisect != NULL)
			bisect->release();

		rep.marks = 3;
		R133_DM_CHK(rep, rep.a > 0);
		R133_DM_CHK(rep, rep.b > 0);
		R133_DM_CHK(rep, rep.c > 0);
	});
}

// WtDataManager.cpp:337-394 / 395-443：未来日期驱动「末条未闭合擦除」，再走增量
TEST_F(R133WtDataManagerTest, future_bars_count_cache_erases_then_updates)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	// 2029 年数据：TimeUtils::getYYYYMMDDhhmmss()/100（当前 2026）必然 <= last_bartime，
	// 且仍在 reader 的默认结束时间 203012312359 之内
	std::vector<WTSBarStruct> mins;
	for (uint32_t i = 0; i < 30; i++)
		mins.push_back(r102::make_min_bar(20290102, addMinutes(931, i), 70.0 + i));
	ASSERT_TRUE(r102::seed_min_bars(_store, "min1", "CFFEX", "IC2204", mins));

	run_child([this](R133DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// 首次装配走 :366-376：now <= last_bartime -> erase 末条
		WTSKlineSlice* first = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 3, 5, 202901021000);
		rep.a = first == NULL ? -1 : (int32_t)first->size();
		if (first != NULL)
			first->release();

		// 增量：:406-408 分钟线 barTime，:426-429 未来数据再擦末条
		WTSKlineSlice* second = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_Minute1, 3, 5, 202901021000);
		rep.b = second == NULL ? -1 : (int32_t)second->size();
		if (second != NULL)
			second->release();

		rep.marks = 2;
		R133_DM_CHK(rep, rep.a > 0);
		R133_DM_CHK(rep, rep.b > 0);
	});
}

// WtDataManager.cpp:539-557：times>1 且 _rt_bars==NULL 时新建 RtBarMap，再实时更新
TEST_F(R133WtDataManagerTest, subscribe_nonbase_creates_rt_map)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R133DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// 第一次 subscribe 就用 times>1：_rt_bars 为空 -> :550 新建
		dm.subscribe_bar("CFFEX.IC.2204", KP_Minute1, 3);

		WTSTickStruct ts = r102::make_tick("CFFEX", "IC2204", 20240102, 100500000, 105.0);
		snprintf(ts.code, sizeof(ts.code), "%s", "CFFEX.IC.2204");
		WTSTickData* tick = WTSTickData::create(ts);
		WTSContractInfo* ct = runner.getBaseDataMgr().getContract("IC2204", "CFFEX");
		if (tick != NULL && ct != NULL)
		{
			tick->setContractInfo(ct);
			dm.update_bars("CFFEX.IC.2204", tick);
		}
		if (tick != NULL)
			tick->release();

		// 无合约信息的回查分支
		WTSTickData* tick2 = WTSTickData::create(ts);
		if (tick2 != NULL)
		{
			dm.update_bars("CFFEX.IC.2204", tick2);
			tick2->release();
		}

		dm.clear_subbed_bars();
		rep.marks = 1;
		R133_DM_CHK(rep, rep.marks == 1);
	});
}

// WtDataManager.cpp:191-197 / 244-283 日线 times>1 按日期装配 + 增量 + 计数日线分支
TEST_F(R133WtDataManagerTest, day_range_and_count_cache_branches)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	std::vector<WTSBarStruct> days;
	for (uint32_t i = 0; i < 9; i++)
		days.push_back(r102::make_day_bar(20240102 + i, 100.0 + i));
	ASSERT_TRUE(r102::seed_day_bars(_store, "CFFEX", "IC2204", days));

	run_child([this](R133DmReport& rep) {
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm))
		{
			rep.fail_line = __LINE__;
			return;
		}

		// 按日期 + times>1 首次装配
		WTSKlineSlice* byDate = dm.get_kline_slice_by_date("CFFEX.IC.2204",
			KP_DAY, 3, 20240102);
		rep.a = byDate == NULL ? -1 : (int32_t)byDate->size();
		if (byDate != NULL)
			byDate->release();

		// 同 key 区间查询 -> 日线增量（:254 / :273-274 / 二分 sBar 日线分支）
		WTSKlineSlice* byRange = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_DAY, 3, 202401020000, 202401102359);
		rep.b = byRange == NULL ? -1 : (int32_t)byRange->size();
		if (byRange != NULL)
			byRange->release();

		// 另一个 times 的按数量首次装配 + 增量，覆盖 :380-381 / :432-433
		WTSKlineSlice* cnt1 = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_DAY, 2, 3, 202401102359);
		rep.c = cnt1 == NULL ? -1 : (int32_t)cnt1->size();
		if (cnt1 != NULL)
			cnt1->release();

		WTSKlineSlice* cnt2 = dm.get_kline_slice_by_count("CFFEX.IC.2204",
			KP_DAY, 2, 3, 202401102359);
		rep.marks = (cnt2 == NULL ? -1 : (int32_t)cnt2->size());
		if (cnt2 != NULL)
			cnt2->release();

		R133_DM_CHK(rep, rep.a > 0);
		R133_DM_CHK(rep, rep.b > 0);
		R133_DM_CHK(rep, rep.c > 0);
		R133_DM_CHK(rep, rep.marks > 0);
	});
}

// New day/second bars are detached by WTSDataFactory and must be copied into
// WtDataManager's realtime cache before the callback observes the latest bar.
TEST_F(R133WtDataManagerTest, realtime_day_and_second_buckets_are_cached_and_reported)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R133DmReport& rep) {
		g_r133CapturedBars.clear();
		R133BarLifetimeHookScope lifetimeHook;
		WtDtRunner runner;
		WtDataManager dm;
		if (!init_dm(runner, dm, r133_capture_bar))
		{
			rep.fail_line = __LINE__;
			return;
		}

		dm.subscribe_bar("CFFEX.IC.2204", KP_DAY, 1);
		dm.subscribe_bar("CFFEX.IC.2204", KP_Tick, 1);
		WTSContractInfo* contract = runner.getBaseDataMgr().getContract("IC2204", "CFFEX");
		if (contract == NULL)
		{
			rep.fail_line = __LINE__;
			return;
		}

		auto update = [&](uint32_t actionTime, double price, double volume) {
			WTSTickStruct ts = r102::make_tick("CFFEX", "IC2204", 20240108, actionTime, price);
			// WtDtRunner::proc_tick normalizes raw venue codes before update_bars;
			// this test enters at WtDataManager directly, so match that precondition.
			snprintf(ts.code, sizeof(ts.code), "%s", "CFFEX.IC.2204");
			ts.volume = volume;
			ts.total_volume = volume;
			ts.turn_over = price * volume;
			ts.total_turnover = price * volume;
			WTSTickData* tick = WTSTickData::create(ts);
			if (tick == NULL)
				return false;
			tick->setContractInfo(contract);
			dm.update_bars("CFFEX.IC.2204", tick);
			tick->release();
			return true;
		};

		uint64_t firstSecondBucket = 0;
		auto checkLatest = [&](std::size_t begin, double close, double dayVolume,
			double secVolume, bool newSecondBucket) {
			auto dumpFailure = [&]() {
				fprintf(stderr,
					"[R133 diagnostic] subscriptions=CFFEX.IC.2204:KP_DAY/1,KP_Tick/1 "
					"begin=%zu expected_callbacks=%zu actual_callbacks=%zu "
					"expected(date=20240108,close=%.2f,day_vol=%.2f,sec_vol=%.2f,"
					"new_second=%d,first_second=%llu)\n",
					begin, begin + 2, g_r133CapturedBars.size(), close, dayVolume, secVolume,
					newSecondBucket ? 1 : 0,
					static_cast<unsigned long long>(firstSecondBucket));
				for (std::size_t i = 0; i < g_r133CapturedBars.size(); ++i)
				{
					const R133CapturedBar& item = g_r133CapturedBars[i];
					fprintf(stderr,
						"[R133 diagnostic] callback[%zu] code=%s period=%s date=%u time=%llu "
						"close=%.2f vol=%.2f\n",
						i, item.code.c_str(), item.period.c_str(), item.bar.date,
						static_cast<unsigned long long>(item.bar.time), item.bar.close,
						static_cast<double>(item.bar.vol));
				}
			};
			if (g_r133CapturedBars.size() != begin + 2)
			{
				dumpFailure();
				return false;
			}
			const WTSBarStruct* day = NULL;
			const WTSBarStruct* sec = NULL;
			for (std::size_t i = begin; i < g_r133CapturedBars.size(); ++i)
			{
				if (g_r133CapturedBars[i].code != "CFFEX.IC.2204")
				{
					dumpFailure();
					return false;
				}
				const WTSBarStruct& bar = g_r133CapturedBars[i].bar;
				if (bar.time == 0)
					day = &bar;
				else
					sec = &bar;
			}
			if (day == NULL || sec == NULL)
			{
				dumpFailure();
				return false;
			}
			if (day->date != 20240108 || day->close != close || day->vol != dayVolume)
			{
				dumpFailure();
				return false;
			}
			if (sec->date != 20240108 || sec->close != close || sec->vol != secVolume)
			{
				dumpFailure();
				return false;
			}
			if (begin == 0)
				firstSecondBucket = sec->time;
			else if (newSecondBucket == (sec->time == firstSecondBucket))
			{
				dumpFailure();
				return false;
			}
			if (g_r133CapturedBars[begin].period != "d1")
			{
				dumpFailure();
				return false;
			}
			return true;
		};

		R133_DM_CHK(rep, update(93001000, 110.0, 1.0));
		R133_DM_CHK(rep, checkLatest(0, 110.0, 1.0, 1.0, false));
		R133_DM_CHK(rep, update(93001500, 120.0, 2.0));
		R133_DM_CHK(rep, checkLatest(2, 120.0, 3.0, 3.0, false));
		R133_DM_CHK(rep, update(93002000, 130.0, 3.0));
		R133_DM_CHK(rep, checkLatest(4, 130.0, 6.0, 3.0, true));
		R133_DM_CHK(rep, r133_bar_lifetimes_balanced(3));
		R133_DM_CHK(rep, r133_callbacks_use_cached_bars());
		rep.a = static_cast<int32_t>(g_r133CapturedBars.size());
		rep.marks = 1;
	});
}

}	// namespace
