/**
 * r151_common.h  (lane R151)
 *
 * 自包含只读夹具，供 p08_btcore_mockers3/r151_*.cpp 使用：
 *   - repo_root()：从 __FILE__ 向上找含 dist/common/sessions.json 的仓库根；
 *   - make_temp_dir()：系统临时目录 / r151_<tag>_<pid>_<seq>；
 *   - write_bars_csv()/mk_bar()：日线与分钟线 CSV 夹具；
 *   - make_cfg()：csv 模式回放配置（commodities + stk_comms + sessions + holidays + fees）；
 *   - Sink：记录 tick 合约/pxType、bar_close 次数、会话与收尾回调。
 *
 * 只使用公开 API；不 #define private public、不镜像类布局、不改宏。
 * 所有临时文件位于系统临时目录，由各 Fixture 的 TearDown 清理。
 */
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

namespace r151
{
	namespace fs = std::filesystem;

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
		static uint32_t seq = 0;
		auto path = fs::temp_directory_path() /
			("r151_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		fs::remove_all(path, ec);
		fs::create_directories(path, ec);
		return path;
	}

	inline WTSBarStruct mk_bar(uint32_t date, uint32_t hhmm,
		double open, double high, double low, double close, double vol)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = date;
		bar.time = static_cast<uint64_t>(date - 19900000u) * 10000u + hhmm;
		bar.open = open;
		bar.high = high;
		bar.low = low;
		bar.close = close;
		bar.vol = vol;
		bar.money = close * vol;
		bar.settle = close;
		return bar;
	}

	// <base>/csv/<stem>.csv；minute=true 时 time 列取 bar.time 的 HHMM 部分
	inline bool write_bars_csv(const fs::path& base, const std::string& stem,
		const std::vector<WTSBarStruct>& bars, bool minute)
	{
		std::error_code ec;
		fs::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (stem + ".csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (const WTSBarStruct& bar : bars)
		{
			const uint32_t hhmm = minute ? static_cast<uint32_t>(bar.time % 10000) : 150000u;
			out << bar.date << ',' << hhmm << ',' << bar.open << ',' << bar.high << ','
				<< bar.low << ',' << bar.close << ',' << bar.vol << ',' << bar.money
				<< ",0,0," << bar.settle << '\n';
		}
		out.close();
		return true;
	}

	inline WTSVariant* make_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tick);
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

	inline bool init_replay(HisDataReplayer& rp, const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick, EventNotifier* notifier = nullptr)
	{
		WTSVariant* cfg = make_cfg(root, base, stime, etime, tick);
		const bool ok = rp.init(cfg, notifier);
		cfg->release();
		return ok;
	}

	class Sink : public IDataSink
	{
	public:
		uint32_t init_calls = 0;
		uint32_t begin_calls = 0;
		uint32_t end_calls = 0;
		uint32_t bar_calls = 0;
		uint32_t done_calls = 0;
		uint32_t schedule_calls = 0;
		std::vector<std::string> tick_codes;
		std::vector<uint32_t> tick_px_types;
		std::vector<uint32_t> bar_dates;

		void handle_tick(const char* stdCode, WTSTickData*, uint32_t pxType) override
		{
			tick_codes.emplace_back(stdCode);
			tick_px_types.emplace_back(pxType);
		}

		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct* newBar) override
		{
			bar_calls++;
			if (newBar != nullptr)
				bar_dates.emplace_back(newBar->date);
		}

		void handle_schedule(uint32_t, uint32_t) override { schedule_calls++; }
		void handle_init() override { init_calls++; }
		void handle_session_begin(uint32_t) override { begin_calls++; }
		void handle_session_end(uint32_t) override { end_calls++; }
		void handle_replay_done() override { done_calls++; }

		uint32_t count_code(const char* code) const
		{
			return static_cast<uint32_t>(std::count(tick_codes.begin(), tick_codes.end(), std::string(code)));
		}
	};

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
