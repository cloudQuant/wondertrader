/**
 * r80_common.h  (lane R80)
 *
 * 自包含只读夹具，供 q02_btcore3/r80_*.cpp 使用：
 *   - 仓库根定位、temp_directory_path()/r80_* 临时目录
 *   - bar CSV 写入、daily/minute bar 工厂
 *   - 可编程 IBtDataLoader 桩（日线/m1/m5/tick，纯内存）
 *   - IDataSink 记录器（tick / bar_close / schedule / session）
 *   - HisDataReplayer 的 csv 配置构造（含基础数据文件覆写）
 * 所有落盘都在系统临时目录，SetUp/TearDown 双向 remove_all，不写仓库。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <WtDataStorage/DataDefine.h>

namespace r80
{

	inline std::filesystem::path root()
	{
		// __FILE__ = <repo>/src/TestUnits/coverage/q02_btcore3/r80_common.h
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	inline std::filesystem::path temp_dir(const char* tag)
	{
		static uint32_t seq = 0;
		auto path = std::filesystem::temp_directory_path() /
			("r80_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	inline WTSBarStruct mk_bar(uint32_t date, uint32_t hhmm,
		double open, double high, double low, double close, double vol)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = date;
		bar.time = (uint64_t)(date - 19900000u) * 10000u + hhmm;
		bar.open = open;
		bar.high = high;
		bar.low = low;
		bar.close = close;
		bar.vol = vol;
		bar.money = close * vol;
		bar.settle = close;
		return bar;
	}

	inline bool write_text(const std::filesystem::path& file, const std::string& content)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), (std::streamsize)content.size());
		out.close();
		return true;
	}

	// <base>/csv/<stem>.csv；minute=true 时 time 列取 bar.time 的 HHMM 部分
	inline bool write_bars_csv(const std::filesystem::path& base, const std::string& stem,
		const std::vector<WTSBarStruct>& bars, bool minute)
	{
		std::error_code ec;
		std::filesystem::create_directories(base / "csv", ec);
		std::ofstream out(base / "csv" / (stem + ".csv"));
		if (!out.is_open())
			return false;
		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		for (const WTSBarStruct& bar : bars)
		{
			const uint32_t hhmm = minute ? (uint32_t)(bar.time % 10000) : 150000u;
			out << bar.date << ',' << hhmm << ',' << bar.open << ',' << bar.high << ','
				<< bar.low << ',' << bar.close << ',' << bar.vol << ',' << bar.money
				<< ",0,0," << bar.settle << '\n';
		}
		out.close();
		return true;
	}

	inline WTSTickStruct mk_tick(uint32_t date, uint32_t hhmm, double price, double volume)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, "SSE");
		strcpy(ts.code, "000001");
		ts.action_date = date;
		ts.trading_date = date;
		ts.action_time = hhmm * 100000u;
		ts.price = price;
		ts.open = price;
		ts.high = price;
		ts.low = price;
		ts.volume = volume;
		ts.total_volume = volume;
		return ts;
	}

	// 只提供内存行情的装载器，不依赖存储引擎/动态库
	class Loader : public IBtDataLoader
	{
	public:
		bool autoTrans { false };
		bool emptyFinalSuccess { false };

		std::map<std::string, std::vector<WTSBarStruct>> dayBars;
		std::map<std::string, std::vector<WTSBarStruct>> m1Bars;
		std::map<std::string, std::vector<WTSBarStruct>> m5Bars;
		std::map<std::string, std::vector<WTSTickStruct>> ticks;	// key: code#date

		std::map<std::string, uint32_t> finalCalls;
		std::map<std::string, uint32_t> tickCalls;

		const std::vector<WTSBarStruct>* pick(WTSKlinePeriod period, const std::string& code) const
		{
			if (period == KP_Minute1)
			{
				auto it = m1Bars.find(code);
				return it == m1Bars.end() ? NULL : &it->second;
			}
			if (period == KP_Minute5)
			{
				auto it = m5Bars.find(code);
				return it == m5Bars.end() ? NULL : &it->second;
			}
			auto it = dayBars.find(code);
			return it == dayBars.end() ? NULL : &it->second;
		}

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			finalCalls[stdCode]++;
			const std::vector<WTSBarStruct>* bars = pick(period, stdCode);
			if (bars == NULL || bars->empty())
				return emptyFinalSuccess;
			cb(obj, const_cast<WTSBarStruct*>(bars->data()), (uint32_t)bars->size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			const std::vector<WTSBarStruct>* bars = pick(period, stdCode);
			if (bars == NULL || bars->empty())
				return false;
			cb(obj, const_cast<WTSBarStruct*>(bars->data()), (uint32_t)bars->size());
			return true;
		}

		bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }
		bool loadAdjFactors(void*, const char*, FuncReadFactors) override { return false; }

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			tickCalls[stdCode]++;
			auto it = ticks.find(std::string(stdCode) + "#" + std::to_string(uDate));
			if (it == ticks.end() || it->second.empty())
				return false;
			cb(obj, it->second.data(), (uint32_t)it->second.size());
			return true;
		}

		bool isAutoTrans() override { return autoTrans; }
	};

	struct TickRec
	{
		std::string code;
		uint32_t pxType { 0 };
		uint32_t date { 0 };
		uint32_t time { 0 };
		double price { 0.0 };
	};

	struct BarRec
	{
		std::string code;
		std::string period;
		uint32_t times { 0 };
		uint32_t date { 0 };
		uint32_t time { 0 };
		double close { 0.0 };
	};

	class Sink : public IDataSink
	{
	public:
		std::function<void()> onInit;
		std::function<void(const TickRec&)> onTick;
		std::function<void(const BarRec&)> onBarClose;

		HisDataReplayer* rp { NULL };
		WTSKlineSlice* slice { NULL };

		std::vector<TickRec> ticks;
		std::vector<BarRec> bars;
		std::vector<uint32_t> scheduleDates;
		std::vector<uint32_t> scheduleTimes;
		std::vector<uint32_t> beginDates;
		std::vector<uint32_t> endDates;

		uint32_t initCalls { 0 };
		uint32_t beginCalls { 0 };
		uint32_t endCalls { 0 };
		uint32_t doneCalls { 0 };
		uint32_t scheduleCalls { 0 };

		void handle_init() override
		{
			initCalls++;
			if (onInit)
				onInit();
		}

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			TickRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			rec.pxType = pxType;
			rec.date = (curTick == NULL) ? 0u : curTick->actiondate();
			rec.time = (curTick == NULL) ? 0u : curTick->actiontime() / 100000u;
			rec.price = (curTick == NULL) ? 0.0 : curTick->price();
			ticks.push_back(rec);
			if (onTick)
				onTick(rec);
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			BarRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			rec.period = (period == NULL) ? std::string() : std::string(period);
			rec.times = times;
			rec.date = (newBar == NULL) ? 0u : newBar->date;
			rec.time = (newBar == NULL) ? 0u : (uint32_t)(newBar->time % 10000u);
			rec.close = (newBar == NULL) ? 0.0 : newBar->close;
			bars.push_back(rec);
			if (onBarClose)
				onBarClose(rec);
		}

		void handle_schedule(uint32_t uDate, uint32_t uTime) override
		{
			scheduleCalls++;
			scheduleDates.push_back(uDate);
			scheduleTimes.push_back(uTime);
		}

		void handle_session_begin(uint32_t curTDate) override
		{
			beginCalls++;
			beginDates.push_back(curTDate);
		}

		void handle_session_end(uint32_t curTDate) override
		{
			endCalls++;
			endDates.push_back(curTDate);
		}

		void handle_replay_done() override { doneCalls++; }
	};

	struct CfgOpts
	{
		bool tick { false };
		bool noSim { false };
		uint32_t cacheDays { 10 };
		std::string mode { "csv" };
		std::string fees;
		std::string session;
		std::string commodity;
		std::string contract;
		std::string holiday;
		std::string hot;
		std::string second;
		std::string adjfactor;
		std::vector<std::string> contracts;						// 非空则按数组形态装载
		std::vector<std::pair<std::string, std::string>> rules;	// tag -> file
	};

	inline std::string pick_path(const std::filesystem::path& repo, const std::string& val, const char* def)
	{
		return val.empty() ? (repo / def).string() : val;
	}

	inline WTSVariant* make_cfg(const std::filesystem::path& repo, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, const CfgOpts& opts)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", opts.mode.c_str());
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", opts.tick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", opts.noSim);
		cfg->append("cache_clear_days", opts.cacheDays);
		cfg->append("fees", opts.fees.c_str());

		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", pick_path(repo, opts.session, "dist/common/sessions.json").c_str());
		basefiles->append("commodity", pick_path(repo, opts.commodity, "dist/common/stk_comms.json").c_str());
		basefiles->append("holiday", pick_path(repo, opts.holiday, "dist/common/holidays.json").c_str());
		if (!opts.contracts.empty())
		{
			WTSVariant* ay = WTSVariant::createArray();
			for (const std::string& file : opts.contracts)
				ay->append(file.c_str());
			basefiles->append("contract", ay);
			ay->release();
		}
		else if (!opts.contract.empty())
		{
			basefiles->append("contract", opts.contract.c_str());
		}
		if (!opts.hot.empty())
			basefiles->append("hot", opts.hot.c_str());
		if (!opts.second.empty())
			basefiles->append("second", opts.second.c_str());
		if (!opts.rules.empty())
		{
			WTSVariant* rules = WTSVariant::createObject();
			for (const auto& item : opts.rules)
				rules->append(item.first.c_str(), item.second.c_str());
			basefiles->append("rules", rules);
			rules->release();
		}
		cfg->append("basefiles", basefiles);
		basefiles->release();

		if (!opts.adjfactor.empty())
			cfg->append("adjfactor", opts.adjfactor.c_str());

		return cfg;
	}

	class Fixture : public ::testing::Test
	{
	protected:
		std::filesystem::path _root;
		std::filesystem::path _base;
		std::filesystem::path _out;

		void SetUp() override
		{
			_root = root();
			_base = temp_dir("data");
			_out = temp_dir("out");
			WtHelper::setOutputDir(_out.string().c_str());
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
			std::filesystem::remove_all(_out, ec);
		}
	};

}	// namespace r80
