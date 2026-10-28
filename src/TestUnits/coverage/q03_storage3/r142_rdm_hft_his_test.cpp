// r142 / WtRdmDtReader：历史逐笔比较器的跨 action_date 分支、缺失/短文件守卫、
//       历史 K 线右界回退
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readOrdQueSliceByRange  历史 lambda 日期分支 :750 / :774
//  - readOrdDtlSliceByRange  历史 lambda 日期分支 :925 / :948
//  - readTransSliceByRange   历史 lambda 日期分支 :1099 / :1122
//  - readOrdDtlSliceByRange  缺失 RT 文件 :831；缺失历史文件 :883
//  - readTransSliceByRange   缺失 RT 文件 :1005；缺失历史文件 :1057
//  - readOrdQueSliceByRange  空历史块第二次访问 :739-740
//  - indexBarFromCacheByRange 历史右界落在两根之间回退 :1760-1766
//
// 历史逐笔载荷用 r142_fixture.h 的不可压缩样本，避开 BUG-R132-001 同型的
// 「resize+append 后旧头部悬垂写」；RT 缺失路径不读文件，短文件在长度校验处
// 直接返回。全部在 q03::Sandbox 临时目录，init(NULL) 不起 5 秒巡检线程。
#include "r142_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r142 {

using namespace q03;
using std::string;
using std::vector;

class R142RdmHftHisTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r142rdm"));
		_bd.set_cur_tdate(20240110);
		_reader.init(NULL, &_sink);
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	BaseDataMgr			_bd;
	HotMgr				_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: readOrdQueSliceByRange L748-782（历史比较器 L750 / L774）
TEST_F(R142RdmHftHisTest, R142RdmHisQueueCrossActionDateByRange)
{
	const uint32_t kT = 20240109;
	const uint32_t kPrev = 20240108;

	vector<WTSOrdQueStruct> items;
	items.push_back(noisy_ordque(kT, kPrev, 210000000, 1));	// 前一日夜盘
	items.push_back(noisy_ordque(kT, kT, 93000000, 4));
	items.push_back(noisy_ordque(kT, kT, 103000000, 7));
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610801.dsb",
		make_block(BT_HIS_OrdQueue, BLOCK_VERSION_CMP_V2, pack_items(items))));

	// 同一交易日窗口 [09:00, 11:00]：eTick/sTick 的 action_date == 0109，
	// 块内首条 action_date == 0108，两个 lower_bound 都命中日期分支。
	WTSOrdQueSlice* s = _reader.readOrdQueSliceByRange("SSE.STK.610801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 2u);
	EXPECT_EQ(s->at(0)->action_date, kT);
	EXPECT_EQ(s->at(0)->action_time, 93000000u);
	EXPECT_EQ(s->at(1)->action_time, 103000000u);
	s->release();
}

// WtRdmDtReader.cpp: readOrdDtlSliceByRange L923-948 / readTransSliceByRange L1097-1122
TEST_F(R142RdmHftHisTest, R142RdmHisDetailTransCrossActionDateByRange)
{
	const uint32_t kT = 20240109;
	const uint32_t kPrev = 20240108;

	vector<WTSOrdDtlStruct> dtls;
	dtls.push_back(noisy_orddtl(kT, kPrev, 210000000, 11));
	dtls.push_back(noisy_orddtl(kT, kT, 93000000, 14));
	dtls.push_back(noisy_orddtl(kT, kT, 103000000, 17));
	ASSERT_TRUE(write_bytes("his/orders/SSE/20240109/620801.dsb",
		make_block(BT_HIS_OrdDetail, BLOCK_VERSION_CMP_V2, pack_items(dtls))));

	vector<WTSTransStruct> trans;
	trans.push_back(noisy_trans(kT, kPrev, 210000000, 21));
	trans.push_back(noisy_trans(kT, kT, 93000000, 24));
	trans.push_back(noisy_trans(kT, kT, 103000000, 27));
	ASSERT_TRUE(write_bytes("his/trans/SSE/20240109/630801.dsb",
		make_block(BT_HIS_Trnsctn, BLOCK_VERSION_CMP_V2, pack_items(trans))));

	WTSOrdDtlSlice* d = _reader.readOrdDtlSliceByRange("SSE.STK.620801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(d != NULL);
	ASSERT_EQ(d->size(), 2u);
	EXPECT_EQ(d->at(0)->action_date, kT);
	EXPECT_EQ(d->at(0)->action_time, 93000000u);
	d->release();

	WTSTransSlice* t = _reader.readTransSliceByRange("SSE.STK.630801",
		20240109090000000ull, 20240109110000000ull);
	ASSERT_TRUE(t != NULL);
	ASSERT_EQ(t->size(), 2u);
	EXPECT_EQ(t->at(0)->action_date, kT);
	EXPECT_EQ(t->at(0)->action_time, 93000000u);
	t->release();
}

// WtRdmDtReader.cpp: L831 / L883 / L1005 / L1057（四类缺失文件的早退）
TEST_F(R142RdmHftHisTest, R142RdmMissingRtAndHisHftFiles)
{
	// 当日 RT 文件缺失：isToday 分支的 getRT*Block 返回 NULL
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620802",
		20240110090000000ull, 20240110100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630802",
		20240110090000000ull, 20240110100000000ull) == NULL);

	// 非当日历史文件缺失：his/orders|trans/<exchg>/<date>/<code>.dsb
	EXPECT_TRUE(_reader.readOrdDtlSliceByRange("SSE.STK.620803",
		20240109090000000ull, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readTransSliceByRange("SSE.STK.630803",
		20240109090000000ull, 20240109100000000ull) == NULL);
}

// WtRdmDtReader.cpp: L711-716 长度校验 → L738-740 空块第二次访问
TEST_F(R142RdmHftHisTest, R142RdmEmptyQueueBlockSecondCallReturnsNull)
{
	ASSERT_TRUE(write_bytes("his/queue/SSE/20240109/610804.dsb", string()));

	// 第一次：文件载荷短于 HisOrdQueBlockV2，清空缓存并返回；
	// 第二次：缓存项已存在但 _block 为空 -> L739-740。
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610804",
		20240109090000000ull, 20240109100000000ull) == NULL);
	EXPECT_TRUE(_reader.readOrdQueSliceByRange("SSE.STK.610804",
		20240109090000000ull, 20240109100000000ull) == NULL);
}

// WtRdmDtReader.cpp: indexBarFromCacheByRange L1756-1766
TEST_F(R142RdmHftHisTest, R142RdmKlineHisEndBetweenBarsBacksOff)
{
	vector<WTSBarStruct> bars;
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 931), 1));
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 933), 2));
	bars.push_back(make_bar(20240110, TimeUtils::timeToMinBar(20240110, 935), 3));
	ASSERT_TRUE(write_bytes("his/min1/SSE/600805.dsb",
		make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(bars))));

	// 截止 09:32 落在 09:31 与 09:33 之间：lower_bound 命中 09:33 后必须回退，
	// 结果只含 09:31。
	WTSKlineSlice* s = _reader.readKlineSliceByRange("SSE.STK.600805", KP_Minute1,
		202401100930ull, 202401100932ull);
	ASSERT_TRUE(s != NULL);
	ASSERT_EQ(s->size(), 1u);
	EXPECT_EQ(s->at(0)->time, TimeUtils::timeToMinBar(20240110, 931));
	s->release();
}

} // namespace r142
