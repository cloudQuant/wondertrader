/*!
 * \file r78_uftcore_ticker_ctx_test.cpp
 * \brief 第十五波 R78: WtUftRtTicker 完整 tick 路径 + WtUftEngine::run + UftStraContext 手工持仓解析失败
 *
 * 目标:
 *  - WtUftRtTicker::run/stop（0-2400 全天时段，线程只做 <=10ms 的一次休眠即被 join）
 *  - WtUftRtTicker::on_tick 的 _thrd != NULL 全链路：首笔定位、分钟推进（触发
 *    on_minute_end + set_date_time/set_trading_date）、同分钟转发、行情时间回退早退
 *  - WtUftEngine::run 的 on_init/calcTradingDate/on_session_begin + ticker 装配
 *  - UftStraContext::load_local_data 的两条解析失败分支：mannual.yaml 为空
 *    （load_from_file 返回 NULL）与缺少 details 段（ayDetails == NULL）
 *
 * 线程约束: 只起 WtUftRtTicker 内部那一个线程；时段固定 0-2400 使工作线程走
 * 「交易时间内 sleep 10ms」分支，run() 返回后立刻 stop() join，不等待墙钟。
 * 不接网络/柜台/共享内存。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftTicker.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

using namespace wtp;

namespace {

// 全天候时段，让 ticker 工作线程必然走 isInTradingTime 真分支
class SessionBaseDataMgr : public q04::FakeBaseDataMgr
{
public:
	WTSSessionInfo* _session = nullptr;
	std::string		_lastSid;

	WTSSessionInfo* getSession(const char* sid) override
	{
		_lastSid = (sid == nullptr) ? "" : sid;
		return _session;
	}

	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override
	{
		return _session;
	}
};

// session 的释放必须晚于 engine（engine 析构会 stop/join ticker）
struct SessionOwner
{
	WTSSessionInfo* _session = nullptr;
	~SessionOwner()
	{
		if (_session != nullptr)
			_session->release();
	}
};

struct TickEnv
{
	SessionOwner			_owner;
	SessionBaseDataMgr		_bd;
	WtUftDtMgr				_dtMgr;
	WtUftEngine				_engine;

	TickEnv()
	{
		_owner._session = WTSSessionInfo::create("ALLDAY", "r78 allday", 0);
		_owner._session->addTradingSection(0, 2400);
		_bd._session = _owner._session;
		_engine.init(nullptr, &_bd, &_dtMgr, nullptr);
	}
};

WTSTickData* makeTick(uint32_t date, uint32_t hhmmss, double price)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, "SHFE");
	wt_strcpy(ts.code, "SHFE.ag2406");
	ts.action_date = date;
	ts.action_time = hhmmss;
	ts.trading_date = date;
	ts.price = price;
	return WTSTickData::create(ts);
}

std::string ctxRoot()
{
	static std::string root = []() {
		std::filesystem::path p = std::filesystem::temp_directory_path() / "r78_uftctx_root";
		std::error_code ec;
		std::filesystem::remove_all(p, ec);
		std::filesystem::create_directories(p, ec);
		WtHelper::setGenerateDir((p.string() + "/").c_str());
		return p.string() + "/";
	}();
	return root;
}

struct CtxEnv
{
	q04::FakeBaseDataMgr	_bd;
	WtUftEngine				_engine;
	std::string				_name;
	std::string				_folder;
	UftStraContext			_ctx;

	explicit CtxEnv(const char* tag)
		: _name(std::string("r78ctx_") + tag + "_" + std::to_string((long)::getpid())
			+ "_" + std::to_string(++s_seq()))
		, _ctx(&_engine, _name.c_str())
	{
		ctxRoot();
		_bd.add("SHFE", "ag", "ag2406");
		_engine.init(nullptr, &_bd, nullptr, nullptr);
		_folder = std::string(WtHelper::getOutputDir()) + _name;
		std::error_code ec;
		std::filesystem::remove_all(_folder, ec);
	}

	~CtxEnv()
	{
		std::error_code ec;
		std::filesystem::remove_all(_folder, ec);
	}

	static std::atomic<uint32_t>& s_seq()
	{
		static std::atomic<uint32_t> seq(0);
		return seq;
	}

	std::string mannual() const { return _folder + "/mannual.yaml"; }
};

}	// namespace

// WtUftTicker.cpp:172-251 run 的线程装配 + :257-264 stop
// WtUftTicker.cpp:68-166 on_tick 全链路（首笔 / 分钟推进 / 同分钟 / 时间回退）
TEST(R78UftTicker, run_stop_and_full_tick_flow)
{
	TickEnv env;
	WtUftRtTicker ticker(&env._engine);
	ticker.init("ALLDAY");
	EXPECT_EQ("ALLDAY", env._bd._lastSid);

	// 线程在 0-2400 时段内只做一次 10ms 休眠，stop() 立即 join
	ticker.run();
	ticker.stop();

	WTSTickData* t1 = makeTick(20991231, 93000000, 10.0);
	ticker.on_tick(t1);
	t1->release();

	// 931 推进一分钟：_last_emit_pos(0) < _cur_pos(931) -> 触发 on_minute_end，
	// 然后 engine->on_tick + set_date_time(..., rawMin=931) + set_trading_date
	WTSTickData* t2 = makeTick(20991231, 93100000, 11.0);
	ticker.on_tick(t2);
	t2->release();
	EXPECT_EQ(20991231u, env._engine.get_date());
	EXPECT_EQ(932u, env._engine.get_min_time());
	EXPECT_EQ(931u, env._engine.get_raw_time());
	EXPECT_EQ(20991231u, env._engine.get_trading_date());
	EXPECT_DOUBLE_EQ(11.0, env._engine.get_cur_price("SHFE.ag2406"));

	// 同一分钟：走 else 的直接转发分支
	WTSTickData* t3 = makeTick(20991231, 93130000, 12.0);
	ticker.on_tick(t3);
	t3->release();
	EXPECT_EQ(932u, env._engine.get_min_time());
	EXPECT_DOUBLE_EQ(12.0, env._engine.get_cur_price("SHFE.ag2406"));

	// 行情时间回退：只转发 tick，不覆盖平台时间
	WTSTickData* t4 = makeTick(20240315, 93000000, 13.0);
	ticker.on_tick(t4);
	t4->release();
	EXPECT_EQ(20991231u, env._engine.get_date());
	EXPECT_DOUBLE_EQ(13.0, env._engine.get_cur_price("SHFE.ag2406"));
}

// WtUftEngine.cpp:326-351 run: 遍历上下文 on_init、new WtUftRtTicker、
// init("ALLDAY")（_cfg 为空分支）、ticker->run；析构时 ticker->stop/delete
TEST(R78UftEngineRun, starts_and_stops_allday_ticker)
{
	TickEnv env;
	env._engine.run();
	// ticker->run 里 calcTradingDate 的返回值已写入引擎
	EXPECT_GT(env._engine.get_date(), 0u);
	EXPECT_EQ(env._engine.get_date(), env._engine.get_trading_date());
}

// UftStraContext.cpp:1044-1058 mannual.yaml 为空 -> load_from_file 返回 NULL
TEST(R78UftCtxLocal, empty_mannual_file_is_renamed_and_ignored)
{
	CtxEnv env("empty");
	std::error_code ec;
	std::filesystem::create_directories(env._folder, ec);
	ASSERT_FALSE(ec);

	// 0 字节：StdFile::read_file_content 为空 -> load_from_file 返回 NULL
	{
		std::ofstream ofs(env.mannual().c_str(), std::ios::out | std::ios::trunc);
		ASSERT_TRUE(ofs.good());
	}

	env._ctx.on_channel_ready(20240315);

	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position("SHFE.ag2406"));
	EXPECT_FALSE(std::filesystem::exists(env.mannual(), ec));
	EXPECT_TRUE(std::filesystem::exists(env._folder + "/position.membin", ec));
}

// UftStraContext.cpp:1059-1061 合法 yaml 但无 details 段 -> break 后仍改名
TEST(R78UftCtxLocal, mannual_without_details_section_is_ignored)
{
	// 新上下文：首次 ready 才触发 load_local_data
	CtxEnv env2("nodetails2");
	std::error_code ec;
	std::filesystem::create_directories(env2._folder, ec);
	ASSERT_FALSE(ec);
	{
		std::ofstream ofs(env2.mannual().c_str(), std::ios::out | std::ios::trunc);
		ofs << "foo: bar\n";
	}
	env2._ctx.on_channel_ready(20240315);
	EXPECT_DOUBLE_EQ(0.0, env2._ctx.stra_get_local_position("SHFE.ag2406"));
	EXPECT_FALSE(std::filesystem::exists(env2.mannual(), ec));
	EXPECT_TRUE(std::filesystem::exists(env2._folder + "/position.membin", ec));
}
