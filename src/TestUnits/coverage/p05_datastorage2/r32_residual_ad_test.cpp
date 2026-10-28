// r32 / WtDataStorageAD：残余覆盖 —— AD 族 createWriter/deleteWriter/createDataReader/
// deleteDataReader extern "C" 入口 + WtDataReaderAD::readKlineSlice 的 etime==0 补钟分支
//
// 覆盖对象（src/WtDataStorageAD/）：
//  - WtDataWriterAD.cpp:34-50（createWriter / deleteWriter，含 NULL 引用入参分支 :44-48）
//  - WtDataWriterAD.cpp:73-104（工厂产物走完整 init：路径/文件名/groupsize/disable 位/loadCache）
//  - WtDataWriterAD.cpp:106-114（release 的无 _task_thrd 分支 —— 工厂对象未调用过
//    writeTick，pushTask 不读 _async_task（R4-BUG-01），release 只判空 shared_ptr，安全）
//  - WtDataWriterAD.cpp:842-860（getCurTick 的「合约在、缓存空」分支，工厂产物直调）
//  - WtDataReaderAD.cpp:32-45（createDataReader / deleteDataReader）
//  - WtDataReaderAD.cpp:374-382（readKlineSlice 的 etime==0 → sink 时钟补齐）
// 说明：本文件自带夹具与 sink（单 TU，匿名命名空间安全）；共享头 r4_ad_fixture.h 的
// fixture 类在具名命名空间 r4fx 内，被 3 个既有 .cpp 包含 —— 新代码不要再把它塞进
// 匿名命名空间（SUITECONF 地雷，见 r4_ad_fixture.h:47-49 注释）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IDataWriter.h"
#include "Includes/IDataReader.h"
#include "Includes/IHotMgr.h"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/DataDefineAD.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtDataWriterAD.h"
#include "WtDataStorageAD/WtDataReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////////////
// AD 一族的 extern "C" 入口（与 r21_lmdbkeys_btreader_test.cpp:42-46 同族声明方式；
// WtDataStorage 的同名工厂不在本目标，绝不引用）
//////////////////////////////////////////////////////////////////////////
extern "C" {
	IDataWriter* createWriter(void);
	void deleteWriter(IDataWriter* &writer);
	IDataReader* createDataReader(void);
	void deleteDataReader(IDataReader* reader);
}

//////////////////////////////////////////////////////////////////////////
// 最小 IBaseDataMgr：单时段 0900-1500、偏移 0；交易日 == 自然日
//////////////////////////////////////////////////////////////////////////
class R32BaseDataMgr : public IBaseDataMgr
{
public:
	R32BaseDataMgr()
	{
		_session = WTSSessionInfo::create("R32_DAY", "R32 日盘", 0);
		_session->addTradingSection(900, 1500);
	}

	virtual ~R32BaseDataMgr()
	{
		for (auto& kv : _contracts) kv.second->release();
		_contracts.clear();
		for (auto& kv : _comms) kv.second->release();
		_comms.clear();
		_session->release();
	}

	WTSCommodityInfo* add_commodity(const char* exchg, const char* pid)
	{
		WTSCommodityInfo* comm = WTSCommodityInfo::create(pid, pid, exchg, _session->id(), "CHN");
		comm->setSessionInfo(_session);
		comm->setCategory(ContractCategory::CC_Stock);
		_comms[std::string(exchg) + "." + pid] = comm;
		return comm;
	}

	WTSContractInfo* add_contract(const char* exchg, const char* code, WTSCommodityInfo* comm)
	{
		WTSContractInfo* ct = WTSContractInfo::create(code, code, exchg, comm->getProduct());
		ct->setCommInfo(comm);
		_contracts[std::string(exchg) + "." + code] = ct;
		return ct;
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		auto it = _comms.find(exchgpid == NULL ? "" : exchgpid);
		return it == _comms.end() ? NULL : it->second;
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		std::string k = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		auto it = _comms.find(k);
		return it == _comms.end() ? NULL : it->second;
	}
	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		std::string k = std::string(exchg == NULL ? "" : exchg) + "." + (code == NULL ? "" : code);
		auto it = _contracts.find(k);
		return it == _contracts.end() ? NULL : it->second;
	}
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return _session; }
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
	WTSSessionInfo* _session;
	wt_hashmap<std::string, WTSCommodityInfo*> _comms;
	wt_hashmap<std::string, WTSContractInfo*> _contracts;
};

//////////////////////////////////////////////////////////////////////////
// IDataWriterSink 替身：只记数，不做广播
//////////////////////////////////////////////////////////////////////////
class R32WriterSink : public IDataWriterSink
{
public:
	explicit R32WriterSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~R32WriterSink() {}

	IBaseDataMgr* getBDMgr() override { return _bd; }
	bool canSessionReceive(const char*) override { return true; }
	void broadcastTick(WTSTickData*) override { _bcast++; }
	void broadcastOrdQue(WTSOrdQueData*) override {}
	void broadcastOrdDtl(WTSOrdDtlData*) override {}
	void broadcastTrans(WTSTransData*) override {}
	CodeSet* getSessionComms(const char*) override { return &_comm_set; }
	uint32_t getTradingDate(const char*) override { return 20230103; }
	void outputLog(WTSLogLevel ll, const char* message) override
	{
		_cnt[ll]++;
		_total++;
		if (message != NULL) _last[ll] = message;
	}
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _cnt.find(ll);
		return it == _cnt.end() ? 0u : it->second;
	}
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}
	uint32_t bcast() const { return _bcast; }

private:
	IBaseDataMgr* _bd;
	CodeSet _comm_set;
	uint32_t _bcast = 0;
	uint32_t _total = 0;
	wt_hashmap<WTSLogLevel, uint32_t> _cnt;
	wt_hashmap<WTSLogLevel, std::string> _last;
};

//////////////////////////////////////////////////////////////////////////
// IDataReaderSink 替身：可控时钟（etime==0 的补齐来源）+ 全量日志计数
//////////////////////////////////////////////////////////////////////////
class R32ReaderSink : public IDataReaderSink
{
public:
	explicit R32ReaderSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~R32ReaderSink() {}

	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override { _bar_calls++; }
	void on_all_bar_updated(uint32_t) override {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return NULL; }
	uint32_t get_date() override { return _date; }
	uint32_t get_min_time() override { return _min_time; }
	uint32_t get_secs() override { return _secs; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_cnt[ll]++;
		_total++;
		if (message != NULL) _last[ll] = message;
	}

	void set_clock(uint32_t d, uint32_t m, uint32_t s) { _date = d; _min_time = m; _secs = s; }
	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _cnt.find(ll);
		return it == _cnt.end() ? 0u : it->second;
	}
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}

private:
	IBaseDataMgr* _bd;
	uint32_t _date = 20230103;
	uint32_t _min_time = 1000;
	uint32_t _secs = 0;
	uint32_t _bar_calls = 0;
	uint32_t _total = 0;
	wt_hashmap<WTSLogLevel, uint32_t> _cnt;
	wt_hashmap<WTSLogLevel, std::string> _last;
};

class R32AdResidualTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("r32_adres_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R32BaseDataMgr();
	}

	void TearDown() override
	{
		delete _bd;
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	// 造 K 线库（与 r4_reader_ad_slices_test.cpp 的 seed_bars 同形，本文件自带）
	void seed_bars(const char* subdir, const char* exchg, const char* code,
		const std::vector<uint32_t>& bar_times, const std::vector<double>& bases)
	{
		std::filesystem::create_directories(_root / subdir / exchg);
		WtLMDB db(false);
		ASSERT_TRUE(db.open((_root / subdir / exchg).string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (std::size_t i = 0; i < bar_times.size(); i++)
		{
			WTSBarStruct bar;
			memset(&bar, 0, sizeof(bar));
			bar.date = 20230103;
			bar.time = bar_times[i];
			bar.open = bases[i];
			bar.high = bases[i] + 1.0;
			bar.low = bases[i] - 1.0;
			bar.close = bases[i] + 0.5;
			bar.vol = 10;
			const LMDBBarKey key(exchg, code, bar_times[i]);
			ASSERT_TRUE(query.put(std::string((const char*)&key, sizeof(key)),
				std::string((const char*)&bar, sizeof(bar))));
		}
		query.commit();
	}

	std::filesystem::path _root;
	R32BaseDataMgr* _bd = NULL;
};

TEST_F(R32AdResidualTest, R32WriterFactoryInitReleaseAndNullTolerantDelete)
{
	IDataWriter* writer = createWriter();
	ASSERT_TRUE(writer != NULL);
	// 工厂产物必须是 AD 这一族（WtDataWriterAD.cpp:36-40）
	EXPECT_TRUE(dynamic_cast<WtDataWriterAD*>(writer) != NULL);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	cfg->append("groupsize", (uint32_t)1000);
	cfg->append("disabletick", true);
	cfg->append("disablemin1", true);
	cfg->append("disablemin5", true);
	cfg->append("disableday", true);
	R32WriterSink sink(_bd);
	// 全禁用 + 不 writeTick：pushTask 的 _async_task 残值分支（R4-BUG-01）不会被读到
	ASSERT_TRUE(writer->init(cfg, &sink));
	cfg->release();

	// 四个缓存块文件在工厂产物上同样落盘（loadCache 新建分支）
	const char* names[] = {"cache_tick.dmb", "cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb"};
	for (const char* name : names)
		EXPECT_TRUE(std::filesystem::exists(_root / name)) << name;

	// getCurTick：合约不在基础数据 → L847-849 NULL；登记后缓存为空 → L854-855 NULL
	EXPECT_TRUE(writer->getCurTick("rb2301", "EXF1") == NULL);
	WTSCommodityInfo* comm = _bd->add_commodity("EXF1", "rb2301");
	_bd->add_contract("EXF1", "rb2301", comm);
	WTSTickData* cur = writer->getCurTick("rb2301", "EXF1");
	EXPECT_TRUE(cur == NULL);

	// release：_task_thrd 为空 shared_ptr → 只置 _terminated（L108-113 的无分支）
	writer->release();

	deleteWriter(writer);
	// deleteWriter 按引用置 NULL（WtDataWriterAD.cpp:42-50）
	EXPECT_TRUE(writer == NULL);
	IDataWriter* nil = NULL;
	deleteWriter(nil);   // :44 的假分支，不崩即通过
}

TEST_F(R32AdResidualTest, R32ReaderFactoryAndZeroEtimeKlineSliceUsesSinkClock)
{
	// 与 r4_reader_ad_slices_test.cpp 相同形态的两段式代码 + add_stock 现货品种，
	// 让 extractStdCode 得到 exchg=EXF2 / code=rb2301 / product=rb2301
	seed_bars("min1", "EXF2", "rb2301",
		{ 3301030931u, 3301030932u, 3301030933u },
		{ 3900.0, 3910.0, 3920.0 });

	IDataReader* reader = createDataReader();
	ASSERT_TRUE(reader != NULL);
	EXPECT_TRUE(dynamic_cast<WtDataReaderAD*>(reader) != NULL);

	R32ReaderSink sink(_bd);
	// 现货品种（isFuture()==false → 不碰 _hot_mgr）
	WTSCommodityInfo* comm = _bd->add_commodity("EXF2", "rb2301");
	_bd->add_contract("EXF2", "rb2301", comm);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _root.string().c_str());
	reader->init(cfg, &sink, NULL);
	cfg->release();
	EXPECT_EQ(sink.count_of(LL_INFO), 1u);

	// etime==0 → L375-382 用 sink 时钟补齐：20230103*1e9 + 1000*100000 + 0
	// K 线时间折算（L426）= (20230103-19900000)*10000 + 1000 = 3301031000，
	// 三根 bar（931/932/933）全部早于该时刻 → 回源 3 根、增量更新 0 根新 bar
	WTSKlineSlice* slice = reader->readKlineSlice("EXF2.rb2301", KP_Minute1, 3, 0);
	ASSERT_TRUE(slice != NULL);
	ASSERT_EQ(slice->get_block_counts(), (std::size_t)1);
	ASSERT_EQ(slice->get_block_size(0), 3u);
	const WTSBarStruct* out = (const WTSBarStruct*)slice->get_block_addr(0);
	ASSERT_TRUE(out != NULL);
	EXPECT_EQ(out[0].time, (uint64_t)3301030931u);
	EXPECT_DOUBLE_EQ(out[0].open, 3900.0);
	EXPECT_EQ(out[2].time, (uint64_t)3301030933u);
	slice->release();

	// 回源日志（WtDataReaderAD.cpp:265，PERIOD_NAME[1]=="min1"，WTSTypes.h:115-123）
	EXPECT_NE(sink.last_log(LL_DEBUG).find("min1 bars of EXF2.rb2301"), std::string::npos)
		<< sink.last_log(LL_DEBUG);
	// 没有手写 cache_m1.dmb → get_rt_cache_bar 走「文件不存在」早退，不补线
	EXPECT_EQ(sink.bar_calls(), 0u);

	deleteDataReader(reader);
	// 只验证入口不崩（WtDataReaderAD.cpp:40-45）
	deleteDataReader(NULL);
}

}  // namespace
