/*
 * 迭代03 第五会话主线程：BUG-R12-13（DateMappingV1，卡1 A+C）——ticker 启动时交易日映射失败须显式失败。
 * 改前：CTA/SEL/HFT/UFT ticker 的 run() 直接调用旧 calcTradingDate，会话无交易时段时仍以哨兵/兜底日期
 * 驱动引擎（on_init/on_session_begin 照常执行）。改后：经 DateMappingClient 映射失败则记错误并拒绝启动。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "q01_sel_env.h"

#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WtCore/TraderAdapter.h>
#include <WtCore/WtCtaEngine.h>
#include <WtCore/WtCtaTicker.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtHelper.h>

USING_NS_WTP;

namespace
{
	class S5R1213CtaEngine : public WtCtaEngine
	{
	public:
		void on_init() override { ++inits; }
		void on_session_begin() override { ++session_begins; }
		void on_session_end() override {}
		uint32_t inits = 0;
		uint32_t session_begins = 0;
	};

	class S5R1213Reader : public IDataReader
	{
	public:
		void init(WTSVariant*, IDataReaderSink*, IHisDataLoader* = NULL) override {}
		void onMinuteEnd(uint32_t, uint32_t, uint32_t = 0) override {}
		WTSTickSlice* readTickSlice(const char*, uint32_t, uint64_t = 0) override { return NULL; }
		WTSKlineSlice* readKlineSlice(const char*, WTSKlinePeriod, uint32_t, uint64_t = 0) override { return NULL; }
	};

	std::string s5_r1213_dir()
	{
		std::filesystem::path p = std::filesystem::temp_directory_path() /
			("s5_r1213_" + std::to_string(static_cast<long>(::getpid())));
		std::error_code ec;
		std::filesystem::create_directories(p, ec);
		return p.string() + "/";
	}
}

TEST(S5MainR1213TickerDate, CtaTickerRefusesToStartWhenTradingDateCannotBeMapped)
{
	WtHelper::setGenerateDir(s5_r1213_dir().c_str());
	q01sel::BaseDataMgr bd;
	q01sel::HotMgr hot;
	WtDtMgr dm;
	bd.regisSession("NOSECTION");	// 会话存在但没有交易时段：交易日无法映射
	WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "ag", "NOSECTION", TM_Both);
	comm->setSessionInfo(bd.session("NOSECTION"));
	bd.regis(comm, q01sel::makeCt("SHFE", "ag", "ag2512"));

	TraderAdapterMgr adapterMgr;
	S5R1213CtaEngine engine;
	WTSVariant* cfg = WTSCfgLoader::load_from_content(
		"{\"product\":{\"session\":\"NOSECTION\"},\"fees\":\"\",\"filters\":\"\"}");
	ASSERT_NE(nullptr, cfg);
	engine.init(cfg, &bd, &dm, &hot, NULL);
	cfg->release();
	engine.set_adapter_mgr(&adapterMgr);
	engine.set_date_time(20250110, 930, 0);

	S5R1213Reader reader;
	WtCtaRtTicker ticker(&engine);
	ticker.init(&reader, "NOSECTION");
	EXPECT_FALSE(ticker.run());
	EXPECT_EQ(0u, engine.inits) << "the engine must not be initialized with an unmapped trading date";
	EXPECT_EQ(0u, engine.session_begins);
	ticker.stop();
	engine.release();
}
