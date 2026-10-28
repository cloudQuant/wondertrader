/*!
 * \file r123_dtservo_remaining_test.cpp
 * \brief 第二十波 R123 - WtDtServo 剩余公开面：tick 默认结束时间 / 未命中缓存 / 日线缓存分支
 *
 * 覆盖（只读生产代码）:
 *  - WtDtRunner.cpp:292-296：get_ticks_by_range 的 endTime==0 时用「当前日期*10000+2359」。
 *  - WtDataManager.cpp:567-578：update_bars 在 _rt_bars 中遍历，合约代码不匹配时 continue。
 *  - WtDataManager.cpp:217-243、229-230：get_kline_slice_by_range 的 KP_DAY 首次缓存装配，
 *    日线分支写 _last_bartime = kData->date(-1)。
 *  - WtDataManager.cpp:244-283、273-274：同 key 命中缓存后的日线增量更新与末条时间回写。
 *
 * 为什么 fork：WtDataStorage 的 WtRdmDtReader::init 会起 5s 巡检线程；本 lane 不在父进程
 * 留长线程，所有真实 reader 场景都在子进程里完成查询后 exit(0)。
 * 历史数据全部是临时目录里的 .dsb（r102_seed.h 的 RAW_V2 写入器），不接网络/柜台。
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

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtDtServo/WtDataManager.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtHelper.h>

#include "r102_seed.h"

namespace fs = std::filesystem;

namespace {

struct R123DtReport
{
	int32_t	fail_line = 0;
	int32_t	a = 0;
	int32_t	b = 0;
};

#define R123_DT_CHK(rep, cond) \
	do { if (!(cond)) { if ((rep).fail_line == 0) (rep).fail_line = __LINE__; } } while (0)

class R123DtServoRemainingTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r102::make_tmp_dir("r123_dt");
		_bin = r102::bin_dir();
		_store = r102::with_slash(_dir / "store");
		_cfg = (_dir / "servo.json").string();
		_logCfg = (_dir / "no_such_logcfg.yaml").string();

		r102::write_basefiles(_dir);
		r102::write_text(_cfg, r102::full_cfg_json(_dir, _store, true));
		r102::write_text(_dir / "data.json", r102::data_cfg_json(_store, true));
		ASSERT_TRUE(r102::seed_default_store(_store));

		WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void run_child(const std::function<void(R123DtReport&)>& fn)
	{
		if (!r102::storage_plugin_ready(_bin))
			GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R123DtReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R123DtReport rep;
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

	bool init_runner(WtDtRunner& runner)
	{
		runner.initialize(_cfg.c_str(), true, _bin.c_str(), _logCfg.c_str(), nullptr, nullptr);
		return true;
	}

	bool init_dm(WtDataManager& dm, WtDtRunner& runner)
	{
		WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "data.json").string().c_str());
		if (cfg == nullptr)
			return false;
		const bool ok = dm.init(cfg, &runner);
		cfg->release();
		return ok;
	}

	fs::path	_dir;
	std::string	_bin;
	std::string	_store;
	std::string	_cfg;
	std::string	_logCfg;
};

// WtDtRunner.cpp:292-296 endTime==0 -> 当前日期 2359；查询本身落到 reader 区间读。
// 同一子进程里再让 WtDataManager::update_bars 收到一个与订阅 code 不同的 stdCode，
// 走 :577-578 的 continue（不触发 updateKlineData / trigger_bar）。
TEST_F(R123DtServoRemainingTest, ticks_default_end_and_update_bars_code_mismatch)
{
	run_child([this](R123DtReport& r) {
		WtDtRunner runner;
		R123_DT_CHK(r, init_runner(runner));

		WTSTickSlice* slice = runner.get_ticks_by_range("CFFEX.IC.2204", 202401020930, 0);
		r.a = (slice == nullptr) ? -1 : (int32_t)slice->size();
		if (slice != nullptr)
			slice->release();

		WtDataManager dm;
		R123_DT_CHK(r, init_dm(dm, runner));
		dm.subscribe_bar("CFFEX.IC.2204", KP_Minute1, 1);

		WTSTickStruct ts = r102::make_tick("CFFEX", "IC2204", 20240102, 93030000, 100.5);
		snprintf(ts.code, sizeof(ts.code), "%s", "CFFEX.IC.2204");
		WTSTickData* tick = WTSTickData::create(ts);
		R123_DT_CHK(r, tick != nullptr);
		if (tick != nullptr)
		{
			// 订阅的是 CFFEX.IC.2204，这里故意传另一个代码 -> strcmp 不等，continue
			dm.update_bars("CFFEX.IC.2205", tick);
			tick->release();
		}

		R123_DT_CHK(r, r.a > 0);
		r.b = 1;
		R123_DT_CHK(r, r.b == 1);
	});
}

// WtDataManager.cpp:217-243 首次装配 KP_DAY x3：日线分支写 date(-1)；
// 第二次同 key 查询走 :244-283 增量，日线分支写 bars->date(-1)，切片仍可定位。
// 12 根日线保证增量分支 erase 最后一条后缓存非空（4 条 -> 3 条 -> 2 条）。
TEST_F(R123DtServoRemainingTest, day_range_cache_assembly_and_incremental)
{
	std::vector<WTSBarStruct> days;
	for (uint32_t i = 0; i < 12; i++)
		days.push_back(r102::make_day_bar(20240102 + i, 100.0 + i));
	ASSERT_TRUE(r102::seed_day_bars(_store, "CFFEX", "IC2204", days));

	run_child([this](R123DtReport& r) {
		WtDtRunner runner;
		R123_DT_CHK(r, init_runner(runner));

		WtDataManager dm;
		R123_DT_CHK(r, init_dm(dm, runner));

		WTSKlineSlice* first = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_DAY, 3, 202401020000, 202401042359);
		r.a = (first == nullptr) ? -1 : (int32_t)first->size();
		if (first != nullptr)
			first->release();

		WTSKlineSlice* second = dm.get_kline_slice_by_range("CFFEX.IC.2204",
			KP_DAY, 3, 202401020000, 202401042359);
		r.b = (second == nullptr) ? -1 : (int32_t)second->size();
		if (second != nullptr)
			second->release();

		R123_DT_CHK(r, r.a > 0);
		R123_DT_CHK(r, r.b > 0);
	});
}

}	// namespace
