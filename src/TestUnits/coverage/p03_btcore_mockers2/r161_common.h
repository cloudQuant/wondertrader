/**
 * r161_common.h  (lane R161 · wave24)
 *
 * 共享夹具（只读复用 p03/r141_common.h 的仓库探测、csv 回放配置、块构造、
 * NullSink 与 make_tick/make_bar；不修改任何生产代码）：
 *   - r161::write_daily_csv：任意合约的 5 根日线 CSV（<base>/csv/<code>_d.csv）；
 *   - r161::write_tick_dsb：20200102 的老版本 tick 块（09:31/09:32 两笔）；
 *   - r161::read_text：读回落盘文件做断言；
 *   - r161::ReplayFixture：tick=true、stime=202001020932 的 csv 回放器，
 *     已 register_sink + prepare，临时目录全部落系统临时目录。
 *
 * 不使用 #define private public、不镜像类布局、不改宏。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "r141_common.h"

namespace r161
{
	namespace fs = std::filesystem;

	inline bool write_daily_csv(const fs::path& base, const char* stdCode, double basePrice)
	{
		std::error_code ec;
		fs::create_directories(base / "csv", ec);

		std::ofstream out(base / "csv" / (std::string(stdCode) + "_d.csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		const uint32_t dates[5] = { 20200102u, 20200103u, 20200106u, 20200107u, 20200108u };
		for (uint32_t i = 0; i < 5; i++)
		{
			const double close = basePrice + i * 0.5;
			out << dates[i] << ",150000,"
				<< (close - 0.2) << "," << (close + 0.4) << ","
				<< (close - 0.5) << "," << close << ","
				<< (1000 + i) << "," << (1000 + i) * close << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	// <base>/bin/ticks/<stdCode>_tick_20200102.dsb，09:31 价 basePrice、09:32 价 basePrice+1
	inline bool write_tick_dsb(const fs::path& base, const char* stdCode, double basePrice)
	{
		const fs::path file = base / "bin" / "ticks" /
			(std::string(stdCode) + "_tick_20200102.dsb");
		return r141::write_text(file, r141::make_old_tick_block(2, basePrice));
	}

	inline std::string read_text(const fs::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		if (!in.is_open())
			return std::string();
		std::stringstream ss;
		ss << in.rdbuf();
		return ss.str();
	}

	class ReplayFixture : public ::testing::Test
	{
	protected:
		HisDataReplayer _rp;
		r141::NullSink _sink;
		fs::path _root;
		fs::path _base;
		fs::path _out;

		void SetUp() override
		{
			_root = r141::repo_root();
			ASSERT_FALSE(_root.empty()) << "cannot locate repo root containing dist/common/sessions.json";

			_base = r141::temp_dir("r161_base");
			_out = r141::temp_dir("r161_out");
			WtHelper::setOutputDir(_out.string().c_str());

			ASSERT_TRUE(write_daily_csv(_base, "SHFE.rb.2405", 3800.0));
			ASSERT_TRUE(write_daily_csv(_base, "SHFE.cu.2406", 70000.0));
			ASSERT_TRUE(write_daily_csv(_base, "SSE.STK.000001", 10.0));
			ASSERT_TRUE(write_daily_csv(_base, "SZSE.STK.000002", 20.0));
			ASSERT_TRUE(write_daily_csv(_base, "DCE.i.2409", 800.0));
			ASSERT_TRUE(write_daily_csv(_base, "DCE.j.2409", 900.0));

			ASSERT_TRUE(write_tick_dsb(_base, "SHFE.rb.2405", 3800.0));
			ASSERT_TRUE(write_tick_dsb(_base, "SSE.STK.000001", 10.0));
			ASSERT_TRUE(write_tick_dsb(_base, "SZSE.STK.000002", 20.0));
			ASSERT_TRUE(write_tick_dsb(_base, "DCE.i.2409", 800.0));
			ASSERT_TRUE(write_tick_dsb(_base, "DCE.j.2409", 900.0));

			WTSVariant* cfg = r141::make_cfg(_root, _base,
				202001020932ull, 202001031500ull, true);
			ASSERT_TRUE(_rp.init(cfg));
			cfg->release();

			_rp.register_sink(&_sink, "r161");
			ASSERT_TRUE(_rp.prepare());
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			std::error_code ec;
			fs::remove_all(_base, ec);
			fs::remove_all(_out, ec);
		}
	};
}	// namespace r161
