/*!
 * \file r45_servo_api_test.cpp
 * \brief WtDtServo/WtDtServo.cpp（C 导出层）补测（迭代01 第十波 R45）
 *
 * 目标是把导出层自己的代码走到，而不是只停在 WtDtRunner 的守卫：
 *  - get_version 的平台前缀 + 缓存（:60-74）
 *  - initialize 的配置加载失败早退（:55-58），文件与内容两种入参
 *  - 8 个查询导出在「runner 未初始化」时返回 0 且不触发任何回调（:76-226）
 *  - subscribe_tick / clear_cache 在未初始化时的安全形态（:228-241）
 *
 * 全局 runner 一旦被成功 initialize 就会一直带 store 状态，本文件刻意
 * 只用「必然失败的配置」驱动 initialize，保证后续用例仍然处于未初始化态，
 * 任何顺序组合都不会去调用已初始化的取数入口。subscribe_bar 会走
 * WtDataManager::subscribe_bar 的 _reader 委托，未初始化时必崩（BUG-R8-003），
 * 这里不启用。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSStruct.h>
#include <WtDtServo/WtDtServo.h>

namespace fs = std::filesystem;

namespace {

struct R45ServoApiProbe
{
	static int bars_cb;
	static int ticks_cb;
	static int cnt_cb;
	static int on_tick;
	static int on_bar;
};
int R45ServoApiProbe::bars_cb = 0;
int R45ServoApiProbe::ticks_cb = 0;
int R45ServoApiProbe::cnt_cb = 0;
int R45ServoApiProbe::on_tick = 0;
int R45ServoApiProbe::on_bar = 0;

void r45_get_bars_cb(WTSBarStruct* /*bar*/, WtUInt32 /*count*/, bool /*isLast*/)
{
	++R45ServoApiProbe::bars_cb;
}

void r45_get_ticks_cb(WTSTickStruct* /*tick*/, WtUInt32 /*count*/, bool /*isLast*/)
{
	++R45ServoApiProbe::ticks_cb;
}

void r45_cnt_cb(WtUInt32 /*cnt*/)
{
	++R45ServoApiProbe::cnt_cb;
}

void r45_on_tick(const char* /*stdCode*/, WTSTickStruct* /*tick*/)
{
	++R45ServoApiProbe::on_tick;
}

void r45_on_bar(const char* /*stdCode*/, const char* /*period*/, WTSBarStruct* /*bar*/)
{
	++R45ServoApiProbe::on_bar;
}

void r45_reset_probe()
{
	R45ServoApiProbe::bars_cb = 0;
	R45ServoApiProbe::ticks_cb = 0;
	R45ServoApiProbe::cnt_cb = 0;
	R45ServoApiProbe::on_tick = 0;
	R45ServoApiProbe::on_bar = 0;
}

fs::path r45_api_make_tmp_dir()
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ ("r45_servoapi_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

class R45ServoApiTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r45_api_make_tmp_dir();
		r45_reset_probe();
		_logCfg = (_dir / "no_such_logcfg.yaml").string();
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path _dir;
	std::string _logCfg;
};

TEST_F(R45ServoApiTest, VersionStringIsCachedAndPrefixedByPlatform)
{
	// WtDtServo.cpp:60-74：首次拼 "UNIX <ver> Build@<date> <time>"，之后复用同一 buffer
	WtString v1 = get_version();
	WtString v2 = get_version();
	ASSERT_NE(v1, nullptr);
	EXPECT_EQ(v1, v2);
	const std::string ver(v1);
	EXPECT_NE(ver.find("UNIX"), std::string::npos);
	EXPECT_NE(ver.find("Build@"), std::string::npos);
}

TEST_F(R45ServoApiTest, AllQueryExportsReturnZeroBeforeInitialize)
{
	// WtDtServo.cpp:76-226 的每个导出都是「取 runner 切片 -> 有则回调 -> 无则 return 0」；
	// 全局 runner 未初始化 -> WtDtRunner 各守卫返回 NULL -> 这里必须拿到 0 且零回调
	EXPECT_EQ(get_bars_by_range("CFFEX.IC.2204", "m1", 202401020930, 202401021500,
		r45_get_bars_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_bars_by_date("CFFEX.IC.2204", "m1", 20240102,
		r45_get_bars_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_bars_by_count("CFFEX.IC.2204", "m1", 10, 202401021500,
		r45_get_bars_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_ticks_by_range("CFFEX.IC.2204", 202401020930, 202401021500,
		r45_get_ticks_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_ticks_by_date("CFFEX.IC.2204", 20240102,
		r45_get_ticks_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_ticks_by_count("CFFEX.IC.2204", 10, 202401021500,
		r45_get_ticks_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(get_sbars_by_date("CFFEX.IC.2204", 30, 20240102,
		r45_get_bars_cb, r45_cnt_cb), 0u);

	EXPECT_EQ(R45ServoApiProbe::bars_cb, 0);
	EXPECT_EQ(R45ServoApiProbe::ticks_cb, 0);
	EXPECT_EQ(R45ServoApiProbe::cnt_cb, 0);
}

TEST_F(R45ServoApiTest, InitializeWithMissingFileKeepsApiUninitialized)
{
	// WtDtServo.cpp:55-58 -> WtDtRunner.cpp:58-64：文件不存在即早退，
	// _is_inited 保持 false，导出层仍返回 0
	initialize((_dir / "no_such_wtdt.json").string().c_str(), true,
		_logCfg.c_str(), r45_on_tick, r45_on_bar);

	EXPECT_EQ(get_ticks_by_date("CFFEX.IC.2204", 20240102,
		r45_get_ticks_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(R45ServoApiProbe::ticks_cb, 0);
	EXPECT_EQ(R45ServoApiProbe::on_tick, 0);
}

TEST_F(R45ServoApiTest, InitializeWithUnparsableContentKeepsApiUninitialized)
{
	// WtDtServo.cpp:55-58 走 isFile=false 的 load_from_content；
	// 内容不是合法 JSON -> NULL -> "Loading config failed"
	initialize("this is not a json document", false,
		_logCfg.c_str(), r45_on_tick, r45_on_bar);

	EXPECT_EQ(get_bars_by_count("CFFEX.IC.2204", "d1", 5, 0,
		r45_get_bars_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(R45ServoApiProbe::bars_cb, 0);
}

TEST_F(R45ServoApiTest, SubscribeTickAndClearCacheAreSafeBeforeInitialize)
{
	// WtDtServo.cpp:228-231 -> WtDtRunner::sub_tick 只写订阅表（无回调注册、无 reader）
	subscribe_tick("CFFEX.IC.2204", true);
	subscribe_tick("SSE.600000,CFFEX.IC.2204", false);
	// WtDtServo.cpp:238-241 -> WtDtManager::clear_cache 的 _reader==NULL 取消分支
	clear_cache();

	// 仍然未初始化：查询保持 0，证明上面两步没有把 runner 带进半初始化态
	EXPECT_EQ(get_ticks_by_count("CFFEX.IC.2204", 5, 0,
		r45_get_ticks_cb, r45_cnt_cb), 0u);
	EXPECT_EQ(R45ServoApiProbe::on_tick, 0);
}

}  // namespace
