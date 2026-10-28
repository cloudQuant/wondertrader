/**
 * r131_common.h  (lane R131)
 *
 * 共享夹具：
 *   - r131::NullSink：满足 HisDataReplayer::prepare() 对 _listener 的非空要求，
 *     只记录 init / replay_done / schedule 次数，不做断言副作用；
 *   - r131::feed：用 r62::make_tick 构造真实 WTSTickData，喂给任意 Mocker 的
 *     公开 handle_tick，随后 release，避免泄漏；
 *   - r131::write_m5_csv：在临时目录写 <code>_m5.csv，供 get_kline_slice("m",5)
 *     的分钟级回放使用。
 *
 * 只读复用 p08/r62_mocker_common.h 的 ReplayFixture / make_tick / repo 探测；
 * 不 #define private public、不镜像类布局、不改宏。
 */
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <Includes/WTSDataDef.hpp>
#include <WtBtCore/HisDataReplayer.h>

#include "../p08_btcore_mockers3/r62_mocker_common.h"

namespace r131
{
	namespace fs = std::filesystem;

	class NullSink : public IDataSink
	{
	public:
		uint32_t init_calls { 0 };
		uint32_t replay_done_calls { 0 };
		uint32_t schedule_calls { 0 };

		void handle_tick(const char*, WTSTickData*, uint32_t) override {}
		void handle_bar_close(const char*, const char*, uint32_t, WTSBarStruct*) override {}
		void handle_schedule(uint32_t, uint32_t) override { schedule_calls++; }
		void handle_init() override { init_calls++; }
		void handle_session_begin(uint32_t) override {}
		void handle_session_end(uint32_t) override {}
		void handle_replay_done() override { replay_done_calls++; }
	};

	/**
	 * 构造并投递一笔 tick。pxType=1 时生产侧会用 _price_map 中的上一笔价格
	 * 作为 last_px（HisDataReplayer / MatchEngine 的模拟 tick 语义）。
	 */
	template <typename Mocker>
	inline void feed(Mocker& m, const char* stdCode, double last, double bid, double ask,
		uint32_t bidQty = 1, uint32_t askQty = 1, uint32_t pxType = 0,
		uint32_t uDate = 20200102u, uint32_t uTimeMs = 93100000u)
	{
		WTSTickData* tick = r62::make_tick(stdCode, uDate, uTimeMs, last, bid, ask, bidQty, askQty);
		m.handle_tick(stdCode, tick, pxType);
		tick->release();
	}

	// <base>/csv/<stdCode>_m5.csv（cacheRawBarsFromCSV 的 KP_Minute5 约定）
	inline bool write_m5_csv(const fs::path& base, const char* stdCode)
	{
		std::error_code ec;
		fs::create_directories(base / "csv", ec);

		std::ofstream out(base / "csv" / (std::string(stdCode) + "_m5.csv"));
		if (!out.is_open())
			return false;

		out << "date,time,open,high,low,close,volume,turnover,open_interest,diff_interest,settle\n";
		const uint32_t hhmm[5] = { 93100u, 93200u, 93500u, 93600u, 93700u };
		for (uint32_t i = 0; i < 5; i++)
		{
			const double close = 3800.0 + i;
			out << "20200102," << hhmm[i] << ","
				<< (close - 1.0) << "," << (close + 2.0) << ","
				<< (close - 2.0) << "," << close << ","
				<< (100 + i) << "," << (100 + i) * close << ",0,0,0\n";
		}
		out.close();
		return true;
	}
}	// namespace r131
