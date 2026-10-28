// Lane L02 - 交易时间模板 / 品种 / 合约信息单元测试
// 覆盖：WTSSessionInfo（offsetTime/originalTime/minuteToTime/timeToMinutes/
//       timeToSeconds/secondsToTime/时段边界判定）、
//       WTSCommodityInfo、WTSContractInfo
// 说明：所有接口均为纯内存计算，不依赖真实时间、共享内存或网络。
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"

USING_NS_WTP;

namespace {

// 日盘：09:30-11:30 / 13:00-15:00，无偏移，集合竞价 09:25-09:30
WTSSessionInfo* make_day_session()
{
	WTSSessionInfo* sInfo = WTSSessionInfo::create("DAY", "日盘", 0);
	sInfo->addTradingSection(930, 1130);
	sInfo->addTradingSection(1300, 1500);
	sInfo->setAuctionTime(925, 930);
	return sInfo;
}

// 夜盘：21:00-23:00 / 00:00-01:00，偏移 -420 分钟（-7 小时）
WTSSessionInfo* make_night_session()
{
	WTSSessionInfo* sInfo = WTSSessionInfo::create("NIGHT", "夜盘", -420);
	sInfo->addTradingSection(2100, 2300);
	sInfo->addTradingSection(0, 100);
	return sInfo;
}

class SessionInfoHolder
{
public:
	explicit SessionInfoHolder(WTSSessionInfo* v = NULL) : _v(v) {}
	~SessionInfoHolder() { reset(); }
	SessionInfoHolder(const SessionInfoHolder&) = delete;
	SessionInfoHolder& operator=(const SessionInfoHolder&) = delete;

	void reset()
	{
		if (_v != NULL)
		{
			_v->release();
			_v = NULL;
		}
	}
	WTSSessionInfo* get() const { return _v; }
	WTSSessionInfo* operator->() const { return _v; }

private:
	WTSSessionInfo* _v;
};

class CommHolder
{
public:
	explicit CommHolder(WTSCommodityInfo* v = NULL) : _v(v) {}
	~CommHolder()
	{
		if (_v != NULL)
			_v->release();
	}
	CommHolder(const CommHolder&) = delete;
	CommHolder& operator=(const CommHolder&) = delete;
	WTSCommodityInfo* get() const { return _v; }
	WTSCommodityInfo* operator->() const { return _v; }

private:
	WTSCommodityInfo* _v;
};

class ContractHolder
{
public:
	explicit ContractHolder(WTSContractInfo* v = NULL) : _v(v) {}
	~ContractHolder()
	{
		if (_v != NULL)
			_v->release();
	}
	ContractHolder(const ContractHolder&) = delete;
	ContractHolder& operator=(const ContractHolder&) = delete;
	WTSContractInfo* get() const { return _v; }
	WTSContractInfo* operator->() const { return _v; }

private:
	WTSContractInfo* _v;
};

class L02SessionFixture : public ::testing::Test
{
protected:
	void SetUp() override { m_day = make_day_session(); }
	void TearDown() override
	{
		if (m_day != NULL)
		{
			m_day->release();
			m_day = NULL;
		}
	}
	WTSSessionInfo* m_day;
};

}  // namespace

///////////////////////////////////////////////////////// WTSSessionInfo 基础属性
TEST_F(L02SessionFixture, IdNameAndSections)
{
	EXPECT_STREQ("DAY", m_day->id());
	EXPECT_STREQ("日盘", m_day->name());
	EXPECT_EQ(0, m_day->getOffsetMins());
	EXPECT_EQ(2u, m_day->getSectionCount());

	m_day->setOffsetMins(-60);
	EXPECT_EQ(-60, m_day->getOffsetMins());
	m_day->setOffsetMins(0);

	const WTSSessionInfo::TradingTimes& secs = m_day->getTradingSections();
	ASSERT_EQ(2u, secs.size());
	EXPECT_EQ(930u, secs[0].first_raw);
	EXPECT_EQ(1130u, secs[0].second_raw);
	EXPECT_EQ(930u, secs[0].first);
	EXPECT_EQ(1130u, secs[0].second);
	EXPECT_EQ(1300u, secs[1].first_raw);
	EXPECT_EQ(1500u, secs[1].second);

	const WTSSessionInfo::TradingTimes& auc = m_day->getAuctionSections();
	ASSERT_EQ(1u, auc.size());
	EXPECT_EQ(925u, auc[0].first_raw);
	EXPECT_EQ(930u, auc[0].second_raw);

	// setAuctionTime 非空时原地覆盖，不新增段
	m_day->setAuctionTime(920, 925);
	ASSERT_EQ(1u, m_day->getAuctionSections().size());
	EXPECT_EQ(920u, m_day->getAuctionStartTime());
	EXPECT_EQ(925u, m_day->getAuctionSections()[0].second_raw);

	// addAuctionTime 追加第二段
	m_day->addAuctionTime(1500, 1510);
	ASSERT_EQ(2u, m_day->getAuctionSections().size());
	EXPECT_EQ(1500u, m_day->getAuctionSections()[1].first_raw);

	// 引用计数：create 返回单引用
	EXPECT_EQ(1u, m_day->retainCount());
	EXPECT_TRUE(m_day->isSingleRefs());
	m_day->retain();
	EXPECT_EQ(2u, m_day->retainCount());
	m_day->release();
	EXPECT_EQ(1u, m_day->retainCount());
}

TEST_F(L02SessionFixture, OpenCloseAndTotals)
{
	EXPECT_EQ(930u, m_day->getOpenTime());
	EXPECT_EQ(930u, m_day->getOpenTime(true));
	EXPECT_EQ(1500u, m_day->getCloseTime());
	EXPECT_EQ(1500u, m_day->getCloseTime(true));
	EXPECT_EQ(240u, m_day->getTradingMins());
	EXPECT_EQ(14400u, m_day->getTradingSeconds());

	const std::vector<uint32_t>& mins = m_day->getSecMinList();
	ASSERT_EQ(2u, mins.size());
	EXPECT_EQ(120u, mins[0]);
	EXPECT_EQ(240u, mins[1]);
	// 第二次调用命中缓存分支
	EXPECT_EQ(mins.size(), m_day->getSecMinList().size());
}

TEST_F(L02SessionFixture, EmptySessionDefaults)
{
	SessionInfoHolder empty(WTSSessionInfo::create("EMPTY", "空模板", 0));
	EXPECT_EQ(0u, empty->getOpenTime());
	EXPECT_EQ(0u, empty->getCloseTime());
	EXPECT_EQ(INVALID_UINT32, empty->timeToMinutes(930));
	EXPECT_EQ(INVALID_UINT32, empty->minuteToTime(10));
	EXPECT_EQ(INVALID_UINT32, empty->timeToSeconds(93000));
	EXPECT_EQ(INVALID_UINT32, empty->secondsToTime(0));
	// 无时段时 getTradingMins/Seconds 视为全天候
	EXPECT_EQ(1440u, empty->getTradingMins());
	EXPECT_EQ(86400u, empty->getTradingSeconds());
	// 无集合竞价时返回 (uint32_t)-1
	EXPECT_EQ(0xffffffffu, empty->getAuctionStartTime());
	EXPECT_FALSE(empty->isInTradingTime(930));
	EXPECT_FALSE(empty->isFirstOfSection(930));
	EXPECT_FALSE(empty->isLastOfSection(930));
	// 空模板的 getSecMinList 兜底为 1440
	EXPECT_EQ(1u, empty->getSecMinList().size());
	EXPECT_EQ(1440u, empty->getSecMinList()[0]);
}

///////////////////////////////////////////////////////// offsetTime / originalTime
TEST_F(L02SessionFixture, OffsetTimeNoOffset)
{
	EXPECT_EQ(930u, m_day->offsetTime(930, true));
	EXPECT_EQ(930u, m_day->offsetTime(930, false));
	// 偏移 0 时 0 点向右对齐成 2400，向左仍是 0000
	EXPECT_EQ(0u, m_day->offsetTime(0, true));
	EXPECT_EQ(2400u, m_day->offsetTime(0, false));
	EXPECT_EQ(930u, m_day->originalTime(930));
}

TEST_F(L02SessionFixture, OffsetTimeWithShift)
{
	SessionInfoHolder s(WTSSessionInfo::create("N1", "夜盘", -420));
	// 2100 - 7h = 1400
	EXPECT_EQ(1400u, s->offsetTime(2100, true));
	// 0100 - 7h = -360min -> 右对齐 +1440 = 1080min = 1800
	EXPECT_EQ(1800u, s->offsetTime(100, false));
	// 左对齐同样落在 +1440 圈
	EXPECT_EQ(1080u, s->offsetTime(100, true) == 1800u ? 1080u : 0u);
	// 反向还原
	EXPECT_EQ(100u, s->originalTime(1800));
	EXPECT_EQ(2100u, s->originalTime(1400));

	SessionInfoHolder p(WTSSessionInfo::create("P1", "正偏移", 30));
	// 2345 + 30min = 1440+ -> 2415 -> 圈回 0015
	EXPECT_EQ(15u, p->offsetTime(2345, true));
	EXPECT_EQ(2345u, p->originalTime(15));
}

TEST_F(L02SessionFixture, OffsetDateCrossDay)
{
	SessionInfoHolder s(WTSSessionInfo::create("N2", "夜盘", -420));
	// 2130 - 7h 仍在同一天
	EXPECT_EQ(20260105u, s->getOffsetDate(20260105, 2130));
	// 0930 - 7h -> 0230 同样不会跨到前一天
	EXPECT_EQ(20260105u, s->getOffsetDate(20260105, 930));

	SessionInfoHolder plus(WTSSessionInfo::create("N3", "正偏移", 500));
	// 2130 + 8h20 -> 越日
	EXPECT_EQ(20260106u, plus->getOffsetDate(20260105, 2130));

	SessionInfoHolder minus(WTSSessionInfo::create("N4", "负偏移", -1300));
	// 2130 - 21h40 -> 负数 -> 回退一天
	EXPECT_EQ(20260104u, minus->getOffsetDate(20260105, 2130));

	// 跨月边界同样由日期计算保证
	SessionInfoHolder month(WTSSessionInfo::create("N5", "正偏移", 60));
	EXPECT_EQ(20260201u, month->getOffsetDate(20260131, 2330));
}

///////////////////////////////////////////////////////// 分钟/时间互转
TEST_F(L02SessionFixture, TimeToMinutesDaySession)
{
	// 段首 -> 0
	EXPECT_EQ(0u, m_day->timeToMinutes(930));
	// 09:30 -> 10:40 = 70 分钟
	EXPECT_EQ(70u, m_day->timeToMinutes(1040));
	// 段尾 1130 属于第一段结束，累计 120
	EXPECT_EQ(120u, m_day->timeToMinutes(1130));
	// 第二段段首
	EXPECT_EQ(120u, m_day->timeToMinutes(1300));
	// 13:00 -> 13:30 = 150
	EXPECT_EQ(150u, m_day->timeToMinutes(1330));
	// 段尾
	EXPECT_EQ(240u, m_day->timeToMinutes(1500));
	// 集合竞价内 -> 0
	EXPECT_EQ(0u, m_day->timeToMinutes(927));
	// 午休 -> 无效
	EXPECT_EQ(INVALID_UINT32, m_day->timeToMinutes(1131));
	// 开盘前 -> 无效
	EXPECT_EQ(INVALID_UINT32, m_day->timeToMinutes(900));
	// 收盘后 -> 大于所有段上界，循环结束仍未命中
	EXPECT_EQ(INVALID_UINT32, m_day->timeToMinutes(1501));

	// autoAdjust：午休时间对齐到下一段段首
	EXPECT_EQ(120u, m_day->timeToMinutes(1131, true));
	// 开盘前对齐到第一段段首 -> 0
	EXPECT_EQ(0u, m_day->timeToMinutes(900, true));
}

TEST_F(L02SessionFixture, MinuteToTimeDaySession)
{
	EXPECT_EQ(930u, m_day->minuteToTime(0));
	EXPECT_EQ(1040u, m_day->minuteToTime(70));
	// 第一段末尾 120 分钟 -> 1130
	EXPECT_EQ(1130u, m_day->minuteToTime(120));
	// 121 分钟进入第二段 -> 1301
	EXPECT_EQ(1301u, m_day->minuteToTime(121));
	// 全天 240 分钟 -> 1500
	EXPECT_EQ(1500u, m_day->minuteToTime(240));
	// 超出全天分钟数 -> 兜底返回收盘时间
	EXPECT_EQ(1500u, m_day->minuteToTime(241));
	EXPECT_EQ(1500u, m_day->minuteToTime(10000));
	// bHeadFirst：段首优先，边界归属下一段段首
	EXPECT_EQ(930u, m_day->minuteToTime(0, true));
	EXPECT_EQ(1300u, m_day->minuteToTime(120, true));
}

TEST_F(L02SessionFixture, SecondsRoundTrip)
{
	// 09:30:00 -> 0 秒
	EXPECT_EQ(0u, m_day->timeToSeconds(93000));
	// 09:30:30 -> 30 秒
	EXPECT_EQ(30u, m_day->timeToSeconds(93030));
	// 11:29:59 -> 7199 秒（第一段 120 分钟 - 1）
	EXPECT_EQ(7199u, m_day->timeToSeconds(112959));
	// 13:00:00 -> 7200
	EXPECT_EQ(7200u, m_day->timeToSeconds(130000));
	// 15:00:00 是全天最后一秒，命中 seconds==stopSecs 时 offset--
	EXPECT_EQ(14399u, m_day->timeToSeconds(150000));
	// 集合竞价时间被当作 0 秒
	EXPECT_EQ(0u, m_day->timeToSeconds(92500));
	// 午休 -> 无效
	EXPECT_EQ(INVALID_UINT32, m_day->timeToSeconds(113100));

	// 反向
	EXPECT_EQ(93000u, m_day->secondsToTime(0));
	EXPECT_EQ(93030u, m_day->secondsToTime(30));
	EXPECT_EQ(113000u, m_day->secondsToTime(7200));
	EXPECT_EQ(130001u, m_day->secondsToTime(7201));
	EXPECT_EQ(150000u, m_day->secondsToTime(14400));
	// 超出总时长 -> 循环走完返回 INVALID_UINT32
	EXPECT_EQ(INVALID_UINT32, m_day->secondsToTime(14401));
}

TEST_F(L02SessionFixture, TradingTimeFlags)
{
	EXPECT_TRUE(m_day->isInTradingTime(930));
	EXPECT_TRUE(m_day->isInTradingTime(1040));
	EXPECT_TRUE(m_day->isInTradingTime(1130));
	// 严格模式下段尾不算交易时间
	EXPECT_FALSE(m_day->isInTradingTime(1130, true));
	EXPECT_FALSE(m_day->isInTradingTime(1500, true));
	EXPECT_TRUE(m_day->isInTradingTime(1459));
	EXPECT_FALSE(m_day->isInTradingTime(1131));
	EXPECT_FALSE(m_day->isInTradingTime(925));

	EXPECT_TRUE(m_day->isFirstOfSection(930));
	EXPECT_TRUE(m_day->isFirstOfSection(1300));
	EXPECT_FALSE(m_day->isFirstOfSection(931));

	EXPECT_TRUE(m_day->isLastOfSection(1130));
	EXPECT_TRUE(m_day->isLastOfSection(1500));
	EXPECT_FALSE(m_day->isLastOfSection(1129));

	EXPECT_TRUE(m_day->isInAuctionTime(925));
	EXPECT_TRUE(m_day->isInAuctionTime(929));
	// 右边界开区间
	EXPECT_FALSE(m_day->isInAuctionTime(930));
	EXPECT_FALSE(m_day->isInAuctionTime(924));

	// (0,0) 的集合竞价段会被跳过
	m_day->addAuctionTime(0, 0);
	EXPECT_FALSE(m_day->isInAuctionTime(0));
}

TEST_F(L02SessionFixture, NightSessionOffsetSections)
{
	SessionInfoHolder night(make_night_session());
	const WTSSessionInfo::TradingTimes& secs = night->getTradingSections();
	ASSERT_EQ(2u, secs.size());
	// 2100 - 7h = 1400
	EXPECT_EQ(1400u, secs[0].first);
	EXPECT_EQ(2100u, secs[0].first_raw);
	// 2300 - 7h = 1600
	EXPECT_EQ(1600u, secs[0].second);
	// 0000 - 7h 右对齐 -> 1700
	EXPECT_EQ(1700u, secs[1].first);
	EXPECT_EQ(1800u, secs[1].second);
	EXPECT_EQ(100u, secs[1].second_raw);

	// 偏移后分钟数：21:30 -> 30
	EXPECT_EQ(30u, night->timeToMinutes(2130));
	// 23:00 段尾 -> 120
	EXPECT_EQ(120u, night->timeToMinutes(2300));
	// 23:30 -> 偏移 1630 落在第二段 [1700,1800] 之外? 23:30-7h=1630 -> 大于第一段尾 1600
	// 第二段下界 1700 > 1630 -> autoAdjust 关闭时无效
	EXPECT_EQ(INVALID_UINT32, night->timeToMinutes(2330));
	EXPECT_EQ(120u, night->timeToMinutes(2330, true));

	// 00:30 -> 偏移 1730 -> 第二段 1730-1700 = 30 -> 累计 150
	EXPECT_EQ(150u, night->timeToMinutes(30));
	// 反向
	EXPECT_EQ(2130u, night->minuteToTime(30));
	EXPECT_EQ(30u, night->secondsToTime(30 * 60));
}

///////////////////////////////////////////////////////// 品种 / 合约
TEST(L02ContractInfo, CommodityBasics)
{
	CommHolder comm(WTSCommodityInfo::create("IF", "沪深指数期货", "CFFEX", "DAY", "CHN", "CNY"));
	EXPECT_STREQ("IF", comm->getProduct());
	EXPECT_STREQ("CFFEX", comm->getExchg());
	EXPECT_STREQ("沪深指数期货", comm->getName());
	EXPECT_STREQ("CNY", comm->getCurrency());
	EXPECT_STREQ("DAY", comm->getSession());
	EXPECT_STREQ("CHN", comm->getTradingTpl());
	EXPECT_STREQ("CFFEX.IF", comm->getFullPid());

	comm->setVolScale(300);
	comm->setPriceTick(0.2);
	comm->setLotsTick(1.0);
	comm->setMinLots(1.0);
	comm->setCategory(CC_Future);
	comm->setCoverMode(CM_CoverToday);
	comm->setPriceMode(PM_Both);
	comm->setTradingMode(TM_Both);
	comm->setMarginRate(0.12);

	EXPECT_EQ(300u, comm->getVolScale());
	EXPECT_DOUBLE_EQ(0.2, comm->getPriceTick());
	EXPECT_DOUBLE_EQ(1.0, comm->getLotsTick());
	EXPECT_DOUBLE_EQ(1.0, comm->getMinLots());
	EXPECT_EQ((int)CC_Future, (int)comm->getCategoty());
	EXPECT_EQ((int)CM_CoverToday, (int)comm->getCoverMode());
	EXPECT_EQ((int)PM_Both, (int)comm->getPriceMode());
	EXPECT_EQ((int)TM_Both, (int)comm->getTradingMode());
	EXPECT_DOUBLE_EQ(0.12, comm->getMarginRate());
	EXPECT_TRUE(comm->isFuture());
	EXPECT_TRUE(comm->canShort());
	EXPECT_FALSE(comm->isT1());
	EXPECT_FALSE(comm->isStock());
	EXPECT_FALSE(comm->isOption());

	comm->addCode("IF2107");
	comm->addCode("IF2108");
	comm->addCode("IF2107");
	EXPECT_EQ(2u, comm->getCodes().size());

	// 分类切换
	comm->setCategory(CC_ETFOption);
	EXPECT_TRUE(comm->isOption());
	EXPECT_FALSE(comm->isFuture());
	comm->setCategory(CC_Stock);
	EXPECT_TRUE(comm->isStock());
	comm->setTradingMode(TM_LongT1);
	EXPECT_TRUE(comm->isT1());
	EXPECT_FALSE(comm->canShort());

	// 币种缺省值
	CommHolder dc(WTSCommodityInfo::create("BTC", "比特币", "BINANCE", "ALL", "NONE"));
	EXPECT_STREQ("CNY", dc->getCurrency());
	EXPECT_STREQ("BINANCE.BTC", dc->getFullPid());
}

TEST(L02ContractInfo, CommodityFeeCalculation)
{
	CommHolder comm(WTSCommodityInfo::create("IF", "沪深", "CFFEX", "DAY", "CHN"));
	comm->setVolScale(300);

	// 未设置费率 -> m_nFeeAlg == -1 -> 返回 0
	EXPECT_DOUBLE_EQ(0.0, comm->calcFee(4500, 1, 0));

	// 按成交量计费
	comm->setFeeRates(1.5, 2.5, 3.5, true);
	EXPECT_DOUBLE_EQ(3.0, comm->calcFee(4500, 2, 0));
	EXPECT_DOUBLE_EQ(5.0, comm->calcFee(4500, 2, 1));
	EXPECT_DOUBLE_EQ(7.0, comm->calcFee(4500, 2, 2));
	// 未知 offset -> 0
	EXPECT_DOUBLE_EQ(0.0, comm->calcFee(4500, 2, 3));

	// 按成交额计费：4000 * 2 * 300 * 0.0001 = 240
	comm->setFeeRates(0.0001, 0.0002, 0.0003, false);
	EXPECT_DOUBLE_EQ(240.0, comm->calcFee(4000, 2, 0));
	EXPECT_DOUBLE_EQ(480.0, comm->calcFee(4000, 2, 1));
	EXPECT_DOUBLE_EQ(720.0, comm->calcFee(4000, 2, 2));
	// 结果按分取整（四舍五入到 2 位小数）
	EXPECT_DOUBLE_EQ(0.0, comm->calcFee(1, 1, 5));
}

TEST(L02ContractInfo, ContractBasics)
{
	CommHolder comm(WTSCommodityInfo::create("IF", "沪深", "CFFEX", "DAY", "CHN"));
	comm->setVolScale(300);

	ContractHolder c(WTSContractInfo::create("IF2107", "沪深2107", "CFFEX", "IF"));
	EXPECT_STREQ("IF2107", c->getCode());
	EXPECT_STREQ("沪深2107", c->getName());
	EXPECT_STREQ("CFFEX", c->getExchg());
	EXPECT_STREQ("IF", c->getProduct());
	EXPECT_STREQ("CFFEX.IF2107", c->getFullCode());
	EXPECT_STREQ("CFFEX.IF", c->getFullPid());

	// 郑商所别称代码
	c->setAltCode("IF2112");
	EXPECT_STREQ("IF2112", c->getAltCode());
	EXPECT_STREQ("CFFEX.IF2112", c->getFullAltCode());

	c->setVolScale(300);
	c->setVolumeLimits(500, 20, 1, 2);
	EXPECT_EQ(500u, c->getMaxMktVol());
	EXPECT_EQ(20u, c->getMaxLmtVol());
	EXPECT_EQ(1u, c->getMinMktVol());
	EXPECT_EQ(2u, c->getMinLmtVol());

	// 缺省日期区间
	EXPECT_EQ(19900101u, c->getOpenDate());
	EXPECT_EQ(30991231u, c->getExpireDate());
	c->setDates(20210416, 20210618);
	EXPECT_EQ(20210416u, c->getOpenDate());
	EXPECT_EQ(20210618u, c->getExpireDate());

	c->setTotalIndex(9);
	EXPECT_EQ(9u, c->getTotalIndex());
	EXPECT_EQ(comm->getProduct() != NULL, true);
	c->setCommInfo(comm.get());
	EXPECT_TRUE(c->getCommInfo() == comm.get());

	// 主力连续标记
	EXPECT_TRUE(c->isFlat());
	c->addHotCode("IF2108");
	c->addHotCode("IF2109");
	EXPECT_FALSE(c->isFlat());
	ASSERT_EQ(2u, c->getHotCodes().size());
	EXPECT_EQ("IF2108", c->getHotCodes()[0]);
	EXPECT_EQ("IF2109", c->getHotCodes()[1]);

	// 扩展数据槽位
	int extra = 42;
	c->setExtData((void*)&extra);
	EXPECT_TRUE(c->getExtData<int>() == &extra);
	c->setExtData<3>((void*)&extra);
	int* ext3 = c->getExtData<int, 3>();
	EXPECT_TRUE(ext3 == &extra);
	// 未使用的槽位为 NULL
	int* ext15 = c->getExtData<int, 15>();
	EXPECT_TRUE(ext15 == NULL);
}

TEST(L02ContractInfo, ContractMarginAndFee)
{
	CommHolder comm(WTSCommodityInfo::create("IF", "沪深", "CFFEX", "DAY", "CHN"));
	comm->setVolScale(300);
	comm->setMarginRate(0.1);

	ContractHolder c(WTSContractInfo::create("IF2107", "沪深2107", "CFFEX", "IF"));
	c->setCommInfo(comm.get());
	c->setMarginRatios(0.2, 0.25, 0);
	// flag==0 且品种保证金率非 0 时取品种值
	EXPECT_DOUBLE_EQ(0.1, c->getLongMarginRatio());
	EXPECT_DOUBLE_EQ(0.1, c->getShortMarginRatio());

	// flag==1 时取合约手工值
	c->setMarginRatios(0.2, 0.25, 1);
	EXPECT_DOUBLE_EQ(0.2, c->getLongMarginRatio());
	EXPECT_DOUBLE_EQ(0.25, c->getShortMarginRatio());

	// 品种保证金率为 0 时回退合约值
	CommHolder zero(WTSCommodityInfo::create("AU", "黄金", "SHFE", "NIGHT", "CHN"));
	zero->setVolScale(1000);
	ContractHolder a(WTSContractInfo::create("AU2106", "黄金2106", "SHFE", "AU"));
	a->setCommInfo(zero.get());
	a->setMarginRatios(0.08, 0.09, 0);
	EXPECT_DOUBLE_EQ(0.08, a->getLongMarginRatio());
	EXPECT_DOUBLE_EQ(0.09, a->getShortMarginRatio());

	// 合约未设置费率 -> 走品种费率
	a->setFeeRates(1.0, 2.0, 3.0, true);
	EXPECT_DOUBLE_EQ(2.0, a->calcFee(380, 2, 0));
	EXPECT_DOUBLE_EQ(4.0, a->calcFee(380, 2, 1));
	EXPECT_DOUBLE_EQ(6.0, a->calcFee(380, 2, 2));
	EXPECT_DOUBLE_EQ(0.0, a->calcFee(380, 2, 9));
	// 成交额模式使用品种 volscale
	a->setFeeRates(0.001, 0.002, 0.003, false);
	EXPECT_DOUBLE_EQ(760.0, a->calcFee(380, 2, 0));

	// 合约无费率时委托品种（品种也未设置 -> 0）
	CommHolder nofee(WTSCommodityInfo::create("RB", "螺纹", "SHFE", "DAY", "CHN"));
	nofee->setVolScale(10);
	ContractHolder r(WTSContractInfo::create("RB2110", "螺纹2110", "SHFE", "RB"));
	r->setCommInfo(nofee.get());
	EXPECT_DOUBLE_EQ(0.0, r->calcFee(5000, 1, 0));
}
