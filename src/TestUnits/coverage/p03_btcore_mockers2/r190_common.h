/**
 * r190_common.h  (lane R190 · wave27)
 *
 * 只读复用 p03 既有夹具：
 *   - r72::Fixture / Loader / Sink / make_cfg / make_bar / make_tick
 *   - r141::make_cmp_bar_block / make_text
 * 本文件只补三个确定性构造函数，不修改任何生产代码，不使用 private 注入。
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <WtBtCore/HisDataReplayer.h>
#include <WtBtCore/WtHelper.h>

#include "r141_common.h"
#include "r72_common.h"

namespace r190
{
	namespace fs = std::filesystem;

	inline WTSBarStruct make_daily(uint32_t date, double price)
	{
		return r72::make_bar(date, 150000u, price);
	}

	inline WTSBarStruct make_minute(uint32_t date, uint32_t hhmm, double price)
	{
		return r72::make_bar(date, hhmm, price);
	}

	// 新版压缩日线块，落到任意 <base>/his/day/<exchg>/<file>.dsb
	inline bool write_cmp_day_dsb(const fs::path& file, uint32_t count, double basePrice)
	{
		return r141::write_text(file, r141::make_cmp_bar_block(count, basePrice));
	}
}	// namespace r190
