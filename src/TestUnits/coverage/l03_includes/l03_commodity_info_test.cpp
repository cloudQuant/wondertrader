// L03 Includes lane 单元测试：WTSCommodityInfo 品种信息
// 目标文件: src/Includes/WTSContractInfo.hpp (L19-L166)
// 说明：WTSCommodityInfo 的多数数值/枚举成员既无默认构造初始化也无 NSDMI（见
// bugs/l03_includes.md BUG-L03-002），因此所有读取都必须先显式写入，测试严格按此约束编排。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSMarcos.h"

using namespace wtp;

namespace
{
// 创建一个已经把全部标量字段初始化过的品种对象，避免读到不确定值
WTSCommodityInfo* mkComm(const char* pid = "rb", const char* exchg = "SHFE")
{
	WTSCommodityInfo* c = WTSCommodityInfo::create(pid, "螺纹钢", exchg, "NIGHT_DAY", "CHN", "CNY");
	c->setVolScale(10);
	c->setPriceTick(1.0);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	c->setMarginRate(0.1);
	return c;
}

//////////////////////////////////////////////////////////////////////////
// create / 标识字段
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, create_fills_identity_fields)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "NIGHT_DAY", "CHN");
	ASSERT_NE(c, nullptr);
	EXPECT_STREQ(c->getProduct(), "rb");
	EXPECT_STREQ(c->getName(), "螺纹钢");
	EXPECT_STREQ(c->getExchg(), "SHFE");
	EXPECT_STREQ(c->getSession(), "NIGHT_DAY");
	EXPECT_STREQ(c->getTradingTpl(), "CHN");
	// 币种默认值 CNY
	EXPECT_STREQ(c->getCurrency(), "CNY");
	// 全品种 ID 由交易所与品种拼接
	EXPECT_STREQ(c->getFullPid(), "SHFE.rb");
	// 手续费算法默认 -1（该字段有 NSDMI，可安全读取）
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 1.0, 0), 0.0);
	c->release();

	WTSCommodityInfo* c2 = WTSCommodityInfo::create("IF", "沪深300", "CFFEX", "STK_INDEX", "CHN", "CNY");
	ASSERT_NE(c2, nullptr);
	EXPECT_STREQ(c2->getFullPid(), "CFFEX.IF");
	c2->release();
}

TEST(l03_commodity, identity_field_boundary_length)
{
	// wt_strcpy 是无边界检查的 memcpy（见 bugs/l03_includes.md BUG-L03-006），
	// 这里只用恰好能塞进 char[64] 的 63 字符边界值，避免越界写。
	const std::string pid(63, 'x');
	const std::string name(63, 'y');
	WTSCommodityInfo* c = WTSCommodityInfo::create(pid.c_str(), name.c_str(), "SHFE", "S", "T");
	ASSERT_NE(c, nullptr);
	EXPECT_EQ(std::strlen(c->getProduct()), 63u);
	EXPECT_EQ(std::strlen(c->getName()), 63u);
	// 全品种 ID 由 snprintf 写入定长缓冲区，超长部分被安全截断
	EXPECT_EQ(std::strlen(c->getFullPid()), 63u);
	EXPECT_EQ(std::strncmp(c->getFullPid(), "SHFE.", 5), 0);
	c->release();

	// 空串也要正确终止
	WTSCommodityInfo* e = WTSCommodityInfo::create("", "", "SHFE", "", "");
	ASSERT_NE(e, nullptr);
	EXPECT_STREQ(e->getProduct(), "");
	EXPECT_STREQ(e->getFullPid(), "SHFE.");
	e->release();
}

//////////////////////////////////////////////////////////////////////////
// 数值字段读写
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, scalar_accessors_round_trip)
{
	WTSCommodityInfo* c = mkComm();
	EXPECT_EQ(c->getVolScale(), 10u);
	EXPECT_DOUBLE_EQ(c->getPriceTick(), 1.0);
	EXPECT_DOUBLE_EQ(c->getLotsTick(), 1.0);
	EXPECT_DOUBLE_EQ(c->getMinLots(), 1.0);
	EXPECT_DOUBLE_EQ(c->getMarginRate(), 0.1);

	c->setVolScale(300);
	c->setPriceTick(0.2);
	c->setLotsTick(0.01);
	c->setMinLots(0.5);
	c->setMarginRate(0.0);
	EXPECT_EQ(c->getVolScale(), 300u);
	EXPECT_DOUBLE_EQ(c->getPriceTick(), 0.2);
	EXPECT_DOUBLE_EQ(c->getLotsTick(), 0.01);
	EXPECT_DOUBLE_EQ(c->getMinLots(), 0.5);
	EXPECT_DOUBLE_EQ(c->getMarginRate(), 0.0);
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// 分类 / 交易模式判定
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, category_predicates)
{
	WTSCommodityInfo* c = mkComm();
	EXPECT_EQ(c->getCategoty(), CC_Future);
	EXPECT_TRUE(c->isFuture());
	EXPECT_FALSE(c->isStock());
	EXPECT_FALSE(c->isOption());

	c->setCategory(CC_Stock);
	EXPECT_TRUE(c->isStock());
	EXPECT_FALSE(c->isFuture());
	EXPECT_FALSE(c->isOption());

	c->setCategory(CC_FutOption);
	EXPECT_TRUE(c->isOption());
	c->setCategory(CC_ETFOption);
	EXPECT_TRUE(c->isOption());
	c->setCategory(CC_SpotOption);
	EXPECT_TRUE(c->isOption());
	c->setCategory(CC_Combination);
	EXPECT_FALSE(c->isOption());
	EXPECT_FALSE(c->isStock());
	EXPECT_FALSE(c->isFuture());
	c->release();
}

TEST(l03_commodity, trading_mode_predicates)
{
	WTSCommodityInfo* c = mkComm();
	EXPECT_TRUE(c->canShort());
	EXPECT_FALSE(c->isT1());

	c->setTradingMode(TM_Long);
	EXPECT_FALSE(c->canShort());
	EXPECT_FALSE(c->isT1());

	c->setTradingMode(TM_LongT1);
	EXPECT_FALSE(c->canShort());
	EXPECT_TRUE(c->isT1());

	c->setTradingMode(TM_None);
	EXPECT_FALSE(c->canShort());
	EXPECT_FALSE(c->isT1());
	EXPECT_EQ(c->getTradingMode(), TM_None);
	c->release();
}

TEST(l03_commodity, cover_and_price_modes)
{
	WTSCommodityInfo* c = mkComm();
	EXPECT_EQ(c->getCoverMode(), CM_OpenCover);
	EXPECT_EQ(c->getPriceMode(), PM_Both);
	c->setCoverMode(CM_CoverToday);
	c->setPriceMode(PM_Limit);
	EXPECT_EQ(c->getCoverMode(), CM_CoverToday);
	EXPECT_EQ(c->getPriceMode(), PM_Limit);
	c->setCoverMode(CM_None);
	c->setPriceMode(PM_None);
	EXPECT_EQ(c->getCoverMode(), CM_None);
	EXPECT_EQ(c->getPriceMode(), PM_None);
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// 合约代码集合
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, code_set_add_and_dedupe)
{
	WTSCommodityInfo* c = mkComm();
	EXPECT_TRUE(c->getCodes().empty());
	c->addCode("rb2401");
	c->addCode("rb2405");
	c->addCode("rb2401");
	// CodeSet 是 hashset，重复代码只保留一份
	EXPECT_EQ(c->getCodes().size(), 2u);
	EXPECT_NE(c->getCodes().find("rb2405"), c->getCodes().end());
	EXPECT_EQ(c->getCodes().find("rb2410"), c->getCodes().end());
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// session info 绑定
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, session_info_binding)
{
	WTSCommodityInfo* c = mkComm();
	WTSSessionInfo* s = WTSSessionInfo::create("NIGHT_DAY", "夜盘白盘", 0);
	ASSERT_NE(s, nullptr);
	c->setSessionInfo(s);
	EXPECT_EQ(c->getSessionInfo(), s);
	EXPECT_STREQ(c->getSessionInfo()->id(), "NIGHT_DAY");
	// 解绑
	c->setSessionInfo(nullptr);
	EXPECT_EQ(c->getSessionInfo(), nullptr);
	s->release();
	c->release();
}

//////////////////////////////////////////////////////////////////////////
// 手续费计算
//////////////////////////////////////////////////////////////////////////
TEST(l03_commodity, calc_fee_by_volume)
{
	WTSCommodityInfo* c = mkComm();
	// byVolume == true -> 算法 0，按成交手数收固定额
	c->setFeeRates(3.5, 3.5, 3.5, true);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 0), 7.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 1), 7.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 2), 7.0);
	// 未知 offset 走 default 分支
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 9), 0.0);
	// 结果为 0 时按万分取整仍是 0
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 0.0, 0), 0.0);
	c->release();
}

TEST(l03_commodity, calc_fee_by_amount_and_rounding)
{
	WTSCommodityInfo* c = mkComm();
	c->setVolScale(10);
	// byVolume == false -> 算法 1，按成交额（价格*手数*放大倍数）比例收费
	c->setFeeRates(0.0001, 0.0001, 0.0002, false);
	// amount = 3500 * 2 * 10 = 70000 -> 7.0
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 0), 7.0);
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 1), 7.0);
	// 平今 14.0
	EXPECT_DOUBLE_EQ(c->calcFee(3500.0, 2.0, 2), 14.0);
	// 万分取整：0.0125 -> 0.01（(int32)(1.25+0.5)=1）
	c->setFeeRates(0.0125, 0.0125, 0.0125, true);
	EXPECT_DOUBLE_EQ(c->calcFee(1.0, 1.0, 0), 0.01);
	// 四舍五入进位：0.015 -> 0.02
	c->setFeeRates(0.015, 0.015, 0.015, true);
	EXPECT_DOUBLE_EQ(c->calcFee(1.0, 1.0, 0), 0.02);
	c->release();
}

TEST(l03_commodity, calc_fee_disabled_after_enable)
{
	WTSCommodityInfo* c = mkComm();
	c->setFeeRates(1.0, 1.0, 1.0, true);
	EXPECT_DOUBLE_EQ(c->calcFee(10.0, 3.0, 0), 3.0);
	// 没有“重新关闭”的入口：只能改成费率 0
	c->setFeeRates(0.0, 0.0, 0.0, true);
	EXPECT_DOUBLE_EQ(c->calcFee(10.0, 3.0, 0), 0.0);
	c->release();
}
}  // namespace
