// r94 / WtRdmDtReader：后复权（exright==2）时按区间读 K 线的实时缓存合并链
//
// 覆盖（src/WtDataStorage/WtRdmDtReader.cpp）：
//  - readKlineSliceByRange 的 `cInfo._exright == 2` 分支 :1909-1981
//      :1913-1919 取 RT 块/缓存长度
//      :1922-1941 增量复制到 _rt_bars 并按 barList._factor 复权（oldSize!=0 的 idx-- 分支）
//      :1945-1964 尾定位 + 首条晚于左界时整段返回
//      :1968-1979 左界落在 RT 段内部时二分起点
//
// 先用 ByCount 预热 _bars_cache（R52 已覆盖 ByCount 的 HFQ 分支），再扩 RT 块后用
// 公开的 ByRange 入口读取；不触发 R52-003（oldSize==0 首次 memcpy 多写一条）。
#include "r74_rdm_fixture.h"

#include "WtDataStorage/WtRdmDtReader.h"

namespace r94 {

using namespace q03;
using std::string;
using std::vector;

namespace {

const uint32_t kCurTDate = 20240110;

} // namespace

class R94RdmHfqRangeTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new Sandbox("r94hfq"));
		_bd.set_cur_tdate(kCurTDate);
		_hot.set_enabled(true);
		_hot.set_raw("rb2402");
		_hot.add_section("rb2402", 20240104, 20240110, 1.2);
		_reader.init(NULL, &_sink);

		// HOT+ 包裹文件：0104/0105 两根
		vector<WTSBarStruct> hot;
		hot.push_back(make_bar(20240104, r52::min_bar_time(20240104, 930), 1));
		hot.push_back(make_bar(20240105, r52::min_bar_time(20240105, 930), 2));
		ASSERT_TRUE(write_bytes("his/min1/SHFE/SHFE.rb_HOT+.dsb",
			make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(hot))));

		// 分月合约：0106/0107 两根，切段按 1.2 复权
		vector<WTSBarStruct> seg;
		seg.push_back(make_bar(20240106, r52::min_bar_time(20240106, 930), 11));
		seg.push_back(make_bar(20240107, r52::min_bar_time(20240107, 930), 12));
		ASSERT_TRUE(write_bytes("his/min1/SHFE/rb2402.dsb",
			make_block(BT_HIS_Minute1, BLOCK_VERSION_RAW_V2, pack_bars(seg))));

		// 当日 RT 两根（capacity=8）
		vector<WTSBarStruct> rt;
		rt.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930), 30));
		rt.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 931), 31));
		ASSERT_TRUE(write_bytes("rt/min1/SHFE/rb2402.dmb",
			make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 2, 8, rt)));
	}

	void TearDown() override
	{
		_reader.clearCache();
		_sandbox.reset();
	}

	std::unique_ptr<Sandbox>	_sandbox;
	r52::BDMgr			_bd;
	r74::HotMgr			_hot;
	RdmSink				_sink{ &_bd, &_hot };
	WtRdmDtReader			_reader;
};

// WtRdmDtReader.cpp: L1909-1981
TEST_F(R94RdmHfqRangeTest, R94RdmHfqRangeMergesRealtimeIntoAdjustedCache)
{
	// 预热：_rt_bars 首次填充 2 条（R52 已覆盖 ByCount 的 HFQ 分支）
	WTSKlineSlice* seed = _reader.readKlineSliceByCount("SHFE.rb.HOT+", KP_Minute1, 3,
		202401100931ull);
	ASSERT_TRUE(seed != NULL);
	EXPECT_EQ(r52::flatten_bars(seed).size(), (std::size_t)3);
	seed->release();

	// 写侧把 RT 块扩到 3 条（capacity 不变，仅 _size/数据变化）
	vector<WTSBarStruct> rt;
	rt.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 930), 30));
	rt.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 931), 31));
	rt.push_back(make_bar(kCurTDate, r52::min_bar_time(kCurTDate, 932), 32));
	ASSERT_TRUE(write_bytes("rt/min1/SHFE/rb2402.dmb",
		make_rt_block<WTSBarStruct>(BT_RT_Minute1, kCurTDate, 3, 8, rt)));

	// 左界早于 RT 首条：RT 段整体返回，且历史段按区间拼接
	WTSKlineSlice* all = _reader.readKlineSliceByRange("SHFE.rb.HOT+", KP_Minute1,
		202401010000ull, 202401100931ull);
	ASSERT_TRUE(all != NULL);
	const vector<WTSBarStruct> got = r52::flatten_bars(all);
	ASSERT_GE(got.size(), (std::size_t)2);
	EXPECT_EQ(got.back().date, kCurTDate);
	EXPECT_DOUBLE_EQ(got.back().open, 41.0 * 1.2);
	EXPECT_DOUBLE_EQ(got[got.size() - 2].open, 40.0 * 1.2);
	all->release();

	// 左界落在 RT 段内部：走 else 分支，history 不再拼接
	WTSKlineSlice* rtOnly = _reader.readKlineSliceByRange("SHFE.rb.HOT+", KP_Minute1,
		202401100930ull, 202401100931ull);
	ASSERT_TRUE(rtOnly != NULL);
	const vector<WTSBarStruct> rgot = r52::flatten_bars(rtOnly);
	ASSERT_EQ(rgot.size(), (std::size_t)2);
	EXPECT_EQ(rgot[0].time, r52::min_bar_time(kCurTDate, 930));
	EXPECT_EQ(rgot[1].time, r52::min_bar_time(kCurTDate, 931));
	EXPECT_DOUBLE_EQ(rgot[0].open, 40.0 * 1.2);
	EXPECT_DOUBLE_EQ(rgot[1].open, 41.0 * 1.2);
	rtOnly->release();
}

} // namespace r94
