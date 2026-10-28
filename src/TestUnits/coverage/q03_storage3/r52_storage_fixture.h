// r52 / WtDataStorage 补测车道共用夹具（非用例 TU）
//
// 在 q03_storage3_fixture.h 之上补三块能力：
//  1) HotMgr    —— IHotMgr 替身：可配置规则标签 / 分月切段 / 当前主力代码，
//                  让 CodeHelper::extractStdCode 走进 _ruletag 分支（主力连续），
//                  再让 splitCustomSections 返回受控切段（WtDataReader/WtRdmDtReader 的 HOT 大块）。
//  2) BDMgr     —— q03::BaseDataMgr 的派生：把 getBoundaryTime 修正成生产语义
//                  `日期*10000 + HHMM`（q03 原实现返回 `日期*1e9 + HHMM`，只够 tile 读，不够分钟线边界）。
//  3) Loader    —— IHisDataLoader 替身（按 stdCode 返回 final/raw bars 与除权因子）。
//  外加：带 on_bar 计数的 ReaderSink、切片摊平、RT 块容量原地改写（重映射分支）。
//
// 仍然没有任何访问权限宏；所有路径都在 q03::Sandbox 临时目录内。
#pragma once

#include <cstddef>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "q03_storage3_fixture.h"

#include "Includes/IDataReader.h"
#include "Includes/IHotMgr.h"

namespace r52 {

using std::string;
using std::vector;

// ---------------------------------------------------------------- 主力连续替身
class HotMgr : public q03::HotMgr
{
public:
	void set_tag(const char* tag) { _tag = tag == NULL ? "" : tag; }
	void set_raw_code(const char* code) { _raw = code == NULL ? "" : code; }
	void add_section(const char* code, uint32_t sdate, uint32_t edate, double factor)
	{
		_secs.emplace_back(code, sdate, edate, factor);
	}
	void clear_sections() { _secs.clear(); }
	uint32_t split_calls() const { return _split_calls; }

	// "HOT" / "HOT+" 都返回 "HOT"；普通股票/分月代码返回空
	const char* getRuleTag(const char* stdCode) override
	{
		if (_tag.empty() || stdCode == NULL)
			return "";
		return strncmp(stdCode, _tag.c_str(), _tag.size()) == 0 ? _tag.c_str() : "";
	}

	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return _raw.c_str(); }

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
	string		_raw;
	HotSections	_secs;
	uint32_t	_split_calls = 0;
};

// 生产语义的边界时间：日期*10000 + HHMM
class BDMgr : public q03::BaseDataMgr
{
public:
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return (uint64_t)tDate * 10000 + (isStart ? 900 : 1500);
	}
};

// on_bar 计数版 sink（q03::ReaderSink 的 on_bar 是虚函数，可继续覆写）
class ReaderSink : public q03::ReaderSink
{
public:
	ReaderSink(IBaseDataMgr* bd, IHotMgr* hot) : q03::ReaderSink(bd, hot) {}

	void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* bar) override
	{
		_bar_cnt++;
		_last_code = stdCode == NULL ? "" : stdCode;
		_last_period = period;
		if (bar != NULL)
			_last_bar = *bar;
	}

	uint32_t bar_count() const { return _bar_cnt; }
	const string& last_code() const { return _last_code; }
	WTSKlinePeriod last_period() const { return _last_period; }
	const WTSBarStruct& last_bar() const { return _last_bar; }

private:
	uint32_t		_bar_cnt = 0;
	string			_last_code;
	WTSKlinePeriod	_last_period = KP_DAY;
	WTSBarStruct	_last_bar = WTSBarStruct();
};

// ---------------------------------------------------------------- 历史数据加载器替身
class Loader : public IHisDataLoader
{
public:
	void add_final(const char* code, const vector<WTSBarStruct>& bars) { _final[code] = bars; }
	void add_raw(const char* code, const vector<WTSBarStruct>& bars) { _raw[code] = bars; }
	void add_all_factors(const char* code, const vector<uint32_t>& dates, const vector<double>& factors,
		bool ret = true)
	{
		_all[code] = std::make_pair(dates, factors);
		_all_ret = ret;
	}
	void add_adj_factors(const char* code, const vector<uint32_t>& dates, const vector<double>& factors)
	{
		_adj[code] = std::make_pair(dates, factors);
	}

	bool loadFinalHisBars(void* obj, const char* stdCode, WTSKlinePeriod, FuncReadBars cb) override
	{
		auto it = _final.find(stdCode);
		if (it == _final.end())
			return false;
		cb(obj, const_cast<WTSBarStruct*>(it->second.data()), (uint32_t)it->second.size());
		return true;
	}

	bool loadRawHisBars(void* obj, const char* stdCode, WTSKlinePeriod, FuncReadBars cb) override
	{
		auto it = _raw.find(stdCode);
		if (it == _raw.end())
			return false;
		cb(obj, const_cast<WTSBarStruct*>(it->second.data()), (uint32_t)it->second.size());
		return true;
	}

	bool loadAllAdjFactors(void* obj, FuncReadFactors cb) override
	{
		for (auto& item : _all)
		{
			const string& code = item.first;
			vector<uint32_t>& dates = item.second.first;
			vector<double>& factors = item.second.second;
			cb(obj, code.c_str(), dates.data(), factors.data(), (uint32_t)dates.size());
		}
		return _all_ret;
	}

	bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
	{
		auto it = _adj.find(stdCode);
		if (it == _adj.end())
			return false;
		cb(obj, stdCode, it->second.first.data(), it->second.second.data(), (uint32_t)it->second.first.size());
		return true;
	}

private:
	std::map<string, vector<WTSBarStruct> >			_final;
	std::map<string, vector<WTSBarStruct> >			_raw;
	std::map<string, std::pair<vector<uint32_t>, vector<double> > >	_all;
	std::map<string, std::pair<vector<uint32_t>, vector<double> > >	_adj;
	bool	_all_ret = false;
};

// ---------------------------------------------------------------- 小工具
inline uint64_t min_bar_time(uint32_t date, uint32_t hhmm)
{
	return TimeUtils::timeToMinBar(date, hhmm);
}

inline vector<WTSTickStruct> flatten_ticks(WTSTickSlice* slice)
{
	vector<WTSTickStruct> ret;
	if (slice == NULL)
		return ret;
	for (std::size_t i = 0; i < slice->get_block_counts(); i++)
	{
		WTSTickStruct* addr = slice->get_block_addr(i);
		uint32_t cnt = slice->get_block_size(i);
		if (addr == NULL)
			continue;
		for (uint32_t k = 0; k < cnt; k++)
			ret.push_back(addr[k]);
	}
	return ret;
}

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

// 原地改写 RT 块头里的 _capacity，模拟 writer 侧扩容后 reader 的重新映射分支
inline bool patch_u32(const string& file, std::size_t off, uint32_t val)
{
	std::fstream fs(file.c_str(), std::ios::in | std::ios::out | std::ios::binary);
	if (!fs.is_open())
		return false;
	fs.seekp((std::streamoff)off);
	fs.write((const char*)&val, (std::streamsize)sizeof(val));
	fs.flush();
	return fs.good();
}

inline bool patch_rt_capacity(const string& file, uint32_t capacity)
{
	// RTDayBlockHeader 是「基类 BlockHeader + size/capacity/date」的继承布局（非 standard-layout，
	// 不能对成员用 offsetof）；#pragma pack(1) 下 _capacity 紧跟 _size：
	// sizeof(BlockHeader)=12，_size 占 4 字节 → _capacity 偏移 16。
	return patch_u32(file, sizeof(BlockHeader) + sizeof(uint32_t), capacity);
}

// 头部声明 capacity，但文件只放 items 条（用于触发 writer 的容量修复分支）
template<typename T>
inline string make_short_rt_block(uint16_t blk_type, uint32_t date, uint32_t claimed_cap, const vector<T>& items)
{
	string ret(sizeof(RTDayBlockHeader), '\0');
	RTDayBlockHeader* head = (RTDayBlockHeader*)ret.data();
	memcpy(head->_blk_flag, BLK_FLAG, FLAG_SIZE);
	head->_type = blk_type;
	head->_version = BLOCK_VERSION_RAW_V2;
	head->_size = (uint32_t)items.size();
	head->_capacity = claimed_cap;
	head->_date = date;

	if (!items.empty())
		ret.append((const char*)items.data(), items.size() * sizeof(T));
	return ret;
}

} // namespace r52
