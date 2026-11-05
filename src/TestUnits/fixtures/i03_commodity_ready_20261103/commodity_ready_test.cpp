// BUG-L03-002: deterministic unconfigured state, explicit ready state and the
// WTSBaseDataMgr admission gate. Compiles against both the pre-edit and the
// current header: the ready API is detected so that the pre-edit build runs
// and fails instead of failing to compile.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>

#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSVariant.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSUtils/WTSCfgLoader.h"

#ifndef WT_I03_DIST_COMMON
#error "WT_I03_DIST_COMMON must point to dist/common"
#endif

using namespace wtp;

namespace {

template<class T, class = void> struct HasReady : std::false_type {};
template<class T>
struct HasReady<T, std::void_t<decltype(std::declval<T&>().markReady()), decltype(std::declval<const T&>().isReady())>>
	: std::true_type {};

template<class T> bool markReady(T* c)
{
	if constexpr (HasReady<T>::value) return c->markReady(); else { (void)c; return false; }
}
template<class T> bool isReady(const T* c)
{
	if constexpr (HasReady<T>::value) return c->isReady(); else { (void)c; return false; }
}

struct Released { void operator()(WTSObject* o) const { if (o) o->release(); } };

WTSSessionInfo* daySession()
{
	WTSSessionInfo* s = WTSSessionInfo::create("DAY", "day");
	s->addTradingSection(900, 1500);
	return s;
}

void configureAllButTradingMode(WTSCommodityInfo* c)
{
	c->setVolScale(10); c->setPriceTick(1.0); c->setLotsTick(1.0); c->setMinLots(1.0);
	c->setCategory(CC_Future); c->setCoverMode(CM_OpenCover); c->setPriceMode(PM_Both);
}

class Files
{
public:
	Files()
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		dir = std::filesystem::temp_directory_path() / ("wt-i03-ready-" + std::to_string(stamp));
		std::filesystem::create_directories(dir);
	}
	~Files() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
	std::string write(const std::string& name, const std::string& text)
	{
		const auto p = dir / name;
		std::ofstream(p) << text;
		return p.string();
	}
	std::filesystem::path dir;
};

void loadDay(WTSBaseDataMgr& mgr, Files& files)
{
	const auto s = files.write("sessions.json", R"({"DAY":{"name":"day","offset":0,"sections":[{"from":900,"to":1500}]}})");
	ASSERT_TRUE(mgr.loadSessions(s.c_str()));
}

} // namespace

TEST(I03CommodityReady, ReadyApiIsPresent)
{
	EXPECT_TRUE(HasReady<WTSCommodityInfo>::value);
}

// Before the fix, the zero-initialized trade mode equals TM_Both, so an
// unconfigured object granted short selling.
TEST(I03CommodityReady, CreateIsDeterministicAndFailsClosed)
{
	std::unique_ptr<WTSCommodityInfo, Released> c(WTSCommodityInfo::create("rb", "rebar", "SHFE", "DAY", "CHINA"));
	ASSERT_NE(nullptr, c);
	EXPECT_EQ(0u, c->getVolScale());
	EXPECT_EQ(0.0, c->getPriceTick());
	EXPECT_EQ(0.0, c->getLotsTick());
	EXPECT_EQ(0.0, c->getMinLots());
	EXPECT_EQ(0u, static_cast<uint32_t>(c->getCategoty()));
	EXPECT_EQ(0u, static_cast<uint32_t>(c->getCoverMode()));
	EXPECT_EQ(0u, static_cast<uint32_t>(c->getPriceMode()));
	EXPECT_EQ(nullptr, c->getSessionInfo());
	EXPECT_EQ(TM_None, c->getTradingMode());
	EXPECT_FALSE(c->canShort());
	EXPECT_FALSE(c->isT1());
	EXPECT_FALSE(c->hasValidConfiguration());
	EXPECT_FALSE(isReady(c.get()));
	EXPECT_FALSE(markReady(c.get()));
	EXPECT_FALSE(isReady(c.get()));
}

TEST(I03CommodityReady, ReadyRequiresExplicitTradingModeAndSession)
{
	std::unique_ptr<WTSCommodityInfo, Released> c(WTSCommodityInfo::create("rb", "rebar", "SHFE", "DAY", "CHINA"));
	ASSERT_NE(nullptr, c);
	configureAllButTradingMode(c.get());
	EXPECT_FALSE(c->hasValidConfiguration()); // trading mode never set
	EXPECT_FALSE(c->canShort());
	EXPECT_FALSE(markReady(c.get()));
	c->setTradingMode(TM_Both);
	EXPECT_TRUE(c->hasValidConfiguration());
	EXPECT_FALSE(markReady(c.get())); // no session bound
	std::unique_ptr<WTSSessionInfo, Released> s(daySession());
	c->setSessionInfo(s.get());
	EXPECT_TRUE(markReady(c.get()));
	EXPECT_TRUE(isReady(c.get()));
	EXPECT_TRUE(c->canShort());
	c->setPriceTick(0.0); // a later invalid setter revokes readiness
	EXPECT_FALSE(isReady(c.get()));
	c->setPriceTick(1.0);
	EXPECT_TRUE(isReady(c.get()));
}

TEST(I03CommodityReady, ExplicitTradingModesKeepTheirMeaning)
{
	std::unique_ptr<WTSCommodityInfo, Released> c(WTSCommodityInfo::create("600000", "pf", "SSE", "DAY", "CHINA"));
	ASSERT_NE(nullptr, c);
	c->setTradingMode(TM_Long);
	EXPECT_EQ(TM_Long, c->getTradingMode()); EXPECT_FALSE(c->canShort()); EXPECT_FALSE(c->isT1());
	c->setTradingMode(TM_LongT1);
	EXPECT_EQ(TM_LongT1, c->getTradingMode()); EXPECT_FALSE(c->canShort()); EXPECT_TRUE(c->isT1());
	c->setTradingMode(TM_Both);
	EXPECT_EQ(TM_Both, c->getTradingMode()); EXPECT_TRUE(c->canShort()); EXPECT_FALSE(c->isT1());
}

TEST(I03CommodityReady, LoaderPublishesOnlyReadyCommodities)
{
	Files files; WTSBaseDataMgr mgr; loadDay(mgr, files);
	const auto cfg = files.write("comms.json",
		R"({"SHFE":{"rb":{"name":"rebar","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":10},)"
		R"("ag":{"name":"silver","session":"DAY","holiday":"CHINA","pricetick":1,"volscale":15,"trademode":1}}})");
	ASSERT_TRUE(mgr.loadCommodities(cfg.c_str()));
	WTSCommodityInfo* rb = mgr.getCommodity("SHFE", "rb");
	ASSERT_NE(nullptr, rb);
	EXPECT_TRUE(isReady(rb));
	EXPECT_EQ(TM_Both, rb->getTradingMode()); // documented loader default for a missing trademode
	EXPECT_TRUE(rb->canShort());
	WTSCommodityInfo* ag = mgr.getCommodity("SHFE", "ag");
	ASSERT_NE(nullptr, ag);
	EXPECT_TRUE(isReady(ag));
	EXPECT_EQ(TM_Long, ag->getTradingMode());
	EXPECT_FALSE(ag->canShort());
}

TEST(I03CommodityReady, MissingRequiredFieldsRejectWholeImport)
{
	const char* bad[] = {
		R"({"name":"x","session":"DAY","volscale":10})",                 // no pricetick
		R"({"name":"x","session":"DAY","pricetick":1})",                 // no volscale
		R"({"name":"x","session":"DAY","pricetick":0,"volscale":10})",
		R"({"name":"x","session":"NOPE","pricetick":1,"volscale":10})",  // unknown session
		R"({"name":"x","session":"DAY","pricetick":1,"volscale":10,"trademode":4})",
	};
	for (const char* entry : bad)
	{
		Files files; WTSBaseDataMgr mgr; loadDay(mgr, files);
		const auto cfg = files.write("c.json", std::string(R"({"SHFE":{"aa":{"name":"ok","session":"DAY","pricetick":1,"volscale":10},"zz":)") + entry + "}}");
		EXPECT_FALSE(mgr.loadCommodities(cfg.c_str())) << entry;
		EXPECT_EQ(nullptr, mgr.getCommodity("SHFE", "aa")) << entry;
		EXPECT_EQ(nullptr, mgr.getCommodity("SHFE", "zz")) << entry;
	}
}

TEST(I03CommodityReady, RulesDerivedCommodityIsReadyOrRejected)
{
	Files files; WTSBaseDataMgr mgr; loadDay(mgr, files);
	const auto bad = files.write("bad.json", R"({"SHFE":{"zz":{"name":"z","exchg":"SHFE","rules":{"session":"DAY","volscale":10}}}})");
	EXPECT_FALSE(mgr.loadContracts(bad.c_str()));
	EXPECT_EQ(nullptr, mgr.getContract("zz", "SHFE"));
	EXPECT_EQ(nullptr, mgr.getCommodity("SHFE", "zz"));
	const auto good = files.write("good.json", R"({"SHFE":{"aa":{"name":"a","exchg":"SHFE","rules":{"session":"DAY","pricetick":1,"volscale":10}}}})");
	ASSERT_TRUE(mgr.loadContracts(good.c_str()));
	WTSContractInfo* ct = mgr.getContract("aa", "SHFE");
	ASSERT_NE(nullptr, ct);
	ASSERT_NE(nullptr, ct->getCommInfo());
	EXPECT_TRUE(isReady(ct->getCommInfo()));
}

// Regression control: every commodity of the shipped base data is admitted.
TEST(I03CommodityReady, ShippedBaseDataIsFullyReady)
{
	WTSBaseDataMgr mgr;
	const std::string root = WT_I03_DIST_COMMON;
	ASSERT_TRUE(mgr.loadSessions((root + "/sessions.json").c_str()));
	ASSERT_TRUE(mgr.loadCommodities((root + "/commodities.json").c_str()));
	WTSVariant* cfg = WTSCfgLoader::load_from_file((root + "/commodities.json").c_str());
	ASSERT_NE(nullptr, cfg);
	uint32_t total = 0, ready = 0;
	for (const std::string& exchg : cfg->memberNames())
		for (const std::string& pid : cfg->get(exchg)->memberNames())
		{
			++total;
			WTSCommodityInfo* c = mgr.getCommodity(exchg.c_str(), pid.c_str());
			if (c && isReady(c)) ++ready;
		}
	cfg->release();
	EXPECT_EQ(70u, total);
	EXPECT_EQ(total, ready);
}
