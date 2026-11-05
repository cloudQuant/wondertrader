/**
 * 迭代03 第五会话主线程：BUG-R12-13（卡1 路径 A）——HisDataReplayer 的日线收盘时刻与 HFT 边界改经 checked 映射。
 *
 * 品种引用未加载的节假日日历（NOPE），默认日历 CHINA 已加载：prepare 用会话默认日历可通过，日线收盘时刻
 * 的映射报 MissingCalendar。改前旧接口按“周末规则”算出收盘时刻并照常回放 5 根日线；改后回放以明确错误终止，
 * 不再静默忽略缺失的日历。对照组为日历已加载的同一品种，照常回放。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>

namespace
{
	std::filesystem::path r1213_root()
	{
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	std::filesystem::path r1213_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("s5_r1213_replay_" + std::string(tag) + "_" + std::to_string(static_cast<long>(::getpid())));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	void r1213_write(const std::filesystem::path& file, const std::string& body)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
		out << body;
	}

	void r1213_daily_csv(const std::filesystem::path& base)
	{
		const uint32_t dates[] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
		std::string body = "date,time,open,high,low,close,vol,money,hold,add,settle\n";
		for (uint32_t i = 0; i < 5; i++)
			body += std::to_string(dates[i]) + ",150000,10,11,9,10.5," + std::to_string(1000 + i) + ",10000,0,0,0\n";
		r1213_write(base / "csv" / "SSE.STK.000001_d.csv", body);
	}

	std::string r1213_commodities(const char* holiday)
	{
		return std::string("{\"SSE\":{\"STK\":{\"category\":0,\"covermode\":0,\"exchg\":\"SSE\",\"holiday\":\"") + holiday +
			"\",\"name\":\"stk\",\"precision\":2,\"pricemode\":1,\"pricetick\":0.01,\"session\":\"SD0930\",\"volscale\":1}}}";
	}

	struct R1213Sink : public IDataSink
	{
		WTSKlineSlice* slice = nullptr;
		HisDataReplayer* replayer = nullptr;
		uint32_t closes = 0;
		uint32_t done = 0;
		void handle_init() override { slice = replayer->get_kline_slice("SSE.STK.000001", "d", 1, 1, true); }
		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override { closes++; }
		void handle_schedule(uint32_t, uint32_t) override {}
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override { done++; }
	};

	struct R1213Result
	{
		bool prepared = false;
		uint32_t closes = 0;
		uint32_t errorCode = 0;
		std::string message;
	};

	R1213Result r1213_replay(const char* holiday)
	{
		R1213Result result;
		const std::filesystem::path base = r1213_dir(holiday);
		const std::filesystem::path out = r1213_dir((std::string(holiday) + "_out").c_str());
		WtHelper::setOutputDir(out.string().c_str());
		r1213_daily_csv(base);
		r1213_write(base / "commodities.json", r1213_commodities(holiday));

		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (r1213_root() / "dist/common/sessions.json").string().c_str());
		basefiles->append("holiday", (r1213_root() / "dist/common/holidays.json").string().c_str());	// 只含 CHINA
		basefiles->append("commodity", (base / "commodities.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();

		HisDataReplayer replayer;
		if (!replayer.init(cfg))
		{
			cfg->release();
			return result;
		}
		cfg->release();
		R1213Sink sink;
		sink.replayer = &replayer;
		replayer.register_sink(&sink, "s5_r1213");
		result.prepared = replayer.prepare();
		if (result.prepared)
			replayer.run(false);
		const WtReplayStatusV1 status = replayer.replay_status();
		result.closes = sink.closes;
		result.errorCode = status.error_code;
		result.message = status.message;
		if (sink.slice != nullptr)
			sink.slice->release();
		std::error_code ec;
		std::filesystem::remove_all(base, ec);
		std::filesystem::remove_all(out, ec);
		return result;
	}
}

TEST(S5MainR1213ReplayerCalendar, DailyCloseWithUnloadedCalendarStopsTheReplay)
{
	const R1213Result missing = r1213_replay("NOPE");
	ASSERT_TRUE(missing.prepared) << "prepare uses the default calendar and must pass";
	EXPECT_NE(0u, missing.errorCode) << "a daily close that cannot be mapped must stop the replay";
	EXPECT_NE(std::string::npos, missing.message.find("MissingCalendar")) << missing.message;
	EXPECT_LT(missing.closes, 5u);
}

TEST(S5MainR1213ReplayerCalendar, LoadedCalendarReplaysAllDailyBars)
{
	const R1213Result loaded = r1213_replay("CHINA");
	ASSERT_TRUE(loaded.prepared);
	EXPECT_EQ(0u, loaded.errorCode) << loaded.message;
	EXPECT_EQ(5u, loaded.closes);
}
