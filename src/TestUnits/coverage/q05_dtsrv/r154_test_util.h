/*!
 * \file r154_test_util.h
 * \brief R154 lane（第二十三波 · WtDtCore 硬面）自用工具，仅 r154_*.cpp 使用。
 *
 * 约定：
 *  - 临时目录只落系统临时目录，名称带 r154_ 前缀 + pid + 序号，TearDown 清理；
 *  - UDP 只走 127.0.0.1 回环；状态机用例只等产品线程的首轮/次轮推进，
 *    墙钟只作为上限超时，不用于断言“必须多久发生”；
 *  - MonitorGuard / CasterGuard 保证任何 ASSERT 早退时也能 join 产品线程，
 *    避免析构 joinable std::thread 触发 std::terminate。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>

#include <filesystem>

#include <gtest/gtest.h>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/StateMonitor.h>
#include <WtDtCore/UDPCaster.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <TimeUtils.hpp>

namespace r154 {

namespace fs = std::filesystem;

inline fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r154_") + tag + "_" + std::to_string(::getpid()) + "_"
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

inline bool wait_until(const std::function<bool()>& pred, int timeout_ms)
{
	const auto deadline = std::chrono::steady_clock::now()
		+ std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (pred())
			return true;

		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	return pred();
}

inline bool wait_state(StateMonitor& mon, const char* sid, SimpleState ss,
	int timeout_ms = 6000)
{
	return wait_until([&mon, sid, ss]() { return mon.isInState(sid, ss); }, timeout_ms);
}

inline uint32_t current_minute()
{
	return TimeUtils::getCurMin() / 100;
}

inline bool is_weekend()
{
	const uint32_t wd = TimeUtils::getWeekDay();
	return wd == 0 || wd == 6;
}

/*
 * StateMonitor 状态机模板。每个模板只挂一个品种（HOL 故意不挂），
 * 使 getSessionComms 返回的集合稳定、节假日判断互不干扰。
 *
 *  - INITALL   ：交易区间 1..2359 换算成 [0,2400)，任意 HHMM 都落在区间内；
 *  - INITEMP   ：交易区间 0..0，stime-- 下溢后 isInSections 恒为 false；
 *  - CLOSEPROC ：普通区间，用于 ORIGINAL->CLOSED->PROCING->PROCED->ORIGINAL；
 *  - CLOSEPAUSE：auction 0..0 提供 getAuctionStartTime()==0，区间为空；
 *  - HOL       ：无品种关联；
 *  - INITAUC   ：auction 2359..2359，INITIALIZED 分支的 offTime<aucStart 路径。
 */
inline const char* state_sessions_json()
{
	return R"JSON({
		"INITALL": {
			"name": "全时段会话", "offset": 0,
			"sections": [{"from": 1, "to": 2359}]
		},
		"INITEMP": {
			"name": "空区间会话", "offset": 0,
			"sections": [{"from": 0, "to": 0}]
		},
		"CLOSEPROC": {
			"name": "收盘处理会话", "offset": 0,
			"sections": [{"from": 1, "to": 2359}]
		},
		"CLOSEPAUSE": {
			"name": "收盘转休盘会话", "offset": 0,
			"auction": {"from": 0, "to": 1},
			"sections": [{"from": 0, "to": 0}]
		},
		"HOL": {
			"name": "无品种会话", "offset": 0,
			"sections": [{"from": 1, "to": 2359}]
		},
		"INITAUC": {
			"name": "竞价前会话", "offset": 0,
			"auction": {"from": 2359, "to": 2359},
			"sections": [{"from": 1, "to": 2359}]
		}
	})JSON";
}

inline const char* state_commodities_json()
{
	return R"JSON({
		"SHFE": {
			"IA": {"name": "全时段品种", "session": "INITALL", "holiday": "R154_NONE"},
			"IE": {"name": "空区间品种", "session": "INITEMP", "holiday": "R154_NONE"},
			"CP": {"name": "收盘处理品种", "session": "CLOSEPROC", "holiday": "R154_NONE"},
			"CU": {"name": "收盘休盘品种", "session": "CLOSEPAUSE", "holiday": "R154_NONE"},
			"AU": {"name": "竞价前品种", "session": "INITAUC", "holiday": "R154_NONE"}
		}
	})JSON";
}

class StateFixture : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("state");
		write_file(_dir / "sessions.json", state_sessions_json());
		write_file(_dir / "commodities.json", state_commodities_json());
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

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	DataManager _dtMgr;	// 无 writer：transHisData/isSessionProceeded 走 DataManager 的空守卫
};

/* stop() 非幂等（run 后二次 join 抛异常），用 armed 标志保证只 join 一次。 */
class MonitorGuard
{
public:
	explicit MonitorGuard(StateMonitor& mon) : _mon(mon) {}
	~MonitorGuard() { stop(); }

	void arm() { _armed = true; }

	void stop()
	{
		if (_armed)
		{
			_armed = false;
			_mon.stop();
		}
	}

private:
	StateMonitor& _mon;
	bool _armed { false };
};

class CasterGuard
{
public:
	explicit CasterGuard(UDPCaster& caster) : _caster(caster) {}
	~CasterGuard() { stop(); }

	void arm() { _armed = true; }

	void stop()
	{
		if (_armed)
		{
			_armed = false;
			_caster.stop();
		}
	}

private:
	UDPCaster& _caster;
	bool _armed { false };
};

/*
 * UDPCaster 的析构顺序有产品缺陷（BUG-R164-001 / 见 bugs/R165.md）：
 * 按成员声明逆序析构时 m_ioservice（UDPCaster.h:96）先于
 * m_sktBroadcast/m_sktSubscribe/m_list*Group（UDPCaster.h:88-95）销毁，
 * 只要 active 装配建过 socket，析构时 socket 访问已销毁的 reactor，SIGSEGV。
 * 本 lane 的 active 用例需要真实 socket，但验证的是 start/broadcast/receive，
 * 不是析构本身，因此用 LeakyCaster 只 stop 不 delete；case_runner 对每条
 * 用例起独立进程，进程退出即回收。缺陷触发占位仍保留在 DISABLED_ 用例中。
 */
class LeakyCaster
{
public:
	LeakyCaster() : _caster(new UDPCaster()) {}
	~LeakyCaster() { stop_once(); }

	LeakyCaster(const LeakyCaster&) = delete;
	LeakyCaster& operator=(const LeakyCaster&) = delete;

	UDPCaster* operator->() const { return _caster; }
	UDPCaster& get() const { return *_caster; }

	void arm() { _armed = true; }

	void stop_once()
	{
		if (_armed)
		{
			_armed = false;
			_caster->stop();
		}
	}

private:
	UDPCaster* _caster;
	bool _armed { false };
};

}	// namespace r154
