/**
 * r103_common.h  (lane R103 · wave18)
 *
 * 在只读复用 r91_common.h 的基础上补充本 lane 需要的夹具：
 *   - 可编程 Loader：final/raw K线、autoTrans、因子、抛异常臂
 *   - 带 bar_close / session_begin / section_end 钩子的 Sink
 *   - make_cfg 包装：可覆写 holiday、可追加 store 配置
 *
 * 所有落盘仍由 r91::Fixture 管理：仅 temp_directory_path()/r91_*，双向 remove_all。
 */
#pragma once

#include "r91_common.h"

#include <functional>

namespace r103
{

	class Loader : public r91::Loader
	{
	public:
		bool autoTrans { false };
		std::map<std::string, std::vector<WTSBarStruct>> dayBars;
		std::map<std::string, std::vector<WTSBarStruct>> m1Bars;
		std::map<std::string, std::vector<WTSBarStruct>> m5Bars;
		std::map<std::string, std::vector<WTSBarStruct>> rawBars;	// key: loadRawHisBars 收到的 stdCode
		std::map<std::string, uint32_t> finalCalls;
		std::map<std::string, uint32_t> rawCalls;
		std::map<std::string, bool> throwStd;
		std::map<std::string, bool> throwOther;

		const std::vector<WTSBarStruct>* pick(WTSKlinePeriod period, const char* stdCode) const
		{
			const std::map<std::string, std::vector<WTSBarStruct>>* src = &dayBars;
			if (period == KP_Minute1)
				src = &m1Bars;
			else if (period == KP_Minute5)
				src = &m5Bars;

			auto it = src->find(stdCode);
			return it == src->end() ? NULL : &it->second;
		}

		bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod period, FuncReadBars cb) override
		{
			finalCalls[stdCode]++;
			if (throwStd.find(stdCode) != throwStd.end())
				throw std::runtime_error("r103 loader failure");
			if (throwOther.find(stdCode) != throwOther.end())
				throw 103;

			const std::vector<WTSBarStruct>* bars = pick(period, stdCode);
			if (bars == NULL || bars->empty())
				return false;

			cb(obj, const_cast<WTSBarStruct*>(bars->data()), (uint32_t)bars->size());
			return true;
		}

		bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod, FuncReadBars cb) override
		{
			rawCalls[stdCode]++;
			auto it = rawBars.find(stdCode);
			if (it == rawBars.end() || it->second.empty())
				return false;

			cb(obj, it->second.data(), (uint32_t)it->second.size());
			return true;
		}

		bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }

		bool isAutoTrans() override { return autoTrans; }
	};

	class Sink : public r91::Sink
	{
	public:
		std::function<void(const r91::BarRec&)> onBarClose;
		std::function<void(uint32_t)> onSessionBegin;
		std::function<void(uint32_t, uint32_t)> onSectionEnd;
		std::function<void(const r91::TickRec&)> onTick;
		std::string dayPriceCode;	// 非空时只对该合约采集 get_day_price
		uint32_t sectionCalls { 0 };

		void handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType) override
		{
			const bool old = collectDayPrice;
			if (!dayPriceCode.empty() && dayPriceCode != stdCode)
				collectDayPrice = false;
			r91::Sink::handle_tick(stdCode, curTick, pxType);
			collectDayPrice = old;
			if (onTick && !ticks.empty())
				onTick(ticks.back());
		}

		void handle_bar_close(const char* stdCode, const char* period,
			uint32_t times, WTSBarStruct* newBar) override
		{
			r91::Sink::handle_bar_close(stdCode, period, times, newBar);
			if (onBarClose && !bars.empty())
				onBarClose(bars.back());
		}

		void handle_session_begin(uint32_t curTDate) override
		{
			r91::Sink::handle_session_begin(curTDate);
			if (onSessionBegin)
				onSessionBegin(curTDate);
		}

		void handle_section_end(uint32_t curTDate, uint32_t curTime) override
		{
			sectionCalls++;
			if (onSectionEnd)
				onSectionEnd(curTDate, curTime);
		}
	};

	inline WTSVariant* make_cfg(const std::filesystem::path& repo, const std::filesystem::path& base,
		uint64_t stime, uint64_t etime, const r91::CfgOpts& opts,
		const std::filesystem::path& holiday = std::filesystem::path(),
		const std::filesystem::path& store = std::filesystem::path())
	{
		WTSVariant* cfg = r91::make_cfg(repo, base, stime, etime, opts);
		WTSVariant* basefiles = cfg->get("basefiles");
		if (!holiday.empty())
			basefiles->append("holiday", holiday.string().c_str());

		if (!store.empty())
		{
			WTSVariant* storeCfg = WTSVariant::createObject();
			storeCfg->append("path", store.string().c_str());
			cfg->append("store", storeCfg);
			storeCfg->release();
		}
		return cfg;
	}

}	// namespace r103
