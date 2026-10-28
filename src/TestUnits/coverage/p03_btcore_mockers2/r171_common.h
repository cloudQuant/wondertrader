/**
 * r171_common.h  (lane R171 · wave25)
 *
 * 只读复用 p03/r161_common.h 与 p03/r141_common.h 的仓库探测、csv 回放配置、
 * NullSink、tick/bar 构造与读写文件工具；本文件只新增：
 *   - r171::line_count：数落盘文件行数（按 '\n'）；
 *   - r171::make_cross_date_tick_block：同一条老版本 tick 块里放两个交易日，
 *     用于驱动 get_last_tick 的跨日比较分支；
 *   - r171::DeferredFixture：tick=false 的 csv 回放器，SetUp 只 init 不 prepare，
 *     便于先通过 get_kline_slice(isMain=true) 写好 _main_period，再 prepare 触发
 *     checkUnbars。
 *
 * 不使用 #define private public、不镜像类布局、不改宏；临时文件只落系统临时目录。
 */
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>

#include "r161_common.h"

namespace r171
{
	namespace fs = std::filesystem;

	inline std::size_t line_count(const std::string& text)
	{
		return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
	}

	inline std::string read_text(const fs::path& p)
	{
		return r161::read_text(p);
	}

	// <base>/csv/<stdCode>_m5.csv，三根 20200102 的 5 分钟线（09:35/09:40/09:45）。
	// 用于 get_kline_slice 的 m5 重采样缓存命中，再构造 times=0 的失败臂。
	inline bool write_m5_csv(const fs::path& base, const char* stdCode, double basePrice)
	{
		std::error_code ec;
		fs::create_directories(base / "csv", ec);

		std::ofstream out(base / "csv" / (std::string(stdCode) + "_m5.csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		const char* times[3] = { "093500", "094000", "094500" };
		for (uint32_t i = 0; i < 3; i++)
		{
			const double close = basePrice + i * 0.1;
			out << "20200102," << times[i] << ","
				<< (close - 0.05) << "," << (close + 0.1) << ","
				<< (close - 0.1) << "," << close << ","
				<< (100 + i) << "," << (100 + i) * close << ",0,0,0\n";
		}
		out.close();
		return true;
	}

	// r141::make_old_tick_block 构造的两笔 tick 都在 20200102；
	// 这里把第二笔改到 secondDate，制造跨交易日块。
	inline std::string make_cross_date_tick_block(uint32_t secondDate, double basePrice)
	{
		std::string content = r141::make_old_tick_block(2, basePrice);
		WTSTickStructOld* ticks = (WTSTickStructOld*)(content.data() + sizeof(BlockHeader));
		ticks[1].action_date = secondDate;
		ticks[1].trading_date = secondDate;
		ticks[1].action_time = 93100000u;
		return content;
	}

	// tick=false 的 csv 回放夹具：SetUp 不 prepare，测试可以先把主 K 线周期写进
	// _main_period（get_kline_slice 的 isMain=true 分支），再显式 prepare。
	class DeferredFixture : public ::testing::Test
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

			_base = r141::temp_dir("r171_base");
			_out = r141::temp_dir("r171_out");
			WtHelper::setOutputDir(_out.string().c_str());

			ASSERT_TRUE(r161::write_daily_csv(_base, "SSE.STK.000001", 10.0));

			WTSVariant* cfg = r141::make_cfg(_root, _base,
				202001020932ull, 202001031500ull, false);
			ASSERT_TRUE(_rp.init(cfg));
			cfg->release();

			_rp.register_sink(&_sink, "r171");
		}

		void TearDown() override
		{
			WtHelper::setOutputDir("");
			std::error_code ec;
			fs::remove_all(_base, ec);
			fs::remove_all(_out, ec);
		}
	};
}	// namespace r171
