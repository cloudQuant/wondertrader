#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <unistd.h>
#include "Includes/WTSSessionInfo.hpp"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WTSTools/WTSLogger.h"
using namespace wtp;

TEST(I03SessionContract, FullDayAuctionSetAndAddSurviveOffset)
{
    for (int offset : {0, 300, -300, 1440, -1440}) {
        auto* s = WTSSessionInfo::create("a", "a", offset);
        ASSERT_TRUE(s->trySetAuctionTime(0, 2400));
        for (int next : {offset, 300, -300, 0}) {
            s->setOffsetMins(next);
            for (uint32_t t : {0u, 1u, 900u, 1200u, 2359u}) EXPECT_TRUE(s->isInAuctionTime(t));
            EXPECT_EQ(0u, s->getAuctionSections()[0].first_raw);
            EXPECT_EQ(2400u, s->getAuctionSections()[0].second_raw);
        }
        ASSERT_TRUE(s->trySetAuctionTime(0, 0));
        for (int next : {300, -300, 0}) {
            s->setOffsetMins(next);
            EXPECT_FALSE(s->isInAuctionTime(0));
            EXPECT_FALSE(s->isInAuctionTime(1200));
        }
        ASSERT_TRUE(s->tryAddAuctionTime(0, 2400));
        for (int next : {300, -300, 0}) {
            s->setOffsetMins(next);
            EXPECT_TRUE(s->isInAuctionTime(0));
            EXPECT_TRUE(s->isInAuctionTime(2359));
        }
        s->release();
    }
}

TEST(I03SessionContract, CheckedSectionsRejectWithoutMutation)
{
    auto* s = WTSSessionInfo::create("a", "a");
    ASSERT_TRUE(s->tryAddTradingSection(900, 1000));
    ASSERT_TRUE(s->trySetAuctionTime(855, 859));
    const auto trading = s->getTradingSections();
    const auto auction = s->getAuctionSections();
    for (auto p : {std::pair<uint32_t,uint32_t>{900,900}, {0,2400}, {2400,0}, {2400,2400}, {1260,1300}, {900,2401}}) {
        EXPECT_FALSE(s->tryAddTradingSection(p.first, p.second));
        s->addTradingSection(p.first, p.second);
    }
    EXPECT_FALSE(s->trySetAuctionTime(2460, 100));
    EXPECT_FALSE(s->tryAddAuctionTime(0, 2401));
    s->setAuctionTime(2460, 100);
    s->addAuctionTime(0, 2401);
    ASSERT_EQ(trading.size(), s->getTradingSections().size());
    ASSERT_EQ(auction.size(), s->getAuctionSections().size());
    EXPECT_EQ(trading[0].first_raw, s->getTradingSections()[0].first_raw);
    EXPECT_EQ(trading[0].second_raw, s->getTradingSections()[0].second_raw);
    EXPECT_EQ(auction[0].first_raw, s->getAuctionSections()[0].first_raw);
    EXPECT_EQ(auction[0].second_raw, s->getAuctionSections()[0].second_raw);
    EXPECT_EQ(60u, s->getTradingMins());
    s->release();
}

TEST(I03SessionContract, TimeQueriesRequireValidHhmm)
{
    auto* s = WTSSessionInfo::create("a", "a", 300);
    ASSERT_TRUE(s->tryAddTradingSection(900, 1000));
    ASSERT_TRUE(s->trySetAuctionTime(855, 859));
    EXPECT_TRUE(s->isInTradingTime(930));
    EXPECT_FALSE(s->isInTradingTime(1100));
    EXPECT_TRUE(s->isInAuctionTime(856));
    EXPECT_FALSE(s->isInAuctionTime(859));
    for (uint32_t invalid : {1260u, 2360u, 2401u}) {
        EXPECT_FALSE(s->isInTradingTime(invalid));
        EXPECT_FALSE(s->isInAuctionTime(invalid));
    }
    ASSERT_TRUE(s->tryAddTradingSection(0, 0));
    ASSERT_TRUE(s->trySetAuctionTime(0, 2400));
    for (uint32_t valid : {0u, 930u, 2359u, 2400u}) {
        EXPECT_TRUE(s->isInTradingTime(valid));
        EXPECT_TRUE(s->isInAuctionTime(valid));
    }
    for (uint32_t invalid : {1260u, 2360u, 2401u}) {
        EXPECT_FALSE(s->isInTradingTime(invalid));
        EXPECT_FALSE(s->isInAuctionTime(invalid));
    }
    s->release();
}

TEST(I03SessionContract, FullDayAndNightMapping)
{
    auto* s = WTSSessionInfo::create("a", "a");
    ASSERT_TRUE(s->tryAddTradingSection(0, 0));
    for (int offset : {0, 300, -300, 2880, -2880}) {
        s->setOffsetMins(offset);
        EXPECT_EQ(1440u, s->getTradingMins());
        EXPECT_EQ(86400u, s->getTradingSeconds());
        ASSERT_EQ(1u, s->getSecMinList().size());
        EXPECT_EQ(1440u, s->getSecMinList()[0]);
        for (uint32_t t : {0u, 1u, 900u, 1200u, 2359u}) {
            EXPECT_TRUE(s->isInTradingTime(t));
            const uint32_t m = t / 100 * 60 + t % 100;
            EXPECT_EQ(m, s->timeToMinutes(t));
            EXPECT_EQ(t, s->minuteToTime(m));
            EXPECT_EQ(m * 60, s->timeToSeconds(t * 100));
            EXPECT_EQ(t * 100, s->secondsToTime(m * 60));
        }
        // 00:00 is the opening label; 24:00 denotes the closing boundary.
        EXPECT_EQ(0u, s->timeToMinutes(0));
        EXPECT_EQ(1440u, s->timeToMinutes(2400));
        EXPECT_EQ(0u, s->timeToSeconds(0));
        EXPECT_EQ(86399u, s->timeToSeconds(240000));
        EXPECT_EQ(0u, s->minuteToTime(0));
        EXPECT_EQ(2400u, s->minuteToTime(1440));
        EXPECT_EQ(0u, s->secondsToTime(0));
        EXPECT_EQ(240000u, s->secondsToTime(86400));
        EXPECT_FALSE(s->isInTradingTime(0, true));
    }
    s->release();
    auto* n = WTSSessionInfo::create("n", "n", -1260);
    ASSERT_TRUE(n->tryAddTradingSection(2100, 2300));
    EXPECT_EQ(30u, n->timeToMinutes(2130));
    EXPECT_EQ(2130u, n->minuteToTime(30));
    EXPECT_EQ(1800u, n->timeToSeconds(213000));
    EXPECT_EQ(213000u, n->secondsToTime(1800));
    n->setOffsetMins(120); // Mapped section is 23:00–01:00.
    EXPECT_EQ(120u, n->getTradingMins());
    EXPECT_EQ(7200u, n->getTradingSeconds());
    EXPECT_EQ(120u, n->getSecMinList().back());
    EXPECT_TRUE(n->isInTradingTime(2200));
    EXPECT_FALSE(n->isInTradingTime(2000));
    EXPECT_EQ(60u, n->timeToMinutes(2200));
    EXPECT_EQ(2200u, n->minuteToTime(60));
    EXPECT_EQ(2300u, n->minuteToTime(120));
    EXPECT_EQ(3600u, n->timeToSeconds(220000));
    EXPECT_EQ(220000u, n->secondsToTime(3600));
    EXPECT_EQ(230000u, n->secondsToTime(7200));
    n->release();
}

static std::string writeConfig(const std::string& body)
{
    char path[] = "/tmp/wt-i03-session-config-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return {};
    close(fd);
    std::string jsonPath = std::string(path) + ".json";
    if (rename(path, jsonPath.c_str()) != 0) return {};
    std::ofstream out(jsonPath);
    out << body;
    return jsonPath;
}

TEST(I03SessionContract, ConfigBoundariesAndAtomicPublication)
{
    WTSLogger::init(R"({"root":{"level":"debug","async":false,"sinks":[]}})", false, nullptr);
    WTSBaseDataMgr manager;
    const auto load = [&](const std::string& text) {
        const std::string path = writeConfig(text);
        if (path.empty()) return false;
        bool result = manager.loadSessions(path.c_str());
        unlink(path.c_str());
        return result;
    };
    ASSERT_TRUE(load(R"({"EXIST":{"name":"existing","offset":0,"sections":[{"from":900,"to":1000}]}})"));
    auto* original = manager.getSession("EXIST");
    ASSERT_NE(nullptr, original);
    for (const auto& bad : {std::string("1441"), std::string("-1441"), std::string("2147483647")}) {
        EXPECT_FALSE(load("{\"CANDIDATE\":{\"name\":\"ok\",\"sections\":[{\"from\":900,\"to\":1000}]},\"INVALID\":{\"name\":\"bad\",\"offset\":" + bad + ",\"sections\":[{\"from\":900,\"to\":1000}]}}"));
        EXPECT_EQ(nullptr, manager.getSession("CANDIDATE"));
        EXPECT_EQ(original, manager.getSession("EXIST"));
    }
    EXPECT_FALSE(load(R"({"CANDIDATE":{"name":"ok","sections":[{"from":900,"to":1000}]},"INVALID":{"name":"bad","sections":[{"from":900,"to":900}]}})"));
    EXPECT_EQ(nullptr, manager.getSession("CANDIDATE"));
    EXPECT_EQ(original, manager.getSession("EXIST"));
    EXPECT_FALSE(load(R"({"INVALID":{"name":"bad","auction":{"from":1260,"to":1300},"sections":[{"from":900,"to":1000}]}})"));
    EXPECT_TRUE(load(R"({"PLUS":{"name":"plus","offset":1440,"auction":{"from":0,"to":2400},"sections":[{"from":0,"to":0}]},"MINUS":{"name":"minus","offset":-1440,"sections":[{"from":2100,"to":2300}]}})"));
    ASSERT_NE(nullptr, manager.getSession("PLUS"));
    ASSERT_NE(nullptr, manager.getSession("MINUS"));
    EXPECT_TRUE(manager.getSession("PLUS")->isInAuctionTime(1200));
    EXPECT_EQ(-1440, manager.getSession("MINUS")->getOffsetMins());
}
