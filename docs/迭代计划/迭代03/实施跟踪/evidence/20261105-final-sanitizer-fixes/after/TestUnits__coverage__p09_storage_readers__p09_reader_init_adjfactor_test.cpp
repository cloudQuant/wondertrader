// p09 / WtDataStorage（非 AD 本体）：WtDataReader 的 init 配置解析与复权因子层
//
// 覆盖对象：
//  - WtDataReader::init（cfg==NULL 提前返回；cfg 为单路径 / 路径数组 / 带 his_path / 带 adjfactor；
//    _rt_dir 与 _his_dir 的拼装、_adjust_flag、无轮询线程）
//  - WtDataReader::loadStkAdjFactorsFromLoader（_loader==NULL 直接 false）
//  - WtDataReader::loadStkAdjFactorsFromFile（文件不存在 / 解析失败 / key 拼装 / 兜底条目 / 排序）
//  - WtDataReader::getAdjFactors（public，未命中且无 loader 时返回空列表）
//  - WtDataReader::getAdjFactorByDate、getAdjustingFlag
//
// 跳过：readTickSlice / readOrd*Slice / readTransSlice / readKlineSlice / onMinuteEnd /
// getRT*Block / cache*Bars*（都要先取 rt/ 下的 BoostMappingFile 命名共享内存块）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
// _loader / _base_data_mgr / _hot_mgr 继承自 IDataReader 的 protected 成员，白盒断言需要放开
#define protected public
#include "Includes/IDataReader.h"
#undef protected
#include "Includes/FasterDefs.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Share/StdUtils.hpp"
#include "Share/BoostMappingFile.hpp"
#include "Share/fmtlib.h"
#include "Share/TimeUtils.hpp"
#include "WtDataStorage/DataDefine.h"

// 覆盖率专用白盒开关：访问权限不参与 Itanium name mangling，链接到的仍是 libWtDataStorage 实现
#define private public
#include "WtDataStorage/WtDataReader.h"
#undef private

using namespace wtp;

namespace {

class P09RDBaseDataMgr : public IBaseDataMgr
{
public:
	P09RDBaseDataMgr()
	{
		_session = WTSSessionInfo::create("SSE_DAY", "日盘", 0);
		_session->addTradingSection(0, 2359);
		_stock = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_stock->setSessionInfo(_session);
		_stock->setCategory(ContractCategory::CC_Stock);
		_fut = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SSE_DAY", "CHN");
		_fut->setSessionInfo(_session);
		_fut->setCategory(ContractCategory::CC_Future);
	}

	virtual ~P09RDBaseDataMgr()
	{
		_stock->release();
		_fut->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _stock;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return strcmp(exchg, "SSE") == 0 ? _stock : _fut;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override { return uDate; }
	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_stock = NULL;
	WTSCommodityInfo*	_fut = NULL;
	std::string		_last_pid;
	std::string		_last_sid;
};

class P09RDHotMgr : public IHotMgr
{
public:
	virtual ~P09RDHotMgr() {}

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

class P09RDSink : public IDataReaderSink
{
public:
	P09RDSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~P09RDSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct* /*newBar*/) override
	{
		_bars[stdCode] = (uint32_t)period;
	}
	void on_all_bar_updated(uint32_t /*updateTime*/) override { _updated++; }
	void reader_log(WTSLogLevel ll, const char* /*message*/) override
	{
		_logs[ll]++;
		_total++;
	}
	// 本车道不驱动行情时钟，三个时间源固定返回 0（只为满足纯虚约束）
	uint32_t get_date() override { return 0; }
	uint32_t get_min_time() override { return 0; }
	uint32_t get_secs() override { return 0; }

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	uint32_t bar_count() const { return (uint32_t)_bars.size(); }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	std::map<std::string, uint32_t>	_bars;
	std::map<WTSLogLevel, uint32_t>	_logs;
	uint32_t		_total = 0;
	uint32_t		_updated = 0;
};

class P09AdjFactorLoader : public IHisDataLoader
{
public:
	bool loadFinalHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
	bool loadRawHisBars(void*, const char*, WTSKlinePeriod, FuncReadBars) override { return false; }
	bool loadAllAdjFactors(void*, FuncReadFactors) override { return false; }
	bool loadAdjFactors(void* obj, const char* stdCode, FuncReadFactors cb) override
	{
		bool defer = false;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			++_calls;
			_keys.emplace_back(stdCode == NULL ? "" : stdCode);
			_retained_keys.push_back(stdCode);
			defer = _deferCallbacks;
			if (defer)
				_pending.push_back(Pending{ obj, stdCode, cb });
		}
		if (!defer)
		{
			uint32_t dates[] = { 20200102 };
			double factors[] = { 0.75 };
			cb(obj, stdCode, dates, factors, 1);
		}
		return true;
	}

	void deferCallbacks(bool defer)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_deferCallbacks = defer;
	}
	void feed(size_t index)
	{
		Pending pending;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			pending = _pending.at(index);
		}
		uint32_t dates[] = { 20200102 };
		double factors[] = { 0.75 };
		pending.callback(pending.obj, pending.stdCode, dates, factors, 1);
	}
	uint32_t calls() const { std::lock_guard<std::mutex> lock(_mutex); return _calls; }
	std::vector<std::string> keys() const { std::lock_guard<std::mutex> lock(_mutex); return _keys; }
	std::vector<const char*> retainedKeys() const
	{
		std::lock_guard<std::mutex> lock(_mutex);
		return _retained_keys;
	}

private:
	struct Pending
	{
		void* obj;
		const char* stdCode;
		FuncReadFactors callback;
	};
	mutable std::mutex _mutex;
	uint32_t _calls = 0;
	bool _deferCallbacks = false;
	std::vector<std::string> _keys;
	std::vector<const char*> _retained_keys;
	std::vector<Pending> _pending;
};

class P09DataReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		std::filesystem::create_directories(_root, _ec);
		_hot.reset(new P09RDHotMgr());
		_bd.reset(new P09RDBaseDataMgr());
		_sink.reset(new P09RDSink(_bd.get(), _hot.get()));
	}

	void TearDown() override
	{
		std::filesystem::remove_all(_root, _ec);
	}

	std::string path_of(const char* name) const { return _root + "/" + name; }

	// 写一个最小可解析的 json 文件，返回路径
	std::string write_json(const char* name, const std::string& content)
	{
		const std::string file = path_of(name);
		std::ofstream ofs(file);
		ofs << content;
		ofs.close();
		return file;
	}

	WTSVariant* load_cfg(const std::string& file)
	{
		WTSVariant* v = WTSCfgLoader::load_from_file(file.c_str());
		return v;
	}

	std::unique_ptr<P09RDHotMgr>		_hot;
	std::unique_ptr<P09RDBaseDataMgr>	_bd;
	std::unique_ptr<P09RDSink>			_sink;
	std::error_code						_ec;
	static uint32_t						_seq;
	std::string							_root;

	P09DataReaderTest()
	{
		_root = (std::filesystem::temp_directory_path()
			/ ("p09_rd_" + std::to_string(::getpid()) + "_" + std::to_string(++_seq))).string();
	}
};
uint32_t P09DataReaderTest::_seq = 0;

TEST_F(P09DataReaderTest, init_null_cfg_returns_before_any_dir_config)
{
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);

	// cfg==NULL 时只注入宿主指针，路径目录与复权标志保持默认
	EXPECT_TRUE(reader._rt_dir.empty());
	EXPECT_TRUE(reader._his_dir.empty());
	EXPECT_EQ(reader._adjust_flag, 0u);
	EXPECT_EQ(reader._base_data_mgr, _bd.get());
	EXPECT_EQ(reader._hot_mgr, _hot.get());
	EXPECT_EQ(reader._loader, nullptr);
	EXPECT_EQ(_sink->total(), 0u);
}

TEST_F(P09DataReaderTest, init_single_path_and_his_path_and_adjust_flag)
{
	const std::string root = path_of("data");
	const std::string extraHis = path_of("extra_his");
	const std::string cfgfile = write_json("reader_single.json",
		"{\n\"path\": \"" + root + "\",\n\"his_path\": \"" + extraHis + "\",\n\"adjust_flag\": 2\n}\n");

	WTSVariant* cfg = load_cfg(cfgfile);
	ASSERT_NE(cfg, nullptr);

	WtDataReader reader;
	reader.init(cfg, _sink.get(), NULL);
	cfg->release();

	ASSERT_EQ(reader._rt_dir.size(), 1u);
	ASSERT_EQ(reader._his_dir.size(), 2u);
	// path 会同时派生 rt/ 与 his/，his_path 再追加一个独立历史目录
	EXPECT_EQ(reader._rt_dir[0].substr(reader._rt_dir[0].size() - 3), "rt/");
	EXPECT_NE(reader._rt_dir[0].find(root), std::string::npos);
	EXPECT_EQ(reader._his_dir[0].substr(reader._his_dir[0].size() - 4), "his/");
	EXPECT_NE(reader._his_dir[0].find(root), std::string::npos);
	EXPECT_NE(reader._his_dir[1].find("extra_his"), std::string::npos);
	EXPECT_EQ(reader._adjust_flag, 2u);
	EXPECT_TRUE(reader._loader == nullptr);
	// init 里那句 "WtDataReader initialized..." 必须落到 sink 日志
	EXPECT_GE(_sink->count_of(LL_INFO), 1u);
	// 未配置 adjfactor -> 走 "loading skipped" 分支
	EXPECT_TRUE(reader._adj_factors.empty());
}

TEST_F(P09DataReaderTest, init_path_array_and_adjfactor_file_is_loaded)
{
	const std::string d1 = path_of("d1");
	const std::string d2 = path_of("d2");
	const std::string adjfile = write_json("adj.json",
		"{\n  \"SSE\": {\n    \"STK.600010\": [ {\"date\": 20210101, \"factor\": 0.25} ]\n  }\n}\n");
	const std::string cfgfile = write_json("reader_array.json",
		"{\n\"path\": [ \"" + d1 + "\", \"" + d2 + "\" ],\n\"adjfactor\": \"" + adjfile + "\"\n}\n");

	WTSVariant* cfg = load_cfg(cfgfile);
	ASSERT_NE(cfg, nullptr);

	WtDataReader reader;
	reader.init(cfg, _sink.get(), NULL);
	cfg->release();

	// 两个 root_dir 各派生 rt/ 与 his/
	EXPECT_EQ(reader._rt_dir.size(), 2u);
	EXPECT_EQ(reader._his_dir.size(), 2u);
	if (reader._rt_dir.size() == 2)
	{
		EXPECT_NE(reader._rt_dir[0].find(d1), std::string::npos);
		EXPECT_NE(reader._rt_dir[1].find(d2), std::string::npos);
	}

	// extloader 返回 false 后按配置的 adjfactor 文件加载
	ASSERT_EQ(reader._adj_factors.count("SSE.STK.600010"), 1u);
	const WtDataReader::AdjFactorList& lst = reader._adj_factors["SSE.STK.600010"];
	ASSERT_EQ(lst.size(), 2u);
	EXPECT_EQ(lst[0]._date, 19900101u);
	EXPECT_DOUBLE_EQ(lst[0]._factor, 1.0);
	EXPECT_EQ(lst[1]._date, 20210101u);
	EXPECT_DOUBLE_EQ(lst[1]._factor, 0.25);
}

TEST_F(P09DataReaderTest, load_adj_factors_from_loader_without_loader)
{
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);
	// _loader == NULL 的短路分支
	EXPECT_FALSE(reader.loadStkAdjFactorsFromLoader());
	EXPECT_TRUE(reader._adj_factors.empty());
}

TEST_F(P09DataReaderTest, load_adj_factors_from_file_failures)
{
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);

	EXPECT_FALSE(reader.loadStkAdjFactorsFromFile(path_of("nothing.json").c_str()));
	EXPECT_EQ(_sink->count_of(LL_ERROR), 1u);

	const std::string bad = write_json("bad.json", "not a json document at all");
	EXPECT_FALSE(reader.loadStkAdjFactorsFromFile(bad.c_str()));
	EXPECT_GE(_sink->count_of(LL_ERROR), 2u);
	EXPECT_TRUE(reader._adj_factors.empty());
}

TEST_F(P09DataReaderTest, load_adj_factors_key_rules_and_ordering)
{
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);

	const std::string adj = write_json("adj2.json",
		"{\n"
		"  \"SSE\": {\n"
		"    \"STK.600000\": [ {\"date\": 20220601, \"factor\": 0.4}, {\"date\": 20200101, \"factor\": 0.7} ],\n"
		"    \"600001\": [ {\"date\": 20210101, \"factor\": 1.5} ],\n"
		"    \"PLAIN\": \"notarray\"\n"
		"  },\n"
		"  \"SZSE\": {}\n"
		"}\n");
	ASSERT_TRUE(reader.loadStkAdjFactorsFromFile(adj.c_str()));

	// 带 PID 的 code 原样拼接，不带的强制补 STK，非数组成员被跳过
	EXPECT_EQ(reader._adj_factors.count("SSE.STK.600000"), 1u);
	EXPECT_EQ(reader._adj_factors.count("SSE.STK.600001"), 1u);
	EXPECT_EQ(reader._adj_factors.count("SSE.STK.PLAIN"), 0u);
	// 空对象成员不产生任何 key
	EXPECT_EQ(reader._adj_factors.count("SZSE.STK."), 0u);
	EXPECT_EQ(reader._adj_factors.size(), 2u);

	const WtDataReader::AdjFactorList& lst = reader._adj_factors["SSE.STK.600000"];
	ASSERT_EQ(lst.size(), 3u);
	EXPECT_EQ(lst[0]._date, 19900101u);
	EXPECT_EQ(lst[1]._date, 20200101u);
	EXPECT_EQ(lst[2]._date, 20220601u);
	EXPECT_DOUBLE_EQ(lst[2]._factor, 0.4);

	// public 访问器：命中同一个 key；未命中返回空列表
	EXPECT_EQ(reader.getAdjFactors("600000", "SSE", "STK").size(), 3u);
	EXPECT_TRUE(reader.getAdjFactors("000001", "SZSE", "STK").empty());
}

TEST_F(P09DataReaderTest, get_adj_factors_loader_receives_complete_dynamic_key_and_cache_hits)
{
	P09AdjFactorLoader loader;
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);
	reader._loader = &loader;

	struct KeyCase
	{
		std::string exchg;
		std::string pid;
		std::string code;
		std::string expected;
	};
	const std::vector<KeyCase> cases = {
		{ "A", "B", "CDEFGHIJKLMNOPQ", "A.B.CDEFGHIJKLMNOPQ" }, // 19 bytes
		{ "A", "B", "CDEFGHIJKLMNOPQR", "A.B.CDEFGHIJKLMNOPQR" }, // 20 bytes
		{ "LONGEXCHANGE", "PRODUCT", "INSTRUMENTCODE", "LONGEXCHANGE.PRODUCT.INSTRUMENTCODE" }, // >20 bytes
		{ std::string(MAX_INSTRUMENT_LENGTH - 1, 'E'),
		  std::string(MAX_INSTRUMENT_LENGTH - 1, 'P'),
		  std::string(MAX_INSTRUMENT_LENGTH - 1, 'C'),
		  std::string(MAX_INSTRUMENT_LENGTH - 1, 'E') + "." +
			std::string(MAX_INSTRUMENT_LENGTH - 1, 'P') + "." +
			std::string(MAX_INSTRUMENT_LENGTH - 1, 'C') } // maximum fixed-width component lengths
	};

	for (const KeyCase& keyCase : cases)
	{
		const uint32_t callsBefore = loader.calls();
		const WtDataReader::AdjFactorList& first = reader.getAdjFactors(
			keyCase.code.c_str(), keyCase.exchg.c_str(), keyCase.pid.c_str());
		ASSERT_EQ(first.size(), 2u);
		EXPECT_EQ(loader.calls(), callsBefore + 1);
		ASSERT_EQ(loader.keys().size(), callsBefore + 1);
		EXPECT_EQ(loader.keys().back(), keyCase.expected);
		EXPECT_EQ(reader._adj_factors.count(keyCase.expected), 1u);
		EXPECT_DOUBLE_EQ(first.back()._factor, 0.75);

		const WtDataReader::AdjFactorList& cached = reader.getAdjFactors(
			keyCase.code.c_str(), keyCase.exchg.c_str(), keyCase.pid.c_str());
		EXPECT_EQ(&cached, &first);
		EXPECT_EQ(loader.calls(), callsBefore + 1);
	}
}

TEST_F(P09DataReaderTest, delayed_loader_key_pointer_survives_return_stack_use_and_other_keys)
{
	P09AdjFactorLoader loader;
	loader.deferCallbacks(true);
	WtDataReader reader;
	reader.init(NULL, NULL, NULL);
	reader._loader = &loader;

	const std::string firstCode = "CDEFGHIJKLMNOPQR"; // composite key is exactly 20 bytes
	const std::string firstExpected = "A.B." + firstCode;
	EXPECT_TRUE(reader.getAdjFactors(firstCode.c_str(), "A", "B").empty());

	std::vector<const char*> retained = loader.retainedKeys();
	ASSERT_EQ(retained.size(), 1u);
	const char* firstPointer = retained[0];
	ASSERT_NE(firstPointer, nullptr);
	EXPECT_STREQ(firstPointer, firstExpected.c_str());

	// Overwrite temporary stack storage and request distinct keys. The loader
	// still holds the first raw pointer after its loadAdjFactors call returned.
	volatile char stackNoise[4096];
	for (size_t i = 0; i < sizeof(stackNoise); ++i)
		stackNoise[i] = static_cast<char>(i);
	EXPECT_NE(stackNoise[sizeof(stackNoise) - 1], 0);

	const std::vector<std::string> otherCodes = {
		"S", "T", "UVWXYZ0123456789012345678901234"
	};
	for (size_t i = 0; i < otherCodes.size(); ++i)
	{
		const std::string exchg = "EX" + std::to_string(i);
		EXPECT_TRUE(reader.getAdjFactors(otherCodes[i].c_str(), exchg.c_str(), "PID").empty());
	}

	retained = loader.retainedKeys();
	ASSERT_EQ(retained.size(), 1u + otherCodes.size());
	for (size_t i = 0; i < retained.size(); ++i)
	{
		ASSERT_NE(retained[i], nullptr);
		for (size_t j = i + 1; j < retained.size(); ++j)
			EXPECT_NE(retained[i], retained[j]);
	}
	EXPECT_EQ(std::string(firstPointer), firstExpected);
	for (size_t i = 0; i < otherCodes.size(); ++i)
	{
		const std::string expected = "EX" + std::to_string(i) + ".PID." + otherCodes[i];
		EXPECT_EQ(std::string(retained[i + 1]), expected);
	}
	loader.feed(0);
	const WtDataReader::AdjFactorList& loaded = reader._adj_factors[firstExpected];
	ASSERT_EQ(loaded.size(), 2u);
	EXPECT_DOUBLE_EQ(loaded.back()._factor, 0.75);
}

TEST_F(P09DataReaderTest, concurrent_readers_keep_different_loader_key_addresses)
{
	P09AdjFactorLoader loader;
	loader.deferCallbacks(true);
	WtDataReader readerA;
	WtDataReader readerB;
	readerA.init(NULL, NULL, NULL);
	readerB.init(NULL, NULL, NULL);
	readerA._loader = &loader;
	readerB._loader = &loader;

	std::thread threadA([&]() { readerA.getAdjFactors("600000", "SSE", "STK"); });
	std::thread threadB([&]() { readerB.getAdjFactors("000001", "SZSE", "STK"); });
	threadA.join();
	threadB.join();

	const std::vector<std::string> keys = loader.keys();
	const std::vector<const char*> retained = loader.retainedKeys();
	ASSERT_EQ(keys.size(), 2u);
	ASSERT_EQ(retained.size(), 2u);
	std::map<std::string, const char*> keyToAddress;
	for (size_t i = 0; i < keys.size(); ++i)
		ASSERT_TRUE(keyToAddress.emplace(keys[i], retained[i]).second);
	ASSERT_EQ(keyToAddress.count("SSE.STK.600000"), 1u);
	ASSERT_EQ(keyToAddress.count("SZSE.STK.000001"), 1u);
	const char* keyA = keyToAddress.at("SSE.STK.600000");
	const char* keyB = keyToAddress.at("SZSE.STK.000001");
	EXPECT_NE(keyA, keyB);
	EXPECT_EQ(std::string(keyA), "SSE.STK.600000");
	EXPECT_EQ(std::string(keyB), "SZSE.STK.000001");
}

TEST_F(P09DataReaderTest, concurrent_distinct_loader_keys_have_independent_stable_addresses)
{
	WtDataReader reader;
	const std::vector<std::string> keys = {
		"SSE.STK.600000", "SZSE.STK.000001", "EXCHANGE.PRODUCT.INSTRUMENT"
	};
	std::vector<const char*> addresses(keys.size(), nullptr);
	std::vector<std::thread> threads;
	threads.reserve(keys.size());
	for (size_t i = 0; i < keys.size(); ++i)
		threads.emplace_back([&reader, &keys, &addresses, i]() {
			addresses[i] = reader.getStableAdjFactorLoaderKey(keys[i]);
		});
	for (std::thread& thread : threads)
		thread.join();

	for (size_t i = 0; i < keys.size(); ++i)
	{
		ASSERT_NE(addresses[i], nullptr);
		EXPECT_EQ(std::string(addresses[i]), keys[i]);
		for (size_t j = i + 1; j < keys.size(); ++j)
			EXPECT_NE(addresses[i], addresses[j]);
	}
}

TEST_F(P09DataReaderTest, get_adj_factor_by_date_branches)
{
	WtDataReader reader;
	reader.init(NULL, _sink.get(), NULL);

	WtDataReader::AdjFactorList& lst = reader._adj_factors["SSE.STK.600100"];
	lst.push_back(WtDataReader::AdjFactor{ 19900101, 1.0 });
	lst.push_back(WtDataReader::AdjFactor{ 20200101, 0.8 });
	lst.push_back(WtDataReader::AdjFactor{ 20210601, 0.5 });
	// getAdjFactorByDate 会按 isExright() 裁掉 stdCode 末位，两种 key 都灌上，
	// 这样断言只依赖本文件的日期分支逻辑，不依赖 CodeHelper 的复权判定。
	// I03-NEW-110：先拷贝再插入。_adj_factors 的值存放在连续 vector 中，插入新 key
	// 可能重新分配，使引用 lst 悬空（ASan heap-use-after-free）。
	const WtDataReader::AdjFactorList lstCopy = lst;
	reader._adj_factors["SSE.STK.60010"] = lstCopy;

	// 非股票 -> 1.0
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SHFE.rb0", 20210101), 1.0);
	// 股票但无因子 -> 1.0
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.STK.999999", 20210101), 1.0);
	// 命中 / 夹在中间 / 超出末尾 / 首条
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.STK.600100", 20200101), 0.8);
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.STK.600100", 20210531), 0.8);
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.STK.600100", 20991231), 0.5);
	EXPECT_DOUBLE_EQ(reader.getAdjFactorByDate("SSE.STK.600100", 19900101), 1.0);

	// inline getAdjustingFlag
	EXPECT_EQ(reader.getAdjustingFlag(), 0u);
}

// 缺失、空或类型错误的存储路径都必须在不生成部分目录状态的情况下安全返回。
TEST_F(P09DataReaderTest, init_missing_path_returns_without_partial_directories)
{
	const std::string cfgfile = write_json("reader_nopath.json", "{\n\"adjust_flag\": 1\n}\n");
	WTSVariant* cfg = load_cfg(cfgfile);
	ASSERT_NE(cfg, nullptr);

	WtDataReader reader;
	reader.init(cfg, _sink.get(), NULL);
	cfg->release();

	EXPECT_TRUE(reader._rt_dir.empty());
	EXPECT_TRUE(reader._his_dir.empty());
	EXPECT_EQ(_sink->count_of(LL_ERROR), 1u);
	EXPECT_EQ(reader.readTickSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readOrdDtlSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readOrdQueSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readTransSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readKlineSlice("SSE.STK.600100", KP_DAY, 1), nullptr);
	EXPECT_NO_THROW(reader.onMinuteEnd(20261029, 1500));
}

TEST_F(P09DataReaderTest, init_null_sinkLeavesReaderNotReady)
{
	const std::string cfgfile = write_json("reader_null_sink.json",
		R"JSON({"path": "/tmp/p09_reader_null_sink"})JSON");
	WTSVariant* cfg = load_cfg(cfgfile);
	ASSERT_NE(cfg, nullptr);

	WtDataReader reader;
	reader.init(cfg, NULL, NULL);
	cfg->release();

	EXPECT_TRUE(reader._rt_dir.empty());
	EXPECT_TRUE(reader._his_dir.empty());
	EXPECT_EQ(reader.readTickSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readKlineSlice("SSE.STK.600100", KP_DAY, 1), nullptr);
	EXPECT_NO_THROW(reader.onMinuteEnd(20261029, 1500));
}

TEST_F(P09DataReaderTest, invalidReinitializationClearsPreviousReadyDirectories)
{
	WTSVariant* validCfg = load_cfg(write_json("valid_reinit.json",
		R"JSON({"path": "/tmp/p09_reader_valid_reinit"})JSON"));
	ASSERT_NE(validCfg, nullptr);
	WtDataReader reader;
	reader.init(validCfg, _sink.get(), NULL);
	validCfg->release();
	ASSERT_EQ(reader._rt_dir.size(), 1u);

	WTSVariant* invalidCfg = load_cfg(write_json("invalid_reinit.json",
		R"JSON({"path": []})JSON"));
	ASSERT_NE(invalidCfg, nullptr);
	reader.init(invalidCfg, _sink.get(), NULL);
	invalidCfg->release();

	EXPECT_TRUE(reader._rt_dir.empty());
	EXPECT_TRUE(reader._his_dir.empty());
	EXPECT_EQ(reader.readTickSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readOrdDtlSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readOrdQueSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readTransSlice("SSE.STK.600100", 1), nullptr);
	EXPECT_EQ(reader.readKlineSlice("SSE.STK.600100", KP_DAY, 1), nullptr);

	WTSVariant* recoveredCfg = load_cfg(write_json("recovered_reinit.json",
		R"JSON({"path": "/tmp/p09_reader_recovered_reinit"})JSON"));
	ASSERT_NE(recoveredCfg, nullptr);
	reader.init(recoveredCfg, _sink.get(), NULL);
	recoveredCfg->release();
	ASSERT_EQ(reader._rt_dir.size(), 1u);
	EXPECT_NE(reader._rt_dir[0].find("p09_reader_recovered_reinit"), std::string::npos);
	EXPECT_NE(reader._his_dir[0].find("p09_reader_recovered_reinit/his/"), std::string::npos);
}

TEST_F(P09DataReaderTest, init_emptyOrInvalidPathReturnsWithoutPartialDirectories)
{
	const std::vector<std::pair<const char*, const char*>> invalidConfigs = {
		{ "reader_empty_array.json", R"JSON({"path": []})JSON" },
		{ "reader_empty_string.json", R"JSON({"path": ""})JSON" },
		{ "reader_null_path.json", R"JSON({"path": null})JSON" },
		{ "reader_bad_array_entry.json", R"JSON({"path": ["/tmp/data", 7]})JSON" }
	};

	for (const auto& entry : invalidConfigs)
	{
		SCOPED_TRACE(entry.first);
		const std::string cfgfile = write_json(entry.first, entry.second);
		WTSVariant* cfg = load_cfg(cfgfile);
		ASSERT_NE(cfg, nullptr);

		WtDataReader reader;
		reader.init(cfg, _sink.get(), NULL);
		cfg->release();

		EXPECT_TRUE(reader._rt_dir.empty());
		EXPECT_TRUE(reader._his_dir.empty());
		EXPECT_EQ(reader.readTickSlice("SSE.STK.600100", 1), nullptr);
	}
	EXPECT_EQ(_sink->count_of(LL_ERROR), invalidConfigs.size());
}

}
