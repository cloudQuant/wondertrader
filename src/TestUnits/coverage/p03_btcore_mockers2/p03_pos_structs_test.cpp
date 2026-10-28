/*!
 * p03_pos_structs_test.cpp
 * Lane p03: WtBtCore 三个模拟交易器持仓结构内联语义
 * 覆盖 CtaMocker/SelMocker/UftMocker 的 PosInfo/PosItem/DetailInfo/SigInfo
 * 纯内联函数: valid()/volume()/frozen()/closeprofit()/dynprofit() 与构造默认值
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "Includes/WTSContractInfo.hpp"

// 持仓嵌套结构在三个 Mocker 中均为 protected/private 成员与类型,
// 通过访问控制重定义在测试 TU 内取得可见性(clang/Itanium ABI 下不改变布局)
#define private public
#define protected public
#include "WtBtCore/CtaMocker.h"
#include "WtBtCore/SelMocker.h"
#include "WtBtCore/UftMocker.h"
#undef protected
#undef private

namespace
{
	using CtaPos = CtaMocker::PosInfo;
	using CtaDetail = CtaMocker::DetailInfo;
	using CtaSig = CtaMocker::SigInfo;
	using SelPos = SelMocker::PosInfo;
	using SelSig = SelMocker::SigInfo;
	using UftPosItem = UftMocker::PosItem;
	using UftPosInfo = UftMocker::PosInfo;
	using UftFund = UftMocker::StraFundInfo;
}

TEST(P03CtaPosInfo, DefaultMembers)
{
	CtaPos pInfo;
	EXPECT_DOUBLE_EQ(0.0, pInfo._volume);
	EXPECT_DOUBLE_EQ(0.0, pInfo._closeprofit);
	EXPECT_DOUBLE_EQ(0.0, pInfo._dynprofit);
	EXPECT_DOUBLE_EQ(0.0, pInfo._frozen);
	EXPECT_EQ(0u, pInfo._last_entertime);
	EXPECT_EQ(0u, pInfo._last_exittime);
	EXPECT_TRUE(pInfo._details.empty());
	// 空持仓的 valid() 恒为 0
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());
}

TEST(P03CtaPosInfo, ValidMinusFrozenLongAndShort)
{
	CtaPos pInfo;
	// 多头: volume 100, frozen 30 -> valid 70
	pInfo._volume = 100;
	pInfo._frozen = 30;
	EXPECT_DOUBLE_EQ(70.0, pInfo.valid());

	// 冻结全部 -> valid 0
	pInfo._frozen = 100;
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());

	// 空头: volume -10, frozen 0 -> valid -10 (内联无符号保护)
	pInfo._volume = -10;
	pInfo._frozen = 0;
	EXPECT_DOUBLE_EQ(-10.0, pInfo.valid());

	// valid() 为纯函数, 不修改状态
	(void)pInfo.valid();
	EXPECT_DOUBLE_EQ(-10.0, pInfo._volume);
	EXPECT_DOUBLE_EQ(0.0, pInfo._frozen);
}

TEST(P03CtaDetailInfo, MemsetZeroInit)
{
	CtaDetail dInfo;
	EXPECT_FALSE(dInfo._long);
	EXPECT_DOUBLE_EQ(0.0, dInfo._price);
	EXPECT_DOUBLE_EQ(0.0, dInfo._volume);
	EXPECT_EQ(0u, dInfo._opentime);
	EXPECT_EQ(0u, dInfo._opentdate);
	EXPECT_DOUBLE_EQ(0.0, dInfo._max_profit);
	EXPECT_DOUBLE_EQ(0.0, dInfo._max_loss);
	EXPECT_DOUBLE_EQ(0.0, dInfo._max_price);
	EXPECT_DOUBLE_EQ(0.0, dInfo._min_price);
	EXPECT_DOUBLE_EQ(0.0, dInfo._profit);
	EXPECT_EQ('\0', dInfo._opentag[0]);
	EXPECT_EQ(0u, dInfo._open_barno);
}

TEST(P03CtaSigInfo, DefaultZeroFields)
{
	CtaSig sInfo;
	EXPECT_DOUBLE_EQ(0.0, sInfo._volume);
	EXPECT_DOUBLE_EQ(0.0, sInfo._sigprice);
	EXPECT_DOUBLE_EQ(0.0, sInfo._desprice);
	EXPECT_EQ(0u, sInfo._sigtype);
	EXPECT_EQ(0u, sInfo._gentime);
	EXPECT_TRUE(sInfo._usertag.empty());

	sInfo._volume = -3.0;
	sInfo._usertag = "short_tag";
	EXPECT_DOUBLE_EQ(-3.0, sInfo._volume);
	EXPECT_EQ("short_tag", sInfo._usertag);
}

TEST(P03SelPosInfo, DefaultAndValid)
{
	SelPos pInfo;
	EXPECT_DOUBLE_EQ(0.0, pInfo._volume);
	EXPECT_DOUBLE_EQ(0.0, pInfo._closeprofit);
	EXPECT_DOUBLE_EQ(0.0, pInfo._dynprofit);
	EXPECT_DOUBLE_EQ(0.0, pInfo._frozen);
	EXPECT_EQ(0u, pInfo._last_entertime);
	EXPECT_EQ(0u, pInfo._last_exittime);
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());

	// T+1 语义: 当日买入 500 冻结, valid 为 0
	pInfo._volume = 500;
	pInfo._frozen = 500;
	EXPECT_DOUBLE_EQ(0.0, pInfo.valid());

	// 次日解冻
	pInfo._frozen = 0;
	EXPECT_DOUBLE_EQ(500.0, pInfo.valid());
}

TEST(P03SelSigInfo, TriggeredDefaultsFalse)
{
	SelSig sInfo;
	EXPECT_DOUBLE_EQ(0.0, sInfo._volume);
	EXPECT_DOUBLE_EQ(0.0, sInfo._sigprice);
	EXPECT_DOUBLE_EQ(0.0, sInfo._desprice);
	EXPECT_FALSE(sInfo._triggered);
	EXPECT_EQ(0u, sInfo._gentime);
	EXPECT_TRUE(sInfo._usertag.empty());

	sInfo._triggered = true;
	EXPECT_TRUE(sInfo._triggered);
}

TEST(P03UftPosItem, DefaultCounters)
{
	UftPosItem item;
	// 构造函数显式清零四个量字段与两个盈亏字段
	EXPECT_DOUBLE_EQ(0.0, item._prevol);
	EXPECT_DOUBLE_EQ(0.0, item._newvol);
	EXPECT_DOUBLE_EQ(0.0, item._preavail);
	EXPECT_DOUBLE_EQ(0.0, item._newavail);
	EXPECT_DOUBLE_EQ(0.0, item._closeprofit);
	EXPECT_DOUBLE_EQ(0.0, item._dynprofit);

	// 昨+今数量合成
	item._prevol = 3.0;
	item._newvol = 2.0;
	EXPECT_DOUBLE_EQ(5.0, item.volume());

	// 可平量 = 昨可平 + 今可平
	item._preavail = 3.0;
	item._newavail = 0.0;
	EXPECT_DOUBLE_EQ(3.0, item.valid());

	// 冻结量 = volume - valid
	EXPECT_DOUBLE_EQ(2.0, item.frozen());

	// 全部解冻后冻结为 0
	item._newavail = 2.0;
	EXPECT_DOUBLE_EQ(5.0, item.valid());
	EXPECT_DOUBLE_EQ(0.0, item.frozen());
}

TEST(P03UftPosItem, NegativeAndZeroBoundaries)
{
	UftPosItem item;
	item._prevol = 0.0;
	item._newvol = 0.0;
	item._preavail = 0.0;
	item._newavail = 0.0;
	EXPECT_DOUBLE_EQ(0.0, item.volume());
	EXPECT_DOUBLE_EQ(0.0, item.valid());
	EXPECT_DOUBLE_EQ(0.0, item.frozen());

	// avail 大于 vol 的脏数据下 valid() 仍按定义返回, 无裁剪
	item._prevol = 1.0;
	item._preavail = 4.0;
	EXPECT_DOUBLE_EQ(1.0, item.volume());
	EXPECT_DOUBLE_EQ(4.0, item.valid());
	EXPECT_DOUBLE_EQ(-3.0, item.frozen());
}

TEST(P03UftPosInfo, AggregateCloseAndDynProfit)
{
	UftPosInfo pInfo;
	EXPECT_DOUBLE_EQ(0.0, pInfo.closeprofit());
	EXPECT_DOUBLE_EQ(0.0, pInfo.dynprofit());

	pInfo._long._closeprofit = 120.0;
	pInfo._short._closeprofit = -30.0;
	pInfo._long._dynprofit = 55.5;
	pInfo._short._dynprofit = -15.5;
	EXPECT_DOUBLE_EQ(90.0, pInfo.closeprofit());
	EXPECT_DOUBLE_EQ(40.0, pInfo.dynprofit());

	// 纯函数不改动成员
	(void)pInfo.closeprofit();
	(void)pInfo.dynprofit();
	EXPECT_DOUBLE_EQ(120.0, pInfo._long._closeprofit);
	EXPECT_DOUBLE_EQ(-15.5, pInfo._short._dynprofit);
}

TEST(P03UftDetailInfo, MemsetZeroInit)
{
	UftMocker::DetailInfo dInfo;
	EXPECT_DOUBLE_EQ(0.0, dInfo._price);
	EXPECT_DOUBLE_EQ(0.0, dInfo._volume);
	EXPECT_EQ(0u, dInfo._opentime);
	EXPECT_EQ(0u, dInfo._opentdate);
	EXPECT_DOUBLE_EQ(0.0, dInfo._max_profit);
	EXPECT_DOUBLE_EQ(0.0, dInfo._max_loss);
	EXPECT_DOUBLE_EQ(0.0, dInfo._profit);
}

TEST(P03UftFundInfo, MemsetZeroInit)
{
	UftFund fund;
	EXPECT_DOUBLE_EQ(0.0, fund._total_profit);
	EXPECT_DOUBLE_EQ(0.0, fund._total_dynprofit);
	EXPECT_DOUBLE_EQ(0.0, fund._total_fees);

	fund._total_fees = 8.8;
	EXPECT_DOUBLE_EQ(8.8, fund._total_fees);
}
