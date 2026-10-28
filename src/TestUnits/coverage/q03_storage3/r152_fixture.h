// r152 / 主存储补测共用夹具（非用例 TU）
//
// 只做三件事，全部走公开接口：
//  1) SectionHotMgr：按用例显式给出的分段表驱动 WtDataReader::cacheIntegratedBars
//     与 WtRdmDtReader 的 HOT 分支；getRuleTag 对 "HOT" 前缀返回 "HOT"。
//  2) ThrowingSplitHotMgr / ThrowingRawLoader：在 splitCustomSections / loadRawHisBars
//     里抛一个**非 std::exception**（int），用于命中原生的 `catch(...)` 分支；
//     这两个成员都是公开虚接口，没有访问权限宏，也没有类布局镜像。
//  3) flatten 小工具：把 WTSKlineSlice 摊平成 vector<WTSBarStruct> 方便逐字段断言。
#pragma once

#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtDataReader.h"

namespace r152 {

using std::string;
using std::vector;

class SectionHotMgr : public q03::HotMgr
{
public:
	void set_tag(const char* tag) { _tag = tag == NULL ? "" : tag; }
	void clear_sections() { _secs.clear(); }
	void add_section(const char* code, uint32_t sdate, uint32_t edate, double factor = 1.0)
	{
		_secs.emplace_back(code, sdate, edate, factor);
	}
	uint32_t split_calls() const { return _split_calls; }

	const char* getRuleTag(const char* ext) override
	{
		if (_tag.empty() || ext == NULL)
			return "";
		return strncmp(ext, _tag.c_str(), _tag.size()) == 0 ? _tag.c_str() : "";
	}

	bool splitCustomSections(const char* tag, const char*, uint32_t, uint32_t, HotSections& out) override
	{
		_split_calls++;
		if (_tag.empty() || tag == NULL || _tag != tag || _secs.empty())
			return false;
		out.insert(out.end(), _secs.begin(), _secs.end());
		return true;
	}

private:
	string		_tag;
	HotSections	_secs;
	uint32_t	_split_calls = 0;
};

// 分段接口抛非 std 异常：验证 cacheHisBarsFromFile 的 catch(...) 而不依赖文件系统错误
class ThrowingSplitHotMgr : public q03::HotMgr
{
public:
	const char* getRuleTag(const char*) override { return "HOT"; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override
	{
		throw 152;
	}
};

// 原始 K 线加载接口抛非 std 异常：驱动 cacheAdjustedStkBars 调用点的 catch(...)
class ThrowingRawLoader : public IHisDataLoader
{
public:
	bool loadFinalHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
	bool loadRawHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { throw 152; }
	bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }
	bool loadAdjFactors(void*, const char*, FuncReadFactors) override { return false; }
};

inline vector<WTSBarStruct> flatten_bars(WTSKlineSlice* slice)
{
	vector<WTSBarStruct> ret;
	if (slice == NULL)
		return ret;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
	{
		WTSBarStruct* addr = slice->get_block_addr(i);
		uint32_t cnt = slice->get_block_size(i);
		if (addr == NULL)
			continue;
		for (uint32_t k = 0; k < cnt; k++)
			ret.push_back(addr[k]);
	}
	return ret;
}

inline uint64_t min_bar_time(uint32_t date, uint32_t hhmm)
{
	return TimeUtils::timeToMinBar(date, hhmm);
}

} // namespace r152
