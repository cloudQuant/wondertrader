/**
 * r111_common.h  (lane R111)
 *
 * 共享夹具（只读复用 r62_mocker_common.h 的真实 HisDataReplayer + csv 日线）：
 *   - r111::exe_dir/bin_dir：从测试可执行文件位置推出 build_x64/.../bin
 *   - r111::lib_file：按平台拼插件文件名
 *   - r111::plugin_path：插件绝对路径（仅本地构建产物，不访问外网）
 *   - r111::TickLoader：纯内存 IBtDataLoader，只为 HftMocker 的撮合分支提供
 *     可编程的 tick（askqty/askprice/bidprice 可以精确置 0）
 *
 * 所有测试只使用公开 API；不使用 #define private public、不镜像类布局、不改宏。
 * 临时文件只写系统临时目录（r62 fixture 负责创建与清理）。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <WtBtCore/HisDataReplayer.h>

#include "r62_mocker_common.h"

namespace r111
{
	namespace fs = std::filesystem;

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

	// 测试二进制位于 <bin>/TestUnits/，插件在 <bin>/ 或 <bin>/<子目录>/
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
		return std::string("lib") + module + ".so";
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

	/**
	 * 纯内存 tick 装载器：key = stdCode，一次只放一个交易日的 tick。
	 * isAutoTrans=false，避免测试往 src 侧写 dsb。
	 */
	class TickLoader : public IBtDataLoader
	{
	public:
		std::map<std::string, WTSTickStruct> ticks;

		void add(const char* stdCode, uint32_t uDate, uint32_t uTimeMs,
			double last, double bid, double ask, uint32_t bidQty, uint32_t askQty)
		{
			WTSTickStruct ts;
			memset(&ts, 0, sizeof(ts));
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
				ts.bid_prices[i] = bid;
				ts.ask_prices[i] = ask;
				ts.bid_qty[i] = bidQty;
				ts.ask_qty[i] = askQty;
			}
			ticks[stdCode] = ts;
		}

		bool loadFinalHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override
		{
			return false;
		}

		bool loadRawHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override
		{
			return false;
		}

		bool loadAllAdjFactors(void*, FuncReadFactors) override
		{
			return false;
		}

		bool loadAdjFactors(void*, const char*, FuncReadFactors) override
		{
			return false;
		}

		bool loadRawHisTicks(void* obj, const char* stdCode, uint32_t uDate, FuncReadTicks cb) override
		{
			auto it = ticks.find(stdCode);
			if (it == ticks.end() || it->second.action_date != uDate)
				return false;

			cb(obj, &it->second, 1);
			return true;
		}

		bool isAutoTrans() override { return false; }
	};

	// tick=true 的回放配置（loader 由调用方传给 init）
	inline WTSVariant* make_tick_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", "csv");
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", true);
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
}
