/**
 * r72_common.h  (lane R72)
 *
 * 共享夹具：真实 HisDataReplayer（csv/bin 模式）+ 纯内存 IBtDataLoader，
 * 基础数据只读 dist/common；行情文件只写系统临时目录。
 *
 * 只使用公开 API：
 *   - WTSVariant / HisDataReplayer::init/prepare/get_*
 *   - WTSTickData::create / release
 *   - DataDefine 中的块头与 WTSBarStructOld / WTSTickStructOld（用于构造
 *     cacheRawBarsFromCSV / cacheRawTicksFromCSV 的老版本 dsb 分支）
 *
 * 不使用 #define private public，不镜像类布局，不改编译宏。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <WtDataStorage/DataDefine.h>

namespace r72
{
	namespace fs = std::filesystem;

	inline fs::path repo_root()
	{
		// __FILE__ = <repo>/src/TestUnits/coverage/p03_btcore_mockers2/r72_common.h
		return fs::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	inline fs::path temp_dir(const char* tag)
	{
		auto path = fs::temp_directory_path() /
			("r72_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		fs::remove_all(path, ec);
		fs::create_directories(path, ec);
		return path;
	}

	inline bool write_text(const fs::path& file, const std::string& content)
	{
		std::error_code ec;
		fs::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), (std::streamsize)content.size());
		out.close();
		return true;
	}

	inline std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
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
			if (c == '\n')
				lines++;
		if (content.back() != '\n')
			lines++;
		return lines;
	}

	inline WTSBarStruct make_bar(uint32_t date, uint32_t hhmm, double close)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(bar));
		bar.date = date;
		bar.time = (uint64_t)(date - 19900000u) * 10000u + hhmm;
		bar.open = close - 0.1;
		bar.high = close + 0.2;
		bar.low = close - 0.2;
		bar.close = close;
		bar.settle = close;
		bar.vol = 100;
		bar.money = close * 100;
		return bar;
	}

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
			const uint32_t hhmm = minute ? (uint32_t)(bar.time % 10000) : 150000u;
			out << bar.date << ',' << hhmm << ',' << bar.open << ',' << bar.high << ','
				<< bar.low << ',' << bar.close << ',' << bar.vol << ',' << bar.money
				<< ",0,0," << bar.settle << '\n';
		}
		out.close();
		return true;
	}

	// 老版本、未压缩的日线块：BlockHeader(12) + WTSBarStructOld[]。
	// 走 proc_block_data 的 bOldVer 分支（HisDataReplayer.cpp L86-L108）。
	inline bool write_old_bar_block(const fs::path& file, const std::vector<WTSBarStructOld>& bars)
	{
		std::string content(sizeof(BlockHeader), 0);
		BlockHeader* head = (BlockHeader*)content.data();
		strcpy(head->_blk_flag, BLK_FLAG);
		head->_type = BT_HIS_Day;
		head->_version = BLOCK_VERSION_RAW;
		content.append((const char*)bars.data(), sizeof(WTSBarStructOld) * bars.size());
		return write_text(file, content);
	}

	// 老版本、未压缩的 tick 块：BlockHeader(12) + WTSTickStructOld[]。
	// 走 proc_block_data 的 bOldVer 分支（HisDataReplayer.cpp L109-L123）。
	inline bool write_old_tick_block(const fs::path& file, const std::vector<WTSTickStructOld>& ticks)
	{
		std::string content(sizeof(BlockHeader), 0);
		BlockHeader* head = (BlockHeader*)content.data();
		strcpy(head->_blk_flag, BLK_FLAG);
		head->_type = BT_HIS_Ticks;
		head->_version = BLOCK_VERSION_RAW;
		content.append((const char*)ticks.data(), sizeof(WTSTickStructOld) * ticks.size());
		return write_text(file, content);
	}

	inline WTSTickData* make_tick(const char* stdCode, uint32_t date, uint32_t hhmmssmmm,
		double price, double bid, double ask, uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		wt_strcpy(ts.code, stdCode);
		ts.action_date = date;
		ts.trading_date = date;
		ts.action_time = hhmmssmmm;
		ts.price = price;
		ts.open = price - 0.5;
		ts.high = price + 0.5;
		ts.low = price - 1.0;
		ts.pre_close = price - 0.2;
		ts.volume = 100;
		ts.total_volume = 1000;
		ts.turn_over = price * 100;
		ts.total_turnover = price * 1000;
		ts.bid_prices[0] = bid;
		ts.ask_prices[0] = ask;
		ts.bid_qty[0] = bidQty;
		ts.ask_qty[0] = askQty;
		return WTSTickData::create(ts);
	}

	inline WTSTickStruct make_tick_struct(const char* stdCode, uint32_t date, uint32_t hhmmssmmm,
		double price, double bid, double ask, uint32_t bidQty = 100, uint32_t askQty = 100)
	{
		WTSTickData* data = make_tick(stdCode, date, hhmmssmmm, price, bid, ask, bidQty, askQty);
		WTSTickStruct ts = data->getTickStruct();
		data->release();
		return ts;
	}

	/**
	 * 纯内存装载器：
	 *   - finalBars：loadFinalHisBars / loadRawHisBars 共用
	 *   - ticks：key = stdCode#date
	 *   - throwFinal：0 正常；1 抛 std::runtime_error；2 抛非 std 异常
	 *   - emptyTickSuccess：loadRawHisTicks 返回 true 但不回调
	 */
	class Loader : public IBtDataLoader
	{
	public:
		std::map<std::string, std::vector<WTSBarStruct>> finalBars;
		std::map<std::string, std::vector<WTSTickStruct>> ticks;
		int throwFinal { 0 };
		bool emptyTickSuccess { false };
		bool autoTrans { false };

		uint32_t finalCalls { 0 };
		uint32_t rawCalls { 0 };
		uint32_t tickCalls { 0 };
		uint32_t adjAllCalls { 0 };
		WTSKlinePeriod lastFinalPeriod { KP_DAY };

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			finalCalls++;
			lastFinalPeriod = period;
			if (throwFinal == 1)
				throw std::runtime_error("r72 loader final bars failure");
			if (throwFinal == 2)
				throw 42;

			auto it = finalBars.find(stdCode);
			if (it == finalBars.end() || it->second.empty())
				return false;

			cb(obj, it->second.data(), (uint32_t)it->second.size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			rawCalls++;
			lastFinalPeriod = period;
			auto it = finalBars.find(stdCode);
			if (it == finalBars.end() || it->second.empty())
				return false;

			cb(obj, it->second.data(), (uint32_t)it->second.size());
			return true;
		}

		bool loadAllAdjFactors(void*, FuncReadFactors) override
		{
			adjAllCalls++;
			return false;
		}

		bool loadAdjFactors(void*, const char*, FuncReadFactors) override { return false; }

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			tickCalls++;
			if (emptyTickSuccess)
				return true;

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
		HisDataReplayer* rp { nullptr };
		std::function<void()> onTick;

		uint32_t initCalls { 0 };
		uint32_t beginCalls { 0 };
		uint32_t endCalls { 0 };
		uint32_t sectionCalls { 0 };
		uint32_t scheduleCalls { 0 };
		uint32_t doneCalls { 0 };
		uint32_t tickCalls { 0 };
		uint32_t barCalls { 0 };

		void handle_tick(const char* /*stdCode*/, WTSTickData* /*curTick*/, uint32_t /*pxType*/) override
		{
			tickCalls++;
			if (onTick)
				onTick();
		}

		void handle_bar_close(const char* /*stdCode*/, const char* /*period*/,
			uint32_t /*times*/, WTSBarStruct* /*newBar*/) override
		{
			barCalls++;
		}

		void handle_schedule(uint32_t, uint32_t) override { scheduleCalls++; }
		void handle_init() override { initCalls++; }
		void handle_session_begin(uint32_t) override { beginCalls++; }
		void handle_session_end(uint32_t) override { endCalls++; }
		void handle_section_end(uint32_t, uint32_t) override { sectionCalls++; }
		void handle_replay_done() override { doneCalls++; }
	};

	inline WTSVariant* make_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick, const char* fees = "")
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
		cfg->append("fees", fees);

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

	class Fixture : public ::testing::Test
	{
	protected:
		fs::path _root;
		fs::path _base;
		fs::path _out;

		void SetUp() override
		{
			_root = repo_root();
			ASSERT_FALSE(_root.empty());
			_base = temp_dir("base");
			_out = temp_dir("out");
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

}	// namespace r72
