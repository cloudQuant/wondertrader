/**
 * p07_tick_loader_test.cpp
 * 覆盖 HisDataReplayer 的「逐tick回放 + 扩展装载器（IBtDataLoader）」两条主链，
 * p04 只走到这些函数的空缓存入口：
 *   run_by_ticks(L741) + replayHftDatasByDay(L1769) + getNextTickTime(L1536)
 *   checkTicks(L2828) 的 loader 分支 + cacheRawTicksFromLoader(L3443)
 *   get_last_tick(L2881) / get_day_price(L3034) / get_cur_price(L3021) 的有数据路径
 *   cacheFinalBarsFromLoader(L3589)：loader 装载分支 + isAutoTrans 的 dsb 转储/回读分支
 *   get_kline_slice(L2229-L2243) 的「先走 extloader」分支
 *   init 里的 loadStkAdjFactorsFromLoader(L302) 回调 + loadFees/calc_fee(L2983) 命中费率表
 * 说明：本 lane 只链 WtBtCore/WTSTools/WTSUtils，IBtDataLoader 是 WtBtCore 自己的接口，
 * 这里用桩实现，不引用 WtCore/WtUftCore/WtDtCore 的任何类。
 * 落盘全部在 temp_directory_path()/p07_* 下，TearDown 清理。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <Share/DLLHelper.hpp>
#include <WtDataStorage/DataDefine.h>

namespace
{
	std::filesystem::path lane_root()
	{
	#ifdef WT_COV_P07_FIXTURE_ROOT
		return std::filesystem::path(WT_COV_P07_FIXTURE_ROOT);
	#else
		return std::filesystem::path(__FILE__).parent_path()
			.parent_path().parent_path().parent_path().parent_path();
	#endif
	}

	std::filesystem::path make_temp_dir(const char* tag)
	{
		auto path = std::filesystem::temp_directory_path() /
			("p07_ld_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
		std::filesystem::create_directories(path, ec);
		return path;
	}

	struct TickRow
	{
		uint32_t date;
		uint32_t hhmm;
		double open;
		double high;
		double low;
		double price;
		double volume;
	};

	/*
	 * 20200102 五笔、20200103 两笔；时间戳都是交易时间内的 HHMMSS*1000。
	 * action_time/100000 必须落在 SD0930 的交易时段里，否则 getNextTickTime 会跳过。
	 */
	const TickRow kTicks[] = {
		{20200102u, 931u, 10.0, 10.5,  9.8, 10.2,  100},
		{20200102u, 932u, 10.2, 11.0, 10.1, 10.8,  200},
		{20200102u, 933u, 10.8, 12.0, 10.6, 11.5,  300},
		{20200102u, 1458u, 11.5, 11.8, 11.2, 11.6, 400},
		{20200102u, 1459u, 11.6, 12.5, 11.4, 12.0, 500},
		{20200103u, 931u, 12.0, 12.4, 11.9, 12.2, 600},
		{20200103u, 932u, 12.2, 13.0, 12.1, 12.8, 700},
	};
	const uint32_t kDay1Ticks = 5;
	const uint32_t kDay2Ticks = 2;

	struct LoaderBar
	{
		uint32_t date;
		uint32_t hhmm;
		double open;
		double high;
		double low;
		double close;
		double vol;
	};

	// 与 p07_minute_replay_test 相同的 9 根 1 分钟线
	const LoaderBar kM1[] = {
		{20200102u, 931u,   9.9, 10.1,  9.7,  10.0,  10},
		{20200102u, 932u,  10.0, 10.5,  9.8,  10.2,  20},
		{20200102u, 933u,  10.2, 11.0, 10.1,  10.8,  30},
		{20200102u, 1458u, 10.8, 12.0, 10.6,  11.5,  40},
		{20200102u, 1459u, 11.5, 11.8, 11.2,  11.6,  50},
		{20200102u, 1500u, 11.6, 12.5, 11.4,  12.0,  60},
		{20200103u, 931u,  12.0, 12.4, 11.9,  12.2,  70},
		{20200103u, 932u,  12.2, 13.0, 12.1,  12.8,  80},
		{20200103u, 1500u, 12.8, 13.5, 12.6,  13.0,  90},
	};
	const uint32_t kM1Cnt = sizeof(kM1) / sizeof(kM1[0]);

	/**
	 * 桩装载器：tick 与 1 分钟线都由内存给出，不依赖任何存储引擎/动态库。
	 */
	class StubBtLoader : public IBtDataLoader
	{
	public:
		bool		_autoTrans {true};
		uint32_t	_finalBarCalls {0};
		uint32_t	_rawBarCalls {0};
		uint32_t	_tickCalls {0};
		uint32_t	_adjAllCalls {0};
		uint32_t	_adjOneCalls {0};
		bool		_giveTicks {true};
		bool		_failRepeatedTickLoads {false};
		bool		_failAfterPartialTickCallback {false};
		bool		_invalidNonemptyTickCallback {false};
		bool		_emptyNullTickCallback {false};
		bool		_multiTickCallbacks {false};
		bool		_concurrentTickCallbacks {false};
		bool		_invalidAfterTickChunk {false};
		bool		_lastTickLoadReturned {false};
		bool		_lastTickCallbackHadObj {false};
		std::vector<uint32_t> _loadedTickDates;

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_finalBarCalls++;
			std::vector<WTSBarStruct> bars;
			if (period == KP_Minute1)
			{
				for (uint32_t i = 0; i < kM1Cnt; i++)
				{
					WTSBarStruct bs;
					memset(&bs, 0, sizeof(bs));
					bs.date = kM1[i].date;
					bs.time = (uint64_t)((kM1[i].date - 19900000) * 10000) + kM1[i].hhmm;
					bs.open = kM1[i].open;
					bs.high = kM1[i].high;
					bs.low = kM1[i].low;
					bs.close = kM1[i].close;
					bs.vol = kM1[i].vol;
					bs.money = kM1[i].vol * 10.0;
					bars.emplace_back(bs);
				}
			}
			else if (period == KP_DAY)
			{
				const uint32_t dates[] = {20200102u, 20200103u};
				for (uint32_t i = 0; i < 2; i++)
				{
					WTSBarStruct bs;
					memset(&bs, 0, sizeof(bs));
					bs.date = dates[i];
					bs.time = dates[i];
					bs.open = 10.0 + i;
					bs.high = 11.0 + i;
					bs.low = 9.5 + i;
					bs.close = 10.5 + i;
					bs.vol = 100 * (i + 1);
					bars.emplace_back(bs);
				}
			}
			else
			{
				return false;
			}

			if (stdCode == NULL || bars.empty())
				return false;

			cb(obj, bars.data(), (uint32_t)bars.size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			_rawBarCalls++;
			(void)obj; (void)stdCode; (void)period; (void)cb;
			return false;
		}

		bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override
		{
			_adjAllCalls++;
			uint32_t dates[] = {20200106u, 20200203u};
			double factors[] = {2.0, 1.5};
			cb(obj, "SSE.STK.600000", dates, factors, 2);
			return true;
		}

		bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
		{
			_adjOneCalls++;
			uint32_t dates[] = {20200106u};
			double factors[] = {2.0};
			cb(obj, stdCode, dates, factors, 1);
			return true;
		}

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			(void)stdCode;
			_tickCalls++;
			_loadedTickDates.push_back(uDate);
			_lastTickLoadReturned = false;
			_lastTickCallbackHadObj = obj != nullptr;
			if (_failRepeatedTickLoads && _tickCalls > 1)
			{
				_lastTickLoadReturned = true;
				return false;
			}
			if (_invalidNonemptyTickCallback)
			{
				cb(obj, nullptr, 1u);
				_lastTickLoadReturned = true;
				return true;
			}
			if (_emptyNullTickCallback)
			{
				cb(obj, nullptr, 0u);
				_lastTickLoadReturned = true;
				return true;
			}
			if (!_giveTicks)
			{
				cb(obj, nullptr, 0u);
				_lastTickLoadReturned = true;
				return true;
			}

			std::vector<WTSTickStruct> ticks;
			for (uint32_t i = 0; i < sizeof(kTicks) / sizeof(kTicks[0]); i++)
			{
				if (kTicks[i].date != uDate)
					continue;
				WTSTickStruct ts;
				memset(&ts, 0, sizeof(ts));
				snprintf(ts.code, sizeof(ts.code), "%s", "600000");
				ts.action_date = kTicks[i].date;
				ts.action_time = kTicks[i].hhmm * 100000;
				ts.open = kTicks[i].open;
				ts.high = kTicks[i].high;
				ts.low = kTicks[i].low;
				ts.price = kTicks[i].price;
				ts.volume = kTicks[i].volume;
				ts.total_volume = kTicks[i].volume;
				ticks.emplace_back(ts);
			}

			if (ticks.empty())
			{
				cb(obj, nullptr, 0u);
				_lastTickLoadReturned = true;
				return true;
			}

			if (_failAfterPartialTickCallback)
			{
				const std::size_t partialCount = std::min<std::size_t>(2, ticks.size());
				cb(obj, ticks.data(), static_cast<uint32_t>(partialCount));
				_lastTickLoadReturned = true;
				return false;
			}

			if (_multiTickCallbacks)
			{
				const std::size_t firstCount = std::min<std::size_t>(2, ticks.size());
				cb(obj, ticks.data(), static_cast<uint32_t>(firstCount));
				cb(obj, nullptr, 0u); // Empty chunks must not discard prior data.

				if (_invalidAfterTickChunk)
				{
					cb(obj, nullptr, 1u);
					_lastTickLoadReturned = true;
					return true;
				}

				const std::size_t remaining = ticks.size() - firstCount;
				if (_concurrentTickCallbacks && remaining > 1)
				{
					const std::size_t secondCount = remaining / 2;
					std::thread second([&]() {
						cb(obj, ticks.data() + firstCount, static_cast<uint32_t>(secondCount));
					});
					std::thread third([&]() {
						cb(obj, ticks.data() + firstCount + secondCount,
							static_cast<uint32_t>(remaining - secondCount));
					});
					second.join();
					third.join();
				}
				else if (remaining > 0)
				{
					cb(obj, ticks.data() + firstCount, static_cast<uint32_t>(remaining));
				}

				_lastTickLoadReturned = true;
				return true;
			}

			cb(obj, ticks.data(), (uint32_t)ticks.size());
			_lastTickLoadReturned = true;
			return true;
		}

		bool isAutoTrans() override { return _autoTrans; }
	};

	// Every chunk is an independent temporary buffer: it is overwritten and
	// destroyed immediately after its callback returns. Concurrent mode joins
	// both callback threads before the loader returns, as the API requires.
	class ExpiringChunkLoader : public StubBtLoader
	{
	public:
		bool concurrent {false};
		uint32_t expiredChunks {0};

		bool loadRawHisTicks(void* obj, const char*, uint32_t uDate, FuncReadTicks cb) override
		{
			++_tickCalls;
			_loadedTickDates.push_back(uDate);
			_lastTickLoadReturned = false;
			_lastTickCallbackHadObj = obj != nullptr;
			std::vector<WTSTickStruct> ticks;
			for (const TickRow& row : kTicks)
			{
				if (row.date != uDate)
					continue;
				WTSTickStruct tick{};
				std::snprintf(tick.code, sizeof(tick.code), "%s", "600000");
				tick.action_date = row.date;
				tick.action_time = row.hhmm * 100000;
				tick.open = row.open;
				tick.high = row.high;
				tick.low = row.low;
				tick.price = row.price;
				tick.volume = row.volume;
				tick.total_volume = row.volume;
				ticks.push_back(tick);
			}
			if (ticks.size() != 5)
			{
				cb(obj, nullptr, 0);
				_lastTickLoadReturned = true;
				return true;
			}
			std::atomic<uint32_t> expired{0};
			auto deliver = [&](std::size_t begin, std::size_t end) {
				std::vector<WTSTickStruct> chunk(ticks.begin() + begin, ticks.begin() + end);
				cb(obj, chunk.data(), static_cast<uint32_t>(chunk.size()));
				volatile unsigned char* bytes = reinterpret_cast<volatile unsigned char*>(chunk.data());
				for (std::size_t i = 0; i < chunk.size() * sizeof(WTSTickStruct); ++i)
					bytes[i] = 0xa5; // Do not let an optimizer elide the overwrite before destruction.
				++expired;
			};
			deliver(0, 2);
			cb(obj, nullptr, 0);
			if (concurrent)
			{
				std::thread a([&]() { deliver(2, 3); });
				std::thread b([&]() { deliver(3, 5); });
				a.join();
				b.join();
			}
			else
			{
				deliver(2, 3);
				deliver(3, 5);
			}
			expiredChunks = expired.load();
			_lastTickLoadReturned = true;
			return true;
		}
	};

	WTSVariant* loader_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, bool tickMode, const std::string& fees)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tickMode);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", fees.c_str());
		WTSVariant* basefiles = WTSVariant::createObject();
		basefiles->append("session", (root / "dist/common/sessions.json").string().c_str());
		basefiles->append("commodity", (root / "dist/common/stk_comms.json").string().c_str());
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	bool write_fee_template(const std::filesystem::path& base, std::string& outPath)
	{
		outPath = (base / "fees_p07.json").string();
		std::ofstream out(outPath);
		if (!out.is_open())
			return false;
		out << "{\n"
			<< "\t\"SSE.STK\":{\"byvolume\":false,\"open\":0.0003,\"close\":0.0013,\"closetoday\":0.0013,\"minfee\":5},\n"
			<< "\t\"SZSE.STK\":{\"byvolume\":false,\"open\":0.00025,\"close\":0.00025,\"closetoday\":0.00025,\"minfee\":0},\n"
			<< "\t\"SSE.IDX\":{\"byvolume\":true,\"open\":2,\"close\":3,\"closetoday\":1,\"minfee\":0}\n"
			<< "}\n";
		out.close();
		return true;
	}

	template <typename T>
	bool write_raw_block(const std::filesystem::path& file, uint16_t type, const std::vector<T>& items)
	{
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		if (ec)
			return false;

		std::string content(sizeof(BlockHeader), '\0');
		BlockHeader* header = reinterpret_cast<BlockHeader*>(content.data());
		std::memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
		header->_type = type;
		header->_version = BLOCK_VERSION_RAW_V2;
		if (!items.empty())
			content.append(reinterpret_cast<const char*>(items.data()), items.size() * sizeof(T));

		std::ofstream out(file, std::ios::binary);
		if (!out.is_open())
			return false;
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		out.close();
		return out.good();
	}

	uint32_t p07_action_time(uint32_t hhmm, uint32_t second = 0, uint32_t millisecond = 0)
	{
		return hhmm * 100000u + second * 1000u + millisecond;
	}

	bool set_data_storage_inst_dir(const std::filesystem::path& root)
	{
		const std::string module = DLLHelper::wrap_module("WtDataStorage");
	#ifdef WT_COV_P07_DATA_STORAGE_MODULE
		(void)root;
		const std::filesystem::path configuredModule(WT_COV_P07_DATA_STORAGE_MODULE);
		std::error_code ec;
		if (configuredModule.filename() != module || !std::filesystem::exists(configuredModule, ec) || ec)
			return false;
		const std::string configuredDir =
			configuredModule.parent_path().string() + std::filesystem::path::preferred_separator;
		WtHelper::setInstDir(configuredDir.c_str());
		return true;
	#else
		const char* const relativeDirs[] = {
			"build-cov/build_x64/Debug/bin",
			"build-cov/build_x64/Release/bin",
			"build_x64/Debug/bin",
			"build_x64/Release/bin",
		};
		for (const char* relativeDir : relativeDirs)
		{
			const std::filesystem::path dir = root / relativeDir;
			std::error_code ec;
			if (std::filesystem::exists(dir / module, ec) && !ec)
			{
				WtHelper::setInstDir((dir.string() + "/").c_str());
				return true;
			}
		}
		return false;
	#endif
	}

	class InstDirRestore
	{
	public:
		InstDirRestore() : _previous(WtHelper::getInstDir()) {}
		~InstDirRestore() { WtHelper::setInstDir(_previous.c_str()); }

	private:
		std::string _previous;
	};

	WTSVariant* bin_tick_cfg(const std::filesystem::path& root, const std::filesystem::path& base,
		uint64_t stime = 202001020930ull, uint64_t etime = 202001021500ull,
		const char* commodityFile = "stk_comms.json",
		const std::filesystem::path& sessionPath = std::filesystem::path(),
		const std::filesystem::path& commodityPath = std::filesystem::path(),
		const std::filesystem::path& additionalCommodityPath = std::filesystem::path())
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "bin");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", true);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", static_cast<uint32_t>(10));
		cfg->append("fees", "");
		WTSVariant* basefiles = WTSVariant::createObject();
		const std::filesystem::path sessions = sessionPath.empty()
			? root / "dist/common/sessions.json" : sessionPath;
		const std::filesystem::path commodities = commodityPath.empty()
			? root / "dist/common" / commodityFile : commodityPath;
		basefiles->append("session", sessions.string().c_str());
		if (additionalCommodityPath.empty())
			basefiles->append("commodity", commodities.string().c_str());
		else
		{
			WTSVariant* commodityFiles = WTSVariant::createArray();
			commodityFiles->append(commodities.string().c_str());
			commodityFiles->append(additionalCommodityPath.string().c_str());
			basefiles->append("commodity", commodityFiles);
			commodityFiles->release();
		}
		basefiles->append("holiday", (root / "dist/common/holidays.json").string().c_str());
		cfg->append("basefiles", basefiles);
		basefiles->release();
		return cfg;
	}

	class TickSink : public IDataSink
	{
	public:
		struct DispatchEvent
		{
			char kind;
			std::string code;
			uint32_t date;
			uint32_t time;
			double price;
		};

		std::function<void()> onInit;
		std::function<void(uint32_t /*tickSeq*/)> onTickProbe;
		std::function<void(WTSOrdDtlData*)> onDetailProbe;
		std::function<void(WTSOrdQueData*)> onQueueProbe;
		std::function<void(WTSTransData*)> onTransactionProbe;
		HisDataReplayer* rp {nullptr};
		std::string probeCode;
		bool peekLastTick {false};

		uint32_t initCalls {0};
		uint32_t beginCalls {0};
		uint32_t endCalls {0};
		std::vector<uint32_t> sessionBeginDates;
		std::vector<uint32_t> sessionEndDates;
		uint32_t scheduleCalls {0};
		uint32_t replayDoneCalls {0};

		std::vector<double> tickPrices;
		std::vector<uint32_t> tickPxTypes;
		std::vector<double> tickOpens;
		std::vector<double> tickHighs;
		std::vector<double> tickLows;
		std::vector<uint32_t> lastTickDates;
		std::vector<uint32_t> lastTickActionTimes;
		std::vector<double> lastTickOpens;
		std::vector<double> lastTickHighs;
		std::vector<double> lastTickLows;
		std::vector<double> lastTickPrices;
		std::vector<double> dayOpen;
		std::vector<double> dayHigh;
		std::vector<double> dayLow;
		std::vector<double> dayLast;
		std::vector<double> curPrices;
		std::vector<uint32_t> tickDates;
		std::vector<uint32_t> tickTimes;
		std::vector<uint32_t> tickActionTimes;
		std::vector<std::string> tickCodes;
		std::vector<bool> tickSimulationStates;
		std::vector<double> detailPrices;
		std::vector<uint32_t> detailTimes;
		std::vector<double> queuePrices;
		std::vector<uint32_t> queueTimes;
		std::vector<double> transactionPrices;
		std::vector<uint32_t> transactionTimes;
		std::vector<char> hftEventOrder;
		std::vector<std::string> hftEventCodes;
		std::vector<bool> level2SimulationStates;
		std::vector<double> closedPrices;
		std::vector<uint32_t> closedDates;
		std::vector<uint32_t> closedTimes;
		std::vector<std::string> closedCodes;
		std::vector<DispatchEvent> dispatchEvents;

		void handle_init() override
		{
			initCalls++;
			if (onInit)
				onInit();
		}

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			if (curTick == nullptr)
				return;
			const WTSTickStruct& ts = curTick->getTickStruct();
			hftEventOrder.push_back('K');
			hftEventCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
			tickCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
			dispatchEvents.push_back({ 'K', stdCode == nullptr ? "" : stdCode,
				ts.action_date, ts.action_time / 100000u, ts.price });
			tickPrices.push_back(ts.price);
			tickPxTypes.push_back(pxType);
			tickOpens.push_back(ts.open);
			tickHighs.push_back(ts.high);
			tickLows.push_back(ts.low);
			tickDates.push_back(ts.action_date);
			tickTimes.push_back(ts.action_time / 100000);
			tickActionTimes.push_back(ts.action_time);
			if (rp != nullptr)
				tickSimulationStates.push_back(rp->is_tick_simulated());
			if (rp != nullptr && !probeCode.empty())
			{
				curPrices.push_back(rp->get_cur_price(probeCode.c_str()));
				// tick 模式下 flag 0/1/2/3 分别读取 open/high/low/price。
				if (peekLastTick)
				{
					dayOpen.push_back(rp->get_day_price(probeCode.c_str(), 0));
					dayHigh.push_back(rp->get_day_price(probeCode.c_str(), 1));
					dayLow.push_back(rp->get_day_price(probeCode.c_str(), 2));
					dayLast.push_back(rp->get_day_price(probeCode.c_str(), 3));
					WTSTickData* last = rp->get_last_tick(probeCode.c_str());
					if (last == nullptr)
					{
						lastTickDates.push_back(0);
						lastTickActionTimes.push_back(0);
						lastTickOpens.push_back(0.0);
						lastTickHighs.push_back(0.0);
						lastTickLows.push_back(0.0);
						lastTickPrices.push_back(0.0);
					}
					else
					{
						const WTSTickStruct& lastTick = last->getTickStruct();
						lastTickDates.push_back(lastTick.action_date);
						lastTickActionTimes.push_back(lastTick.action_time);
						lastTickOpens.push_back(lastTick.open);
						lastTickHighs.push_back(lastTick.high);
						lastTickLows.push_back(lastTick.low);
						lastTickPrices.push_back(lastTick.price);
						last->release();
					}
				}
			}
			if (onTickProbe)
				onTickProbe((uint32_t)tickPrices.size());
		}

		void handle_bar_close(const char* stdCode, const char*, uint32_t, WTSBarStruct* newBar) override
		{
			if (newBar != nullptr)
			{
				closedCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
				closedDates.push_back(newBar->date);
				closedTimes.push_back(static_cast<uint32_t>(newBar->time % 10000));
				closedPrices.push_back(newBar->close);
				dispatchEvents.push_back({ 'C', stdCode == nullptr ? "" : stdCode,
					newBar->date, static_cast<uint32_t>(newBar->time % 10000), newBar->close });
			}
		}

		void handle_order_detail(const char* stdCode, WTSOrdDtlData* data) override
		{
			if (data != nullptr)
			{
				hftEventOrder.push_back('D');
				hftEventCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
				if (rp != nullptr)
					level2SimulationStates.push_back(rp->is_tick_simulated());
				detailTimes.push_back(data->actiontime() / 100000u);
				detailPrices.push_back(data->getOrdDtlStruct().price);
				if (onDetailProbe)
					onDetailProbe(data);
			}
		}

		void handle_order_queue(const char* stdCode, WTSOrdQueData* data) override
		{
			if (data != nullptr)
			{
				hftEventOrder.push_back('Q');
				hftEventCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
				if (rp != nullptr)
					level2SimulationStates.push_back(rp->is_tick_simulated());
				queueTimes.push_back(data->actiontime() / 100000u);
				queuePrices.push_back(data->getOrdQueStruct().price);
				if (onQueueProbe)
					onQueueProbe(data);
			}
		}

		void handle_transaction(const char* stdCode, WTSTransData* data) override
		{
			if (data != nullptr)
			{
				hftEventOrder.push_back('T');
				hftEventCodes.emplace_back(stdCode == nullptr ? "" : stdCode);
				if (rp != nullptr)
					level2SimulationStates.push_back(rp->is_tick_simulated());
				transactionTimes.push_back(data->actiontime() / 100000u);
				transactionPrices.push_back(data->getTransStruct().price);
				if (onTransactionProbe)
					onTransactionProbe(data);
			}
		}

		void handle_schedule(uint32_t, uint32_t) override { scheduleCalls++; }
		void handle_session_begin(uint32_t tradingDate) override
		{
			beginCalls++;
			sessionBeginDates.push_back(tradingDate);
		}
		void handle_session_end(uint32_t tradingDate) override
		{
			endCalls++;
			sessionEndDates.push_back(tradingDate);
		}
		void handle_replay_done() override { replayDoneCalls++; }
	};
}

// Test-only access to replay date buckets without changing the public C++ or C ABI.
struct HisDataReplayerTestAccess
{
	static void on_minute_end(HisDataReplayer& replayer, uint32_t date,
		uint32_t time, uint32_t endTDate)
	{
		replayer.onMinuteEnd(date, time, endTDate);
	}

	static bool replay_hft_data(HisDataReplayer& replayer, uint64_t stime, uint64_t etime)
	{
		return replayer.replayHftDatas(stime, etime);
	}

	static std::size_t replay_tick_bucket_count(const HisDataReplayer& replayer)
	{
		std::size_t count = 0;
		for (const auto& codeEntry : replayer._replay_ticks_cache)
			count += codeEntry.second.size();
		return count;
	}

	static bool has_completed_hft_ranges(const HisDataReplayer& replayer)
	{
		return replayer._has_replay_completed_natural_date;
	}

	static uint32_t completed_hft_natural_date(const HisDataReplayer& replayer)
	{
		return replayer._replay_completed_natural_date;
	}

	static const std::vector<std::pair<uint64_t, uint64_t>>& completed_hft_ranges(const HisDataReplayer& replayer)
	{
		return replayer._replay_completed_natural_ranges;
	}

	static bool replay_terminated(const HisDataReplayer& replayer)
	{
		return replayer._terminated;
	}

	static void reset_replayer(HisDataReplayer& replayer)
	{
		replayer.reset();
	}
};

class P07TickLoaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_root = lane_root();
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/sessions.json"));
		ASSERT_TRUE(std::filesystem::exists(_root / "dist/common/stk_comms.json"));
		_base = make_temp_dir("ld");
		_out = make_temp_dir("out");
		WtHelper::setOutputDir(_out.string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_base, ec);
		std::filesystem::remove_all(_out, ec);
	}

	std::filesystem::path _root;
	std::filesystem::path _base;
	std::filesystem::path _out;
};

/*
 * 逐tick回放主循环：只订阅tick（不订阅K线）+ tick模式 -> run() 派发到 run_by_ticks。
 * replayHftDatasByDay 每个交易日把该日的tick按时间序回放；
 * 两日 fixture 的 5+2 笔都在 SD0930 交易时段和回测窗口内，应逐笔回放一次。
 */
TEST_F(P07TickLoaderTest, tickOnlyReplayDrivesHftLoopPerDay)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	std::string fees;
	ASSERT_TRUE(write_fee_template(_base, fees));
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, true, fees);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	sink.onInit = [&replayer]() {
		// prepare() 会先 reset 并清空订阅；与策略初始化一致，在 handle_init 中订阅。
		replayer.sub_tick(11, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_ticks");

	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	EXPECT_TRUE(replayer.is_tick_enabled());
	// init 里带 loader 时会通过扩展装载器拉一次全量复权因子
	EXPECT_EQ(1u, loader._adjAllCalls);

	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200103u}), loader._loadedTickDates);
	ASSERT_EQ(kDay1Ticks + kDay2Ticks, sink.tickPrices.size());
	EXPECT_EQ(std::vector<double>({10.2, 10.8, 11.5, 11.6, 12.0, 12.2, 12.8}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u, 20200102u, 20200102u,
		20200103u, 20200103u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u, 1458u, 1459u, 931u, 932u}), sink.tickTimes);

	// 每个有tick的交易日各一对 session_begin/end，收尾再补 1 次 replay_done
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
	// 没有订阅K线 -> 不应有闭盘
	EXPECT_EQ(0u, sink.closedPrices.size());

	// update_price 由回放中的每笔tick驱动，最后一笔是 20200103 的第 2 笔
	EXPECT_DOUBLE_EQ(12.8, replayer.get_cur_price("SSE.STK.600000"));
	// 20200104 已超出 end_tdate，装载器没有该日数据 -> 缓存被清空 -> 取不到last tick
	EXPECT_EQ(nullptr, replayer.get_last_tick("SSE.STK.600000"));
}

TEST_F(P07TickLoaderTest, externalTickLoaderAccumulatesSynchronousConcurrentChunks)
{
	StubBtLoader loader;
	loader._multiTickCallbacks = true;
	loader._concurrentTickCallbacks = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(11u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_loader_multi_callback_chunks");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u}), loader._loadedTickDates);
	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u, 1458u, 1459u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({10.2, 10.8, 11.5, 11.6, 12.0}), sink.tickPrices);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, externalTickLoaderCopiesEachExpiringCallbackChunk)
{
	for (const bool concurrent : {false, true})
	{
		SCOPED_TRACE(concurrent ? "concurrent callbacks" : "sequential callbacks");
		ExpiringChunkLoader loader;
		loader.concurrent = concurrent;
		loader._autoTrans = false;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();
		bool replayAfterLoad = true;
		TickSink sink;
		sink.onInit = [&replayer]() { replayer.sub_tick(11u, "SSE.STK.600000"); };
		sink.onTickProbe = [&](uint32_t) { replayAfterLoad = replayAfterLoad && loader._lastTickLoadReturned; };
		replayer.register_sink(&sink, "p07_loader_expiring_chunks");
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);

		EXPECT_EQ(1u, loader._tickCalls);
		EXPECT_EQ(3u, loader.expiredChunks);
		EXPECT_TRUE(loader._lastTickLoadReturned);
		EXPECT_TRUE(replayAfterLoad);
		EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u, 1458u, 1459u}), sink.tickTimes);
		EXPECT_EQ(std::vector<double>({10.2, 10.8, 11.5, 11.6, 12.0}), sink.tickPrices);
		EXPECT_EQ(1u, sink.replayDoneCalls);
	}
}

TEST_F(P07TickLoaderTest, externalTickLoaderChunksAreAvailableInHistoricalSlice)
{
	StubBtLoader loader;
	loader._multiTickCallbacks = true;
	loader._concurrentTickCallbacks = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	bool sliceHasFirstTick = false;
	TickSink sink;
	sink.onInit = [&]() {
		replayer.sub_tick(11u, "SSE.STK.600000");
		WTSTickSlice* slice = replayer.get_tick_slice("SSE.STK.600000", 10u, 202001020932ull);
		sliceHasFirstTick = slice != nullptr && slice->size() == 1 &&
			slice->at(0)->action_time == p07_action_time(931u) && slice->at(0)->price == 10.2;
		if (slice != nullptr)
			slice->release();
	};
	replayer.register_sink(&sink, "p07_loader_multi_callback_slice");
	ASSERT_TRUE(replayer.prepare());

	EXPECT_TRUE(sliceHasFirstTick);
	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), loader._loadedTickDates);
	EXPECT_TRUE(sink.tickTimes.empty());
}

// One-item lists use cursor==count for their final item; tick and all three Level2
// streams must dispatch that record once before advancing to count+1.
TEST_F(P07TickLoaderTest, singleItemHftStreamsReplayFinalRecord)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = 20200102u;
	tick.trading_date = 20200102u;
	tick.action_time = 931u * 100000u;
	tick.open = 10.0;
	tick.high = 11.0;
	tick.low = 9.0;
	tick.price = 10.5;
	tick.volume = 100.0;
	tick.total_volume = 100.0;

	WTSOrdDtlStruct detail;
	std::memset(&detail, 0, sizeof(detail));
	std::strcpy(detail.exchg, "SSE");
	std::strcpy(detail.code, "600000");
	detail.action_date = 20200102u;
	detail.trading_date = 20200102u;
	detail.action_time = 931u * 100000u;
	detail.price = 10.25;
	detail.volume = 2;

	WTSOrdQueStruct queue;
	std::memset(&queue, 0, sizeof(queue));
	std::strcpy(queue.exchg, "SSE");
	std::strcpy(queue.code, "600000");
	queue.action_date = 20200102u;
	queue.trading_date = 20200102u;
	queue.action_time = 931u * 100000u;
	queue.price = 10.0;
	queue.qsize = 3;
	queue.volumes[0] = 3;

	WTSTransStruct transaction;
	std::memset(&transaction, 0, sizeof(transaction));
	std::strcpy(transaction.exchg, "SSE");
	std::strcpy(transaction.code, "600000");
	transaction.action_date = 20200102u;
	transaction.trading_date = 20200102u;
	transaction.action_time = 931u * 100000u;
	transaction.price = 10.4;
	transaction.volume = 4;

	const std::filesystem::path dayDir = _base / "his";
	ASSERT_TRUE(write_raw_block(dayDir / "ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));
	ASSERT_TRUE(write_raw_block(dayDir / "orders/SSE/20200102/600000.dsb", BT_HIS_OrdDetail,
		std::vector<WTSOrdDtlStruct>{detail}));
	ASSERT_TRUE(write_raw_block(dayDir / "queue/SSE/20200102/600000.dsb", BT_HIS_OrdQueue,
		std::vector<WTSOrdQueStruct>{queue}));
	ASSERT_TRUE(write_raw_block(dayDir / "trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{transaction}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	TickSink sink;
	sink.onInit = [&replayer]() {
		replayer.sub_tick(1u, "SSE.STK.600000");
		replayer.sub_order_detail(2u, "SSE.STK.600000");
		replayer.sub_order_queue(3u, "SSE.STK.600000");
		replayer.sub_transaction(4u, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_one_item_hft");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({10.5}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.detailTimes);
	EXPECT_EQ(std::vector<double>({10.25}), sink.detailPrices);
	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.queueTimes);
	EXPECT_EQ(std::vector<double>({10.0}), sink.queuePrices);
	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.transactionTimes);
	EXPECT_EQ(std::vector<double>({10.4}), sink.transactionPrices);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, invalidNonemptyTickLoaderCallbackFailsBeforeSessionBegin)
{
	StubBtLoader loader;
	loader._invalidNonemptyTickCallback = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_invalid_loader_callback");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_TRUE(loader._lastTickLoadReturned);
	EXPECT_TRUE(loader._lastTickCallbackHadObj);
	EXPECT_FALSE(HisDataReplayerTestAccess::has_completed_hft_ranges(replayer));
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, invalidTickLoaderChunkAfterValidChunkFailsClosed)
{
	StubBtLoader loader;
	loader._multiTickCallbacks = true;
	loader._invalidAfterTickChunk = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_loader_invalid_followup_chunk");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, emptyNullTickLoaderCallbackIsAValidEmptyBucket)
{
	StubBtLoader loader;
	loader._emptyNullTickCallback = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_empty_null_loader_callback");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_TRUE(loader._lastTickLoadReturned);
	EXPECT_TRUE(loader._lastTickCallbackHadObj);
	EXPECT_FALSE(HisDataReplayerTestAccess::replay_terminated(replayer));
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, failedTickLoaderResultFailsClosedWithoutDispatchingPartialChunk)
{
	StubBtLoader loader;
	loader._failAfterPartialTickCallback = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_loader_failed_after_partial_chunk");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(1u, loader._tickCalls);
	EXPECT_TRUE(loader._lastTickLoadReturned);
	EXPECT_TRUE(loader._lastTickCallbackHadObj);
	EXPECT_TRUE(HisDataReplayerTestAccess::replay_terminated(replayer));
	EXPECT_FALSE(HisDataReplayerTestAccess::has_completed_hft_ranges(replayer));
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(0u, sink.scheduleCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, emptyReplayBucketsStayBoundedAcrossLongNoDataRange)
{
	StubBtLoader loader;
	loader._giveTicks = false;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202003021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_bounded_empty_replay_cache");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_GT(loader._tickCalls, 30u);
	EXPECT_LE(HisDataReplayerTestAccess::replay_tick_bucket_count(replayer), 10u);
	EXPECT_FALSE(HisDataReplayerTestAccess::replay_terminated(replayer));
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, overnightTickBucketAcrossWeekendUsesTradingCalendarAndResets)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	auto makeTick = [](uint32_t actionDate, uint32_t tradingDate, uint32_t hhmm, double price) {
		WTSTickStruct tick;
		std::memset(&tick, 0, sizeof(tick));
		std::strcpy(tick.exchg, "SHFE");
		std::strcpy(tick.code, "rb2001");
		tick.action_date = actionDate;
		tick.trading_date = tradingDate;
		tick.action_time = p07_action_time(hhmm, 30u, 125u);
		tick.open = tick.high = tick.low = tick.price = price;
		tick.volume = tick.total_volume = 1.0;
		return tick;
	};

	// Friday's SHFE night session belongs to Monday's trading-date file. The
	// next tick belongs to Tuesday and follows naturally after the weekend.
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200106/rb2001.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{makeTick(20200103u, 20200106u, 2200u, 55.1)}));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200107/rb2001.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{
			makeTick(20200106u, 20200107u, 2200u, 55.2),
			makeTick(20200106u, 20200107u, 2202u, 55.3),
		}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001032130ull, 202001062201ull, "commodities.json");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SHFE.rb.2001"); };
	replayer.register_sink(&sink, "p07_weekend_overnight_bucket");

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<std::string>({"SHFE.rb.2001", "SHFE.rb.2001"}), sink.tickCodes);
	EXPECT_EQ(std::vector<uint32_t>({20200103u, 20200106u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({2200u, 2200u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({55.1, 55.2}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({p07_action_time(2200u, 30u, 125u),
		p07_action_time(2200u, 30u, 125u)}), sink.tickActionTimes);

	// prepare() clears replay-owned date buckets and cursors; replaying the same
	// range on the same object must produce the same two records exactly once.
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<std::string>({"SHFE.rb.2001", "SHFE.rb.2001", "SHFE.rb.2001", "SHFE.rb.2001"}), sink.tickCodes);
	EXPECT_EQ(std::vector<uint32_t>({20200103u, 20200106u, 20200103u, 20200106u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({2200u, 2200u, 2200u, 2200u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({55.1, 55.2, 55.1, 55.2}), sink.tickPrices);
}

TEST_F(P07TickLoaderTest, continuous24HourReplayReadsFullSaturdayNaturalDateBucket)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	const std::filesystem::path sessions = _base / "p07_24h_sessions.json";
	const std::filesystem::path commodities = _base / "p07_24h_commodities.json";
	{
		std::ofstream out(sessions);
		ASSERT_TRUE(out.is_open());
		out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H":{"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]}})";
	}
	{
		std::ofstream out(commodities);
		ASSERT_TRUE(out.is_open());
		out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}}})";
	}

	std::vector<WTSTickStruct> ticks;
	auto makeTick = [](uint32_t hhmm, uint32_t second, uint32_t millisecond, double price)
	{
		WTSTickStruct tick{};
		std::strcpy(tick.exchg, "CRY");
		std::strcpy(tick.code, "BTC");
		tick.action_date = tick.trading_date = 20200104u; // Saturday
		tick.action_time = p07_action_time(hhmm, second, millisecond);
		tick.open = tick.high = tick.low = tick.price = price;
		tick.volume = tick.total_volume = 1.0;
		return tick;
	};
	ticks.push_back(makeTick(0u, 0u, 0u, 1.0));
	ticks.push_back(makeTick(1200u, 0u, 0u, 2.0));
	ticks.push_back(makeTick(2359u, 59u, 999u, 3.0));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200104/BTC.dsb", BT_HIS_Ticks, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200104000000ull, 20200105000000ull,
		"stk_comms.json", sessions, commodities);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_NE(nullptr, replayer.get_session_info("CRY.COIN.BTC", true));
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "CRY.COIN.BTC"); };
	replayer.register_sink(&sink, "p07_continuous_saturday");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200104u, 20200104u, 20200104u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({0u, 1200u, 2359u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({p07_action_time(0u), p07_action_time(1200u),
		p07_action_time(2359u, 59u, 999u)}), sink.tickActionTimes);
	EXPECT_EQ(std::vector<double>({1.0, 2.0, 3.0}), sink.tickPrices);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
}

TEST_F(P07TickLoaderTest, continuous24HourPositiveOffsetUsesNaturalDateCut)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	const std::filesystem::path sessions = _base / "p07_24h_offset_sessions.json";
	const std::filesystem::path commodities = _base / "p07_24h_offset_commodities.json";
	{
		std::ofstream out(sessions);
		ASSERT_TRUE(out.is_open());
		out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H_POS":{"name":"continuous-offset","offset":300,"sections":[{"from":0,"to":0}]}})";
	}
	{
		std::ofstream out(commodities);
		ASSERT_TRUE(out.is_open());
		out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous-offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H_POS","volscale":1}}})";
	}
	WTSTickStruct beforeCut{};
	std::strcpy(beforeCut.exchg, "CRY");
	std::strcpy(beforeCut.code, "BTC");
	beforeCut.action_date = 20200104u;
	beforeCut.trading_date = 20200104u;
	beforeCut.action_time = p07_action_time(1859u, 59u, 999u);
	beforeCut.price = 0.9;
	WTSTickStruct atCut = beforeCut;
	atCut.trading_date = 20200105u;
	atCut.action_time = p07_action_time(1900u);
	atCut.price = 1.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200104/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{beforeCut}));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200105/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{atCut}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200104185900ull, 20200104190000ull,
		"stk_comms.json", sessions, commodities);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_NE(nullptr, replayer.get_session_info("CRY.COIN.BTC", true));
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "CRY.COIN.BTC"); };
	replayer.register_sink(&sink, "p07_continuous_offset_natural_cut");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200104u, 20200104u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({1859u, 1900u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({p07_action_time(1859u, 59u, 999u), p07_action_time(1900u)}),
		sink.tickActionTimes);
	EXPECT_EQ(std::vector<double>({0.9, 1.0}), sink.tickPrices);
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, continuous24HourNegativeOffsetUsesNaturalDateCut)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	const std::filesystem::path sessions = _base / "p07_24h_negative_sessions.json";
	const std::filesystem::path commodities = _base / "p07_24h_negative_commodities.json";
	{
		std::ofstream out(sessions);
		ASSERT_TRUE(out.is_open());
		out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H_NEG":{"name":"continuous-negative-offset","offset":-300,"sections":[{"from":0,"to":0}]}})";
	}
	{
		std::ofstream out(commodities);
		ASSERT_TRUE(out.is_open());
		out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous-negative-offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H_NEG","volscale":1}}})";
	}
	WTSTickStruct beforeCut{};
	std::strcpy(beforeCut.exchg, "CRY");
	std::strcpy(beforeCut.code, "BTC");
	beforeCut.action_date = 20200104u;
	beforeCut.trading_date = 20200103u;
	beforeCut.action_time = p07_action_time(459u, 59u, 999u);
	beforeCut.price = 1.9;
	WTSTickStruct atCut = beforeCut;
	atCut.trading_date = 20200104u;
	atCut.action_time = p07_action_time(500u);
	atCut.price = 2.0;
	WTSTickStruct lastIncluded = atCut;
	lastIncluded.action_time = p07_action_time(500u, 0u, 999u);
	lastIncluded.price = 2.1;
	WTSTickStruct afterEnd = atCut;
	afterEnd.action_time = p07_action_time(501u);
	afterEnd.price = 2.2;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200103/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{beforeCut}));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200104/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{atCut, lastIncluded, afterEnd}));
	// At natural 20200104 05:00, offset -300 maps the contract to trading date
	// 20200104, while the default TRADING session's +300 offset maps this
	// Saturday boundary to Monday 20200106. This later bucket must not open an
	// extra session beyond the subscribed contract's end trading date.
	WTSTickStruct laterTradingDate = beforeCut;
	laterTradingDate.action_date = laterTradingDate.trading_date = 20200106u;
	laterTradingDate.action_time = p07_action_time(501u);
	laterTradingDate.price = 3.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200106/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{laterTradingDate}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200104045900ull, 20200104050000ull,
		"stk_comms.json", sessions, commodities);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_NE(nullptr, replayer.get_session_info("CRY.COIN.BTC", true));
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "CRY.COIN.BTC"); };
	replayer.register_sink(&sink, "p07_continuous_negative_offset_end_cut");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200104u, 20200104u, 20200104u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({459u, 500u, 500u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({p07_action_time(459u, 59u, 999u), p07_action_time(500u),
		p07_action_time(500u, 0u, 999u)}),
		sink.tickActionTimes);
	EXPECT_EQ(std::vector<double>({1.9, 2.0, 2.1}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200103u, 20200104u}), sink.sessionBeginDates);
	EXPECT_EQ(std::vector<uint32_t>({20200103u, 20200104u}), sink.sessionEndDates);
	EXPECT_EQ(2u, sink.beginCalls);
	EXPECT_EQ(2u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, missingHftDateIsSkippedBeforeLaterValidDate)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	WTSTickStruct tick{};
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = tick.trading_date = 20200109u;
	tick.action_time = p07_action_time(931u);
	tick.price = 9.1;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200109/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));
	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108093000ull, 20200109093200ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_missing_day_skip");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200109u}), sink.tickDates);
	EXPECT_EQ(std::vector<double>({9.1}), sink.tickPrices);
}

TEST_F(P07TickLoaderTest, corruptHftFileStopsBeforeLaterValidDate)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	const std::filesystem::path corrupt = _base / "his/ticks/SSE/20200108/600000.dsb";
	std::error_code ec;
	std::filesystem::create_directories(corrupt.parent_path(), ec);
	ASSERT_FALSE(ec);
	{
		std::ofstream out(corrupt, std::ios::binary);
		ASSERT_TRUE(out.is_open());
		BlockHeaderV2 header{};
		std::memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
		header._type = BT_HIS_Ticks;
		header._version = BLOCK_VERSION_CMP_V2;
		header._size = 1;
		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
	}
	WTSTickStruct tick{};
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = tick.trading_date = 20200109u;
	tick.action_time = p07_action_time(931u);
	tick.price = 9.1;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200109/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));
	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108093000ull, 20200109093200ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_corrupt_day_fail_closed");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, adjacentReplayWindowsDispatchOrdinaryCloseTickExactlyOnce)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	WTSTickStruct beforeClose{};
	std::strcpy(beforeClose.exchg, "SSE");
	std::strcpy(beforeClose.code, "600000");
	beforeClose.action_date = beforeClose.trading_date = 20200108u;
	beforeClose.action_time = p07_action_time(1459u, 30u);
	beforeClose.price = 14.9;
	WTSTickStruct atClose = beforeClose;
	atClose.action_time = p07_action_time(1500u);
	atClose.price = 15.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200108/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{beforeClose, atClose}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108145900ull, 20200108150100ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_ordinary_close_endpoint");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_TRUE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001081459ull, 202001081500ull));
	EXPECT_EQ(std::vector<uint32_t>({1459u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({14.9}), sink.tickPrices);
	ASSERT_TRUE(HisDataReplayerTestAccess::has_completed_hft_ranges(replayer));
	EXPECT_EQ(20200108u, HisDataReplayerTestAccess::completed_hft_natural_date(replayer));
	ASSERT_EQ(1u, HisDataReplayerTestAccess::completed_hft_ranges(replayer).size());
	EXPECT_EQ(std::make_pair(20200108145900000ull, 20200108150000000ull),
		HisDataReplayerTestAccess::completed_hft_ranges(replayer).front());

	EXPECT_TRUE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001081500ull, 202001081501ull));
	EXPECT_EQ(std::vector<uint32_t>({1459u, 1500u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({14.9, 15.0}), sink.tickPrices);
	ASSERT_EQ(1u, HisDataReplayerTestAccess::completed_hft_ranges(replayer).size());
	EXPECT_EQ(std::make_pair(20200108145900000ull, 20200108150100000ull),
		HisDataReplayerTestAccess::completed_hft_ranges(replayer).front());

	EXPECT_FALSE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001081500ull, 202001081501ull));
	EXPECT_EQ(std::vector<uint32_t>({1459u, 1500u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({14.9, 15.0}), sink.tickPrices);
	EXPECT_EQ(1u, HisDataReplayerTestAccess::completed_hft_ranges(replayer).size());

	HisDataReplayerTestAccess::reset_replayer(replayer);
	EXPECT_FALSE(HisDataReplayerTestAccess::has_completed_hft_ranges(replayer));
	EXPECT_TRUE(HisDataReplayerTestAccess::completed_hft_ranges(replayer).empty());
	replayer.clear_cache();
	EXPECT_FALSE(HisDataReplayerTestAccess::has_completed_hft_ranges(replayer));
}

TEST_F(P07TickLoaderTest, completedNaturalDateRangeRejectsBackwardScheduling)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	WTSTickStruct first{};
	std::strcpy(first.exchg, "SSE");
	std::strcpy(first.code, "600000");
	first.action_date = first.trading_date = 20200108u;
	first.action_time = p07_action_time(1459u, 30u);
	first.price = 14.9;
	WTSTickStruct next = first;
	next.action_date = next.trading_date = 20200109u;
	next.action_time = p07_action_time(1459u, 30u);
	next.price = 15.9;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200108/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{first}));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200109/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{next}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108145900ull, 20200109150100ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	replayer.register_sink(&sink, "p07_monotonic_completed_ranges");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_TRUE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001081459ull, 202001081500ull));
	EXPECT_TRUE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001091459ull, 202001091500ull));
	EXPECT_EQ(std::vector<uint32_t>({20200108u, 20200109u}), sink.tickDates);
	EXPECT_EQ(std::vector<double>({14.9, 15.9}), sink.tickPrices);
	EXPECT_EQ(20200109u, HisDataReplayerTestAccess::completed_hft_natural_date(replayer));

	EXPECT_FALSE(HisDataReplayerTestAccess::replay_hft_data(replayer, 202001081459ull, 202001081500ull));
	EXPECT_TRUE(HisDataReplayerTestAccess::replay_terminated(replayer));
	EXPECT_EQ(std::vector<uint32_t>({20200108u, 20200109u}), sink.tickDates);
	EXPECT_EQ(std::vector<double>({14.9, 15.9}), sink.tickPrices);
}

TEST_F(P07TickLoaderTest, fatalLevel2ReadStopsBeforeSessionCallbacks)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	WTSTickStruct tick{};
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = tick.trading_date = 20200102u;
	tick.action_time = p07_action_time(931u);
	tick.price = 10.1;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));
	const std::filesystem::path corruptDetail = _base / "his/orders/SSE/20200102/600000.dsb";
	std::error_code ec;
	std::filesystem::create_directories(corruptDetail.parent_path(), ec);
	ASSERT_FALSE(ec);
	{
		BlockHeader header{};
		std::memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
		header._type = BT_HIS_OrdDetail;
		header._version = BLOCK_VERSION_RAW_V2;
		std::ofstream out(corruptDetail, std::ios::binary);
		ASSERT_TRUE(out.is_open());
		out.write(reinterpret_cast<const char*>(&header), sizeof(header));
		out.put('x');
	}
	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001021500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() {
		replayer.sub_tick(1u, "SSE.STK.600000");
		replayer.sub_order_detail(2u, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_fatal_level2_before_session");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_TRUE(sink.detailTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, asynchronousOvernightBucketDrainsAfterMainBarAcrossWeekend)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strcpy(tick.exchg, "SHFE");
	std::strcpy(tick.code, "rb2001");
	tick.action_date = 20200103u;
	tick.trading_date = 20200106u;
	tick.action_time = p07_action_time(2200u, 30u, 125u);
	tick.open = tick.high = tick.low = tick.price = 55.1;
	tick.volume = tick.total_volume = 1.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SHFE/20200106/rb2001.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));

	WTSBarStruct bar;
	std::memset(&bar, 0, sizeof(bar));
	bar.date = 20200103u;
	bar.time = static_cast<uint64_t>(bar.date - 19900000u) * 10000u + 1500u;
	bar.open = 10.0;
	bar.high = 11.0;
	bar.low = 9.5;
	bar.close = 10.5;
	bar.vol = 20.0;
	bar.money = 210.0;
	WTSBarStruct historicalBar = bar;
	historicalBar.time = static_cast<uint64_t>(historicalBar.date - 19900000u) * 10000u + 1458u;
	historicalBar.open = 9.0;
	historicalBar.high = 9.5;
	historicalBar.low = 8.5;
	historicalBar.close = 9.2;
	historicalBar.vol = 10.0;
	historicalBar.money = 92.0;
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600000.dsb", BT_HIS_Minute1,
		std::vector<WTSBarStruct>{historicalBar, bar}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001031459ull, 202001032301ull,
		"commodities.json", {}, {}, _root / "dist/common/stk_comms.json");
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	const std::string mainCode = "SSE.STK.600000";
	const std::string hftCode = "SHFE.rb.2001";
	ASSERT_NE(nullptr, replayer.get_session_info(mainCode.c_str(), true));
	TickSink sink;
	sink.rp = &replayer;
	bool mainSliceValid = false;
	sink.onInit = [&replayer, &mainCode, &hftCode, &mainSliceValid]() {
		WTSKlineSlice* main = replayer.get_kline_slice(mainCode.c_str(), "m", 10u, 1u, true);
		if (main != nullptr)
		{
			mainSliceValid = true;
			main->release();
		}
		replayer.sub_tick(1u, mainCode.c_str());
		replayer.sub_tick(2u, hftCode.c_str());
	};
	replayer.register_sink(&sink, "p07_async_overnight_after_main_bar");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(mainSliceValid);
	ASSERT_EQ(5u, sink.tickCodes.size());
	EXPECT_EQ(std::vector<std::string>({mainCode, mainCode, mainCode, mainCode, hftCode}), sink.tickCodes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 1u, 2u, 3u, 0u}), sink.tickPxTypes);
	EXPECT_EQ(std::vector<bool>({true, true, true, true, false}), sink.tickSimulationStates);
	EXPECT_EQ(std::vector<uint32_t>({1500u, 1500u, 1500u, 1500u, 2200u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({20200103u, 20200103u, 20200103u, 20200103u, 20200103u}), sink.tickDates);
	EXPECT_EQ(std::vector<double>({10.0, 11.0, 9.5, 10.5, 55.1}), sink.tickPrices);
	ASSERT_EQ(1u, sink.closedCodes.size());
	EXPECT_EQ(mainCode, sink.closedCodes.front());
	EXPECT_EQ(20200103u, sink.closedDates.front());
	EXPECT_EQ(1500u, sink.closedTimes.front());

	ASSERT_EQ(6u, sink.dispatchEvents.size());
	for (std::size_t index = 0; index < 4; ++index)
	{
		EXPECT_EQ('K', sink.dispatchEvents[index].kind);
		EXPECT_EQ(mainCode, sink.dispatchEvents[index].code);
		EXPECT_EQ(20200103u, sink.dispatchEvents[index].date);
		EXPECT_EQ(1500u, sink.dispatchEvents[index].time);
	}
	EXPECT_EQ('C', sink.dispatchEvents[4].kind);
	EXPECT_EQ(mainCode, sink.dispatchEvents[4].code);
	EXPECT_EQ(1500u, sink.dispatchEvents[4].time);
	EXPECT_EQ('K', sink.dispatchEvents[5].kind);
	EXPECT_EQ(hftCode, sink.dispatchEvents[5].code);
	EXPECT_EQ(20200103u, sink.dispatchEvents[5].date);
	EXPECT_EQ(2200u, sink.dispatchEvents[5].time);
}

TEST_F(P07TickLoaderTest, fourHftStreamsHonorInclusiveSecondRangeAndStableOrder)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	// Two records per stream share each included millisecond. The scheduler must
	// emit D/T/K/Q in rounds at each timestamp, rather than draining all details
	// before the other stream kinds.
	const uint32_t seconds[] = { 9u, 10u, 10u, 10u, 10u, 11u };
	const uint32_t millis[] = { 999u, 0u, 0u, 999u, 999u, 0u };
	std::vector<WTSTickStruct> ticks;
	std::vector<WTSOrdDtlStruct> details;
	std::vector<WTSOrdQueStruct> queues;
	std::vector<WTSTransStruct> transactions;
	for (uint32_t i = 0; i < 6; i++)
	{
		WTSTickStruct tick{};
		std::strcpy(tick.exchg, "SSE");
		std::strcpy(tick.code, "600000");
		tick.action_date = tick.trading_date = 20200102u;
		tick.action_time = p07_action_time(931u, seconds[i], millis[i]);
		tick.open = tick.high = tick.low = tick.price = 10.0 + i;
		tick.volume = tick.total_volume = 1.0;
		ticks.push_back(tick);

		WTSOrdDtlStruct detail{};
		std::strcpy(detail.exchg, "SSE");
		std::strcpy(detail.code, "600000");
		detail.action_date = detail.trading_date = 20200102u;
		detail.action_time = tick.action_time;
		detail.price = 20.0 + i;
		detail.volume = i + 1;
		details.push_back(detail);

		WTSOrdQueStruct queue{};
		std::strcpy(queue.exchg, "SSE");
		std::strcpy(queue.code, "600000");
		queue.action_date = queue.trading_date = 20200102u;
		queue.action_time = tick.action_time;
		queue.price = 30.0 + i;
		queue.qsize = i + 1;
		queues.push_back(queue);

		WTSTransStruct transaction{};
		std::strcpy(transaction.exchg, "SSE");
		std::strcpy(transaction.code, "600000");
		transaction.action_date = transaction.trading_date = 20200102u;
		transaction.action_time = tick.action_time;
		transaction.price = 40.0 + i;
		transaction.volume = i + 1;
		transactions.push_back(transaction);
	}

	const std::filesystem::path dayDir = _base / "his";
	ASSERT_TRUE(write_raw_block(dayDir / "ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks, ticks));
	ASSERT_TRUE(write_raw_block(dayDir / "orders/SSE/20200102/600000.dsb", BT_HIS_OrdDetail, details));
	ASSERT_TRUE(write_raw_block(dayDir / "queue/SSE/20200102/600000.dsb", BT_HIS_OrdQueue, queues));
	ASSERT_TRUE(write_raw_block(dayDir / "trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn, transactions));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200102093110ull, 20200102093110ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() {
		replayer.sub_tick(1u, "SSE.STK.600000");
		replayer.sub_order_detail(2u, "SSE.STK.600000");
		replayer.sub_transaction(3u, "SSE.STK.600000");
		replayer.sub_order_queue(4u, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_four_stream_range");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<double>({11.0, 12.0, 13.0, 14.0}), sink.tickPrices);
	EXPECT_EQ(std::vector<double>({21.0, 22.0, 23.0, 24.0}), sink.detailPrices);
	EXPECT_EQ(std::vector<double>({31.0, 32.0, 33.0, 34.0}), sink.queuePrices);
	EXPECT_EQ(std::vector<double>({41.0, 42.0, 43.0, 44.0}), sink.transactionPrices);
	EXPECT_EQ(std::vector<char>({ 'D', 'T', 'K', 'Q', 'D', 'T', 'K', 'Q',
		'D', 'T', 'K', 'Q', 'D', 'T', 'K', 'Q' }), sink.hftEventOrder);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, minuteEndIncludesLastSecondAndEndsFinalTradingDateOnce)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	const std::filesystem::path sessions = _base / "p07_minute_end_sessions.json";
	const std::filesystem::path commodities = _base / "p07_minute_end_commodities.json";
	{
		std::ofstream out(sessions);
		ASSERT_TRUE(out.is_open());
		out << R"({"TRADING":{"name":"default","offset":0,"sections":[{"from":0,"to":0}]},"P07_24H":{"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]}})";
	}
	{
		std::ofstream out(commodities);
		ASSERT_TRUE(out.is_open());
		out << R"({"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}}})";
	}

	auto makeTick = [](uint32_t actionDate, uint32_t tradingDate, uint32_t hhmm,
		uint32_t second, uint32_t millisecond, double price)
	{
		WTSTickStruct tick{};
		std::strcpy(tick.exchg, "CRY");
		std::strcpy(tick.code, "BTC");
		tick.action_date = actionDate;
		tick.trading_date = tradingDate;
		tick.action_time = p07_action_time(hhmm, second, millisecond);
		tick.open = tick.high = tick.low = tick.price = price;
		tick.volume = tick.total_volume = 1.0;
		return tick;
	};
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200102/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{
			makeTick(20200102u, 20200102u, 2059u, 59u, 999u, 0.9),
			makeTick(20200102u, 20200102u, 2100u, 0u, 0u, 1.0),
			makeTick(20200102u, 20200102u, 2100u, 59u, 999u, 1.1),
			makeTick(20200102u, 20200102u, 2101u, 0u, 0u, 1.2),
		}));
	// 21:00 maps to trading date 20200103 in the default TRADING session, but
	// this subscribed zero-offset 24-hour contract still ends on 20200102.
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/CRY/20200103/BTC.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{makeTick(20200103u, 20200103u, 931u, 0u, 0u, 2.0)}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001022059ull, 202001022100ull,
		"stk_comms.json", sessions, commodities);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	ASSERT_NE(nullptr, replayer.get_session_info("CRY.COIN.BTC", true));
	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "CRY.COIN.BTC"); };
	replayer.register_sink(&sink, "p07_minute_end_once");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u, 20200102u, 20200102u}), sink.tickDates);
	EXPECT_EQ(std::vector<uint32_t>({2059u, 2100u, 2100u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({p07_action_time(2059u, 59u, 999u), p07_action_time(2100u),
		p07_action_time(2100u, 59u, 999u)}), sink.tickActionTimes);
	EXPECT_EQ(std::vector<double>({0.9, 1.0, 1.1}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.sessionBeginDates);
	EXPECT_EQ(std::vector<uint32_t>({20200102u}), sink.sessionEndDates);
	EXPECT_EQ(1u, sink.beginCalls);
	EXPECT_EQ(1u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, sameTimestampOrdersRoundsAcrossCodesAndTradingDateBuckets)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	const std::filesystem::path sessions = _base / "p07_multibucket_sessions.json";
	const std::filesystem::path commodities = _base / "p07_multibucket_commodities.json";
	{
		std::ofstream out(sessions);
		ASSERT_TRUE(out.is_open());
		out << R"({
			"TRADING": {"name":"default","offset":0,"sections":[{"from":0,"to":0}]},
			"P07_24H": {"name":"continuous","offset":0,"sections":[{"from":0,"to":0}]},
			"P07_NEG": {"name":"offset-negative","offset":-300,"sections":[{"from":0,"to":200}]}
		})";
	}
	{
		std::ofstream out(commodities);
		ASSERT_TRUE(out.is_open());
		out << R"({
			"CRY":{"COIN":{"category":1,"covermode":0,"exchg":"CRY","holiday":"CHINA","name":"continuous","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_24H","volscale":1}},
			"ZZZ":{"NEG":{"category":1,"covermode":0,"exchg":"ZZZ","holiday":"CHINA","name":"negative offset","precision":2,"pricemode":1,"pricetick":0.01,"session":"P07_NEG","volscale":1}}
		})";
	}

	const std::string codeA = "CRY.COIN.BTC";
	const std::string codeB = "ZZZ.NEG.N1";
	const uint32_t actionDate = 20200108u;
	const uint32_t actionTime = p07_action_time(131u, 10u, 100u);
	auto writeCodeBucket = [&](const char* exchg, const char* rawCode, uint32_t tradingDate, double priceBase)
	{
		std::vector<WTSTickStruct> ticks;
		std::vector<WTSOrdDtlStruct> details;
		std::vector<WTSTransStruct> transactions;
		std::vector<WTSOrdQueStruct> queues;
		for (uint32_t index = 0; index < 2; ++index)
		{
			WTSTickStruct tick{};
			std::strcpy(tick.exchg, exchg);
			std::strcpy(tick.code, rawCode);
			tick.action_date = actionDate;
			tick.trading_date = tradingDate;
			tick.action_time = actionTime;
			tick.price = priceBase + index;
			tick.open = tick.high = tick.low = tick.price;
			tick.volume = tick.total_volume = 1.0;
			ticks.push_back(tick);

			WTSOrdDtlStruct detail{};
			std::strcpy(detail.exchg, exchg);
			std::strcpy(detail.code, rawCode);
			detail.action_date = actionDate;
			detail.trading_date = tradingDate;
			detail.action_time = actionTime;
			detail.price = priceBase + 10.0 + index;
			details.push_back(detail);

			WTSTransStruct transaction{};
			std::strcpy(transaction.exchg, exchg);
			std::strcpy(transaction.code, rawCode);
			transaction.action_date = actionDate;
			transaction.trading_date = tradingDate;
			transaction.action_time = actionTime;
			transaction.price = priceBase + 20.0 + index;
			transactions.push_back(transaction);

			WTSOrdQueStruct queue{};
			std::strcpy(queue.exchg, exchg);
			std::strcpy(queue.code, rawCode);
			queue.action_date = actionDate;
			queue.trading_date = tradingDate;
			queue.action_time = actionTime;
			queue.price = priceBase + 30.0 + index;
			queues.push_back(queue);
		}
		const std::string date = std::to_string(tradingDate);
		ASSERT_TRUE(write_raw_block(_base / "his/ticks" / exchg / date / (std::string(rawCode) + ".dsb"), BT_HIS_Ticks, ticks));
		ASSERT_TRUE(write_raw_block(_base / "his/orders" / exchg / date / (std::string(rawCode) + ".dsb"), BT_HIS_OrdDetail, details));
		ASSERT_TRUE(write_raw_block(_base / "his/trans" / exchg / date / (std::string(rawCode) + ".dsb"), BT_HIS_Trnsctn, transactions));
		ASSERT_TRUE(write_raw_block(_base / "his/queue" / exchg / date / (std::string(rawCode) + ".dsb"), BT_HIS_OrdQueue, queues));
	};
	// Both codes report 20200108 01:31:10.100. The continuous market uses
	// natural-date bucket 20200108; the offset-negative session maps the same
	// action time to trading-date bucket 20200107.
	writeCodeBucket("CRY", "BTC", 20200108u, 100.0);
	writeCodeBucket("ZZZ", "N1", 20200107u, 200.0);

	auto runWithSubscriptionOrder = [&](bool reverse)
	{
		HisDataReplayer replayer;
		WTSVariant* cfg = bin_tick_cfg(_root, _base, 20200108013110ull, 20200108013110ull,
			"stk_comms.json", sessions, commodities);
		EXPECT_TRUE(replayer.init(cfg));
		cfg->release();
		TickSink sink;
		sink.onInit = [&]() {
			const std::string& first = reverse ? codeB : codeA;
			const std::string& second = reverse ? codeA : codeB;
			replayer.sub_order_detail(1u, first.c_str());
			replayer.sub_order_detail(2u, second.c_str());
			replayer.sub_transaction(3u, first.c_str());
			replayer.sub_transaction(4u, second.c_str());
			replayer.sub_tick(5u, first.c_str());
			replayer.sub_tick(6u, second.c_str());
			replayer.sub_order_queue(7u, first.c_str());
			replayer.sub_order_queue(8u, second.c_str());
		};
		replayer.register_sink(&sink, reverse ? "p07_multibucket_reverse" : "p07_multibucket_forward");
		EXPECT_TRUE(replayer.prepare());
		replayer.run(false);
		return std::make_pair(sink.hftEventOrder, sink.hftEventCodes);
	};

	const auto forward = runWithSubscriptionOrder(false);
	const auto reverse = runWithSubscriptionOrder(true);
	ASSERT_EQ(16u, forward.first.size());
	EXPECT_EQ(forward.first, reverse.first);
	EXPECT_EQ(forward.second, reverse.second);
	const std::vector<char> expectedKinds = {
		'D','D','T','T','K','K','Q','Q', 'D','D','T','T','K','K','Q','Q'
	};
	const std::vector<std::string> expectedCodes = {
		codeA,codeB,codeA,codeB,codeA,codeB,codeA,codeB,
		codeA,codeB,codeA,codeB,codeA,codeB,codeA,codeB
	};
	EXPECT_EQ(expectedKinds, forward.first);
	EXPECT_EQ(expectedCodes, forward.second);
}

TEST_F(P07TickLoaderTest, level2OnlyReplayPreservesTransactionsAfterMissingOtherStreams)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTransStruct transaction{};
	std::strcpy(transaction.exchg, "SSE");
	std::strcpy(transaction.code, "600000");
	transaction.action_date = transaction.trading_date = 20200102u;
	transaction.action_time = p07_action_time(931u);
	transaction.price = 10.4;
	transaction.volume = 4;
	ASSERT_TRUE(write_raw_block(_base / "his/trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{transaction}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	bool transactionSliceLoaded = false;
	TickSink sink;
	sink.onInit = [&]() {
		replayer.sub_order_detail(1u, "SSE.STK.600000");
		replayer.sub_order_queue(2u, "SSE.STK.600000");
		replayer.sub_transaction(3u, "SSE.STK.600000");
		WTSTransSlice* slice = replayer.get_transaction_slice("SSE.STK.600000", 1u, 202001020931ull);
		transactionSliceLoaded = slice != nullptr && slice->size() == 1 && slice->at(0)->price == 10.4;
		if (slice != nullptr)
			slice->release();
		EXPECT_EQ(nullptr, replayer.get_order_detail_slice("SSE.STK.600000", 1u));
		EXPECT_EQ(nullptr, replayer.get_order_queue_slice("SSE.STK.600000", 1u));
	};
	replayer.register_sink(&sink, "p07_level2_only");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(transactionSliceLoaded);
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_TRUE(sink.detailTimes.empty());
	EXPECT_TRUE(sink.queueTimes.empty());
	EXPECT_EQ(std::vector<double>({10.4}), sink.transactionPrices);
	EXPECT_EQ(std::vector<char>({ 'T' }), sink.hftEventOrder);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, unmappedReplayableLevel2SessionFailsClosed)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTransStruct transaction{};
	std::strcpy(transaction.exchg, "ZZZ");
	std::strcpy(transaction.code, "000001");
	transaction.action_date = transaction.trading_date = 20200103u;
	transaction.action_time = p07_action_time(931u);
	transaction.price = 10.4;
	transaction.volume = 1;
	ASSERT_TRUE(write_raw_block(_base / "his/trans/ZZZ/20200103/000001.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{transaction}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001031500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	TickSink sink;
	sink.onInit = [&replayer]() {
		replayer.sub_transaction(1u, "ZZZ.STK.000001");
	};
	replayer.register_sink(&sink, "p07_unmapped_hft_session");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(sink.transactionTimes.empty());
	EXPECT_EQ(0u, sink.beginCalls);
	EXPECT_EQ(0u, sink.endCalls);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, finalBarHftDrainKeepsPreloadedTransactionsWhenOtherStreamsAreMissing)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTransStruct transaction{};
	std::strcpy(transaction.exchg, "SSE");
	std::strcpy(transaction.code, "600000");
	transaction.action_date = transaction.trading_date = 20200102u;
	transaction.action_time = p07_action_time(1500u, 30u, 999u);
	transaction.price = 15.4;
	transaction.volume = 5;
	ASSERT_TRUE(write_raw_block(_base / "his/trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{transaction}));

	StubBtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001021500ull);
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	bool transactionSliceLoaded = false;
	TickSink sink;
	sink.onInit = [&]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1u, 1u, true);
		if (main != nullptr)
			main->release();
		replayer.sub_order_detail(1u, "SSE.STK.600000");
		replayer.sub_order_queue(2u, "SSE.STK.600000");
		replayer.sub_transaction(3u, "SSE.STK.600000");
		WTSTransSlice* slice = replayer.get_transaction_slice("SSE.STK.600000", 1u, 202001021501ull);
		transactionSliceLoaded = slice != nullptr && slice->size() == 1 && slice->at(0)->price == 15.4;
		if (slice != nullptr)
			slice->release();
		EXPECT_EQ(nullptr, replayer.get_order_detail_slice("SSE.STK.600000", 1u));
		EXPECT_EQ(nullptr, replayer.get_order_queue_slice("SSE.STK.600000", 1u));
	};
	replayer.register_sink(&sink, "p07_final_hft_drain");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(transactionSliceLoaded);
	EXPECT_TRUE(sink.detailTimes.empty());
	EXPECT_TRUE(sink.queueTimes.empty());
	EXPECT_EQ(std::vector<double>({15.4}), sink.transactionPrices);
	EXPECT_EQ(std::vector<uint32_t>({1500u}), sink.transactionTimes);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u, 1458u, 1459u, 1500u}), sink.closedTimes);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, historyQueriesDoNotMoveTickReplayCursor)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	std::vector<WTSTickStruct> ticks(3);
	for (uint32_t i = 0; i < ticks.size(); ++i)
	{
		WTSTickStruct& tick = ticks[i];
		std::memset(&tick, 0, sizeof(tick));
		std::strcpy(tick.exchg, "SSE");
		std::strcpy(tick.code, "600000");
		tick.action_date = tick.trading_date = 20200102u;
		tick.action_time = p07_action_time(931u + i);
		tick.open = tick.high = tick.low = tick.price = 10.0 + i;
		tick.volume = tick.total_volume = i + 1;
	}
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	bool initSliceLoaded = false;
	uint32_t callbackQueries = 0;
	TickSink sink;
	sink.onInit = [&]() {
		replayer.sub_tick(1u, "SSE.STK.600000");
		WTSTickSlice* slice = replayer.get_tick_slice("SSE.STK.600000", 2u, 202001020932ull);
		initSliceLoaded = slice != nullptr && slice->size() == 1 && slice->at(0)->price == 10.0;
		if (slice != nullptr)
			slice->release();
	};
	sink.onTickProbe = [&](uint32_t) {
		++callbackQueries;
		WTSTickSlice* slice = replayer.get_tick_slice("SSE.STK.600000", 2u);
		if (slice != nullptr)
			slice->release();
	};
	replayer.register_sink(&sink, "p07_tick_query_cursor");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_TRUE(initSliceLoaded);
	EXPECT_EQ(3u, callbackQueries);
	EXPECT_EQ(std::vector<double>({10.0, 11.0, 12.0}), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u, 933u}), sink.tickTimes);
}

TEST_F(P07TickLoaderTest, replayPresenceProbesDoNotReplaceBorrowedHistorySlice)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";
	auto makeTick = [](uint32_t date, double price)
	{
		WTSTickStruct tick{};
		std::strcpy(tick.exchg, "SSE");
		std::strcpy(tick.code, "600000");
		tick.action_date = tick.trading_date = date;
		tick.action_time = p07_action_time(931u);
		tick.price = price;
		tick.open = tick.high = tick.low = price;
		tick.volume = tick.total_volume = 1.0;
		return tick;
	};
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200108/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{makeTick(20200108u, 10.1)}));
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200109/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{makeTick(20200109u, 10.2)}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001080930ull, 202001091500ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	WTSTickSlice* retainedSlice = nullptr;
	bool sliceSurvivedSecondTradingDateProbe = false;
	TickSink sink;
	sink.onInit = [&]()
	{
		replayer.sub_tick(1u, "SSE.STK.600000");
		retainedSlice = replayer.get_tick_slice("SSE.STK.600000", 1u, 202001080932ull);
	};
	sink.onTickProbe = [&](uint32_t callbackCount)
	{
		if (callbackCount == 2u && retainedSlice != nullptr)
			sliceSurvivedSecondTradingDateProbe = retainedSlice->size() == 1u &&
				retainedSlice->at(0)->price == 10.1;
	};
	replayer.register_sink(&sink, "p07_borrowed_slice_probe");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	if (retainedSlice != nullptr)
		retainedSlice->release();

	EXPECT_TRUE(sliceSurvivedSecondTradingDateProbe);
	EXPECT_EQ(std::vector<uint32_t>({20200108u, 20200109u}), sink.tickDates);
	EXPECT_EQ(std::vector<double>({10.1, 10.2}), sink.tickPrices);
}

TEST_F(P07TickLoaderTest, level2CallbackExceptionsReleaseDataInBothReplayPaths)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSOrdDtlStruct detail{};
	std::strcpy(detail.exchg, "SSE");
	std::strcpy(detail.code, "600000");
	detail.action_date = detail.trading_date = 20200102u;
	detail.action_time = p07_action_time(931u);
	detail.price = 10.1;
	detail.volume = 1;
	WTSOrdQueStruct queue{};
	std::strcpy(queue.exchg, "SSE");
	std::strcpy(queue.code, "600000");
	queue.action_date = queue.trading_date = 20200102u;
	queue.action_time = p07_action_time(931u);
	queue.price = 10.2;
	queue.qsize = 1;
	WTSTransStruct transaction{};
	std::strcpy(transaction.exchg, "SSE");
	std::strcpy(transaction.code, "600000");
	transaction.action_date = transaction.trading_date = 20200102u;
	transaction.action_time = p07_action_time(931u);
	transaction.price = 10.3;
	transaction.volume = 1;
	const std::filesystem::path dayDir = _base / "his";
	ASSERT_TRUE(write_raw_block(dayDir / "orders/SSE/20200102/600000.dsb", BT_HIS_OrdDetail,
		std::vector<WTSOrdDtlStruct>{detail}));
	ASSERT_TRUE(write_raw_block(dayDir / "queue/SSE/20200102/600000.dsb", BT_HIS_OrdQueue,
		std::vector<WTSOrdQueStruct>{queue}));
	ASSERT_TRUE(write_raw_block(dayDir / "trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{transaction}));

	for (bool withMainBars : { false, true })		// false: replayHftDatasByDay, true: replayHftDatas
	{
		for (char stream : { 'D', 'T', 'Q' })
		{
			StubBtLoader loader;
			HisDataReplayer replayer;
			WTSVariant* cfg = bin_tick_cfg(_root, _base);
			const bool initialized = withMainBars
				? replayer.init(cfg, NULL, &loader)
				: replayer.init(cfg);
			cfg->release();
			ASSERT_TRUE(initialized);

			WTSObject* retained = nullptr;
			TickSink sink;
			sink.onInit = [&]() {
				if (withMainBars)
				{
					WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1u, 1u, true);
					if (main != nullptr)
						main->release();
				}
				if (stream == 'D')
					replayer.sub_order_detail(1u, "SSE.STK.600000");
				else if (stream == 'T')
					replayer.sub_transaction(1u, "SSE.STK.600000");
				else
					replayer.sub_order_queue(1u, "SSE.STK.600000");
			};
			auto retainAndThrow = [&retained](WTSObject* object)
			{
				retained = object;
				object->retain();
				throw std::runtime_error("Level2 callback failure");
			};
			sink.onDetailProbe = [&retainAndThrow](WTSOrdDtlData* object) { retainAndThrow(object); };
			sink.onTransactionProbe = [&retainAndThrow](WTSTransData* object) { retainAndThrow(object); };
			sink.onQueueProbe = [&retainAndThrow](WTSOrdQueData* object) { retainAndThrow(object); };
			replayer.register_sink(&sink, "p07_level2_exception");
			ASSERT_TRUE(replayer.prepare());
			EXPECT_THROW(replayer.run(false), std::runtime_error);
			ASSERT_NE(nullptr, retained);
			EXPECT_EQ(1u, retained->retainCount());
			retained->release();
		}
	}
}

TEST_F(P07TickLoaderTest, hftTickCallbackExceptionAllowsReplayByDayRetry)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = 20200102u;
	tick.trading_date = 20200102u;
	tick.action_time = 931u * 100000u;
	tick.open = 10.0;
	tick.high = 11.0;
	tick.low = 9.0;
	tick.price = 10.5;
	tick.volume = 100.0;
	tick.total_volume = 100.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	TickSink sink;
	sink.rp = &replayer;
	bool throwOnce = true;
	sink.onInit = [&replayer]() { replayer.sub_tick(1u, "SSE.STK.600000"); };
	sink.onTickProbe = [&throwOnce](uint32_t) {
		if (throwOnce)
		{
			throwOnce = false;
			throw std::runtime_error("HFT per-day tick callback failure");
		}
	};
	replayer.register_sink(&sink, "p07_hft_by_day_exception");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_THROW(replayer.run(false), std::runtime_error);
	sink.tickPrices.clear();
	sink.tickDates.clear();
	sink.tickTimes.clear();
	sink.tickPxTypes.clear();

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({10.5}), sink.tickPrices);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, hftTickCallbackExceptionAllowsBarReplayRetry)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001020932ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	sink.rp = &replayer;
	bool throwOnce = true;
	sink.onInit = [&replayer]() {
		WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 10, 1, true);
		if (main != NULL)
			main->release();
		replayer.sub_tick(1u, "SSE.STK.600000");
	};
	sink.onTickProbe = [&sink, &replayer, &throwOnce](uint32_t) {
		if (throwOnce && !replayer.is_tick_simulated() && sink.tickPrices.back() == 10.2)
		{
			throwOnce = false;
			throw std::runtime_error("HFT bar tick callback failure");
		}
	};
	replayer.register_sink(&sink, "p07_hft_bar_exception");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_THROW(replayer.run(false), std::runtime_error);
	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({10.0}), sink.closedPrices);
	EXPECT_EQ(std::vector<uint32_t>({931u, 931u, 931u, 931u, 931u}), sink.tickTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 1u, 2u, 3u, 0u}), sink.tickPxTypes);
	EXPECT_EQ(std::vector<bool>({true, true, true, true, false}), sink.tickSimulationStates);
	EXPECT_EQ(std::vector<double>({9.9, 10.1, 9.7, 10.0, 10.2}), sink.tickPrices);
	sink.tickPrices.clear();
	sink.tickDates.clear();
	sink.tickTimes.clear();
	sink.tickActionTimes.clear();
	sink.tickCodes.clear();
	sink.tickPxTypes.clear();
	sink.tickSimulationStates.clear();
	sink.closedPrices.clear();
	sink.closedDates.clear();
	sink.closedTimes.clear();

	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u}), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({10.0, 10.2}), sink.closedPrices);
	std::vector<uint32_t> simulatedTimes, realTimes, simulatedPxTypes, realPxTypes;
	std::vector<double> simulatedPrices, realPrices;
	for (std::size_t i = 0; i < sink.tickTimes.size(); ++i)
	{
		if (sink.tickSimulationStates[i])
		{
			simulatedTimes.push_back(sink.tickTimes[i]);
			simulatedPxTypes.push_back(sink.tickPxTypes[i]);
			simulatedPrices.push_back(sink.tickPrices[i]);
		}
		else
		{
			realTimes.push_back(sink.tickTimes[i]);
			realPxTypes.push_back(sink.tickPxTypes[i]);
			realPrices.push_back(sink.tickPrices[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>(4u, 931u), simulatedTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 1u, 2u, 3u}), simulatedPxTypes);
	EXPECT_EQ(std::vector<double>({9.9, 10.1, 9.7, 10.0}), simulatedPrices);
	EXPECT_EQ(std::vector<uint32_t>({931u, 932u}), realTimes);
	EXPECT_EQ(std::vector<uint32_t>({0u, 0u}), realPxTypes);
	EXPECT_EQ(std::vector<double>({10.2, 10.8}), realPrices);
}

TEST_F(P07TickLoaderTest, realHftTickSubscriptionStartsAtNextCompleteBar)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	std::vector<WTSTickStruct> ticks(2);
	const uint32_t hhmm[] = { 930u, 932u };
	const double prices[] = { 10.5, 10.8 };
	for (std::size_t i = 0; i < ticks.size(); ++i)
	{
		WTSTickStruct& tick = ticks[i];
		std::memset(&tick, 0, sizeof(tick));
		std::strcpy(tick.exchg, "SSE");
		std::strcpy(tick.code, "600000");
		tick.action_date = tick.trading_date = 20200102u;
		tick.action_time = p07_action_time(hhmm[i], 30u);
		tick.open = tick.high = tick.low = tick.price = prices[i];
		tick.volume = tick.total_volume = 1.0;
	}
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks, ticks));

	std::vector<WTSBarStruct> minuteBars(3);
	const uint32_t barHHMM[] = { 931u, 932u, 933u };
	const double barOpen[] = { 9.9, 10.0, 10.2 };
	const double barHigh[] = { 10.1, 10.5, 10.8 };
	const double barLow[] = { 9.7, 9.8, 10.1 };
	const double barClose[] = { 10.0, 10.2, 10.6 };
	const double barVolume[] = { 10.0, 20.0, 30.0 };
	for (std::size_t i = 0; i < minuteBars.size(); ++i)
	{
		WTSBarStruct& bar = minuteBars[i];
		std::memset(&bar, 0, sizeof(bar));
		bar.date = 20200102u;
		bar.time = static_cast<uint64_t>(20200102u - 19900000u) * 10000u + barHHMM[i];
		bar.open = barOpen[i];
		bar.high = barHigh[i];
		bar.low = barLow[i];
		bar.close = barClose[i];
		bar.vol = barVolume[i];
		bar.money = barVolume[i] * barClose[i];
	}
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600000.dsb", BT_HIS_Minute1, minuteBars));
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600001.dsb", BT_HIS_Minute1, minuteBars));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001020933ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	const std::string mainCode = "SSE.STK.600000";
	const std::string newCode = "SSE.STK.600001";
	TickSink sink;
	sink.rp = &replayer;
	bool newSeriesRequested = false;
	sink.onInit = [&replayer, &mainCode]() {
		WTSKlineSlice* main = replayer.get_kline_slice(mainCode.c_str(), "m", 10u, 1u, true);
		if (main != nullptr)
			main->release();
		replayer.sub_tick(1u, mainCode.c_str());
	};
	sink.onTickProbe = [&replayer, &newSeriesRequested, &newCode](uint32_t tickSeq) {
		if (newSeriesRequested || tickSeq != 1u)
			return;
		newSeriesRequested = true;
		WTSKlineSlice* newlyRequested = replayer.get_kline_slice(newCode.c_str(), "m", 10u, 1u, false);
		if (newlyRequested != nullptr)
			newlyRequested->release();
	};
	replayer.register_sink(&sink, "p07_real_tick_subscribe_next_bar");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_TRUE(newSeriesRequested);
	std::vector<double> rawTickPrices;
	std::vector<uint32_t> rawTickDates, rawTickTimes, rawTickActionTimes, rawTickPxTypes;
	std::vector<bool> rawTickSimulationStates;
	for (std::size_t i = 0; i < sink.tickCodes.size(); ++i)
	{
		if (sink.tickCodes[i] == mainCode && sink.tickActionTimes[i] % 100000u == 30000u)
		{
			rawTickPrices.push_back(sink.tickPrices[i]);
			rawTickDates.push_back(sink.tickDates[i]);
			rawTickTimes.push_back(sink.tickTimes[i]);
			rawTickActionTimes.push_back(sink.tickActionTimes[i]);
			rawTickPxTypes.push_back(sink.tickPxTypes[i]);
			rawTickSimulationStates.push_back(sink.tickSimulationStates[i]);
		}
	}
	EXPECT_EQ(std::vector<double>({ 10.5, 10.8 }), rawTickPrices);
	EXPECT_EQ(std::vector<uint32_t>({ 20200102u, 20200102u }), rawTickDates);
	EXPECT_EQ(std::vector<uint32_t>({ 930u, 932u }), rawTickTimes);
	EXPECT_EQ(std::vector<uint32_t>({ p07_action_time(930u, 30u), p07_action_time(932u, 30u) }), rawTickActionTimes);
	EXPECT_EQ(std::vector<uint32_t>({ 0u, 0u }), rawTickPxTypes);
	EXPECT_EQ(std::vector<bool>({ false, false }), rawTickSimulationStates);
	std::vector<uint32_t> simulatedActionTimes;
	std::vector<bool> simulatedStates;
	for (std::size_t i = 0; i < sink.tickActionTimes.size(); ++i)
		if (sink.tickActionTimes[i] % 100000u == 0)
		{
			simulatedActionTimes.push_back(sink.tickActionTimes[i]);
			simulatedStates.push_back(sink.tickSimulationStates[i]);
		}
	std::vector<uint32_t> expectedSimulatedActionTimes(8u, p07_action_time(932u));
	expectedSimulatedActionTimes.insert(expectedSimulatedActionTimes.end(), 4u, p07_action_time(933u));
	EXPECT_EQ(expectedSimulatedActionTimes, simulatedActionTimes);
	EXPECT_EQ(std::vector<bool>(12u, true), simulatedStates);

	std::vector<double> newSeriesTickPrices;
	std::vector<uint32_t> newSeriesTickDates, newSeriesTickTimes, newSeriesTickActionTimes, newSeriesTickPxTypes;
	for (std::size_t i = 0; i < sink.tickCodes.size(); ++i)
	{
		if (sink.tickCodes[i] == newCode)
		{
			newSeriesTickPrices.push_back(sink.tickPrices[i]);
			newSeriesTickDates.push_back(sink.tickDates[i]);
			newSeriesTickTimes.push_back(sink.tickTimes[i]);
			newSeriesTickActionTimes.push_back(sink.tickActionTimes[i]);
			newSeriesTickPxTypes.push_back(sink.tickPxTypes[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>(8u, 20200102u), newSeriesTickDates);
	EXPECT_EQ(std::vector<uint32_t>({ 932u, 932u, 932u, 932u, 933u, 933u, 933u, 933u }), newSeriesTickTimes);
	std::vector<uint32_t> expectedNewSeriesActionTimes(4u, p07_action_time(932u));
	expectedNewSeriesActionTimes.insert(expectedNewSeriesActionTimes.end(), 4u, p07_action_time(933u));
	EXPECT_EQ(expectedNewSeriesActionTimes, newSeriesTickActionTimes);
	EXPECT_EQ(std::vector<uint32_t>({ 0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u }), newSeriesTickPxTypes);
	EXPECT_EQ(std::vector<double>({ 10.0, 10.5, 9.8, 10.2, 10.2, 10.8, 10.1, 10.6 }), newSeriesTickPrices);
	// The newly subscribed contract has no real Tick, so it keeps OHLC fallback
	// at 09:32 and 09:33 even when the main contract receives a real Tick at 09:32:30.

	std::vector<uint32_t> mainCloseTimes, newSeriesCloseTimes;
	std::vector<double> mainCloses, newSeriesCloses;
	for (std::size_t i = 0; i < sink.closedCodes.size(); ++i)
	{
		if (sink.closedCodes[i] == mainCode)
		{
			mainCloseTimes.push_back(sink.closedTimes[i]);
			mainCloses.push_back(sink.closedPrices[i]);
		}
		else if (sink.closedCodes[i] == newCode)
		{
			newSeriesCloseTimes.push_back(sink.closedTimes[i]);
			newSeriesCloses.push_back(sink.closedPrices[i]);
		}
	}
	EXPECT_EQ(std::vector<uint32_t>({ 931u, 932u, 933u }), mainCloseTimes);
	EXPECT_EQ(std::vector<double>({ 10.0, 10.2, 10.6 }), mainCloses);
	// The 09:30:30 callback adds the new series. It skips 09:31, then closes
	// on each subsequent complete bar even when OHLC fallback varies by bar.
	EXPECT_EQ(std::vector<uint32_t>({ 932u, 933u }), newSeriesCloseTimes);
	EXPECT_EQ(std::vector<double>({ 10.2, 10.6 }), newSeriesCloses);
}

TEST_F(P07TickLoaderTest, level2AndOutOfRangeTicksDoNotSuppressOhlcFallback)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct outOfRangeTick{};
	std::strcpy(outOfRangeTick.exchg, "SSE");
	std::strcpy(outOfRangeTick.code, "600000");
	outOfRangeTick.action_date = outOfRangeTick.trading_date = 20200102u;
	outOfRangeTick.action_time = p07_action_time(933u, 30u);
	outOfRangeTick.open = outOfRangeTick.high = outOfRangeTick.low = outOfRangeTick.price = 99.0;
	outOfRangeTick.volume = outOfRangeTick.total_volume = 1.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{outOfRangeTick}));

	WTSOrdDtlStruct detail{};
	std::strcpy(detail.exchg, "SSE");
	std::strcpy(detail.code, "600000");
	detail.action_date = detail.trading_date = 20200102u;
	detail.action_time = p07_action_time(930u, 30u);
	detail.price = 10.1;
	detail.volume = 1;
	WTSOrdQueStruct queue{};
	std::strcpy(queue.exchg, "SSE");
	std::strcpy(queue.code, "600000");
	queue.action_date = queue.trading_date = 20200102u;
	queue.action_time = p07_action_time(930u, 30u);
	queue.price = 10.2;
	queue.qsize = 1;
	WTSTransStruct firstTransaction{};
	std::strcpy(firstTransaction.exchg, "SSE");
	std::strcpy(firstTransaction.code, "600000");
	firstTransaction.action_date = firstTransaction.trading_date = 20200102u;
	firstTransaction.action_time = p07_action_time(930u, 30u);
	firstTransaction.price = 10.3;
	firstTransaction.volume = 1;
	WTSTransStruct laterTransaction = firstTransaction;
	laterTransaction.action_time = p07_action_time(931u, 30u);
	laterTransaction.price = 10.4;
	const std::filesystem::path dayDir = _base / "his";
	ASSERT_TRUE(write_raw_block(dayDir / "orders/SSE/20200102/600000.dsb", BT_HIS_OrdDetail,
		std::vector<WTSOrdDtlStruct>{detail}));
	ASSERT_TRUE(write_raw_block(dayDir / "queue/SSE/20200102/600000.dsb", BT_HIS_OrdQueue,
		std::vector<WTSOrdQueStruct>{queue}));
	ASSERT_TRUE(write_raw_block(dayDir / "trans/SSE/20200102/600000.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{firstTransaction, laterTransaction}));

	std::vector<WTSBarStruct> bars(2);
	const uint32_t barTimes[] = { 931u, 932u };
	const double opens[] = { 9.9, 10.0 };
	const double highs[] = { 10.1, 10.5 };
	const double lows[] = { 9.7, 9.8 };
	const double closes[] = { 10.0, 10.2 };
	for (std::size_t i = 0; i < bars.size(); ++i)
	{
		WTSBarStruct& bar = bars[i];
		std::memset(&bar, 0, sizeof(bar));
		bar.date = 20200102u;
		bar.time = static_cast<uint64_t>(20200102u - 19900000u) * 10000u + barTimes[i];
		bar.open = opens[i];
		bar.high = highs[i];
		bar.low = lows[i];
		bar.close = closes[i];
		bar.vol = 10.0 + i;
		bar.money = bar.vol * bar.close;
	}
	ASSERT_TRUE(write_raw_block(dayDir / "min1/SSE/600000.dsb", BT_HIS_Minute1, bars));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001020932ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	const std::string code = "SSE.STK.600000";
	TickSink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer, &code]() {
		WTSKlineSlice* main = replayer.get_kline_slice(code.c_str(), "m", 10u, 1u, true);
		if (main != nullptr)
			main->release();
		replayer.sub_tick(1u, code.c_str());
		replayer.sub_order_detail(2u, code.c_str());
		replayer.sub_order_queue(3u, code.c_str());
		replayer.sub_transaction(4u, code.c_str());
	};
	replayer.register_sink(&sink, "p07_level2_does_not_count_as_tick");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({ 930u }), sink.detailTimes);
	EXPECT_EQ(std::vector<uint32_t>({ 930u }), sink.queueTimes);
	EXPECT_EQ(std::vector<uint32_t>({ 930u, 931u }), sink.transactionTimes);
	EXPECT_EQ(std::vector<bool>({ true, true, true, true }), sink.level2SimulationStates);
	std::vector<uint32_t> rawTickActionTimes;
	for (uint32_t actionTime : sink.tickActionTimes)
		if (actionTime % 100000u == 30000u)
			rawTickActionTimes.push_back(actionTime);
	EXPECT_TRUE(rawTickActionTimes.empty());
	EXPECT_EQ(std::vector<uint32_t>({ 0u, 1u, 2u, 3u, 0u, 1u, 2u, 3u }), sink.tickPxTypes);
	EXPECT_EQ(std::vector<uint32_t>({ 931u, 931u, 931u, 931u, 932u, 932u, 932u, 932u }), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({ 9.9, 10.1, 9.7, 10.0, 10.0, 10.5, 9.8, 10.2 }), sink.tickPrices);
	EXPECT_EQ(std::vector<uint32_t>({ 931u, 932u }), sink.closedTimes);
	EXPECT_EQ(std::vector<double>({ 10.0, 10.2 }), sink.closedPrices);
	EXPECT_EQ(std::vector<bool>(8u, true), sink.tickSimulationStates);
}

TEST_F(P07TickLoaderTest, level2StopDoesNotSynthesizeInterruptedBar)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSOrdDtlStruct detail{};
	std::strcpy(detail.exchg, "SSE");
	std::strcpy(detail.code, "600000");
	detail.action_date = detail.trading_date = 20200102u;
	detail.action_time = p07_action_time(930u, 30u);
	detail.price = 10.1;
	detail.volume = 1;
	ASSERT_TRUE(write_raw_block(_base / "his/orders/SSE/20200102/600000.dsb", BT_HIS_OrdDetail,
		std::vector<WTSOrdDtlStruct>{detail}));

	WTSBarStruct bar{};
	bar.date = 20200102u;
	bar.time = static_cast<uint64_t>(bar.date - 19900000u) * 10000u + 931u;
	bar.open = 9.9;
	bar.high = 10.1;
	bar.low = 9.7;
	bar.close = 10.0;
	bar.vol = 10.0;
	bar.money = bar.vol * bar.close;
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600000.dsb", BT_HIS_Minute1,
		std::vector<WTSBarStruct>{bar}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001020931ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	const std::string code = "SSE.STK.600000";
	TickSink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer, &code]() {
		WTSKlineSlice* main = replayer.get_kline_slice(code.c_str(), "m", 10u, 1u, true);
		if (main != nullptr)
			main->release();
		replayer.sub_order_detail(1u, code.c_str());
	};
	sink.onDetailProbe = [&replayer](WTSOrdDtlData*) { replayer.stop(); };
	replayer.register_sink(&sink, "p07_level2_stop_no_ohlc");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({ 930u }), sink.detailTimes);
	EXPECT_TRUE(sink.tickTimes.empty());
	EXPECT_TRUE(sink.closedTimes.empty());
	EXPECT_EQ(std::vector<bool>({ true }), sink.level2SimulationStates);
}

TEST_F(P07TickLoaderTest, rawTickSuppressesQfqAndHfqBarsForSameBaseCode)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct tick{};
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600000");
	tick.action_date = tick.trading_date = 20200102u;
	tick.action_time = p07_action_time(930u, 30u);
	tick.open = tick.high = tick.low = tick.price = 10.5;
	tick.volume = tick.total_volume = 1.0;
	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600000.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));

	auto make_minute_bar = [](double open, double high, double low, double close)
	{
		WTSBarStruct bar{};
		bar.date = 20200102u;
		bar.time = static_cast<uint64_t>(bar.date - 19900000u) * 10000u + 931u;
		bar.open = open;
		bar.high = high;
		bar.low = low;
		bar.close = close;
		bar.vol = 10.0;
		bar.money = bar.vol * close;
		return bar;
	};
	// '-' is QFQ and '+' is HFQ in CodeHelper; the DSB names follow that suffix.
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600000-.dsb", BT_HIS_Minute1,
		std::vector<WTSBarStruct>{make_minute_bar(20.0, 21.0, 19.0, 20.5)}));
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600000+.dsb", BT_HIS_Minute1,
		std::vector<WTSBarStruct>{make_minute_bar(30.0, 31.0, 29.0, 30.5)}));
	ASSERT_TRUE(write_raw_block(_base / "his/min1/SSE/600001.dsb", BT_HIS_Minute1,
		std::vector<WTSBarStruct>{make_minute_bar(40.0, 41.0, 39.0, 40.5)}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base, 202001020930ull, 202001020931ull);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();
	const std::string qfqCode = "SSE.STK.600000-";
	const std::string hfqCode = "SSE.STK.600000+";
	const std::string otherCode = "SSE.STK.600001";
	TickSink sink;
	sink.rp = &replayer;
	sink.onInit = [&replayer, &qfqCode, &hfqCode, &otherCode]() {
		WTSKlineSlice* qfq = replayer.get_kline_slice(qfqCode.c_str(), "m", 10u, 1u, true);
		if (qfq != nullptr)
			qfq->release();
		WTSKlineSlice* hfq = replayer.get_kline_slice(hfqCode.c_str(), "m", 10u, 1u, false);
		if (hfq != nullptr)
			hfq->release();
		WTSKlineSlice* other = replayer.get_kline_slice(otherCode.c_str(), "m", 10u, 1u, false);
		if (other != nullptr)
			other->release();
		replayer.sub_tick(1u, qfqCode.c_str());
		replayer.sub_tick(2u, hfqCode.c_str());
	};
	replayer.register_sink(&sink, "p07_adjusted_bar_tick_identity");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	ASSERT_EQ(5u, sink.tickCodes.size());
	EXPECT_EQ("SSE.STK.600000", sink.tickCodes.front());
	EXPECT_EQ(20200102u, sink.tickDates.front());
	EXPECT_EQ(p07_action_time(930u, 30u), sink.tickActionTimes.front());
	EXPECT_DOUBLE_EQ(10.5, sink.tickPrices.front());
	EXPECT_EQ(0u, sink.tickPxTypes.front());
	EXPECT_FALSE(sink.tickSimulationStates.front());
	for (std::size_t i = 1; i < sink.tickCodes.size(); ++i)
	{
		EXPECT_EQ(otherCode, sink.tickCodes[i]);
		EXPECT_EQ(p07_action_time(931u), sink.tickActionTimes[i]);
		EXPECT_EQ(static_cast<uint32_t>(i - 1), sink.tickPxTypes[i]);
		EXPECT_TRUE(sink.tickSimulationStates[i]);
	}
	EXPECT_EQ(std::vector<double>({ 40.0, 41.0, 39.0, 40.5 }),
		std::vector<double>(sink.tickPrices.begin() + 1, sink.tickPrices.end()));

	std::vector<std::string> closedCodes = sink.closedCodes;
	std::sort(closedCodes.begin(), closedCodes.end());
	EXPECT_EQ(std::vector<std::string>({ hfqCode, qfqCode, otherCode }), closedCodes);
	EXPECT_EQ(std::vector<uint32_t>(3u, 931u), sink.closedTimes);
}

TEST_F(P07TickLoaderTest, tickInSessionGapIsSkippedUntilNextSession)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	std::vector<WTSTickStruct> ticks(3);
	const uint32_t tickTimes[] = {1130u, 1131u, 1300u};
	const double tickPrices[] = {11.3, 11.31, 13.0};
	for (std::size_t i = 0; i < ticks.size(); i++)
	{
		WTSTickStruct& tick = ticks[i];
		std::memset(&tick, 0, sizeof(tick));
		std::strcpy(tick.exchg, "SSE");
		std::strcpy(tick.code, "600002");
		tick.action_date = 20200102u;
		tick.trading_date = 20200102u;
		tick.action_time = tickTimes[i] * 100000u;
		tick.open = tickPrices[i];
		tick.high = tickPrices[i];
		tick.low = tickPrices[i];
		tick.price = tickPrices[i];
		tick.volume = 100.0;
		tick.total_volume = 100.0 * (i + 1);
	}

	ASSERT_TRUE(write_raw_block(_base / "his/ticks/SSE/20200102/600002.dsb", BT_HIS_Ticks, ticks));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	TickSink sink;
	sink.onInit = [&replayer]() { replayer.sub_tick(5u, "SSE.STK.600002"); };
	replayer.register_sink(&sink, "p07_gap_tick");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({1130u, 1300u}), sink.tickTimes);
	EXPECT_EQ(std::vector<double>({11.3, 13.0}), sink.tickPrices);
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

TEST_F(P07TickLoaderTest, emptyLevel2StreamsDoNotBlockTickReplay)
{
	InstDirRestore restoreInstDir;
	ASSERT_TRUE(set_data_storage_inst_dir(_root)) << "WtDataStorage module is not available in the configured build output";

	WTSTickStruct tick;
	std::memset(&tick, 0, sizeof(tick));
	std::strcpy(tick.exchg, "SSE");
	std::strcpy(tick.code, "600001");
	tick.action_date = 20200102u;
	tick.trading_date = 20200102u;
	tick.action_time = 931u * 100000u;
	tick.open = 10.0;
	tick.high = 11.0;
	tick.low = 9.0;
	tick.price = 10.5;
	tick.volume = 100.0;
	tick.total_volume = 100.0;

	const std::filesystem::path dayDir = _base / "his";
	ASSERT_TRUE(write_raw_block(dayDir / "ticks/SSE/20200102/600001.dsb", BT_HIS_Ticks,
		std::vector<WTSTickStruct>{tick}));
	ASSERT_TRUE(write_raw_block(dayDir / "orders/SSE/20200102/600001.dsb", BT_HIS_OrdDetail,
		std::vector<WTSOrdDtlStruct>{}));
	ASSERT_TRUE(write_raw_block(dayDir / "queue/SSE/20200102/600001.dsb", BT_HIS_OrdQueue,
		std::vector<WTSOrdQueStruct>{}));
	ASSERT_TRUE(write_raw_block(dayDir / "trans/SSE/20200102/600001.dsb", BT_HIS_Trnsctn,
		std::vector<WTSTransStruct>{}));

	HisDataReplayer replayer;
	WTSVariant* cfg = bin_tick_cfg(_root, _base);
	ASSERT_TRUE(replayer.init(cfg));
	cfg->release();

	TickSink sink;
	sink.onInit = [&replayer]() {
		replayer.sub_tick(1u, "SSE.STK.600001");
		replayer.sub_order_detail(2u, "SSE.STK.600001");
		replayer.sub_order_queue(3u, "SSE.STK.600001");
		replayer.sub_transaction(4u, "SSE.STK.600001");
	};
	replayer.register_sink(&sink, "p07_empty_hft");
	ASSERT_TRUE(replayer.prepare());
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({931u}), sink.tickTimes);
	EXPECT_TRUE(sink.detailTimes.empty());
	EXPECT_TRUE(sink.queueTimes.empty());
	EXPECT_TRUE(sink.transactionTimes.empty());
	EXPECT_EQ(1u, sink.replayDoneCalls);
}

/*
 * 回调收到的 tick 应是当前最新行情；tick 模式的 get_last_tick 和
 * get_day_price(0..3) 都应对应这同一条 tick。本用例固定检查当天全部五笔，
 * 并比较每笔的 date/time/OHLC/price，避免尾笔数量问题掩盖 getter 是否错位。
 */
TEST_F(P07TickLoaderTest, lastTickAndDayPriceMatchCurrentCallbackTick)
{
	StubBtLoader loader;
	loader._failRepeatedTickLoads = true;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001021500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	sink.rp = &replayer;
	sink.probeCode = "SSE.STK.600000";
	sink.peekLastTick = true;
	bool strategySawLoaderReturn = true;
	sink.onTickProbe = [&loader, &strategySawLoaderReturn](uint32_t) {
		strategySawLoaderReturn = strategySawLoaderReturn && loader._lastTickLoadReturned;
	};
	sink.onInit = [&replayer]() {
		// prepare() 会 reset 并清空订阅；在策略初始化回调中登记订阅。
		replayer.sub_tick(11, "SSE.STK.600000");
	};
	replayer.register_sink(&sink, "p07_peek");

	ASSERT_TRUE(replayer.prepare());
	ASSERT_NE(nullptr, replayer.get_session_info("SSE.STK.600000", true));
	replayer.run(false);

	EXPECT_EQ(std::vector<uint32_t>({20200102u}), loader._loadedTickDates);
	EXPECT_TRUE(loader._lastTickLoadReturned);
	EXPECT_TRUE(loader._lastTickCallbackHadObj);
	EXPECT_TRUE(strategySawLoaderReturn);
	ASSERT_EQ(static_cast<std::size_t>(kDay1Ticks), sink.tickPrices.size());
	const std::vector<uint32_t> expectedDates(kDay1Ticks, 20200102u);
	const std::vector<uint32_t> expectedTimes({ 931u, 932u, 933u, 1458u, 1459u });
	const std::vector<uint32_t> expectedActionTimes({ p07_action_time(931u), p07_action_time(932u),
		p07_action_time(933u), p07_action_time(1458u), p07_action_time(1459u) });
	const std::vector<double> expectedPrices({ 10.2, 10.8, 11.5, 11.6, 12.0 });
	const std::vector<double> expectedOpens({ 10.0, 10.2, 10.8, 11.5, 11.6 });
	const std::vector<double> expectedHighs({ 10.5, 11.0, 12.0, 11.8, 12.5 });
	const std::vector<double> expectedLows({ 9.8, 10.1, 10.6, 11.2, 11.4 });
	EXPECT_EQ(expectedDates, sink.tickDates);
	EXPECT_EQ(expectedTimes, sink.tickTimes);
	EXPECT_EQ(expectedActionTimes, sink.tickActionTimes);
	EXPECT_EQ(expectedPrices, sink.tickPrices);
	EXPECT_EQ(expectedOpens, sink.tickOpens);
	EXPECT_EQ(expectedHighs, sink.tickHighs);
	EXPECT_EQ(expectedLows, sink.tickLows);

	// Pin both ends explicitly so this getter test cannot pass if the final event is omitted.
	EXPECT_EQ(20200102u, sink.tickDates.front());
	EXPECT_EQ(p07_action_time(931u), sink.tickActionTimes.front());
	EXPECT_DOUBLE_EQ(10.0, sink.tickOpens.front());
	EXPECT_DOUBLE_EQ(10.5, sink.tickHighs.front());
	EXPECT_DOUBLE_EQ(9.8, sink.tickLows.front());
	EXPECT_DOUBLE_EQ(10.2, sink.tickPrices.front());
	EXPECT_EQ(20200102u, sink.tickDates.back());
	EXPECT_EQ(p07_action_time(1459u), sink.tickActionTimes.back());
	EXPECT_DOUBLE_EQ(11.6, sink.tickOpens.back());
	EXPECT_DOUBLE_EQ(12.5, sink.tickHighs.back());
	EXPECT_DOUBLE_EQ(11.4, sink.tickLows.back());
	EXPECT_DOUBLE_EQ(12.0, sink.tickPrices.back());

	ASSERT_EQ(sink.tickPrices.size(), sink.tickOpens.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.tickHighs.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.tickLows.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickDates.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickActionTimes.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickOpens.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickHighs.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickLows.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.lastTickPrices.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayOpen.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayHigh.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayLow.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.dayLast.size());
	ASSERT_EQ(sink.tickPrices.size(), sink.curPrices.size());

	// 每次回调中的 last tick 与 day-price 字段必须来自回调本身的当前 tick。
	EXPECT_EQ(sink.tickDates, sink.lastTickDates);
	EXPECT_EQ(sink.tickActionTimes, sink.lastTickActionTimes);
	EXPECT_EQ(sink.tickOpens, sink.lastTickOpens);
	EXPECT_EQ(sink.tickHighs, sink.lastTickHighs);
	EXPECT_EQ(sink.tickLows, sink.lastTickLows);
	EXPECT_EQ(sink.tickPrices, sink.lastTickPrices);
	EXPECT_EQ(sink.tickOpens, sink.dayOpen);
	EXPECT_EQ(sink.tickHighs, sink.dayHigh);
	EXPECT_EQ(sink.tickLows, sink.dayLow);
	EXPECT_EQ(sink.tickPrices, sink.dayLast);
	// get_cur_price 是 update_price 写入的「已回放」价
	EXPECT_EQ(sink.tickPrices, sink.curPrices);
}

/*
 * loader 装载 1 分钟线并转储 dsb：
 * 第一次运行 cacheFinalBarsFromLoader 走 extloader 分支并写 his/min1/SSE/600000.dsb，
 * 第二次（新对象）走 isAutoTrans 的 dsb 回读分支，两次闭盘结果必须一致。
 */
TEST_F(P07TickLoaderTest, loaderBarsDumpDsbAndReRead)
{
	StubBtLoader loader;
	std::vector<double> firstCloses;
	std::vector<uint32_t> firstCloseDates;
	std::vector<uint32_t> firstTimes;
	std::vector<double> firstTicks;
	std::vector<uint32_t> firstTickDates;
	std::vector<uint32_t> firstTickTimes;
	std::vector<uint32_t> firstTickPxTypes;
	{
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, "");
		ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
		cfg->release();

		TickSink sink;
		sink.rp = &replayer;
		sink.probeCode = "SSE.STK.600000";
		replayer.register_sink(&sink, "p07_ld1");
		sink.onInit = [&replayer]() {
			WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
			if (main != NULL)
				main->release();
		};

		ASSERT_TRUE(replayer.prepare());
		EXPECT_EQ(1u, loader._finalBarCalls);
		replayer.run(false);

		// All nine bars close, each producing open/high/low/close ticks at that bar's timestamp.
		ASSERT_EQ(kM1Cnt, sink.closedPrices.size());
		std::vector<double> expectCloses;
		std::vector<uint32_t> expectCloseDates;
		std::vector<uint32_t> expectTimes;
		std::vector<double> expectTicks;
		std::vector<uint32_t> expectTickDates;
		std::vector<uint32_t> expectTickTimes;
		std::vector<uint32_t> expectTickPxTypes;
		for (uint32_t i = 0; i < kM1Cnt; i++)
		{
			const LoaderBar& bar = kM1[i];
			expectCloses.push_back(bar.close);
			expectCloseDates.push_back(bar.date);
			expectTimes.push_back(bar.hhmm);
			const double ohlc[] = { bar.open, bar.high, bar.low, bar.close };
			for (uint32_t pxType = 0; pxType < 4; pxType++)
			{
				expectTicks.push_back(ohlc[pxType]);
				expectTickDates.push_back(bar.date);
				expectTickTimes.push_back(bar.hhmm);
				expectTickPxTypes.push_back(pxType);
			}
		}
		EXPECT_EQ(expectCloses, sink.closedPrices);
		EXPECT_EQ(expectCloseDates, sink.closedDates);
		EXPECT_EQ(expectTimes, sink.closedTimes);
		EXPECT_EQ(expectTicks, sink.tickPrices);
		EXPECT_EQ(expectTickDates, sink.tickDates);
		EXPECT_EQ(expectTickTimes, sink.tickTimes);
		EXPECT_EQ(expectTickPxTypes, sink.tickPxTypes);
		ASSERT_EQ(kM1Cnt * 4, sink.tickPrices.size());
		for (uint32_t i = 0; i < kM1Cnt; i++)
		{
			const uint32_t closeTick = i * 4 + 3;
			EXPECT_EQ(sink.closedDates[i], sink.tickDates[closeTick]);
			EXPECT_EQ(sink.closedTimes[i], sink.tickTimes[closeTick]);
			EXPECT_DOUBLE_EQ(sink.closedPrices[i], sink.tickPrices[closeTick]);
		}
		firstCloses = sink.closedPrices;
		firstCloseDates = sink.closedDates;
		firstTimes = sink.closedTimes;
		firstTicks = sink.tickPrices;
		firstTickDates = sink.tickDates;
		firstTickTimes = sink.tickTimes;
		firstTickPxTypes = sink.tickPxTypes;
	}

	// 转储产物必须落在临时目录
	std::error_code ec;
	bool dsbFound = false;
	for (auto& entry : std::filesystem::recursive_directory_iterator(_base / "his", ec))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".dsb")
		{
			dsbFound = true;
			EXPECT_GT(std::filesystem::file_size(entry.path(), ec), (std::uintmax_t)0);
		}
	}
	ASSERT_TRUE(dsbFound) << "loader 自动转储没有产生 .dsb";

	{
		StubBtLoader second;
		HisDataReplayer replayer;
		WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, "");
		ASSERT_TRUE(replayer.init(cfg, NULL, &second));
		cfg->release();

		TickSink sink;
		replayer.register_sink(&sink, "p07_ld2");
		sink.onInit = [&replayer]() {
			WTSKlineSlice* main = replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
			if (main != NULL)
				main->release();
		};
		ASSERT_TRUE(replayer.prepare());
		replayer.run(false);

		// 命中 dsb 回读分支：不会再调用 extloader
		EXPECT_EQ(0u, second._finalBarCalls);
		EXPECT_EQ(firstCloses, sink.closedPrices);
		EXPECT_EQ(firstCloseDates, sink.closedDates);
		EXPECT_EQ(firstTimes, sink.closedTimes);
		EXPECT_EQ(firstTicks, sink.tickPrices);
		EXPECT_EQ(firstTickDates, sink.tickDates);
		EXPECT_EQ(firstTickTimes, sink.tickTimes);
		EXPECT_EQ(firstTickPxTypes, sink.tickPxTypes);
	}
}

/*
 * 关闭自动转储时，cacheFinalBarsFromLoader 每次都要走 extloader；
 * 装载器没有 5 分钟线 -> 返回 false -> get_kline_slice 直接返回 NULL。
 */
TEST_F(P07TickLoaderTest, loaderWithoutDataYieldsNullSlice)
{
	StubBtLoader loader;
	loader._autoTrans = false;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020931ull, 202001031500ull, false, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	TickSink sink;
	replayer.register_sink(&sink, "p07_nodata");
	ASSERT_TRUE(replayer.prepare());

	// m5：桩装载器只给 m1/d
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600000", "m", 5, 5, false));
	EXPECT_EQ(1u, loader._finalBarCalls);
	// m1 能拿到，且没有转储文件
	WTSKlineSlice* sli = replayer.get_kline_slice("SSE.STK.600000", "m", 3, 1, true);
	ASSERT_NE(nullptr, sli);
	EXPECT_EQ(1, sli->size());
	sli->release();
	// 目录会被 cacheFinalBarsFromLoader 预创建，但 isAutoTrans=false 时不能落 .dsb
	std::error_code ec;
	uint32_t dsbCnt = 0;
	for (auto& entry : std::filesystem::recursive_directory_iterator(_base / "his", ec))
	{
		if (entry.is_regular_file() && entry.path().extension() == ".dsb")
			dsbCnt++;
	}
	EXPECT_EQ(0u, dsbCnt);
}

/*
 * calc_fee 的有数据路径（p04 只做了空/未命中）：
 * 金额计费按 price*qty*volscale*rate 再套最低手续费，按量计费直接 rate*qty，
 * 结果统一四舍五入到 2 位小数。SSE.STK volscale=1（dist/common/stk_comms.json）。
 */
TEST_F(P07TickLoaderTest, calcFeeUsesLoadedFeeTemplate)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	std::string fees;
	ASSERT_TRUE(write_fee_template(_base, fees));
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, false, fees);
	// 卡13d：费用按可核验的实际合约计算，夹具提供三只合约元数据
	const std::filesystem::path contracts = _base / "p07_fee_contracts.json";
	std::ofstream(contracts) << "{\"SSE\":{\"600000\":{\"name\":\"pf\",\"code\":\"600000\",\"exchg\":\"SSE\",\"product\":\"STK\","
		"\"opendate\":20000101,\"expiredate\":20991231},"
		"\"000001\":{\"name\":\"idx\",\"code\":\"000001\",\"exchg\":\"SSE\",\"product\":\"IDX\","
		"\"opendate\":20000101,\"expiredate\":20991231}},"
		"\"SZSE\":{\"000001\":{\"name\":\"pa\",\"code\":\"000001\",\"exchg\":\"SZSE\",\"product\":\"STK\","
		"\"opendate\":20000101,\"expiredate\":20991231}}}";
	cfg->get("basefiles")->append("contract", contracts.string().c_str());
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();

	// 1000*0.0003 = 0.3 -> 低于最低 5 块
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 0));
	// 1000*0.0013 = 1.3 -> 仍然低于最低 5 块
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 1));
	EXPECT_DOUBLE_EQ(5.0, replayer.calc_fee("SSE.STK.600000", 10.0, 100.0, 2));
	// 抬高成交额让最低收费不起作用：200000*0.0003 = 60
	EXPECT_DOUBLE_EQ(60.0, replayer.calc_fee("SSE.STK.600000", 20.0, 10000.0, 0));
	// minfee=0 -> 直接按比例：10000*0.00025 = 2.5
	EXPECT_DOUBLE_EQ(2.5, replayer.calc_fee("SZSE.STK.000001", 20.0, 500.0, 0));
	// byvolume：open=2 -> 2*3 = 6
	EXPECT_DOUBLE_EQ(6.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 0));
	EXPECT_DOUBLE_EQ(9.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 1));
	EXPECT_DOUBLE_EQ(3.0, replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 2));
	// 卡13d“缺费报错”：offset 非法与费率表里没有的品种均返回可观察错误（NaN）
	// （两条旧断言期望 0.0，与已裁定卡13d冲突，改写；原文件见证据 before/）
	EXPECT_TRUE(std::isnan(replayer.calc_fee("SSE.IDX.000001", 10.0, 3.0, 9)));
	EXPECT_TRUE(std::isnan(replayer.calc_fee("DCE.IDX.999999", 10.0, 3.0, 0)));
}

/*
 * 崩溃/挂死风险统一收进这一个禁用用例，启用用例里零残留：
 * replayHftDatas(L1904) 的外层是 `for(;;)`，每轮都用同一个 `stime` 去算 nextTime，
 * 只有「本次确实回放了某笔数据」时游标才前进；
 * 如果订阅列表里存在 action_date 与 _cur_tdate 不一致的tick（数据源串日），
 * `lastTime <= nextTime` 恒不成立 -> 游标永不推进 -> 死循环（本波已有多条 HANG 同类）。
 * 这里不构造该输入，只做归档；启用用例全部走 run_by_ticks 的按日循环（有日期边界，可收敛）。
 */
TEST_F(P07TickLoaderTest, DISABLED_mismatchedTickDateHangsReplayHftDatas)
{
	StubBtLoader loader;
	HisDataReplayer replayer;
	WTSVariant* cfg = loader_cfg(_root, _base, 202001020930ull, 202001031500ull, true, "");
	ASSERT_TRUE(replayer.init(cfg, NULL, &loader));
	cfg->release();
	TickSink sink;
	replayer.register_sink(&sink, "p07_hang");
	replayer.sub_tick(11, "SSE.STK.600000");
	replayer.get_kline_slice("SSE.STK.600000", "m", 1, 1, true);
	ASSERT_TRUE(replayer.prepare());
	// 有主K线 + tick 模式 -> run_by_bars 内部调 replayHftDatas
	replayer.run(false);
}
