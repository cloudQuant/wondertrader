/**
 * r67_common.h  (lane R67)
 *
 * 供 p07_btcore_replayer2/r67_*.cpp 与 q02_btcore3/r67_*.cpp 共用的只读夹具：
 *   - 仓库根定位、temp_directory_path()/r67_* 临时目录
 *   - bar / tick CSV 与 K 线块(dsb) 写入
 *   - 可编程 IBtDataLoader 桩（按周期/合约给内存行情，记录调用次数）
 *   - IDataSink 记录器（tick / bar_close / session 事件）
 *   - HisDataReplayer 的 csv 模式配置构造
 * 所有落盘都在系统临时目录，SetUp/TearDown 双向 remove_all，不写仓库。
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <WtDataStorage/DataDefine.h>
#include <WTSUtils/WTSCmpHelper.hpp>

namespace r67
{

	inline std::filesystem::path root()
	{
		// __FILE__ = <repo>/src/TestUnits/coverage/p07_btcore_replayer2/r67_common.h
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	inline std::filesystem::path temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("r67_" + std::string(tag) + "_" + std::to_string(::getpid()));
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

	// 生成压缩块（HisKlineBlockV2 + zstd），供 cache*FromBin/FromLoader 的 dsb 分支读取
	inline bool write_kline_dsb(const std::filesystem::path& file, BlockType type,
		const std::vector<WTSBarStruct>& bars)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		std::string cmp = WTSCmpHelper::compress_data(bars.data(), sizeof(WTSBarStruct) * bars.size());
		std::string content(sizeof(HisKlineBlockV2), 0);
		HisKlineBlockV2* blk = (HisKlineBlockV2*)content.data();
		strcpy(blk->_blk_flag, BLK_FLAG);
		blk->_type = type;
		blk->_version = BLOCK_VERSION_CMP_V2;
		blk->_size = cmp.size();
		content.append(cmp);
		std::ofstream out(file, std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), (std::streamsize)content.size());
		out.close();
		return true;
	}

	struct TickRow
	{
		uint32_t date;
		uint32_t hhmm;
		double price;
		double volume;
	};

	inline WTSTickStruct mk_tick(const TickRow& row)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, "SSE");
		strcpy(ts.code, "600000");
		ts.action_date = row.date;
		ts.trading_date = row.date;
		ts.action_time = row.hhmm * 100000u;
		ts.price = row.price;
		ts.open = row.price;
		ts.high = row.price;
		ts.low = row.price;
		ts.volume = row.volume;
		ts.total_volume = row.volume;
		return ts;
	}

	// 只记录调用次数与最近参数的内存装载器；不依赖存储引擎/动态库
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
		std::map<std::string, uint32_t> rawCalls;
		std::map<std::string, uint32_t> tickCalls;

		WTSKlinePeriod lastFinalPeriod { KP_DAY };
		WTSKlinePeriod lastRawPeriod { KP_DAY };
		std::string lastCode;

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
			lastCode = stdCode;
			lastFinalPeriod = period;
			const std::vector<WTSBarStruct>* bars = pick(period, stdCode);
			if (bars == NULL || bars->empty())
				return emptyFinalSuccess;
			cb(obj, const_cast<WTSBarStruct*>(bars->data()), (uint32_t)bars->size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			rawCalls[stdCode]++;
			lastCode = stdCode;
			lastRawPeriod = period;
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
			lastCode = stdCode;
			auto it = ticks.find(std::string(stdCode) + "#" + std::to_string(uDate));
			if (it == ticks.end() || it->second.empty())
				return false;
			cb(obj, it->second.data(), (uint32_t)it->second.size());
			return true;
		}

		bool isAutoTrans() override { return autoTrans; }
	};

	class Sink : public IDataSink
	{
	public:
		struct TickRec
		{
			std::string code;
			uint32_t pxType;
			uint32_t date;
			double price;
		};

		struct BarRec
		{
			std::string code;
			std::string period;
			uint32_t times;
			uint32_t date;
			double close;
		};

		std::function<void()> onInit;
		std::function<void()> onTick;
		std::function<void(uint32_t)> onSessionBegin;
		HisDataReplayer* rp { NULL };

		std::vector<TickRec> ticks;
		std::vector<BarRec> bars;

		uint32_t initCalls { 0 };
		uint32_t beginCalls { 0 };
		uint32_t endCalls { 0 };
		uint32_t doneCalls { 0 };
		uint32_t sectionCalls { 0 };
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
			rec.price = (curTick == NULL) ? 0.0 : curTick->price();
			ticks.push_back(rec);
			if (onTick)
				onTick();
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			BarRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			rec.period = (period == NULL) ? std::string() : std::string(period);
			rec.times = times;
			rec.date = (newBar == NULL) ? 0u : newBar->date;
			rec.close = (newBar == NULL) ? 0.0 : newBar->close;
			bars.push_back(rec);
		}

		void handle_schedule(uint32_t, uint32_t) override { scheduleCalls++; }
		void handle_session_begin(uint32_t curTDate) override
		{
			beginCalls++;
			if (onSessionBegin)
				onSessionBegin(curTDate);
		}
		void handle_session_end(uint32_t) override { endCalls++; }
		void handle_section_end(uint32_t, uint32_t) override { sectionCalls++; }
		void handle_replay_done() override { doneCalls++; }
	};

	inline WTSVariant* make_cfg(const std::filesystem::path& repo,
		const std::filesystem::path& base, uint64_t stime, uint64_t etime,
		bool tick, uint32_t cacheDays = 10, bool nosim = false, const char* fees = "")
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", nosim);
		cfg->append("cache_clear_days", cacheDays);
		cfg->append("fees", fees);

		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (repo / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (repo / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (repo / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
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

}	// namespace r67
