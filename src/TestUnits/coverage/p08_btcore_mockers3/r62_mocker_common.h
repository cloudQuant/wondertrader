/**
 * r62_mocker_common.h  (lane R62)
 *
 * 共享夹具：真实 HisDataReplayer（csv 模式、无策略 DLL、无真实行情/柜台），
 * 基础数据来自仓库 dist/common（只读），日线 CSV 写在系统临时目录。
 * 测试只使用公开 API：WtHelper::setOutputDir、HisDataReplayer::init、
 * WTSTickData::create/release、各 Mocker 的 IDataSink / I*StraCtx 公共方法。
 *
 * 不使用 #define private public，不镜像类布局，不绕过公开接口。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <unistd.h>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

namespace r62
{
	namespace fs = std::filesystem;

	// 从 __FILE__ / 当前目录向上找仓库根（含 dist/common/sessions.json）。
	// 这样不依赖编译期 __FILE__ 是绝对还是相对路径，也不依赖测试进程的 cwd。
	inline fs::path probe_repo_root(fs::path p)
	{
		std::error_code ec;
		if (p.empty())
			return fs::path();

		p = fs::absolute(p, ec);
		while (!p.empty())
		{
			if (fs::exists(p / "dist" / "common" / "sessions.json", ec))
				return p;

			fs::path parent = p.parent_path();
			if (parent == p)
				break;
			p = parent;
		}
		return fs::path();
	}

	inline fs::path repo_root()
	{
		fs::path root = probe_repo_root(fs::path(__FILE__).parent_path());
		if (!root.empty())
			return root;

		return probe_repo_root(fs::current_path());
	}

	inline fs::path make_temp_dir(const char* tag)
	{
		auto path = fs::temp_directory_path() /
			("r62_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		fs::remove_all(path, ec);
		fs::create_directories(path, ec);
		return path;
	}

	// 日线 CSV：<base>/csv/<stdCode>_d.csv（HisDataReplayer::cacheRawBarsFromCSV 约定）。
	inline bool write_daily_csv(const fs::path& base, const char* stdCode)
	{
		std::error_code ec;
		fs::create_directories(base / "csv", ec);

		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		const uint32_t dates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
		for (uint32_t i = 0; i < 5; i++)
		{
			const double close = 10.0 + i * 0.5;
			out << dates[i] << ",150000,"
				<< (close - 0.2) << "," << (close + 0.4) << ","
				<< (close - 0.5) << "," << close << ","
				<< (1000 + i) << "," << (1000 + i) * close << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	inline WTSVariant* make_replay_cfg(const fs::path& root, const fs::path& base)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", static_cast<uint64_t>(202001020930));
		cfg->append("etime", static_cast<uint64_t>(202001081500));
		cfg->append("tick", false);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", (root / "dist/common/fees.json").string().c_str());

		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());

		WTSVariant* comms = WTSVariant::createArray();
		comms->append((root / "dist/common/commodities.json").string().c_str());
		comms->append((root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("commodity", comms);
		comms->release();

		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	inline WTSTickData* make_tick(const char* stdCode, uint32_t uDate, uint32_t uTimeMs,
		double last, double bid, double ask, uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(WTSTickStruct));
		wt_strcpy(ts.code, stdCode);
		ts.action_date = uDate;
		ts.trading_date = uDate;
		ts.action_time = uTimeMs;
		ts.price = last;
		ts.open = last;
		ts.high = last;
		ts.low = last;
		ts.pre_close = last;
		ts.volume = 100;
		ts.total_volume = 1000;
		ts.turn_over = last * 100;
		ts.total_turnover = last * 1000;
		for (int32_t i = 0; i < 10; i++)
		{
			ts.bid_prices[i] = bid - i;
			ts.ask_prices[i] = ask + i;
			ts.bid_qty[i] = bidQty;
			ts.ask_qty[i] = askQty;
		}
		return WTSTickData::create(ts);
	}

	inline WTSBarStruct make_bar(uint32_t uDate, uint32_t hhmm, double close)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(WTSBarStruct));
		bar.date = uDate;
		bar.time = static_cast<uint64_t>(uDate - 19900000u) * 10000 + hhmm;
		bar.open = close;
		bar.high = close;
		bar.low = close;
		bar.close = close;
		bar.vol = 100;
		bar.money = close * 100;
		return bar;
	}

	inline std::string read_file(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	inline uint32_t line_count(const std::string& content)
	{
		if (content.empty())
			return 0;
		uint32_t lines = 0;
		for (char c : content)
		{
			if (c == '\n')
				lines++;
		}
		if (content.back() != '\n')
			lines++;
		return lines;
	}

	class ReplayFixture : public ::testing::Test
	{
	protected:
		HisDataReplayer _rp;
		fs::path _root;
		fs::path _base;
		fs::path _out;

		void SetUp() override
		{
			_root = repo_root();
			ASSERT_FALSE(_root.empty()) << "cannot locate repo root containing dist/common/sessions.json";

			_base = make_temp_dir("base");
			_out = make_temp_dir("out");
			WtHelper::setOutputDir(_out.string().c_str());

			ASSERT_TRUE(write_daily_csv(_base, "SHFE.rb.2405"));
			ASSERT_TRUE(write_daily_csv(_base, "SHFE.cu.2406"));
			ASSERT_TRUE(write_daily_csv(_base, "SSE.STK.000001"));

			WTSVariant* cfg = make_replay_cfg(_root, _base);
			ASSERT_TRUE(_rp.init(cfg));
			cfg->release();
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			std::error_code ec;
			fs::remove_all(_base, ec);
			fs::remove_all(_out, ec);
		}
	};
}
