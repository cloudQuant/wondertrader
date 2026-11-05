// Lane R56 - src/WTSTools/WTSBaseDataMgr.cpp 剩余未覆盖面
// 覆盖：合约自动建品种（rules 无 session -> ALLDAY）、minmarketqty 读取、
//       getContract 按日期窗口落空、空 session 品种的 getBoundaryTime 早退、
//       loadHolidays 的非数组条目、24 小时交易时段的 calcTradingDate 三个方向、
//       非 24 小时时段在后偏移/前偏移/零偏移下的周末与跨日分支。
// 所有查询都使用显式日期；不触碰 uDate/uTime 传 0 的挂钟分支。
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "r56_tmp.h"

namespace {

const char* kAutoContracts = R"({
	"DCE": {
		"i2405": {
			"name": "r56自动品种合约",
			"exchg": "DCE",
			"rules": {"pricetick": 0.5, "volscale": 100, "covermode": 0, "pricemode": 0, "lotstick": 2, "minlots": 3},
			"maxmarketqty": 9, "minmarketqty": 7,
			"opendate": 20240101, "expiredate": 20241231
		}
	}
})";

// I03-NEW-053（整批校验后发布）：引用未加载品种的合约使整份文件被拒，旧“跳过该条”的期望改为单独夹具。
const char* kMissingProductContracts = R"({
	"DCE": {
		"i2406": {
			"name": "r56缺失品种",
			"exchg": "DCE",
			"product": "nope",
			"opendate": 20240101, "expiredate": 20241231
		}
	}
})";

const char* kHolidays = R"({
	"CHN": [20240101, 20240102],
	"R56BAD": {"not": "an array"}
})";

// 卡2b：全天时段唯一表达为 0–0；2400 不是合法 HHMM，旧夹具的 0–2400 整份被拒。
const char* kFullDaySessions = R"({
	"R56FULL0":   {"name": "24h零偏移", "offset": 0,    "sections": [{"from": 0, "to": 0}]},
	"R56FULLPOS": {"name": "24h后偏移", "offset": 300,  "sections": [{"from": 0, "to": 0}]},
	"R56FULLNEG": {"name": "24h前偏移", "offset": -300, "sections": [{"from": 0, "to": 0}]}
})";

const char* kLegacy2400Session = R"({
	"R56FULL2400": {"name": "旧24h写法", "offset": 0, "sections": [{"from": 0, "to": 2400}]}
})";

const char* kPartDaySessions = R"({
	"R56OFFPOS":  {"name": "后偏移夜盘", "offset": 300,  "sections": [{"from": 2100, "to": 2300}]},
	"R56OFFNEG":  {"name": "前偏移夜盘", "offset": -300, "sections": [{"from": 2100, "to": 2300}]},
	"R56OFFZERO": {"name": "零偏移日盘", "offset": 0,    "sections": [{"from": 900, "to": 1500}]}
})";

}	// namespace

class R56BaseDataMgr : public ::testing::Test
{
protected:
	void SetUp() override { m_dir = r56tmp::unique_dir("r56_bdm"); }
	void TearDown() override { r56tmp::clean_dir(m_dir); }

	std::filesystem::path	m_dir;
	WTSBaseDataMgr			m_mgr;
};

// WTSBaseDataMgr.cpp:394-470,487-512,605-630,91-134
TEST_F(R56BaseDataMgr, ContractRulesAutoCreateCommodityAndWindows)
{
	const std::string file = r56tmp::write_file(m_dir, "contracts.json", kAutoContracts);
	// I03-NEW-053（整批校验后发布）：rules 自动建的品种必须挂上已加载的会话；ALLDAY 未加载时整份文件被拒
	// （改前期望：接受并得到 session 信息为空的品种）。
	{
		WTSBaseDataMgr noSession;
		EXPECT_FALSE(noSession.loadContracts(file.c_str()));
		EXPECT_EQ(nullptr, noSession.getCommodity("DCE", "i2405"));
		EXPECT_EQ(nullptr, noSession.getContract("i2405", ""));
	}
	const std::string sessFile = r56tmp::write_file(m_dir, "sessions_allday.json",
		R"({"ALLDAY": {"name": "全天", "offset": 0, "sections": [{"from": 0, "to": 0}]}})");
	ASSERT_TRUE(m_mgr.loadSessions(sessFile.c_str()));
	ASSERT_TRUE(m_mgr.loadContracts(file.c_str()));

	// 无 product 但带 rules：自动建品种，session 缺省为 ALLDAY
	WTSCommodityInfo* comm = m_mgr.getCommodity("DCE", "i2405");
	ASSERT_NE(nullptr, comm);
	EXPECT_STREQ("i2405", comm->getProduct());
	EXPECT_STREQ("DCE.i2405", comm->getFullPid());
	EXPECT_STREQ("ALLDAY", comm->getSession());
	EXPECT_STREQ("", comm->getTradingTpl());
	EXPECT_EQ(100u, comm->getVolScale());
	EXPECT_DOUBLE_EQ(0.5, comm->getPriceTick());
	EXPECT_DOUBLE_EQ(2.0, comm->getLotsTick());
	EXPECT_DOUBLE_EQ(3.0, comm->getMinLots());
	// 缺省会话 ALLDAY 已加载，品种挂上该会话
	ASSERT_NE(nullptr, m_mgr.getSession("ALLDAY"));
	EXPECT_EQ(m_mgr.getSession("ALLDAY"), comm->getSessionInfo());

	// 合约手数限制与日期窗口
	WTSContractInfo* ct = m_mgr.getContract("i2405", "", 20240601);
	ASSERT_NE(nullptr, ct);
	EXPECT_EQ(9u, ct->getMaxMktVol());
	EXPECT_EQ(7u, ct->getMinMktVol());
	EXPECT_EQ(20240101u, ct->getOpenDate());
	EXPECT_EQ(20241231u, ct->getExpireDate());
	EXPECT_STREQ("i2405", ct->getAltCode());
	// 日期窗口之外按代码查（无交易所）应落空，走到循环结束的 return NULL
	EXPECT_EQ(nullptr, m_mgr.getContract("i2405", "", 20250101));
	// I03-NEW-053：引用未加载品种的合约文件整份被拒，不发布任何合约，已加载的合约不受影响
	const std::string bad = r56tmp::write_file(m_dir, "contracts_missing_product.json", kMissingProductContracts);
	EXPECT_FALSE(m_mgr.loadContracts(bad.c_str()));
	EXPECT_EQ(nullptr, m_mgr.getContract("i2406", ""));
	EXPECT_NE(nullptr, m_mgr.getContract("i2405", "", 20240601));

	// “session 为空的品种”在 I03-NEW-053 后不可经加载器产生，原 getBoundaryTime==0 早退断言随之移除
	EXPECT_EQ(nullptr, m_mgr.getCommodity("DCE", "nope"));
}

// WTSBaseDataMgr.cpp:571-603,791-843
TEST_F(R56BaseDataMgr, HolidayTableSkipsNonArrayAndDrivesTradingDays)
{
	const std::string file = r56tmp::write_file(m_dir, "holidays.json", kHolidays);
	ASSERT_TRUE(m_mgr.loadHolidays(file.c_str()));

	EXPECT_TRUE(m_mgr.isHoliday("CHN", 20240101, true));
	EXPECT_TRUE(m_mgr.isHoliday("CHN", 20240102, true));
	EXPECT_FALSE(m_mgr.isHoliday("CHN", 20240103, true));
	// 非数组条目被跳过，不产生模板；未知模板按非节假日处理
	EXPECT_FALSE(m_mgr.isHoliday("R56BAD", 20240102, true));
	EXPECT_FALSE(m_mgr.isHoliday("R56NONE", 20240102, true));

	EXPECT_FALSE(m_mgr.isTradingDate("CHN", 20240101, true));
	EXPECT_TRUE(m_mgr.isTradingDate("CHN", 20240103, true));

	// 20231230/31 周末 + 20240101/02 节假日 -> 下一交易日 20240103
	EXPECT_EQ(20240103u, m_mgr.getNextTDate("CHN", 20231229, 1, true));
	EXPECT_EQ(20231229u, m_mgr.getPrevTDate("CHN", 20240103, 1, true));
	EXPECT_EQ(20240105u, m_mgr.getNextTDate("CHN", 20240103, 2, true));
}

// WTSBaseDataMgr.cpp:673-717
TEST_F(R56BaseDataMgr, CalcTradingDateFullDaySessions)
{
	const std::string file = r56tmp::write_file(m_dir, "sessions_full.json", kFullDaySessions);
	ASSERT_TRUE(m_mgr.loadSessions(file.c_str()));

	WTSSessionInfo* full0 = m_mgr.getSession("R56FULL0");
	WTSSessionInfo* fullPos = m_mgr.getSession("R56FULLPOS");
	WTSSessionInfo* fullNeg = m_mgr.getSession("R56FULLNEG");
	ASSERT_NE(nullptr, full0);
	ASSERT_NE(nullptr, fullPos);
	ASSERT_NE(nullptr, fullNeg);
	EXPECT_EQ(1440u, full0->getTradingMins());
	EXPECT_EQ(1440u, fullPos->getTradingMins());
	EXPECT_EQ(1440u, fullNeg->getTradingMins());

	// 24h + 零偏移：交易日即自然日
	EXPECT_EQ(20240108u, m_mgr.calcTradingDate("R56FULL0", 20240108, 1000, true));
	// 24h + 后偏移：时间晚于偏移点 -> 下一自然日
	EXPECT_EQ(20240109u, m_mgr.calcTradingDate("R56FULLPOS", 20240108, 2300, true));
	// 24h + 前偏移：时间早于偏移点 -> 上一自然日
	EXPECT_EQ(20240107u, m_mgr.calcTradingDate("R56FULLNEG", 20240108, 100, true));

	// 卡2b：旧的 0–2400 写法整份被拒，不登记模板
	WTSBaseDataMgr legacy;
	const std::string legacyFile = r56tmp::write_file(m_dir, "sessions_2400.json", kLegacy2400Session);
	EXPECT_FALSE(legacy.loadSessions(legacyFile.c_str()));
	EXPECT_EQ(nullptr, legacy.getSession("R56FULL2400"));
}

// WTSBaseDataMgr.cpp:719-758
TEST_F(R56BaseDataMgr, CalcTradingDatePartDayOffsetAndWeekend)
{
	const std::string file = r56tmp::write_file(m_dir, "sessions_part.json", kPartDaySessions);
	ASSERT_TRUE(m_mgr.loadSessions(file.c_str()));

	// 后偏移：周六凌晨仍算下一交易日
	EXPECT_EQ(20240108u, m_mgr.calcTradingDate("R56OFFPOS", 20240106, 100, true));
	// 后偏移：晚于偏移点 -> 下一交易日
	EXPECT_EQ(20240109u, m_mgr.calcTradingDate("R56OFFPOS", 20240108, 2300, true));
	// 前偏移：早于偏移点 -> 上一交易日
	EXPECT_EQ(20240105u, m_mgr.calcTradingDate("R56OFFNEG", 20240108, 100, true));
	// 前偏移：周六晚 -> 下一交易日
	EXPECT_EQ(20240108u, m_mgr.calcTradingDate("R56OFFNEG", 20240106, 2300, true));
	// 零偏移：周末 -> 下一交易日；工作日 -> 自然日
	EXPECT_EQ(20240108u, m_mgr.calcTradingDate("R56OFFZERO", 20240106, 1000, true));
	EXPECT_EQ(20240109u, m_mgr.calcTradingDate("R56OFFZERO", 20240109, 1000, true));
	// 未知 session：sInfo 为空直接返回原日期
	EXPECT_EQ(20240109u, m_mgr.calcTradingDate("R56NOSUCH", 20240109, 1000, true));
}

// WTSBaseDataMgr.cpp:300-326,328-391
// I03-NEW-053/K28（整批校验后发布）：缺 session、session 未加载的品种不再被“跳过”，而是整份
// 品种文件被拒；空 sections 的会话模板整份会话文件被拒。旧夹具中的 ag/cu/R56EMPTY 改为单独拒绝夹具。
TEST_F(R56BaseDataMgr, CommodityLoadingSkipsMissingSession)
{
	const char* sessions = R"({
		"R56DAY": {"name": "日盘", "offset": 0, "auction": {"from": 915, "to": 925},
			"sections": [{"from": 900, "to": 1130}, {"from": 1300, "to": 1500}]}
	})";
	const char* commodities = R"({
		"SHFE": {
			"rb": {"name": "螺纹", "session": "R56DAY", "holiday": "CHN", "pricetick": 1,
				"volscale": 10, "covermode": 0, "pricemode": 1, "trademode": 0, "lotstick": 1, "minlots": 1}
		}
	})";
	const std::string sfile = r56tmp::write_file(m_dir, "sessions.json", sessions);
	const std::string cfile = r56tmp::write_file(m_dir, "commodities.json", commodities);
	ASSERT_TRUE(m_mgr.loadSessions(sfile.c_str()));
	ASSERT_TRUE(m_mgr.loadCommodities(cfile.c_str()));

	// K28：空 sections 的会话文件整份被拒，已登记的 R56DAY 不受影响
	const std::string emptySecs = r56tmp::write_file(m_dir, "sessions_empty.json",
		R"({"R56EMPTY": {"name": "无时段", "offset": 0, "sections": []}})");
	EXPECT_FALSE(m_mgr.loadSessions(emptySecs.c_str()));
	EXPECT_EQ(nullptr, m_mgr.getSession("R56EMPTY"));
	// I03-NEW-053：缺 session / session 未加载的品种使整份品种文件被拒，同文件的合法品种也不发布
	const std::string noSess = r56tmp::write_file(m_dir, "commodities_nosession.json", R"({"SHFE": {
		"hc": {"name": "热卷", "session": "R56DAY", "pricetick": 1, "volscale": 10},
		"ag": {"name": "白银", "session": "", "pricetick": 1, "volscale": 15}}})");
	EXPECT_FALSE(m_mgr.loadCommodities(noSess.c_str()));
	const std::string unknownSess = r56tmp::write_file(m_dir, "commodities_unknown.json", R"({"SHFE": {
		"cu": {"name": "铜", "session": "R56NOSUCH", "pricetick": 10, "volscale": 5}}})");
	EXPECT_FALSE(m_mgr.loadCommodities(unknownSess.c_str()));
	EXPECT_EQ(nullptr, m_mgr.getCommodity("SHFE.hc"));

	// 正常品种：类别/交易模式/手数步长按配置落库
	WTSCommodityInfo* rb = m_mgr.getCommodity("SHFE.rb");
	ASSERT_NE(nullptr, rb);
	EXPECT_DOUBLE_EQ(1.0, rb->getPriceTick());
	EXPECT_EQ(10u, rb->getVolScale());
	EXPECT_TRUE(rb->isFuture());
	EXPECT_TRUE(rb->canShort());
	EXPECT_EQ(2u, rb->getSessionInfo()->getSectionCount());
	CodeSet* comms = m_mgr.getSessionComms("R56DAY");
	ASSERT_NE(nullptr, comms);
	EXPECT_TRUE(comms->find("SHFE.rb") != comms->end());

	// 被拒文件中的品种均未发布
	EXPECT_EQ(nullptr, m_mgr.getCommodity("SHFE.ag"));
	EXPECT_EQ(nullptr, m_mgr.getCommodity("SHFE.cu"));

	WTSArray* all = m_mgr.getAllSessions();
	ASSERT_NE(nullptr, all);
	EXPECT_EQ(1u, all->size());
	all->release();
}

// WTSBaseDataMgr.cpp:243-298
TEST_F(R56BaseDataMgr, SessionAuctionVariantsAndMissingSections)
{
	const char* sessions = R"({
		"R56AUC1": {"name": "单集合竞价", "offset": 0, "auction": {"from": 915, "to": 925},
			"sections": [{"from": 930, "to": 1130}]},
		"R56AUC2": {"name": "多集合竞价", "offset": 0,
			"auctions": [{"from": 915, "to": 920}, {"from": 921, "to": 925}],
			"sections": [{"from": 930, "to": 1130}]}
	})";
	const std::string file = r56tmp::write_file(m_dir, "sessions_auc.json", sessions);
	ASSERT_TRUE(m_mgr.loadSessions(file.c_str()));

	WTSSessionInfo* auc1 = m_mgr.getSession("R56AUC1");
	ASSERT_NE(nullptr, auc1);
	EXPECT_EQ(1u, auc1->getAuctionSections().size());
	EXPECT_EQ(915u, auc1->getAuctionStartTime());
	EXPECT_EQ(1u, auc1->getSectionCount());

	WTSSessionInfo* auc2 = m_mgr.getSession("R56AUC2");
	ASSERT_NE(nullptr, auc2);
	EXPECT_EQ(2u, auc2->getAuctionSections().size());
	EXPECT_EQ(915u, auc2->getAuctionStartTime());
	EXPECT_EQ(1u, auc2->getSectionCount());

	// I03-NEW-053：缺 sections 的条目使整份会话文件被拒（旧语义为跳过该条），同文件合法模板也不发布
	const std::string noSec = r56tmp::write_file(m_dir, "sessions_nosec.json", R"({
		"R56AUC3": {"name": "合法", "offset": 0, "sections": [{"from": 930, "to": 1130}]},
		"R56NOSEC": {"name": "缺 sections", "offset": 0}
	})");
	EXPECT_FALSE(m_mgr.loadSessions(noSec.c_str()));
	EXPECT_EQ(nullptr, m_mgr.getSession("R56NOSEC"));
	EXPECT_EQ(nullptr, m_mgr.getSession("R56AUC3"));
	EXPECT_EQ(nullptr, m_mgr.getSessionComms("R56NOSEC"));
	EXPECT_NE(nullptr, m_mgr.getSession("R56AUC1"));
}
