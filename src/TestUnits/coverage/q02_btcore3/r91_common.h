/**
 * r91_common.h  (lane R91)
 *
 * 自包含只读夹具，供 q02_btcore3/r91_*.cpp 使用：
 *   - 仓库根/temp 目录、安装目录探测（WtDataStorage 动态库）
 *   - 未压缩 RAW_V2 块（BlockHeader + POD 数组）写盘：day/min K线、tick、Level2
 *   - 可编程 IBtDataLoader（只提供复权因子；K线/tick 故意返回 false 以走 bin 存储）
 *   - IDataSink 记录器（tick/bar/Level2/session/schedule，可在 tick 回调内取日线价格）
 *   - HisDataReplayer 的 bin/csv 配置构造
 * 所有落盘都在系统临时目录，SetUp/TearDown 双向 remove_all，不写仓库。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <WtDataStorage/DataDefine.h>

namespace r91
{

	inline std::filesystem::path root()
	{
		// __FILE__ = <repo>/src/TestUnits/coverage/q02_btcore3/r91_common.h
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	}

	inline std::filesystem::path temp_dir(const char* tag)
	{
		static uint32_t seq = 0;
		auto path = std::filesystem::temp_directory_path() /
			("r91_" + std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	// WtDataStorage 已链接进测试二进制；这里把安装目录钉到包含该模块的构建目录，
	// 避免同二进制内其它用例残留的 setInstDir 让 HisDataMgr 装载失败。
	inline void prepare_inst_dir()
	{
		WtHelper::setInstDir("");

		const std::string module = DLLHelper::wrap_module("WtDataStorage");
		const char* const rels[] = {
			"build-cov/build_x64/Debug/bin",
			"build-cov/build_x64/Release/bin",
			"build_x64/Debug/bin",
			"build_x64/Release/bin",
		};
		for (const char* rel : rels)
		{
			std::filesystem::path dir = root() / rel;
			std::error_code ec;
			if (std::filesystem::exists(dir / module, ec))
			{
				WtHelper::setInstDir((dir.string() + "/").c_str());
				return;
			}
		}
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

	// 生产侧 WtDataReader::proc_block_data 对 RAW_V2 只跳过 sizeof(BlockHeader)，
	// 因此夹具也写成 12 字节 BlockHeader + 原始 POD 数组（与读取实现一致）。
	template <typename T>
	inline bool write_block(const std::filesystem::path& file, uint16_t type, const std::vector<T>& items)
	{
		std::string content(sizeof(BlockHeader), '\0');
		BlockHeader* header = (BlockHeader*)content.data();
		strcpy(header->_blk_flag, BLK_FLAG);
		header->_type = type;
		header->_version = BLOCK_VERSION_RAW_V2;
		if (!items.empty())
			content.append((const char*)items.data(), items.size() * sizeof(T));
		return write_text(file, content);
	}

	inline WTSBarStruct mk_bar(uint32_t date, uint32_t hhmm,
		double open, double high, double low, double close, double vol)
	{
		WTSBarStruct bar;
		bar.date = date;
		bar.time = (uint64_t)(date - 19900000u) * 10000u + hhmm;
		bar.open = open;
		bar.high = high;
		bar.low = low;
		bar.close = close;
		bar.settle = close;
		bar.money = close * vol;
		bar.vol = vol;
		bar.hold = vol / 2.0;
		bar.add = vol / 20.0;
		return bar;
	}

	inline bool write_bars(const std::filesystem::path& base, const char* exchg,
		const char* code, const char* period, const std::vector<WTSBarStruct>& bars)
	{
		return write_block(base / "his" / period / exchg / (std::string(code) + ".dsb"),
			BT_HIS_Day, bars);
	}

	inline WTSTickStruct mk_tick(uint32_t date, uint32_t hhmm, double price, double volume,
		const char* code = "000001")
	{
		WTSTickStruct tick;
		strcpy(tick.exchg, "SSE");
		strcpy(tick.code, code);
		tick.action_date = date;
		tick.trading_date = date;
		tick.action_time = hhmm * 100000u;
		tick.price = price;
		tick.open = price - 0.5;
		tick.high = price + 1.0;
		tick.low = price - 2.0;
		tick.volume = volume;
		tick.total_volume = volume;
		return tick;
	}

	inline bool write_ticks(const std::filesystem::path& base, const char* exchg,
		const char* code, uint32_t date, const std::vector<WTSTickStruct>& ticks)
	{
		return write_block(base / "his" / "ticks" / exchg / std::to_string(date) /
			(std::string(code) + ".dsb"), BT_HIS_Ticks, ticks);
	}

	inline WTSOrdDtlStruct mk_orddtl(uint32_t date, uint32_t hhmm, double price, uint32_t volume,
		const char* code = "000001")
	{
		WTSOrdDtlStruct item;
		strcpy(item.exchg, "SSE");
		strcpy(item.code, code);
		item.action_date = date;
		item.trading_date = date;
		item.action_time = hhmm * 100000u;
		item.price = price;
		item.volume = volume;
		return item;
	}

	inline WTSOrdQueStruct mk_ordque(uint32_t date, uint32_t hhmm, double price, uint32_t volume,
		const char* code = "000001")
	{
		WTSOrdQueStruct item;
		strcpy(item.exchg, "SSE");
		strcpy(item.code, code);
		item.action_date = date;
		item.trading_date = date;
		item.action_time = hhmm * 100000u;
		item.price = price;
		item.qsize = volume;
		item.volumes[0] = volume;
		return item;
	}

	inline WTSTransStruct mk_trans(uint32_t date, uint32_t hhmm, double price, uint32_t volume,
		const char* code = "000001")
	{
		WTSTransStruct item;
		strcpy(item.exchg, "SSE");
		strcpy(item.code, code);
		item.action_date = date;
		item.trading_date = date;
		item.action_time = hhmm * 100000u;
		item.price = price;
		item.volume = volume;
		return item;
	}

	inline bool write_orddtl(const std::filesystem::path& base, const char* exchg,
		const char* code, uint32_t date, const std::vector<WTSOrdDtlStruct>& items)
	{
		return write_block(base / "his" / "orders" / exchg / std::to_string(date) /
			(std::string(code) + ".dsb"), BT_HIS_OrdDetail, items);
	}

	inline bool write_ordque(const std::filesystem::path& base, const char* exchg,
		const char* code, uint32_t date, const std::vector<WTSOrdQueStruct>& items)
	{
		return write_block(base / "his" / "queue" / exchg / std::to_string(date) /
			(std::string(code) + ".dsb"), BT_HIS_OrdQueue, items);
	}

	inline bool write_trans(const std::filesystem::path& base, const char* exchg,
		const char* code, uint32_t date, const std::vector<WTSTransStruct>& items)
	{
		return write_block(base / "his" / "trans" / exchg / std::to_string(date) /
			(std::string(code) + ".dsb"), BT_HIS_Trnsctn, items);
	}

	// 只提供复权因子；K线/tick 一律返回 false，迫使 bin 分支从 WtDataStorage 目录读取
	class Loader : public IBtDataLoader
	{
	public:
		std::map<std::string, std::vector<std::pair<uint32_t, double>>> factors;

		bool loadFinalHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
		bool loadRawHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
		bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }
		bool loadRawHisTicks(void*, const char*, uint32_t, FuncReadTicks) override { return false; }

		bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
		{
			auto it = factors.find(stdCode);
			if (it == factors.end() || it->second.empty())
				return false;

			std::vector<uint32_t> dates;
			std::vector<double> values;
			for (const auto& pair : it->second)
			{
				dates.push_back(pair.first);
				values.push_back(pair.second);
			}
			cb(obj, stdCode, dates.data(), values.data(), (uint32_t)dates.size());
			return true;
		}

		bool isAutoTrans() override { return false; }
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
		double vol { 0.0 };
		double money { 0.0 };
		double hold { 0.0 };
		double add { 0.0 };
	};

	struct HftRec
	{
		std::string code;
		uint32_t date { 0 };
		uint32_t time { 0 };
		double price { 0.0 };
		double volume { 0.0 };
	};

	class Sink : public IDataSink
	{
	public:
		HisDataReplayer* rp { NULL };
		bool collectDayPrice { false };

		std::vector<TickRec> ticks;
		std::vector<BarRec> bars;
		std::vector<HftRec> details;
		std::vector<HftRec> queues;
		std::vector<HftRec> trans;
		std::map<uint32_t, std::vector<double>> dayPrices;

		std::vector<uint32_t> scheduleDates;
		std::vector<uint32_t> scheduleTimes;
		std::vector<uint32_t> beginDates;
		std::vector<uint32_t> endDates;

		uint32_t initCalls { 0 };
		uint32_t beginCalls { 0 };
		uint32_t endCalls { 0 };
		uint32_t doneCalls { 0 };
		uint32_t scheduleCalls { 0 };

		void handle_init() override { initCalls++; }

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			TickRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			rec.pxType = pxType;
			rec.date = (curTick == NULL) ? 0u : curTick->actiondate();
			rec.time = (curTick == NULL) ? 0u : curTick->actiontime() / 100000u;
			rec.price = (curTick == NULL) ? 0.0 : curTick->price();
			ticks.push_back(rec);

			if (collectDayPrice && rp != NULL)
			{
				std::vector<double> prices;
				for (int flag = 0; flag < 5; flag++)
					prices.push_back(rp->get_day_price(stdCode, flag));
				dayPrices[rec.time] = prices;
			}
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
			rec.vol = (newBar == NULL) ? 0.0 : newBar->vol;
			rec.money = (newBar == NULL) ? 0.0 : newBar->money;
			rec.hold = (newBar == NULL) ? 0.0 : newBar->hold;
			rec.add = (newBar == NULL) ? 0.0 : newBar->add;
			bars.push_back(rec);
		}

		void handle_order_detail(const char* stdCode, WTSOrdDtlData* curOrdDtl) override
		{
			HftRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			if (curOrdDtl != NULL)
			{
				rec.date = curOrdDtl->actiondate();
				rec.time = curOrdDtl->actiontime() / 100000u;
				const WTSOrdDtlStruct& item = curOrdDtl->getOrdDtlStruct();
				rec.price = item.price;
				rec.volume = item.volume;
			}
			details.push_back(rec);
		}

		void handle_order_queue(const char* stdCode, WTSOrdQueData* curOrdQue) override
		{
			HftRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			if (curOrdQue != NULL)
			{
				rec.date = curOrdQue->actiondate();
				rec.time = curOrdQue->actiontime() / 100000u;
				const WTSOrdQueStruct& item = curOrdQue->getOrdQueStruct();
				rec.price = item.price;
				rec.volume = item.qsize;
			}
			queues.push_back(rec);
		}

		void handle_transaction(const char* stdCode, WTSTransData* curTrans) override
		{
			HftRec rec;
			rec.code = (stdCode == NULL) ? std::string() : std::string(stdCode);
			if (curTrans != NULL)
			{
				rec.date = curTrans->actiondate();
				rec.time = curTrans->actiontime() / 100000u;
				const WTSTransStruct& item = curTrans->getTransStruct();
				rec.price = item.price;
				rec.volume = item.volume;
			}
			trans.push_back(rec);
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
		uint32_t adjustFlag { 0 };
		uint32_t cacheDays { 0 };
		std::string mode { "bin" };
		std::string fees;
		std::string adjfactor;
		std::vector<std::string> commodities;
		std::vector<std::pair<std::string, std::string>> rules;
	};

	inline WTSVariant* make_cfg(const std::filesystem::path& repo, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, const CfgOpts& opts)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", opts.mode.c_str());
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", opts.tick);
		cfg->append("adjust_flag", opts.adjustFlag);
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", opts.noSim);
		cfg->append("cache_clear_days", opts.cacheDays);
		cfg->append("fees", opts.fees.c_str());
		if (!opts.adjfactor.empty())
			cfg->append("adjfactor", opts.adjfactor.c_str());

		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (repo / "dist/common/sessions.json").string().c_str());
		basefiles->append("holiday", (repo / "dist/common/holidays.json").string().c_str());

		std::vector<std::string> commodities = opts.commodities;
		if (commodities.empty())
			commodities.push_back((repo / "dist/common/stk_comms.json").string());

		if (commodities.size() == 1)
		{
			basefiles->append("commodity", commodities[0].c_str());
		}
		else
		{
			WTSVariant* ay = WTSVariant::createArray();
			for (const std::string& file : commodities)
				ay->append(file.c_str());
			basefiles->append("commodity", ay);
			ay->release();
		}

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
			prepare_inst_dir();
			WtHelper::setOutputDir(_out.string().c_str());
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			WtHelper::setInstDir("");
			std::error_code ec;
			std::filesystem::remove_all(_base, ec);
			std::filesystem::remove_all(_out, ec);
		}
	};

}	// namespace r91
