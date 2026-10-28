/**
 * r180_common.h  (lane R180 · wave26)
 *
 * 只读复用 p03/r161_common.h 与 p03/r72_common.h 的仓库探测、临时目录、
 * csv 回放配置、NullSink、Loader、tick/bar 构造；本文件只新增：
 *   - r180::make_exec_cfg：ExecMocker 的公开初始化配置（module 指向本机构建
 *     产出的 libWtExeFact，executer=WtTWapExeUnit，参数保证 total_times>0）；
 *   - r180::make_m5_bars / make_daily_bars：确定性 K 线；
 *   - r180::line_count：数落盘文件行数。
 *
 * 不使用 #define private public、不镜像类布局、不改宏；临时文件只落系统临时目录。
 */
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "r161_common.h"
#include "r72_common.h"

namespace r180
{
	namespace fs = std::filesystem;

	inline std::string exec_module()
	{
		return r141::plugin_path("WtExeFact");
	}

	// r72 csv 回放配置 + ExecMocker 所需字段。module 必须能在本机 dlopen，
	// executer.name 必须是 WtExeFact 中真实注册的单元名，否则 createExeUnit
	// 返回 NULL，init 内部会解引用空指针。
	inline WTSVariant* make_exec_cfg(const fs::path& root, const fs::path& base,
		const char* stdCode, int32_t volmode, const char* execId)
	{
		WTSVariant* cfg = r72::make_cfg(root, base, 202001020930ull, 202001021500ull, true);
		cfg->append("module", exec_module().c_str());
		cfg->append("code", stdCode);
		cfg->append("period", "m1");
		cfg->append("volunit", 1.0);
		cfg->append("volmode", volmode);

		WTSVariant* params = WTSVariant::createObject();
		params->append("ord_sticky", static_cast<uint32_t>(0));
		params->append("begin_time", static_cast<uint32_t>(930));
		params->append("end_time", static_cast<uint32_t>(1500));
		params->append("total_secs", static_cast<uint32_t>(3600));
		params->append("tail_secs", static_cast<uint32_t>(0));
		params->append("total_times", static_cast<uint32_t>(4));
		params->append("price_mode", static_cast<uint32_t>(0));
		params->append("price_offset", static_cast<uint32_t>(0));
		params->append("lots", 1.0);

		WTSVariant* exec = WTSVariant::createObject();
		exec->append("id", execId);
		exec->append("name", "WtTWapExeUnit");
		exec->append("params", params);
		params->release();

		cfg->append("executer", exec);
		exec->release();
		return cfg;
	}

	inline std::vector<WTSBarStruct> make_m5_bars(uint32_t date, int count, double basePrice)
	{
		std::vector<WTSBarStruct> bars;
		int minutes = 9 * 60 + 35;
		for (int i = 0; i < count; i++)
		{
			const uint32_t hhmm = static_cast<uint32_t>((minutes / 60) * 100 + minutes % 60);
			bars.push_back(r72::make_bar(date, hhmm, basePrice + i * 0.1));
			minutes += 5;
		}
		return bars;
	}

	inline std::vector<WTSBarStruct> make_daily_bars(double basePrice)
	{
		const uint32_t dates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
		std::vector<WTSBarStruct> bars;
		for (uint32_t i = 0; i < 5; i++)
			bars.push_back(r72::make_bar(dates[i], 150000u, basePrice + i * 0.5));
		return bars;
	}

	inline std::size_t line_count(const std::string& text)
	{
		return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
	}
}	// namespace r180
