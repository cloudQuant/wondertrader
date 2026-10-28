// r100 / WtDataStorageAD 读取器补测夹具（非用例 TU）
//
// 只提供公开 API 所需的最小宿主：IDataReaderSink + IHotMgr 替身、按生产写入器口径
// 落 LMDB 的 tick/bar、以及可写映射改 RTBarCache 头/条目的辅助（用于模拟 writer
// 在同一缓存块里新增合约或推进最后一根未闭合 bar）。
//
// 没有 #define private public、没有类布局镜像；所有路径都在 temp_directory_path()
// 下的 r100_* 唯一目录，TearDown 删除。
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "r4_ad_fixture.h"

#include "Includes/IDataReader.h"
#include "Includes/IHotMgr.h"
#include "Share/BoostMappingFile.hpp"
#include "WtDataStorageAD/WtDataReaderAD.h"

namespace r100fx {

using std::string;
using std::vector;

class R100HotMgr : public IHotMgr
{
public:
	virtual ~R100HotMgr() {}

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return _raw.c_str(); }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return _raw.c_str(); }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char* ext) override
	{
		if (_tag.empty() || ext == NULL)
			return "";
		return strstr(ext, _tag.c_str()) != NULL ? _tag.c_str() : "";
	}
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }

	void set_tag(const char* tag) { _tag = tag == NULL ? "" : tag; }
	void set_raw(const char* raw) { _raw = raw == NULL ? "" : raw; }

private:
	string	_tag;
	string	_raw;
};

class R100ReaderSink : public IDataReaderSink
{
public:
	R100ReaderSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~R100ReaderSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }

	void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* bar) override
	{
		_bar_calls++;
		if (bar != NULL)
		{
			_last_bar = *bar;
			_last_code = stdCode == NULL ? "" : stdCode;
			_last_period = period;
		}
	}

	void on_all_bar_updated(uint32_t updateTime) override
	{
		_all_updated++;
		_last_update_time = updateTime;
	}

	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		if (message != NULL)
		{
			_last[ll] = message;
			_all.push_back(message);
		}
	}

	void set_clock(uint32_t d, uint32_t mint, uint32_t secs)
	{
		_date = d;
		_min_time = mint;
		_secs = secs;
	}

	uint32_t get_date() override { return _date; }
	uint32_t get_min_time() override { return _min_time; }
	uint32_t get_secs() override { return _secs; }

	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t all_updated() const { return _all_updated; }
	uint32_t last_update_time() const { return _last_update_time; }
	const WTSBarStruct& last_bar() const { return _last_bar; }
	const string& last_bar_code() const { return _last_code; }
	WTSKlinePeriod last_period() const { return _last_period; }

	uint32_t log_count(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total_logs() const { return _total; }
	string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? string() : it->second;
	}
	uint32_t count_containing(const char* sub) const
	{
		uint32_t ret = 0;
		const string s = sub == NULL ? "" : sub;
		for (const string& item : _all)
		{
			if (item.find(s) != string::npos)
				ret++;
		}
		return ret;
	}

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	uint32_t		_date = 20230103;
	uint32_t		_min_time = 1000;
	uint32_t		_secs = 0;
	uint32_t		_bar_calls = 0;
	uint32_t		_all_updated = 0;
	uint32_t		_last_update_time = 0;
	WTSBarStruct		_last_bar;
	string			_last_code;
	WTSKlinePeriod	_last_period = KP_DAY;
	uint32_t		_total = 0;
	vector<string>		_all;
	wt_hashmap<WTSLogLevel, uint32_t>	_logs;
	wt_hashmap<WTSLogLevel, string>		_last;
};

class R100ADReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("r100_rad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R4BaseDataMgr();
		_hot = new R100HotMgr();
		_sink = new R100ReaderSink(_bd, _hot);
	}

	void TearDown() override
	{
		// 只读映射必须早于临时目录删除
		_reader.reset();
		delete _sink;
		delete _hot;
		delete _bd;
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	void init_reader()
	{
		_reader.reset(new WtDataReaderAD());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_reader->init(cfg, _sink, NULL);
		cfg->release();
	}

	WTSContractInfo* add_stock(const char* exchg, const char* code)
	{
		WTSCommodityInfo* comm = _bd->add_commodity(exchg, code, ContractCategory::CC_Stock);
		return _bd->add_contract(exchg, code, comm);
	}

	WTSContractInfo* add_future(const char* exchg, const char* pid, const char* code)
	{
		WTSCommodityInfo* comm = _bd->add_commodity(exchg, pid, ContractCategory::CC_Future);
		return _bd->add_contract(exchg, code, comm);
	}

	string path_of(const string& name) const { return (_root / name).string(); }

	void seed_ticks(const char* exchg, const char* code, const vector<WTSTickStruct>& ticks)
	{
		std::filesystem::create_directories(_root / "ticks" / exchg / code);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(path_of(string("ticks/") + exchg + "/" + code).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSTickStruct& tick : ticks)
		{
			LMDBHftKey key(exchg, code, tick.trading_date, tick.action_time);
			ASSERT_TRUE(query.put(string((const char*)&key, sizeof(key)),
				string((const char*)&tick, sizeof(tick))));
		}
		query.commit();
	}

	void seed_bars(const char* subdir, const char* exchg, const char* code,
		const vector<WTSBarStruct>& bars, bool by_date)
	{
		std::filesystem::create_directories(_root / subdir / exchg);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(path_of(string(subdir) + "/" + exchg).c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSBarStruct& bar : bars)
		{
			const uint32_t tail = by_date ? (uint32_t)bar.date : (uint32_t)bar.time;
			LMDBBarKey key(exchg, code, tail);
			ASSERT_TRUE(query.put(string((const char*)&key, sizeof(key)),
				string((const char*)&bar, sizeof(bar))));
		}
		query.commit();
	}

	// 写 cache_*.dmb：capacity 个槽位、前 size 个为有效条目，其余补 0
	void write_cache(const char* filename, const vector<BarCacheItem>& items, uint32_t size, uint32_t capacity)
	{
		vector<BarCacheItem> slots(capacity);
		for (size_t i = 0; i < items.size() && i < capacity; i++)
			slots[i] = items[i];
		r4fx::write_block_file(path_of(filename), BLK_FLAG, BT_RT_Cache, 1,
			size, capacity, r4fx::bar_payload(slots));
	}

	// 原地改缓存块里第 idx 条的 bar 时间/日期（模拟 writer 推进未闭合 bar）
	bool patch_cache_bar(const char* filename, uint32_t idx, uint32_t date, uint64_t time)
	{
		BoostMappingFile mf;
		if (!mf.map(path_of(filename).c_str()))
			return false;
		RTBarCache* blk = (RTBarCache*)mf.addr();
		if (blk == NULL || idx >= blk->_capacity)
			return false;
		blk->_items[idx]._bar.date = date;
		blk->_items[idx]._bar.time = time;
		mf.sync();
		return true;
	}

	// 原地改头部 _size（模拟 writer 在缓存块里登记了新合约）
	bool patch_cache_size(const char* filename, uint32_t size)
	{
		BoostMappingFile mf;
		if (!mf.map(path_of(filename).c_str()))
			return false;
		RTBarCache* blk = (RTBarCache*)mf.addr();
		if (blk == NULL || size > blk->_capacity)
			return false;
		blk->_size = size;
		mf.sync();
		return true;
	}

	bool patch_cache_item_code(const char* filename, uint32_t idx, const char* exchg, const char* code)
	{
		BoostMappingFile mf;
		if (!mf.map(path_of(filename).c_str()))
			return false;
		RTBarCache* blk = (RTBarCache*)mf.addr();
		if (blk == NULL || idx >= blk->_capacity)
			return false;
		r4fx::r4_set_str(blk->_items[idx]._exchg, 16, exchg);
		r4fx::r4_set_str(blk->_items[idx]._code, 32, code);
		mf.sync();
		return true;
	}

	std::filesystem::path	_root;
	R4BaseDataMgr*		_bd = NULL;
	R100HotMgr*		_hot = NULL;
	R100ReaderSink*		_sink = NULL;
	std::unique_ptr<WtDataReaderAD>	_reader;
};

} // namespace r100fx
