/*!
 * \file r85_uftcore_parser_ticker_test.cpp
 * \brief 迭代01 第十六波 R85 - UftCore ParserAdapter 残留守卫 + WtUftRtTicker 收市分钟与线程体
 *
 * 覆盖（只读生产代码）:
 *  - ParserAdapter.cpp:81-86 插件可 dlopen 但不导出 createParser -> 直接拒绝
 *  - ParserAdapter.cpp:183-202 initExt 在 _filters 非空时按六种键匹配合约集合
 *  - ParserAdapter.cpp:271-279 handleOrderQueue 的过滤/未知合约早退
 *  - ParserAdapter.cpp:292-296 handleOrderDetail 的过滤/时间戳早退
 *  - ParserAdapter.cpp:313-317 handleTransaction 的过滤/时间戳早退
 *  - WtUftTicker.cpp:101-105 isLastOfSection 命中时 minutes-- 后再 ++（curMin=100）
 *  - WtUftTicker.cpp:172-176 重复 run() 的早退
 *  - WtUftTicker.cpp:190-202 工作线程体：全天时段内 10ms 休眠 + 未到检查点的 else 路径
 *
 * 线程约束：只起 ticker 自己的那一个线程，时段为 (0,100)+(100,2400) 两段拼接，
 * init 写下的墙钟只有恰好在 00:00/01:00 才会落进 10s 休息分支；其余时刻都是 10ms 休眠，
 * sleep 40ms 后 stop() 立即 join。不接 socket/shm/柜台。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftTicker.h"

#include "Includes/IParserApi.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSSessionInfo.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	fs::path p = fs::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

// ParserAdapter 的模块根是 <instdir>/parsers/，在临时目录里铺同名软链
bool stageParser(const fs::path& root, const std::string& bin,
	const char* stageName, const char* realName)
{
	std::error_code ec;
	fs::create_directories(root / "parsers", ec);
	if (ec)
		return false;
	const std::string src = bin + realName;
	if (!fs::exists(src, ec))
		return false;
	fs::create_symlink(src, root / "parsers" / stageName, ec);
	return !ec;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

class FakeParserApi : public IParserApi
{
public:
	uint32_t _initCnt = 0;
	uint32_t _regSpiCnt = 0;
	uint32_t _subscribeCnt = 0;
	bool	 _initRet = true;

	bool init(WTSVariant* /*config*/) override { _initCnt++; return _initRet; }
	void registerSpi(IParserSpi* /*spi*/) override { _regSpiCnt++; }
	void subscribe(const CodeSet& setCodes) override { _subscribeCnt += (uint32_t)setCodes.size(); }
};

WTSOrdQueData* makeOrdQue(const char* exchg, const char* code,
	uint32_t actionDate, uint32_t tradingDate)
{
	WTSOrdQueStruct st;
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = actionDate;
	st.trading_date = tradingDate;
	st.action_time = 100000000;
	return WTSOrdQueData::create(st);
}

WTSOrdDtlData* makeOrdDtl(const char* exchg, const char* code,
	uint32_t actionDate, uint32_t tradingDate)
{
	WTSOrdDtlStruct st;
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = actionDate;
	st.trading_date = tradingDate;
	st.action_time = 100000000;
	return WTSOrdDtlData::create(st);
}

WTSTransData* makeTrans(const char* exchg, const char* code,
	uint32_t actionDate, uint32_t tradingDate)
{
	WTSTransStruct st;
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = actionDate;
	st.trading_date = tradingDate;
	st.action_time = 100000000;
	return WTSTransData::create(st);
}

// 固定日盘时段：上午 900-1130，下午 1300-1500
class SessionBaseDataMgr : public q04::FakeBaseDataMgr
{
public:
	WTSSessionInfo* _session = nullptr;

	WTSSessionInfo* getSession(const char* /*sid*/) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override
	{
		return _session;
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

}	// namespace

// ParserAdapter.cpp:81-86 无 createParser 的模块被拒绝
TEST(R85UftParserInit, module_without_create_symbol_is_rejected)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";

	q04::TempRoot tmp("r85_parser_nosym");
	if (!stageParser(tmp.dir(), bin, "libNoParser.dylib", "libWtDataStorageAD.dylib"))
		GTEST_SKIP() << "libWtDataStorageAD.dylib not found in this build";
	InstDirGuard instdir(tmp.dir().string() + "/");

	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	IParserStub stub;

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "NoParser");
	cfg->append("filter", "");

	ParserAdapter adapter;
	EXPECT_FALSE(adapter.init("r85nosym", cfg, &stub, &bd));
	EXPECT_FALSE(adapter.run());

	cfg->release();
}

// ParserAdapter.cpp:183-202 initExt 使用 init 阶段留下的 _filters
// ParserAdapter.cpp:271-279 / 292-296 / 313-317 三个 handle 的过滤与时间戳早退
TEST(R85UftParserInit, init_ext_applies_filters_and_handles_reject_unknown)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";

	q04::TempRoot tmp("r85_parser_filter");
	if (!stageParser(tmp.dir(), bin, "libParserUDP.dylib", "libParserUDP.dylib"))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	InstDirGuard instdir(tmp.dir().string() + "/");

	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	IParserStub stub;

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	// 同时含交易所与合约标记：handle* 的过滤只看 exchg，initExt 的匹配看六种键
	cfg->append("filter", "SHFE,ag2406");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17901);
	cfg->append("sport", (uint32_t)17902);

	ParserAdapter adapter;
	ASSERT_TRUE(adapter.init("r85filter", cfg, &stub, &bd));

	// 交易所不在过滤集合 -> 三个 handle 各自直接返回
	WTSOrdQueData* oqDce = makeOrdQue("DCE", "m2409", 20240315, 20240315);
	adapter.handleOrderQueue(oqDce);
	oqDce->release();
	WTSOrdDtlData* odDce = makeOrdDtl("DCE", "m2409", 20240315, 20240315);
	adapter.handleOrderDetail(odDce);
	odDce->release();
	WTSTransData* trDce = makeTrans("DCE", "m2409", 20240315, 20240315);
	adapter.handleTransaction(trDce);
	trDce->release();

	// 交易所命中过滤、但合约查不到 -> 未知合约早退
	WTSOrdQueData* oqUnk = makeOrdQue("SHFE", "none2401", 20240315, 20240315);
	adapter.handleOrderQueue(oqUnk);
	oqUnk->release();

	// 交易所命中、时间戳不完整 -> 日期守卫早退
	WTSOrdDtlData* odNoDate = makeOrdDtl("SHFE", "ag2406", 0, 20240315);
	adapter.handleOrderDetail(odNoDate);
	odNoDate->release();
	WTSTransData* trNoDate = makeTrans("SHFE", "ag2406", 20240315, 0);
	adapter.handleTransaction(trNoDate);
	trNoDate->release();

	adapter.release();

	// _filters 仍是 {SHFE, ag2406}：initExt 走非空分支，只命中一个合约
	FakeParserApi api;
	EXPECT_TRUE(adapter.initExt("r85filter2", &api, &stub, &bd));
	EXPECT_EQ(1u, api._initCnt);
	EXPECT_EQ(1u, api._regSpiCnt);
	EXPECT_EQ(1u, api._subscribeCnt);

	cfg->release();
}

// WtUftTicker.cpp:101-105 收市分钟 + :172-176 重复 run + :190-202 线程体
TEST(R85UftTicker, last_section_minute_and_worker_loop)
{
	SessionBaseDataMgr bd;
	WTSSessionInfo* sInfo = WTSSessionInfo::create("R85SPLIT", "r85 split", 0);
	ASSERT_NE(nullptr, sInfo);
	// 两段拼接覆盖全天：只有 01:00 这一分钟是「小节最后一分钟」，
	// 工作线程只有恰好在 00:00/01:00 启动时才会落进 10s 休息分支（概率极低且不影响断言）
	sInfo->addTradingSection(0, 100);
	sInfo->addTradingSection(100, 2400);
	bd._session = sInfo;

	{
		WtUftDtMgr dtMgr;
		WtUftEngine engine;
		engine.init(nullptr, &bd, &dtMgr, nullptr);

		WtUftRtTicker ticker(&engine);
		// init 会把 _time 写成当前墙钟；全天时段保证工作线程走 10ms 休眠的真分支
		ticker.init("R85SPLIT");

		ticker.run();
		// 第二次 run 在 _thrd 非空时直接返回
		ticker.run();
		// 给工作线程至少两次迭代（每轮 10ms 休眠 + 未到检查点）
		std::this_thread::sleep_for(std::chrono::milliseconds(40));
		ticker.stop();

		// _thrd 仍非空：这一笔的 curMin=100 命中 isLastOfSection(第一段收市)
		// -> minutes-- / minutes++ 的收市分钟修正
		WTSTickData* t = makeTick(20991231, 10000000, 10.0);
		ticker.on_tick(t);
		t->release();
		// 第二笔同分钟：_cur_pos 已是 1440，走 else 直接 set_date_time
		WTSTickData* t2 = makeTick(20991231, 10000000, 11.0);
		ticker.on_tick(t2);
		t2->release();
		EXPECT_EQ(20991231u, engine.get_date());
	}

	sInfo->release();
}
