// 迭代03 s5 storage-read 第二轮 / WtDataStorage 读端（WtRdmDtReader、WtDataReader）。
//
//  - BUG-R132-001：历史块 12 字节打包头使 `_buffer` 内的记录按 4 字节偏移，直接以
//    记录类型访问是未对齐 UB。经公开文件读取路径（CMP_V2 历史块）逐字段核对
//    tick/委托队列/委托明细/成交，配合 UBSan alignment 构建作红绿。
//  - I03-NEW-052：未知品种（getCommodity 返回 NULL）时各入口不得先解引用，返回空并记 ERROR。
//  - I03-NEW-039：缓存键拼接调用方 stdCode；63 字节的合法 stdCode 加 "-YYYYMMDD" 超过旧的
//    64 字节固定缓冲。此处只能做功能回归（thread_local 缓冲不受 ASan 插桩），红控见证据说明。
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtDataReader.h"
#include "WtDataStorage/WtRdmDtReader.h"
#include "WTSUtils/WTSCfgLoader.h"

namespace s5sg {

using namespace q03;

// Commodity lookups for product "NOPE" fail, everything else behaves like q03::BaseDataMgr.
class UnknownCommodityBdMgr : public BaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		if (exchgpid != NULL && std::string(exchgpid).find("NOPE") != std::string::npos)
			return NULL;
		return BaseDataMgr::getCommodity(exchgpid);
	}
	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		if (pid != NULL && std::strcmp(pid, "NOPE") == 0)
			return NULL;
		return BaseDataMgr::getCommodity(exchg, pid);
	}
};

class LoggingReaderSink : public IDataReaderSink
{
public:
	LoggingReaderSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~LoggingReaderSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void on_bar(const char*, WTSKlinePeriod, WTSBarStruct*) override {}
	void on_all_bar_updated(uint32_t) override {}
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		if (ll == LL_ERROR && message != NULL)
			_errors.push_back(message);
	}
	uint32_t get_date() override { return 20240110; }
	uint32_t get_min_time() override { return 1000; }
	uint32_t get_secs() override { return 0; }
	uint32_t errors_containing(const char* sub) const
	{
		uint32_t n = 0;
		for (const std::string& e : _errors)
			if (e.find(sub) != std::string::npos)
				++n;
		return n;
	}

private:
	IBaseDataMgr* _bd;
	IHotMgr* _hot;
	std::vector<std::string> _errors;
};

class LoggingRdmSink : public IRdmDtReaderSink
{
public:
	LoggingRdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~LoggingRdmSink() {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		if (ll == LL_ERROR && message != NULL)
			_errors.push_back(message);
	}
	uint32_t errors_containing(const char* sub) const
	{
		uint32_t n = 0;
		for (const std::string& e : _errors)
			if (e.find(sub) != std::string::npos)
				++n;
		return n;
	}

private:
	IBaseDataMgr* _bd;
	IHotMgr* _hot;
	std::vector<std::string> _errors;
};

static const uint32_t kHisDate = 20240109;	// q03::BaseDataMgr current trading date is 20240110

class S5StorageReaderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("s5sg"));
		_root = _sandbox->root_abs();
	}
	void TearDown() override { _sandbox.reset(); }

	WTSVariant* reader_cfg()
	{
		const string cfgfile = _root + "reader.json";
		EXPECT_TRUE(write_bytes(cfgfile, "{\"path\":\"" + _root + "\",\"adjust_flag\":0}"));
		return WTSCfgLoader::load_from_file(cfgfile.c_str());
	}

	std::unique_ptr<Sandbox> _sandbox;
	string _root;
};

//////////////////////////////////////////////////////////////////////////
// BUG-R132-001：历史块记录经对齐副本读取（UBSan alignment 构建下改前中止）
//////////////////////////////////////////////////////////////////////////
TEST_F(S5StorageReaderTest, RdmHistoryHftRecordsAreReadThroughAlignedStorage)
{
	const uint32_t at = 100000000u;	// 10:00:00.000
	vector<WTSOrdQueStruct> ques{ make_ordque(kHisDate, kHisDate, at, 1), make_ordque(kHisDate, kHisDate, at + 1000, 2) };
	vector<WTSOrdDtlStruct> dtls{ make_orddtl(kHisDate, kHisDate, at, 3), make_orddtl(kHisDate, kHisDate, at + 1000, 4) };
	vector<WTSTransStruct> trans{ make_trans(kHisDate, kHisDate, at, 5), make_trans(kHisDate, kHisDate, at + 1000, 6) };
	// Separate codes: readOrdDtl/readTrans share the ordque cache lookup (BUG-R74-001).
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/600001.dsb", make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(ques))));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/600002.dsb", make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtls))));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/600003.dsb", make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));

	BaseDataMgr bd;
	HotMgr hot;
	RdmSink sink(&bd, &hot);
	WtRdmDtReader reader;
	reader.init(NULL, &sink);

	const uint64_t stime = (uint64_t)kHisDate * 1000000000ull + 90000000ull;
	const uint64_t etime = (uint64_t)kHisDate * 1000000000ull + 110000000ull;
	WTSOrdQueSlice* q = reader.readOrdQueSliceByRange("SSE.600001", stime, etime);
	ASSERT_TRUE(q != NULL);
	ASSERT_EQ(q->size(), 2u);
	EXPECT_EQ(0u, (uintptr_t)q->at(0) % alignof(WTSOrdQueStruct));
	EXPECT_EQ(q->at(1)->action_time, at + 1000);
	EXPECT_DOUBLE_EQ(q->at(1)->price, ques[1].price);
	q->release();

	WTSOrdDtlSlice* d = reader.readOrdDtlSliceByRange("SSE.600002", stime, etime);
	ASSERT_TRUE(d != NULL);
	ASSERT_EQ(d->size(), 2u);
	EXPECT_EQ(0u, (uintptr_t)d->at(0) % alignof(WTSOrdDtlStruct));
	EXPECT_DOUBLE_EQ(d->at(0)->price, dtls[0].price);
	d->release();

	WTSTransSlice* t = reader.readTransSliceByRange("SSE.600003", stime, etime);
	ASSERT_TRUE(t != NULL);
	ASSERT_EQ(t->size(), 2u);
	EXPECT_EQ(0u, (uintptr_t)t->at(0) % alignof(WTSTransStruct));
	EXPECT_DOUBLE_EQ(t->at(1)->price, trans[1].price);
	t->release();
}

TEST_F(S5StorageReaderTest, RdmAndDataReaderHistoryTicksAreReadThroughAlignedStorage)
{
	vector<WTSTickStruct> ticks{ make_tick(kHisDate, kHisDate, 93000000, 1), make_tick(kHisDate, kHisDate, 93100000, 2),
		make_tick(kHisDate, kHisDate, 93200000, 3) };
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/600001.dsb", make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(ticks))));

	BaseDataMgr bd;
	HotMgr hot;
	{
		RdmSink sink(&bd, &hot);
		WtRdmDtReader reader;
		reader.init(NULL, &sink);
		WTSTickSlice* byDate = reader.readTickSliceByDate("SSE.STK.600001", kHisDate);
		ASSERT_TRUE(byDate != NULL);
		ASSERT_EQ(byDate->size(), 3u);
		EXPECT_EQ(0u, (uintptr_t)byDate->at(0) % alignof(WTSTickStruct));
		EXPECT_DOUBLE_EQ(byDate->at(2)->price, ticks[2].price);
		byDate->release();

		WTSTickSlice* byRange = reader.readTickSliceByRange("SSE.STK.600001",
			(uint64_t)kHisDate * 1000000000ull + 93000000ull, (uint64_t)kHisDate * 1000000000ull + 93100000ull);
		ASSERT_TRUE(byRange != NULL);
		ASSERT_EQ(byRange->size(), 2u);
		EXPECT_DOUBLE_EQ(byRange->at(1)->price, ticks[1].price);
		byRange->release();

		WTSTickSlice* byCount = reader.readTickSliceByCount("SSE.STK.600001", 2, (uint64_t)kHisDate * 1000000000ull + 150000000ull);
		ASSERT_TRUE(byCount != NULL);
		ASSERT_EQ(byCount->size(), 2u);
		EXPECT_DOUBLE_EQ(byCount->at(-1)->price, ticks[2].price);
		byCount->release();
	}
	{
		ReaderSink sink(&bd, &hot);
		WtDataReader reader;
		WTSVariant* cfg = reader_cfg();
		ASSERT_TRUE(cfg != NULL);
		reader.init(cfg, &sink, NULL);
		cfg->release();
		WTSTickSlice* his = reader.readTickSlice("SSE.STK.600001", 2, (uint64_t)kHisDate * 1000000000ull + 150000000ull);
		ASSERT_TRUE(his != NULL);
		ASSERT_EQ(his->size(), 2u);
		EXPECT_EQ(0u, (uintptr_t)his->at(0) % alignof(WTSTickStruct));
		EXPECT_DOUBLE_EQ(his->at(-1)->price, ticks[2].price);
		his->release();
	}
}

//////////////////////////////////////////////////////////////////////////
// I03-NEW-052：未知品种不解引用（改前 SEGV）
//////////////////////////////////////////////////////////////////////////
TEST_F(S5StorageReaderTest, RdmReaderUnknownCommodityIsRefusedWithoutDereference)
{
	UnknownCommodityBdMgr bd;
	HotMgr hot;
	LoggingRdmSink sink(&bd, &hot);
	WtRdmDtReader reader;
	reader.init(NULL, &sink);
	const char* code = "SSE.NOPE.600001";
	const uint64_t s = (uint64_t)kHisDate * 1000000000ull + 93000000ull;
	const uint64_t e = (uint64_t)kHisDate * 1000000000ull + 150000000ull;
	EXPECT_TRUE(reader.readTickSliceByDate(code, kHisDate) == NULL);
	EXPECT_TRUE(reader.readTickSliceByRange(code, s, e) == NULL);
	EXPECT_TRUE(reader.readTickSliceByCount(code, 2, e) == NULL);
	EXPECT_TRUE(reader.readOrdQueSliceByRange(code, s, e) == NULL);
	EXPECT_TRUE(reader.readOrdDtlSliceByRange(code, s, e) == NULL);
	EXPECT_TRUE(reader.readTransSliceByRange(code, s, e) == NULL);
	EXPECT_TRUE(reader.readKlineSliceByRange(code, KP_DAY, 202401010000ull, 202401100000ull) == NULL);
	EXPECT_TRUE(reader.readKlineSliceByCount(code, KP_DAY, 2, 202401100000ull) == NULL);
	EXPECT_DOUBLE_EQ(1.0, reader.getAdjFactorByDate(code, kHisDate));
	EXPECT_GE(sink.errors_containing("not found"), 7u);
}

TEST_F(S5StorageReaderTest, DataReaderUnknownCommodityIsRefusedWithoutDereference)
{
	UnknownCommodityBdMgr bd;
	HotMgr hot;
	LoggingReaderSink sink(&bd, &hot);
	WtDataReader reader;
	WTSVariant* cfg = reader_cfg();
	ASSERT_TRUE(cfg != NULL);
	reader.init(cfg, &sink, NULL);
	cfg->release();
	const char* code = "SSE.NOPE.600001";
	const uint64_t e = (uint64_t)kHisDate * 1000000000ull + 150000000ull;
	EXPECT_TRUE(reader.readTickSlice(code, 2, e) == NULL);
	EXPECT_TRUE(reader.readOrdQueSlice(code, 2, e) == NULL);
	EXPECT_TRUE(reader.readOrdDtlSlice(code, 2, e) == NULL);
	EXPECT_TRUE(reader.readTransSlice(code, 2, e) == NULL);
	EXPECT_DOUBLE_EQ(1.0, reader.getAdjFactorByDate(code, kHisDate));
	EXPECT_GE(sink.errors_containing("not found"), 4u);
}

//////////////////////////////////////////////////////////////////////////
// I03-NEW-039：63 字节合法 stdCode 的历史 tick 缓存键（功能回归）
// DISABLED：读端缓存键已改为 std::string，但该用例在键之后由 WTSTickSlice::create
// 把 stdCode 无界复制进 32 字节 `_code`（WTSDataDef.hpp，另报新问题）而破坏堆并中止；
// 待切片 `_code` 有界化后启用。证据 runs/r2/longcode-slice-overflow.log。
//////////////////////////////////////////////////////////////////////////
TEST_F(S5StorageReaderTest, DISABLED_LongestLegalStdCodeHistoryTickKeyStaysExact)
{
	const std::string product(27, 'P');	// exchange "SSE" + '.' + 27 < 32 (stdCommID capacity)
	const std::string codeA(30, 'C');
	const std::string codeB = codeA + "D";
	ASSERT_EQ(31u, codeB.size());
	vector<WTSTickStruct> ta{ make_tick(kHisDate, kHisDate, 93000000, 1) };
	vector<WTSTickStruct> tb{ make_tick(kHisDate, kHisDate, 93000000, 2), make_tick(kHisDate, kHisDate, 93100000, 3) };
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/" + codeA + ".dsb", make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(ta))));
	ASSERT_TRUE(write_bytes("his/ticks/SSE/20240109/" + codeB + ".dsb", make_block(BT_HIS_Ticks, BLOCK_VERSION_CMP_V2, pack_ticks(tb))));

	BaseDataMgr bd;
	HotMgr hot;
	ReaderSink sink(&bd, &hot);
	WtDataReader reader;
	WTSVariant* cfg = reader_cfg();
	ASSERT_TRUE(cfg != NULL);
	reader.init(cfg, &sink, NULL);
	cfg->release();
	const std::string stdA = "SSE." + product + "." + codeA;
	const std::string stdB = "SSE." + product + "." + codeB;
	ASSERT_EQ(63u, stdB.size());
	const uint64_t e = (uint64_t)kHisDate * 1000000000ull + 150000000ull;
	WTSTickSlice* a = reader.readTickSlice(stdA.c_str(), 5, e);
	WTSTickSlice* b = reader.readTickSlice(stdB.c_str(), 5, e);
	ASSERT_TRUE(a != NULL);
	ASSERT_TRUE(b != NULL);
	EXPECT_EQ(1u, a->size());
	EXPECT_EQ(2u, b->size());
	EXPECT_DOUBLE_EQ(b->at(-1)->price, tb[1].price);
	a->release();
	b->release();
}

} // namespace s5sg
