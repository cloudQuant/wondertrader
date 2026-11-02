/**
 * r170_eventnotifier_json_test.cpp — lane R170 / wave25
 *
 * 目标：src/WtCore/EventNotifier.cpp 的 tradeToJson/orderToJson 空对象兜底。
 * 两个函数是 private，正常 notify 入口先判空后才会调用，兜底分支只能直接调用；
 * 本 TU 用一次 `#define private public` 只包住 EventNotifier.h（同 r105/r160 手法），
 * 不修改生产头文件，也不起 MQ/线程。
 *
 * 覆盖行段：EventNotifier.cpp:234-238（tradeToJson NULL）、:269-273（orderToJson NULL）
 */
#include <gtest/gtest.h>

#include <rapidjson/document.h>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define private public
#include <WtCore/EventNotifier.h>
#undef private
#include "Includes/WTSTradeDef.hpp"

using namespace wtp;

namespace {

struct PublishedMessage
{
    std::string topic;
    std::string data;
};

std::vector<PublishedMessage> g_published;

void capturePublish(uint32_t, const char* topic, const char* data, uint32_t length)
{
    g_published.push_back({ topic == nullptr ? "" : topic,
        data == nullptr ? "" : std::string(data, length) });
}

struct TradeReleaser
{
    void operator()(WTSTradeInfo* value) const { if (value != nullptr) value->release(); }
};

struct OrderReleaser
{
    void operator()(WTSOrderInfo* value) const { if (value != nullptr) value->release(); }
};

} // namespace

TEST(R170EventNotifier, NullTradeAndOrderSerializeAsEmptyObject)
{
    EventNotifier notifier;

    std::string trade = "sentinel";
    notifier.tradeToJson("r170", 7, "SHFE.ag.2512", NULL, trade);
    EXPECT_EQ("{}", trade);

    std::string order = "sentinel";
    notifier.orderToJson("r170", 7, "SHFE.ag.2512", NULL, order);
    EXPECT_EQ("{}", order);
}

TEST(R170EventNotifier, PublishesOnlyCompleteJsonForFiniteTradeAndOrderValues)
{
    g_published.clear();
    EventNotifier notifier;
    notifier._mq_sid = 42;
    notifier._publisher = &capturePublish;

    const double nonFinite = std::numeric_limits<double>::quiet_NaN();
    std::unique_ptr<WTSTradeInfo, TradeReleaser> finiteTrade(WTSTradeInfo::create("ag2406", "SHFE"));
    std::unique_ptr<WTSTradeInfo, TradeReleaser> invalidTrade(WTSTradeInfo::create("ag2406", "SHFE"));
    std::unique_ptr<WTSOrderInfo, OrderReleaser> finiteOrder(WTSOrderInfo::create());
    std::unique_ptr<WTSOrderInfo, OrderReleaser> invalidOrder(WTSOrderInfo::create());
    ASSERT_NE(nullptr, finiteTrade.get());
    ASSERT_NE(nullptr, invalidTrade.get());
    ASSERT_NE(nullptr, finiteOrder.get());
    ASSERT_NE(nullptr, invalidOrder.get());

    finiteTrade->setVolume(2.0);
    finiteTrade->setPrice(3500.25);
    invalidTrade->setVolume(2.0);
    invalidTrade->setPrice(nonFinite);
    finiteOrder->setCode("ag2406");
    finiteOrder->setExchange("SHFE");
    finiteOrder->setVolume(3.0);
    finiteOrder->setPrice(3510.75);
    invalidOrder->setCode("ag2406");
    invalidOrder->setExchange("SHFE");
    invalidOrder->setVolume(3.0);
    invalidOrder->setPrice(nonFinite);

    notifier.notify("r170", 1, "SHFE.ag2406", finiteTrade.get());
    notifier.notify("r170", 2, "SHFE.ag2406", invalidTrade.get());
    notifier.notify("r170", 3, "SHFE.ag2406", finiteOrder.get());
    notifier.notify("r170", 4, "SHFE.ag2406", invalidOrder.get());
    notifier._asyncio.poll();

    ASSERT_EQ(2u, g_published.size()) << "failed writer output must not reach the publisher";
    EXPECT_EQ("TRD_TRADE", g_published[0].topic);
    EXPECT_EQ("TRD_ORDER", g_published[1].topic);
    for (const PublishedMessage& message : g_published)
    {
        rapidjson::Document parsed;
        parsed.Parse(message.data.c_str());
        ASSERT_FALSE(parsed.HasParseError()) << message.data;
        ASSERT_TRUE(parsed.IsObject()) << message.data;
        ASSERT_TRUE(parsed.HasMember("price")) << message.data;
        EXPECT_TRUE(std::isfinite(parsed["price"].GetDouble()));
        if (message.topic == "TRD_TRADE")
            EXPECT_DOUBLE_EQ(3500.25, parsed["price"].GetDouble());
        else
            EXPECT_DOUBLE_EQ(3510.75, parsed["price"].GetDouble());
    }
    EXPECT_EQ(1u, finiteTrade->retainCount());
    EXPECT_EQ(1u, invalidTrade->retainCount());
    EXPECT_EQ(1u, finiteOrder->retainCount());
    EXPECT_EQ(1u, invalidOrder->retainCount());
}
