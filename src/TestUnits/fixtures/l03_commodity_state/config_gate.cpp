#include <gtest/gtest.h>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "Includes/WTSContractInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"

namespace {
class CommodityFiles
{
public:
    CommodityFiles()
    {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir = std::filesystem::temp_directory_path() / ("wt-l03-ready-" + std::to_string(stamp));
        std::filesystem::create_directory(dir);
    }
    ~CommodityFiles() { std::filesystem::remove_all(dir); }
    std::string write(const std::string& name, const std::string& text)
    {
        auto file = dir / name;
        std::ofstream out(file);
        out << text;
        out.close();
        return file.string();
    }
    std::filesystem::path dir;
};

void loadSession(WTSBaseDataMgr& manager, CommodityFiles& files)
{
    const auto session = files.write("sessions.json",
        R"({"DAY":{"name":"session","offset":0,"sections":[{"from":900,"to":1500}]}})");
    ASSERT_TRUE(manager.loadSessions(session.c_str()));
}
}


TEST(L03CommodityReady, CompleteCommodityAndAssociatedContract)
{
    CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
    const auto cfg = files.write("good.json", R"({"SHFE":{"rb":{"name":"rebar","session":"DAY","pricetick":1,"volscale":10,"trademode":0,"covermode":0,"pricemode":0}}})");
    ASSERT_TRUE(manager.loadCommodities(cfg.c_str()));
    auto* commodity = manager.getCommodity("SHFE", "rb");
    ASSERT_NE(nullptr, commodity); EXPECT_TRUE(commodity->hasValidConfiguration()); EXPECT_TRUE(commodity->canShort());
    const auto contracts = files.write("linked.json", R"({"SHFE":{"rb2510":{"name":"rebar","exchg":"SHFE","product":"rb"}}})");
    ASSERT_TRUE(manager.loadContracts(contracts.c_str()));
    auto* contract = manager.getContract("rb2510", "SHFE");
    ASSERT_NE(nullptr, contract); EXPECT_EQ(commodity, contract->getCommInfo());
}

TEST(L03CommodityReady, MissingAndInvalidInputsRejectWholeCommodityImport)
{
    for (const auto& bad : {R"({"volscale":10})", R"({"pricetick":1})", R"({"pricetick":0,"volscale":10})", R"({"pricetick":1,"volscale":-1})", R"({"pricetick":1,"volscale":1.5})", R"({"pricetick":"nan","volscale":10})", R"({"pricetick":1,"volscale":10,"lotstick":0})", R"({"pricetick":1,"volscale":10,"trademode":4})"})
    {
        CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
        std::string invalid(bad); invalid.pop_back(); invalid += R"(,"name":"bad","session":"DAY"})";
        const auto cfg = files.write("mixed.json", R"({"SHFE":{"aa":{"name":"good","session":"DAY","pricetick":1,"volscale":10},"zz":)" + invalid + "}}");
        EXPECT_FALSE(manager.loadCommodities(cfg.c_str())) << bad;
        EXPECT_EQ(nullptr, manager.getCommodity("SHFE", "aa"));
        EXPECT_EQ(nullptr, manager.getCommodity("SHFE", "zz"));
    }
}

TEST(L03CommodityReady, RulesImportRejectsAtomicallyAndAcceptsCompleteRules)
{
    CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
    const auto bad = files.write("bad-rules.json", R"({"SHFE":{"aa":{"name":"good","exchg":"SHFE","rules":{"session":"DAY","pricetick":1,"volscale":10}},"zz":{"name":"bad","exchg":"SHFE","rules":{"session":"DAY","volscale":10}}}})");
    EXPECT_FALSE(manager.loadContracts(bad.c_str()));
    EXPECT_EQ(nullptr, manager.getContract("aa", "SHFE")); EXPECT_EQ(nullptr, manager.getCommodity("SHFE", "aa"));
    EXPECT_EQ(nullptr, manager.getContract("zz", "SHFE")); EXPECT_EQ(nullptr, manager.getCommodity("SHFE", "zz"));
    const auto good = files.write("good-rules.json", R"({"SHFE":{"aa":{"name":"good","exchg":"SHFE","rules":{"session":"DAY","pricetick":1,"volscale":10}}}})");
    ASSERT_TRUE(manager.loadContracts(good.c_str()));
    auto* contract = manager.getContract("aa", "SHFE"); ASSERT_NE(nullptr, contract);
    ASSERT_NE(nullptr, contract->getCommInfo()); EXPECT_TRUE(contract->getCommInfo()->hasValidConfiguration());
    EXPECT_EQ(contract->getCommInfo(), manager.getCommodity("SHFE", "aa"));
}

TEST(L03CommodityReady, FailedContractImportDoesNotAlterExistingCommodity)
{
    CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
    const auto cfg = files.write("good.json", R"({"SHFE":{"rb":{"name":"rebar","session":"DAY","pricetick":1,"volscale":10}}})");
    ASSERT_TRUE(manager.loadCommodities(cfg.c_str()));
    auto* commodity = manager.getCommodity("SHFE", "rb"); ASSERT_NE(nullptr, commodity);
    const auto bad = files.write("mixed-contracts.json", R"({"SHFE":{"aa":{"name":"good","exchg":"SHFE","product":"rb"},"zz":{"name":"bad","exchg":"SHFE","rules":{"session":"DAY","pricetick":1}}}})");
    EXPECT_FALSE(manager.loadContracts(bad.c_str())); EXPECT_EQ(nullptr, manager.getContract("aa", "SHFE"));
    EXPECT_TRUE(commodity->getCodes().empty()); EXPECT_EQ(commodity, manager.getCommodity("SHFE", "rb"));
}

TEST(L03CommodityReady, RulesRequireMultiplierAndExistingSession)
{
    for (const auto& rules : {R"({"session":"DAY","pricetick":1})", R"({"session":"MISSING","pricetick":1,"volscale":10})"})
    {
        CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
        const auto cfg = files.write("bad.json", std::string(R"({"SHFE":{"rb":{"name":"rebar","exchg":"SHFE","rules":)") + rules + "}}}");
        EXPECT_FALSE(manager.loadContracts(cfg.c_str()));
        EXPECT_EQ(nullptr, manager.getCommodity("SHFE", "rb"));
        EXPECT_EQ(nullptr, manager.getContract("rb", "SHFE"));
    }
}

TEST(L03CommodityReady, ExistingConfigAndContractImport)
{
    CommodityFiles files; WTSBaseDataMgr manager;
    // Stable copies of the existing r104qf control inputs; no external files required.
    const auto sessions = files.write("sessions.json", R"({"SD0930":{"name":"r104","offset":0,"auction":{"from":929,"to":930},"sections":[{"from":930,"to":1130},{"from":1300,"to":1500}]}})");
    const auto commodities = files.write("commodities.json", R"({"SSE":{"r104p":{"covermode":0,"pricemode":0,"category":1,"precision":1,"pricetick":0.01,"volscale":100,"name":"r104p","exchg":"SSE","session":"SD0930","holiday":"CHINA"}}})");
    const auto contracts = files.write("contracts.json", R"({"SSE":{"r104p2412":{"name":"r104p2412","code":"r104p2412","exchg":"SSE","product":"r104p","maxlimitqty":20,"maxmarketqty":10}}})");
    ASSERT_TRUE(manager.loadSessions(sessions.c_str()));
    ASSERT_TRUE(manager.loadCommodities(commodities.c_str()));
    ASSERT_TRUE(manager.loadContracts(contracts.c_str()));
    auto* commodity = manager.getCommodity("SSE", "r104p");
    auto* contract = manager.getContract("r104p2412", "SSE");
    ASSERT_NE(nullptr, commodity); ASSERT_NE(nullptr, contract);
    EXPECT_TRUE(commodity->hasValidConfiguration()); EXPECT_EQ(commodity, contract->getCommInfo());
}

TEST(L03CommodityReady, ContractCanReferenceEarlierRuleDerivedCommodity)
{
    CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
    const auto cfg = files.write("derived-and-linked.json", R"({"SHFE":{"aa":{"name":"derived","exchg":"SHFE","rules":{"session":"DAY","pricetick":1,"volscale":10}},"zz":{"name":"linked","exchg":"SHFE","product":"aa"}}})");
    ASSERT_TRUE(manager.loadContracts(cfg.c_str()));
    auto* first = manager.getContract("aa", "SHFE"); auto* second = manager.getContract("zz", "SHFE");
    ASSERT_NE(nullptr, first); ASSERT_NE(nullptr, second);
    EXPECT_EQ(first->getCommInfo(), second->getCommInfo());
}

TEST(L03CommodityReady, CommaNumericLocalePreservesNumericAndStringValues)
{
    struct NumericLocaleGuard
    {
        std::string previous;
        NumericLocaleGuard() : previous(std::setlocale(LC_NUMERIC, nullptr) ? std::setlocale(LC_NUMERIC, nullptr) : "C") {}
        ~NumericLocaleGuard() { std::setlocale(LC_NUMERIC, previous.c_str()); }
    } guard;

    const char* selected = nullptr;
    for (const char* candidate : {"de_DE.UTF-8", "fr_FR.UTF-8", "nl_NL.UTF-8"})
    {
        if (std::setlocale(LC_NUMERIC, candidate) && std::string(std::localeconv()->decimal_point) == ",")
        {
            selected = candidate;
            break;
        }
    }
    if (!selected) GTEST_SKIP() << "No available decimal-comma LC_NUMERIC locale";
    SCOPED_TRACE(selected);
    std::printf("LC_NUMERIC locale: %s, decimal point: %s\n", selected, std::localeconv()->decimal_point);

    CommodityFiles files; WTSBaseDataMgr manager; loadSession(manager, files);
    const auto cfg = files.write("locale.json", R"({"SHFE":{"num":{"name":"numeric","session":"DAY","pricetick":0.25,"volscale":10,"category":1,"covermode":1,"pricemode":1,"trademode":1,"lotstick":0.5,"minlots":1.5},"str":{"name":"string","session":"DAY","pricetick":"0.125","volscale":"20","category":"2","covermode":"2","pricemode":"2","trademode":"2","lotstick":"0.25","minlots":"1.25"}}})");
    ASSERT_TRUE(manager.loadCommodities(cfg.c_str()));
    auto* numeric = manager.getCommodity("SHFE", "num");
    auto* text = manager.getCommodity("SHFE", "str");
    ASSERT_NE(nullptr, numeric); ASSERT_NE(nullptr, text);
    EXPECT_DOUBLE_EQ(0.25, numeric->getPriceTick()); EXPECT_EQ(10u, numeric->getVolScale());
    EXPECT_EQ(CC_Future, numeric->getCategoty()); EXPECT_EQ(CM_CoverToday, numeric->getCoverMode());
    EXPECT_EQ(PM_Limit, numeric->getPriceMode()); EXPECT_EQ(TM_Long, numeric->getTradingMode());
    EXPECT_DOUBLE_EQ(0.5, numeric->getLotsTick()); EXPECT_DOUBLE_EQ(1.5, numeric->getMinLots());
    EXPECT_DOUBLE_EQ(0.125, text->getPriceTick()); EXPECT_EQ(20u, text->getVolScale());
    EXPECT_EQ(CC_FutOption, text->getCategoty()); EXPECT_EQ(CM_UNFINISHED, text->getCoverMode());
    EXPECT_EQ(PM_Market, text->getPriceMode()); EXPECT_EQ(TM_LongT1, text->getTradingMode());
    EXPECT_DOUBLE_EQ(0.25, text->getLotsTick()); EXPECT_DOUBLE_EQ(1.25, text->getMinLots());

    const auto rules = files.write("rules-locale.json", R"({"SHFE":{"derived":{"name":"rules","exchg":"SHFE","rules":{"session":"DAY","pricetick":"0.0625","volscale":"40","category":"1","covermode":"1","pricemode":"1","trademode":"1","lotstick":"0.125","minlots":"1.75"}}}})");
    ASSERT_TRUE(manager.loadContracts(rules.c_str()));
    auto* derived = manager.getCommodity("SHFE", "derived"); ASSERT_NE(nullptr, derived);
    EXPECT_DOUBLE_EQ(0.0625, derived->getPriceTick()); EXPECT_EQ(40u, derived->getVolScale());
    EXPECT_EQ(CC_Future, derived->getCategoty()); EXPECT_EQ(CM_CoverToday, derived->getCoverMode());
    EXPECT_EQ(PM_Limit, derived->getPriceMode()); EXPECT_EQ(TM_Long, derived->getTradingMode());
    EXPECT_DOUBLE_EQ(0.125, derived->getLotsTick()); EXPECT_DOUBLE_EQ(1.75, derived->getMinLots());
}
