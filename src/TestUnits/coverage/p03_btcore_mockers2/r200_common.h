/**
 * r200_common.h  (lane R200 · wave28)
 *
 * 只读复用 p03 既有夹具（r72::Fixture / Loader / Sink / make_cfg / make_bar /
 * make_tick）。本文件只补充：
 *   - make_daily / make_minute：确定性的日线、分钟线；
 *   - make_csv_cfg：可指定 cache_clear_days 的 csv 配置；
 *   - make_bin_cfg：bin 模式配置，可挂自定义规则文件；
 *   - write_rules：写一份最小可用规则 JSON。
 *
 * 不修改生产代码，不使用 private 注入；临时文件只落 r72 的系统临时目录。
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <WtBtCore/HisDataReplayer.h>

#include "r72_common.h"

namespace r200
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

	inline WTSVariant* make_base_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick, const char* mode,
		uint32_t cacheClearDays)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("mode", mode);
		cfg->append("path", base.string().c_str());
		cfg->append("stime", stime);
		cfg->append("etime", etime);
		cfg->append("tick", tick);
		cfg->append("adjust_flag", static_cast<uint32_t>(0));
		cfg->append("align_by_section", false);
		cfg->append("dont_simtick_if_notrade", false);
		cfg->append("cache_clear_days", cacheClearDays);
		cfg->append("fees", "");

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

	inline WTSVariant* make_csv_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick, uint32_t cacheClearDays = 10)
	{
		return make_base_cfg(root, base, stime, etime, tick, "csv", cacheClearDays);
	}

	inline WTSVariant* make_bin_cfg(const fs::path& root, const fs::path& base,
		uint64_t stime, uint64_t etime, bool tick, const fs::path& rulesFile)
	{
		WTSVariant* cfg = make_base_cfg(root, base, stime, etime, tick, "bin", 10);
		WTSVariant* rules = WTSVariant::createObject();
		rules->append("HOT", rulesFile.string().c_str());
		cfg->get("basefiles")->append("rules", rules);
		rules->release();
		return cfg;
	}

	inline bool write_rules(const fs::path& file)
	{
		return r72::write_text(file,
			"{\"SHFE\":{\"rb\":[{\"date\":20200102,\"from\":\"\","
			"\"to\":\"rb2406\",\"oldclose\":0,\"newclose\":1}]}}");
	}
}	// namespace r200
