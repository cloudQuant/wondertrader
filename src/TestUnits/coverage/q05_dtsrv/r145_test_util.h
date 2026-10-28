/*!
 * \file r145_test_util.h
 * \brief R145 lane（第二十二波 · WtDtCore 硬面）自用工具，仅本 lane 的 r145_*.cpp 使用。
 *
 * 约定：
 *  - 临时目录只落系统临时目录，名称带 r145_ 前缀 + pid + 序号，TearDown remove_all；
 *  - 不建 socket、不建共享内存映射、不 sleep、不读挂钟做断言；
 *  - run() 用例只让产品线程跑完「第一轮」：主线程用 yield 自旋观察状态，
 *    观察到目标状态后立即 stop()/join，线程随即退出（不进入 1 秒节流循环）。
 *    一用例一进程的覆盖率 harness 下，第一轮不会受 run() 内部 static lastTime 影响。
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <filesystem>

#include <gtest/gtest.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/StateMonitor.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <TimeUtils.hpp>

namespace r145 {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r145_") + tag + "_" + std::to_string(::getpid()) + "_"
			+ std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

inline void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

/*
 * 6 个会话模板，覆盖 StateMonitor.cpp:196-296（SS_ORIGINAL）的各个出口：
 *  - HOLIDAY：没有关联品种 -> getSessionComms 返回 NULL -> L243-248 直接转 SS_Holiday；
 *  - CLOSED ：offset +1，close_time=0 -> offsetTime(0,false)=1，白盘绝大多数分钟 offTime>=1；
 *  - INIT   ：无集合竞价，close_time=2400 -> 不收盘、不进竞价分支 -> L288-292 SS_INITIALIZED；
 *  - RECV   ：auction 0..0（换算后 [0,2400)）覆盖全天 -> L263-270 SS_RECEIVING；
 *  - PAUSE  ：auction 0..1（换算后 [0,1)），trading 0..0（换算后下溢成空区间）
 *             -> L272-279，getCloseTime(true)=2400 -> SS_PAUSED；
 *  - SETTLE ：auction 0..1，trading 0..1 -> getCloseTime(true)=1，
 *             offTime>=1 时不在区间但未到收盘 -> L280-284 SS_RECEIVING（结算价窗口）。
 *  - HLM    ：给 HOLCOMM 品种用的普通白盘模板。
 */
inline const char* sessions_json()
{
	return R"JSON({
		"HOLIDAY": {
			"name": "无品种会话", "offset": 0,
			"sections": [{"from": 930, "to": 1500}]
		},
		"CLOSED": {
			"name": "收盘会话", "offset": 1,
			"auction": {"from": 0, "to": 0},
			"sections": [{"from": 1, "to": 2359}]
		},
		"INIT": {
			"name": "初始化会话", "offset": 0,
			"sections": [{"from": 1, "to": 2359}]
		},
		"RECV": {
			"name": "接收会话", "offset": 0,
			"auction": {"from": 0, "to": 0},
			"sections": [{"from": 1, "to": 2359}]
		},
		"PAUSE": {
			"name": "休盘会话", "offset": 0,
			"auction": {"from": 0, "to": 1},
			"sections": [{"from": 0, "to": 0}]
		},
		"SETTLE": {
			"name": "结算价会话", "offset": 0,
			"auction": {"from": 0, "to": 1},
			"sections": [{"from": 0, "to": 1}]
		},
		"HLM": {
			"name": "节假日品种会话", "offset": 0,
			"sections": [{"from": 1, "to": 2359}]
		}
	})JSON";
}

/*
 * 品种 -> 会话映射。HOLIDAY 故意不挂任何品种（isEmptySession 场景）；
 * 其余每个会话一个独立品种，避免同一 getSessionComms 集合互相干扰。
 * holiday 模板不存在的品种按 isHoliday 默认返回 false（工作日为非节假日）。
 */
inline const char* commodities_json()
{
	return R"JSON({
		"R145": {
			"CLS": {"name": "收盘品种", "session": "CLOSED", "holiday": "R145_NONE"},
			"INI": {"name": "初始化品种", "session": "INIT", "holiday": "R145_NONE"},
			"RCV": {"name": "接收品种", "session": "RECV", "holiday": "R145_NONE"},
			"PSE": {"name": "休盘品种", "session": "PAUSE", "holiday": "R145_NONE"},
			"STL": {"name": "结算品种", "session": "SETTLE", "holiday": "R145_NONE"},
			"HLC": {"name": "节假日品种", "session": "HLM", "holiday": "R145_HOL"}
		}
	})JSON";
}

/* 今天 ±1 天的节假日数组，避免 00:00 前后跨日导致“今天”漂移。 */
inline std::string today_window()
{
	const uint32_t today = TimeUtils::getCurDate();
	return std::string("[") + std::to_string(TimeUtils::getNextDate(today, -1))
		+ "," + std::to_string(today)
		+ "," + std::to_string(TimeUtils::getNextDate(today, 1)) + "]";
}

/* 工作日才可能出现非节假日分支；周末 isTradingDate 硬编码为 false。 */
inline bool is_weekend()
{
	const uint32_t wd = TimeUtils::getWeekDay();
	return wd == 0 || wd == 6;
}

/*
 * 等待 run() 线程把目标会话推进到 ss。
 *
 * 为什么不用 sleep/墙钟：状态转换的条件全部由本 lane 的会话模板保证与当前钟点无关，
 * 这里只等调度；yield 自旋不会把测试结论建立在“睡了多久”上。StateMonitor 没有
 * 状态变化的线程安全出口，_state 是裸枚举（BUG-R145-002），Debug(-O0) 下按字长读取，
 * harness 一用例一进程，故自旋上限取 4M 次足够覆盖首轮。
 */
inline bool wait_state(StateMonitor& mon, const char* sid, SimpleState ss,
	int spins = 4000000)
{
	for (int i = 0; i < spins; i++)
	{
		if (mon.isInState(sid, ss))
			return true;

		std::this_thread::yield();
	}
	return false;
}

class DtcoreFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("dtc");
		write_file(_dir / "sessions.json", sessions_json());
		write_file(_dir / "commodities.json", commodities_json());
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	fs::path state_cfg(const std::string& body)
	{
		fs::path p = _dir / "state.json";
		write_file(p, body);
		return p;
	}

	void load_holidays(const std::string& body)
	{
		fs::path p = _dir / "holidays.json";
		write_file(p, body);
		ASSERT_TRUE(_bdMgr.loadHolidays(p.string().c_str()));
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	DataManager _dm;	// _writer==NULL：run() 的 transHisData 只会走 DataManager.cpp:213-217 的守卫
};

}	// namespace r145
