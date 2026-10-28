// R122 / WtDataReader：RT 块跨自然日比较器、etime==0 默认时钟、
// future+ruleTag 原始码映射、缺失的 RT/历史文件回退。
//
// 覆盖（src/WtDataStorage/WtDataReader.cpp）：
//  - readTickSlice       RT lambda 跨 action_date :373；回退 :382-385
//  - readOrdQueSlice     etime==0 :466-472；future+ruleTag :488-492；
//                        RT lambda :509；回退 :518-521；RT 缺失 :503；历史缺失 :554
//  - readOrdDtlSlice     同型：:623-629 / :645-649 / :666 / :675-678 / :660 / :711
//  - readTransSlice      同型：:780-786 / :802-806 / :823 / :832-835 / :817 / :868
//
// 夹具沿用 q03_storage3_fixture.h：cfg 绝对路径注入、零线程、零访问权限宏。
#include "q03_storage3_fixture.h"

#include "WtDataStorage/WtDataReader.h"

namespace q03 {

// 只覆写 ruleTag 识别，让 SHFE.rb.HOT 走到 getCustomRawCode 分支
class R122RuleHotMgr : public HotMgr
{
public:
	const char* getRuleTag(const char* ext) override
	{
		if(ext != NULL && strcmp(ext, "HOT") == 0)
			return "HOT";
		return "";
	}

	const char* getCustomRawCode(const char*, const char*, uint32_t) override
	{
		_raw = "rb2210";
		return _raw.c_str();
	}

private:
	string	_raw;
};

class R122ReaderRtBranchesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r122rd"));
		_root = _sandbox->root_abs();

		string cfgfile = _root + "reader.json";
		string json = fmtutil::format("{{\"path\":\"{}\",\"adjust_flag\":0}}", _root);
		ASSERT_TRUE(write_bytes(cfgfile, json));

		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_TRUE(cfg != NULL);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	string		_root;
	BaseDataMgr	_bd;
	R122RuleHotMgr	_hot;
	ReaderSink	_sink{ &_bd, &_hot };
	WtDataReader	_reader;
};

// WtDataReader.cpp: L363-390
TEST_F(R122ReaderRtBranchesTest, R122ReaderTickRtCrossDateAndFallback)
{
	vector<WTSTickStruct> ticks;
	ticks.push_back(make_tick(20240110, 20240109, 140000000, 1));
	ticks.push_back(make_tick(20240110, 20240109, 143000000, 2));
	ticks.push_back(make_tick(20240110, 20240110, 93000000, 3));
	ticks.push_back(make_tick(20240110, 20240110, 103000000, 4));
	ASSERT_TRUE(write_bytes("rt/ticks/SSE/600800.dmb",
		make_rt_block<WTSTickStruct>(BT_RT_Ticks, 20240110, (uint32_t)ticks.size(), 8, ticks)));

	// 截止 20240110 09:00，lower_bound 的中点落在 20240109，命中跨日比较后回退一条
	WTSTickSlice* s1 = _reader.readTickSlice("SSE.STK.600800", 10, 20240110090000000ull);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->get_block_counts(), 1u);
	WTSTickStruct* base = s1->get_block_addr(0);
	ASSERT_TRUE(base != NULL);
	EXPECT_EQ(s1->get_block_size(0), 2u);
	EXPECT_EQ(base[0].action_date, 20240109u);
	EXPECT_EQ(base[0].action_time, 140000000u);
	EXPECT_EQ(base[1].action_time, 143000000u);
	s1->release();

	// etime==0 走 sink 的固定时钟（20240110 15:00），四条全部命中
	WTSTickSlice* s2 = _reader.readTickSlice("SSE.STK.600800", 10, 0);
	ASSERT_TRUE(s2 != NULL);
	EXPECT_EQ(s2->get_block_size(0), 4u);
	s2->release();

	// 当日 RT 文件缺失
	EXPECT_TRUE(_reader.readTickSlice("SSE.STK.600899", 5, 20240110100000000ull) == NULL);
}

// WtDataReader.cpp: L458-610
TEST_F(R122ReaderRtBranchesTest, R122ReaderOrdQueRtEtimeZeroHotAndMissing)
{
	vector<WTSOrdQueStruct> que;
	que.push_back(make_ordque(20240110, 20240109, 140000000, 11));
	que.push_back(make_ordque(20240110, 20240109, 143000000, 12));
	que.push_back(make_ordque(20240110, 20240110, 93000000, 13));
	que.push_back(make_ordque(20240110, 20240110, 103000000, 14));
	ASSERT_TRUE(write_bytes("rt/queue/SHFE/rb2210.dmb",
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, 20240110, (uint32_t)que.size(), 8, que)));

	// etime==0：读 sink 固定时钟（15:00），同时命中 future+ruleTag 的原始码映射
	WTSOrdQueSlice* s1 = _reader.readOrdQueSlice("SHFE.rb.HOT", 10, 0);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 4u);
	s1->release();

	// 截止 09:00：中点落在 20240109，命中跨日比较并回退
	WTSOrdQueSlice* s2 = _reader.readOrdQueSlice("SHFE.rb.HOT", 10, 20240110090000000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 2u);
	EXPECT_EQ(s2->at(0)->action_date, 20240109u);
	EXPECT_EQ(s2->at(0)->action_time, 140000000u);
	EXPECT_EQ(s2->at(1)->action_time, 143000000u);
	s2->release();

	// 当日无 RT 块
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600899", 5, 20240110100000000ull) == NULL);

	// 非当日且历史文件不存在
	EXPECT_TRUE(_reader.readOrdQueSlice("SSE.STK.600898", 5, 20240108100000000ull) == NULL);
}

// WtDataReader.cpp: L615-769
TEST_F(R122ReaderRtBranchesTest, R122ReaderOrdDtlRtEtimeZeroHotAndMissing)
{
	vector<WTSOrdDtlStruct> dtl;
	dtl.push_back(make_orddtl(20240110, 20240109, 140000000, 21));
	dtl.push_back(make_orddtl(20240110, 20240109, 143000000, 22));
	dtl.push_back(make_orddtl(20240110, 20240110, 93000000, 23));
	dtl.push_back(make_orddtl(20240110, 20240110, 103000000, 24));
	ASSERT_TRUE(write_bytes("rt/orders/SHFE/rb2210.dmb",
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, 20240110, (uint32_t)dtl.size(), 8, dtl)));

	WTSOrdDtlSlice* s1 = _reader.readOrdDtlSlice("SHFE.rb.HOT", 10, 0);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 4u);
	s1->release();

	WTSOrdDtlSlice* s2 = _reader.readOrdDtlSlice("SHFE.rb.HOT", 10, 20240110090000000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 2u);
	EXPECT_EQ(s2->at(0)->action_date, 20240109u);
	EXPECT_EQ(s2->at(0)->action_time, 140000000u);
	s2->release();

	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.600899", 5, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdDtlSlice("SSE.STK.600898", 5, 20240108100000000ull) == NULL);
}

// WtDataReader.cpp: L772-926
TEST_F(R122ReaderRtBranchesTest, R122ReaderTransRtEtimeZeroHotAndMissing)
{
	vector<WTSTransStruct> trs;
	trs.push_back(make_trans(20240110, 20240109, 140000000, 31));
	trs.push_back(make_trans(20240110, 20240109, 143000000, 32));
	trs.push_back(make_trans(20240110, 20240110, 93000000, 33));
	trs.push_back(make_trans(20240110, 20240110, 103000000, 34));
	ASSERT_TRUE(write_bytes("rt/trans/SHFE/rb2210.dmb",
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, 20240110, (uint32_t)trs.size(), 8, trs)));

	WTSTransSlice* s1 = _reader.readTransSlice("SHFE.rb.HOT", 10, 0);
	ASSERT_TRUE(s1 != NULL);
	ASSERT_EQ(s1->size(), 4u);
	s1->release();

	WTSTransSlice* s2 = _reader.readTransSlice("SHFE.rb.HOT", 10, 20240110090000000ull);
	ASSERT_TRUE(s2 != NULL);
	ASSERT_EQ(s2->size(), 2u);
	EXPECT_EQ(s2->at(0)->action_date, 20240109u);
	EXPECT_EQ(s2->at(0)->action_time, 140000000u);
	s2->release();

	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.600899", 5, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSlice("SSE.STK.600898", 5, 20240108100000000ull) == NULL);
}

} // namespace q03
