// r94 / WtRdmDtReader：当日逐笔区间读取时，主力连续代码必须解析到自定义主力合约
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange 的 commInfo->isFuture()/ruleTag -> getCustomRawCode :635-641
//  - readOrdDtlSliceByRange 同型分支                                     :810-816
//  - readTransSliceByRange  同型分支                                     :984-990
//
// 用 r74::HotMgr（getRuleTag 能识别第三段 HOT）让 CodeHelper 真正写入 _ruletag；
// rt 块只放在 rb2401 名下，若未走 getCustomRawCode，读取会因找不到 rb.HOT 文件而返回 NULL。
// init(NULL) 不起巡检线程；数据全部在 q03::Sandbox 临时目录内。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

string rt_hft_file(const char* kind, const char* code = "rb2401")
{
	return fmtutil::format("rt/{}/SHFE/{}.dmb", kind, code);
}

} // namespace

class R94RdmHotHftTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94hot"));
		_bd.set_cur_tdate(kCurTDate);
		_hot.set_enabled(true);
		_hot.set_raw("rb2401");
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	r74::HotMgr			_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: L635-641 / L810-816 / L984-990
TEST_F(R94RdmHotHftTest, R94RdmTodayHftUsesCustomRawCode)
{
	vector<WTSOrdQueStruct> ques;
	ques.push_back(make_ordque(kCurTDate, kCurTDate, 93000000, 1));
	ques.push_back(make_ordque(kCurTDate, kCurTDate, 93100000, 2));
	ASSERT_TRUE(write_bytes(rt_hft_file("queue"),
		make_rt_block<WTSOrdQueStruct>(BT_RT_OrdQueue, kCurTDate, 2, 8, ques)));

	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93000000, 11));
	dtls.push_back(make_orddtl(kCurTDate, kCurTDate, 93100000, 12));
	ASSERT_TRUE(write_bytes(rt_hft_file("orders"),
		make_rt_block<WTSOrdDtlStruct>(BT_RT_OrdDetail, kCurTDate, 2, 8, dtls)));

	vector<WTSTransStruct> trs;
	trs.push_back(make_trans(kCurTDate, kCurTDate, 93000000, 21));
	trs.push_back(make_trans(kCurTDate, kCurTDate, 93100000, 22));
	ASSERT_TRUE(write_bytes(rt_hft_file("trans"),
		make_rt_block<WTSTransStruct>(BT_RT_Trnsctn, kCurTDate, 2, 8, trs)));

	WTSOrdQueSlice* sq = _reader.readOrdQueSliceByRange("SHFE.rb.HOT",
		20240110093000000ull, 20240110093200000ull);
	ASSERT_TRUE(sq != NULL);
	ASSERT_EQ(sq->size(), 2u);
	EXPECT_DOUBLE_EQ(sq->at(0)->price, ques[0].price);
	EXPECT_DOUBLE_EQ(sq->at(1)->price, ques[1].price);
	sq->release();

	WTSOrdDtlSlice* sd = _reader.readOrdDtlSliceByRange("SHFE.rb.HOT",
		20240110093000000ull, 20240110093200000ull);
	ASSERT_TRUE(sd != NULL);
	ASSERT_EQ(sd->size(), 2u);
	EXPECT_EQ(sd->at(1)->index, dtls[1].index);
	sd->release();

	WTSTransSlice* st = _reader.readTransSliceByRange("SHFE.rb.HOT",
		20240110093000000ull, 20240110093200000ull);
	ASSERT_TRUE(st != NULL);
	ASSERT_EQ(st->size(), 2u);
	EXPECT_EQ(st->at(1)->askorder, trs[1].askorder);
	st->release();
}

} // namespace r94
