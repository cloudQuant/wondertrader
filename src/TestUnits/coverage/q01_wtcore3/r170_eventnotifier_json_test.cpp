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

#include <string>

#define private public
#include <WtCore/EventNotifier.h>
#undef private

using namespace wtp;

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
