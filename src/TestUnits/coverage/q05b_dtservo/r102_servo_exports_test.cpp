/*!
 * \file r102_servo_exports_test.cpp
 * \brief WtDtServo/WtDtServo.cpp C 导出层的成功回调路径（迭代01 第十八波 R102）
 *
 * WtDtServo.cpp 的 8 个查询导出只有在 `getRunner()` 返回真实切片时才会走
 * `cbCnt + 逐块 cb + release` 的循环体。全局 runner 是进程级单例，r45/r85 的用例
 * 明确依赖「未初始化」，如果在父进程成功 initialize 会把后续所有用例带进半初始化态。
 * 因此本文件在 fork 子进程里初始化全局 runner：子进程用临时目录里的 .dsb 数据
 * 完成查询后 exit(0)，父进程的全局 runner 始终未初始化，测试顺序互不影响。
 *
 * 覆盖（WtDtServo.cpp 行号）：
 *  - :55-58 initialize 成功路径（basefiles + data.store，AD 之外的 WtDataStorage reader）
 *  - :76-94  get_bars_by_range 成功（日线 times==1）
 *  - :96-114 get_bars_by_date 成功（m5 -> 交易日边界）
 *  - :116-138 get_ticks_by_range 成功
 *  - :140-158 get_bars_by_count 成功（m5 times==1 -> readKlineSliceByCount）
 *  - :160-182 get_ticks_by_count 成功（股票代码，避开 R23 已登记的期货查表崩溃）
 *  - :184-206 get_ticks_by_date 成功
 *  - :208-226 get_sbars_by_date 成功（tick -> 30s 合成）
 *  - :233-236 subscribe_bar 转发
 *  - :238-241 clear_cache 转发
 *
 * 子进程里 WtRdmDtReader 会起 5s 巡检线程；它在查询完成后随 exit(0) 终止，
 * 父进程不建线程、不等待、不 sleep。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSStruct.h>
#include <WtDtServo/WtDtRunner.h>
#include <WtDtServo/WtDtServo.h>
#include <WtDtServo/WtHelper.h>

#include "r102_seed.h"

// WtDtServo.cpp:49-53 定义、供全部 C 导出使用的全局 runner 访问器。
// C 导出的 initialize 会把模块目录钉成可执行体所在的 TestUnits/（插件实际在上一级），
// 无法在这里成功装载 store；直接对同一个单例调用 WtDtRunner::initialize 并显式传 bin 目录。
WtDtRunner& getRunner();

namespace fs = std::filesystem;

namespace {

struct R102ServoProbe
{
	static uint32_t bars_cb;
	static uint32_t ticks_cb;
	static uint32_t cnt_cb;
	static uint32_t rows;
	static uint32_t on_tick;
	static uint32_t on_bar;
};
uint32_t R102ServoProbe::bars_cb = 0;
uint32_t R102ServoProbe::ticks_cb = 0;
uint32_t R102ServoProbe::cnt_cb = 0;
uint32_t R102ServoProbe::rows = 0;
uint32_t R102ServoProbe::on_tick = 0;
uint32_t R102ServoProbe::on_bar = 0;

void r102_bars_cb(WTSBarStruct* /*bar*/, WtUInt32 count, bool /*isLast*/)
{
	R102ServoProbe::bars_cb++;
	R102ServoProbe::rows += count;
}

void r102_ticks_cb(WTSTickStruct* /*tick*/, WtUInt32 count, bool /*isLast*/)
{
	R102ServoProbe::ticks_cb++;
	R102ServoProbe::rows += count;
}

void r102_cnt_cb(WtUInt32 /*cnt*/)
{
	R102ServoProbe::cnt_cb++;
}

void r102_on_tick(const char* /*stdCode*/, WTSTickStruct* /*tick*/)
{
	R102ServoProbe::on_tick++;
}

void r102_on_bar(const char* /*stdCode*/, const char* /*period*/, WTSBarStruct* /*bar*/)
{
	R102ServoProbe::on_bar++;
}

void r102_reset_probe()
{
	R102ServoProbe::bars_cb = 0;
	R102ServoProbe::ticks_cb = 0;
	R102ServoProbe::cnt_cb = 0;
	R102ServoProbe::rows = 0;
}

struct R102ServoReport
{
	int32_t		fail_line = 0;
	uint32_t	rets[8] = { 0 };
	uint32_t	cbs[8] = { 0 };
	uint32_t	cnts[8] = { 0 };
	uint32_t	rows[8] = { 0 };
	uint32_t	on_tick = 0;
	uint32_t	on_bar = 0;
};

class R102ServoExportsTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r102::make_tmp_dir("servo");
		_bin = r102::bin_dir();
		_store = r102::with_slash(_dir / "store");
		_logCfg = (_dir / "no_such_logcfg.yaml").string();

		r102::write_basefiles(_dir);
		r102::write_text(_dir / "servo.json", r102::full_cfg_json(_dir, _store, true));
		ASSERT_TRUE(r102::seed_default_store(_store));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	void run_child(const std::function<void(R102ServoReport&)>& fn)
	{
		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R102ServoReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R102ServoReport rep;
		memset(&rep, 0, sizeof(rep));
		ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
		::close(fds[0]);

		int status = 0;
		ASSERT_EQ(::waitpid(pid, &status, 0), pid);
		ASSERT_TRUE(WIFEXITED(status)) << "子进程异常退出, status=" << status;
		ASSERT_EQ(WEXITSTATUS(status), 0);
		ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
		EXPECT_EQ(rep.fail_line, 0) << "子进程检查失败行 " << rep.fail_line;

		for (int i = 0; i < 8; i++)
		{
			EXPECT_GT(rep.rets[i], 0u) << "export#" << i;
			EXPECT_GE(rep.cbs[i], 1u) << "export#" << i;
			EXPECT_EQ(rep.cnts[i], 1u) << "export#" << i;
			EXPECT_GT(rep.rows[i], 0u) << "export#" << i;
		}
		EXPECT_EQ(rep.on_tick, 0u);
		EXPECT_EQ(rep.on_bar, 0u);
	}

	fs::path		_dir;
	std::string		_bin;
	std::string		_store;
	std::string		_logCfg;
};

TEST_F(R102ServoExportsTest, AllQueryExportsDispatchCallbacksOnSeededStore)
{
	if (!r102::storage_plugin_ready(_bin))
		GTEST_SKIP() << "libWtDataStorage.dylib / createRdmDtReader 不可用";

	run_child([this](R102ServoReport& rep) {
		// 与 WtDtServo.cpp:55-58 的 C 导出 initialize 等价，但显式给插件目录。
		getRunner().initialize((_dir / "servo.json").string().c_str(), true,
			_bin.c_str(), _logCfg.c_str(), r102_on_tick, r102_on_bar);

		r102_reset_probe();
		// :76-94 d1 times==1 -> WtRdmDtReader::readKlineSliceByRange
		rep.rets[0] = get_bars_by_range("CFFEX.IC.2204", "d1",
			202401020000, 202401042359, r102_bars_cb, r102_cnt_cb);
		rep.cbs[0] = R102ServoProbe::bars_cb;
		rep.cnts[0] = R102ServoProbe::cnt_cb;
		rep.rows[0] = R102ServoProbe::rows;

		r102_reset_probe();
		// :76-94 m5 times==1，另一条周期解析分支
		rep.rets[1] = get_bars_by_range("CFFEX.IC.2204", "m5",
			202401020930, 202401021500, r102_bars_cb, r102_cnt_cb);
		rep.cbs[1] = R102ServoProbe::bars_cb;
		rep.cnts[1] = R102ServoProbe::cnt_cb;
		rep.rows[1] = R102ServoProbe::rows;

		r102_reset_probe();
		// :96-114 按日期 -> getBoundaryTime -> 区间
		rep.rets[2] = get_bars_by_date("CFFEX.IC.2204", "m5", 20240102,
			r102_bars_cb, r102_cnt_cb);
		rep.cbs[2] = R102ServoProbe::bars_cb;
		rep.cnts[2] = R102ServoProbe::cnt_cb;
		rep.rows[2] = R102ServoProbe::rows;

		r102_reset_probe();
		// :140-158 按数量 -> WtRdmDtReader::readKlineSliceByCount
		rep.rets[3] = get_bars_by_count("CFFEX.IC.2204", "m5", 3, 202401021000,
			r102_bars_cb, r102_cnt_cb);
		rep.cbs[3] = R102ServoProbe::bars_cb;
		rep.cnts[3] = R102ServoProbe::cnt_cb;
		rep.rows[3] = R102ServoProbe::rows;

		r102_reset_probe();
		// :116-138 tick 区间
		rep.rets[4] = get_ticks_by_range("CFFEX.IC.2204",
			202401020930, 202401021000, r102_ticks_cb, r102_cnt_cb);
		rep.cbs[4] = R102ServoProbe::ticks_cb;
		rep.cnts[4] = R102ServoProbe::cnt_cb;
		rep.rows[4] = R102ServoProbe::rows;

		r102_reset_probe();
		// :184-206 tick 按日期
		rep.rets[5] = get_ticks_by_date("CFFEX.IC.2204", 20240102,
			r102_ticks_cb, r102_cnt_cb);
		rep.cbs[5] = R102ServoProbe::ticks_cb;
		rep.cnts[5] = R102ServoProbe::cnt_cb;
		rep.rows[5] = R102ServoProbe::rows;

		r102_reset_probe();
		// :160-182 tick 按数量；股票代码让 getCommodity(exchg, code) 命中
		rep.rets[6] = get_ticks_by_count("SSE.600000", 2, 202401021000,
			r102_ticks_cb, r102_cnt_cb);
		rep.cbs[6] = R102ServoProbe::ticks_cb;
		rep.cnts[6] = R102ServoProbe::cnt_cb;
		rep.rows[6] = R102ServoProbe::rows;

		r102_reset_probe();
		// :208-226 秒线（tick -> 30s）
		rep.rets[7] = get_sbars_by_date("CFFEX.IC.2204", 30, 20240102,
			r102_bars_cb, r102_cnt_cb);
		rep.cbs[7] = R102ServoProbe::bars_cb;
		rep.cnts[7] = R102ServoProbe::cnt_cb;
		rep.rows[7] = R102ServoProbe::rows;

		// :233-236 WtDtServo.cpp -> WtDtRunner::sub_bar -> WtDataManager::subscribe_bar
		subscribe_bar("CFFEX.IC.2204", "m5");
		// :238-241
		clear_cache();

		rep.on_tick = R102ServoProbe::on_tick;
		rep.on_bar = R102ServoProbe::on_bar;

		for (int i = 0; i < 8; i++)
		{
			if (rep.rets[i] == 0 || rep.cbs[i] == 0 || rep.cnts[i] != 1)
			{
				rep.fail_line = __LINE__;
				break;
			}
		}
	});
}

}	// namespace
