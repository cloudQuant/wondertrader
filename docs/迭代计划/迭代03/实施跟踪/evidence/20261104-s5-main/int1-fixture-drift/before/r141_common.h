/**
 * r141_common.h  (lane R141 · wave22)
 *
 * 共享夹具（只写本 lane，未修改任何生产代码）：
 *   - r141::repo_root / temp_dir：仓库根探测与系统临时目录；
 *   - r141::make_cfg：csv 模式回放配置（basefiles 只读 dist/common）；
 *   - r141::plugin_path：本机构建产物 dylib 的绝对路径（不联网）；
 *   - r141::NullSink：prepare() 所需的最小 IDataSink；
 *   - r141::make_tick / make_bar：确定性内存行情；
 *   - 块数据构造：老版本 tick/bar（BLOCK_VERSION_RAW）与新版压缩 bar
 *     （BLOCK_VERSION_CMP_V2），用于驱动 HisDataReplayer::proc_block_data。
 *
 * 不使用 #define private public、不镜像类布局；临时文件只落系统临时目录。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCmpHelper.hpp>
#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

namespace r141
{
	namespace fs = std::filesystem;

	inline fs::path probe_root(fs::path p)
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
		fs::path root = probe_root(fs::path(__FILE__).parent_path());
		if (!root.empty())
			return root;

		return probe_root(fs::current_path());
	}

	inline fs::path temp_dir(const char* tag)
	{
		auto path = fs::temp_directory_path() /
			("r141_" + std::string(tag) + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		fs::remove_all(path, ec);
		fs::create_directories(path, ec);
		return path;
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

	inline std::string exe_dir()
	{
#ifdef __APPLE__
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::vector<char> buf(size + 1, 0);
		_NSGetExecutablePath(buf.data(), &size);
		std::error_code ec;
		fs::path exe = fs::weakly_canonical(fs::path(buf.data()), ec);
		if (ec)
			exe = fs::path(buf.data());
		return exe.parent_path().string();
#else
		char buf[4096] = { 0 };
		ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		if (len <= 0)
			return std::string();
		std::error_code ec;
		fs::path exe = fs::weakly_canonical(fs::path(std::string(buf, (std::size_t)len)), ec);
		if (ec)
			exe = fs::path(std::string(buf, (std::size_t)len));
		return exe.parent_path().string();
#endif
	}

	inline std::string bin_dir()
	{
		fs::path dir(exe_dir());
		if (dir.filename() == "TestUnits")
			return dir.parent_path().string();
		return dir.string();
	}

	inline std::string lib_file(const char* module)
	{
#ifdef __APPLE__
		return std::string("lib") + module + ".dylib";
#elif defined(_WIN32)
		return std::string(module) + ".dll";
#else
		return std::string(module) + ".so";
#endif
	}

	inline std::string plugin_path(const char* module, const char* subdir = nullptr)
	{
		fs::path p(bin_dir());
		if (subdir != nullptr && subdir[0] != '\0')
			p /= subdir;
		p /= lib_file(module);
		return p.string();
	}

	class NullSink : public IDataSink
	{
	public:
		uint32_t initCalls { 0 };
		uint32_t beginCalls { 0 };
		uint32_t endCalls { 0 };
		uint32_t doneCalls { 0 };
		uint32_t scheduleCalls { 0 };

		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override { scheduleCalls++; }
		void handle_init() override { initCalls++; }
		void handle_session_begin(uint32_t) override { beginCalls++; }
		void handle_session_end(uint32_t) override { endCalls++; }
		void handle_replay_done() override { doneCalls++; }
	};

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

	inline bool write_text(const fs::path& p, const std::string& content)
	{
		std::error_code ec;
		fs::create_directories(p.parent_path(), ec);
		FILE* fp = fopen(p.string().c_str(), "wb");
		if (fp == NULL)
			return false;
		bool ok = fwrite(content.data(), 1, content.size(), fp) == content.size();
		fclose(fp);
		return ok;
	}

	// 老版本 tick 块：BlockHeader(BLOCK_VERSION_RAW) + WTSTickStructOld[count]
	inline std::string make_old_tick_block(uint32_t count, double basePrice)
	{
		std::string content(sizeof(BlockHeader) + count * sizeof(WTSTickStructOld), '\0');
		BlockHeader* hdr = (BlockHeader*)&content[0];
		memset(hdr, 0, sizeof(*hdr));
		memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
		hdr->_type = BT_HIS_Ticks;
		hdr->_version = BLOCK_VERSION_RAW;

		WTSTickStructOld* ticks = (WTSTickStructOld*)(content.data() + sizeof(BlockHeader));
		for (uint32_t i = 0; i < count; i++)
		{
			memset(&ticks[i], 0, sizeof(WTSTickStructOld));
			snprintf(ticks[i].exchg, sizeof(ticks[i].exchg), "%s", "SHFE");
			snprintf(ticks[i].code, sizeof(ticks[i].code), "%s", "2405");
			ticks[i].action_date = 20200102u;
			ticks[i].trading_date = 20200102u;
			ticks[i].action_time = 93100000u + i * 30000u;
			ticks[i].price = basePrice + i;
			ticks[i].open = basePrice;
			ticks[i].high = basePrice + 2;
			ticks[i].low = basePrice - 2;
			ticks[i].pre_close = basePrice;
			ticks[i].volume = 10 + i;
			ticks[i].total_volume = 100 + i;
			for (int32_t k = 0; k < 10; k++)
			{
				ticks[i].bid_prices[k] = basePrice - 1 - k;
				ticks[i].ask_prices[k] = basePrice + 1 + k;
				ticks[i].bid_qty[k] = 50;
				ticks[i].ask_qty[k] = 50;
			}
		}
		return content;
	}

	// 老版本 bar 块：BlockHeader(BLOCK_VERSION_RAW) + WTSBarStructOld[count]
	inline std::string make_old_bar_block(uint32_t count, double basePrice)
	{
		std::string content(sizeof(BlockHeader) + count * sizeof(WTSBarStructOld), '\0');
		BlockHeader* hdr = (BlockHeader*)&content[0];
		memset(hdr, 0, sizeof(*hdr));
		memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
		hdr->_type = BT_HIS_Day;
		hdr->_version = BLOCK_VERSION_RAW;

		WTSBarStructOld* bars = (WTSBarStructOld*)(content.data() + sizeof(BlockHeader));
		for (uint32_t i = 0; i < count; i++)
		{
			memset(&bars[i], 0, sizeof(WTSBarStructOld));
			bars[i].date = 20200102u + i;
			bars[i].time = 150000u;
			bars[i].open = basePrice + i;
			bars[i].high = basePrice + i + 1;
			bars[i].low = basePrice + i - 1;
			bars[i].close = basePrice + i + 0.5;
			bars[i].settle = basePrice + i;
			bars[i].money = 1000.0 + i;
			bars[i].vol = 100 + i;
			bars[i].hold = 200 + i;
			bars[i].add = (int32_t)i;
		}
		return content;
	}

	// 新版压缩 bar 块：BlockHeaderV2(BLOCK_VERSION_CMP_V2) + 压缩的 WTSBarStruct[count]
	inline std::string make_cmp_bar_block(uint32_t count, double basePrice)
	{
		std::string payload(count * sizeof(WTSBarStruct), '\0');
		WTSBarStruct* bars = (WTSBarStruct*)&payload[0];
		for (uint32_t i = 0; i < count; i++)
		{
			memset(&bars[i], 0, sizeof(WTSBarStruct));
			bars[i].date = 20200102u + i;
			bars[i].time = 150000u;
			bars[i].open = basePrice + i;
			bars[i].high = basePrice + i + 1;
			bars[i].low = basePrice + i - 1;
			bars[i].close = basePrice + i + 0.25;
			bars[i].settle = basePrice + i;
			bars[i].money = 2000.0 + i;
			bars[i].vol = 200 + i;
			bars[i].hold = 300 + i;
		}

		std::string cmp = WTSCmpHelper::compress_data(payload.data(), payload.size());
		std::string content(sizeof(BlockHeaderV2), '\0');
		BlockHeaderV2* hdr = (BlockHeaderV2*)&content[0];
		memset(hdr, 0, sizeof(*hdr));
		memcpy(hdr->_blk_flag, BLK_FLAG, sizeof(hdr->_blk_flag));
		hdr->_type = BT_HIS_Day;
		hdr->_version = BLOCK_VERSION_CMP_V2;
		hdr->_size = cmp.size();
		content.append(cmp);
		return content;
	}
}
