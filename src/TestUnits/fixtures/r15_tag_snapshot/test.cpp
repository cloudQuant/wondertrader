#include "../../coverage/q01_wtcore3/r15_hft_stra_context.cpp"

#include <atomic>
#include <thread>
#include "../../../WtCore/ActionPolicyMgr.h"

namespace {
class ReentrantOrderStrategy : public R15SpyStra {
public:
    enum class Action { None, Erase, Replace };
    R15HftCtx* owner = nullptr;
    Action action = Action::None;
    std::string observed;
    void on_order(IHftStraCtx*, uint32_t localid, const char*, bool,
                  double, double, double, bool, const char* tag) override {
        if (action == Action::Erase)
            owner->eraseOrderTag(localid);
        else if (action == Action::Replace)
        {
            const std::string replacement(512, 'B');
            owner->setUserTag(localid, replacement.c_str());
        }
        observed.assign(tag);
    }
};

// Fake API holds the actual entrust while a test chooses whether an error
// response reaches the real adapter before or after orderInsert returns.
class TimingTraderApi : public p11fix::FakeTraderApi {
public:
    TraderAdapter* adapter = nullptr;
    WTSEntrust* pending = nullptr;
    bool synchronous = false;
    ~TimingTraderApi() override { if (pending) pending->release(); }
    int orderInsert(WTSEntrust* entrust) override {
        const int result = p11fix::FakeTraderApi::orderInsert(entrust);
        if (result < 0) return result;
        entrust->retain();
        pending = entrust;
        if (synchronous) fire();
        return result;
    }
    void fire() {
        WTSError* error = WTSError::create(WEC_ORDERINSERT, "controlled rejection");
        adapter->onRspEntrust(pending, error);
        error->release();
        pending->release();
        pending = nullptr;
    }
};
}

TEST_F(R15CtxTest, order_callback_reads_tag_without_reentrant_mutation) {
    R15HftCtx ctx(&_eng, "r15_tag_control", false, 0);
    ReentrantOrderStrategy strategy;
    strategy.owner = &ctx;
    ctx.set_strategy(&strategy);
    ctx.on_init();
    const std::string expected(256, 'A');
    ctx.setUserTag(17, expected.c_str());
    ctx.on_order(17, kAg, true, 1, 1, 1, false);
    EXPECT_EQ(strategy.observed, expected);
}

TEST_F(R15CtxTest, tag_snapshot_survives_reentrant_erase_during_order_callback) {
    R15HftCtx ctx(&_eng, "r15_tag_reentrant", false, 0);
    ReentrantOrderStrategy strategy;
    strategy.owner = &ctx;
    strategy.action = ReentrantOrderStrategy::Action::Erase;
    ctx.set_strategy(&strategy);
    ctx.on_init();
    const std::string expected(256, 'A');
    ctx.setUserTag(17, expected.c_str());
    ctx.on_order(17, kAg, true, 1, 1, 1, false);
    EXPECT_EQ(strategy.observed, expected);
}

TEST_F(R15CtxTest, tag_snapshot_survives_reentrant_replacement_during_order_callback) {
    R15HftCtx ctx(&_eng, "r15_tag_replace", false, 0);
    ReentrantOrderStrategy strategy;
    strategy.owner = &ctx;
    strategy.action = ReentrantOrderStrategy::Action::Replace;
    ctx.set_strategy(&strategy);
    ctx.on_init();
    const std::string expected(256, 'A');
    ctx.setUserTag(17, expected.c_str());
    ctx.on_order(17, kAg, true, 1, 1, 1, false);
    EXPECT_EQ(strategy.observed, expected);
}

TEST_F(R15CtxTest, tag_snapshot_concurrent_read_write) {
    R15HftCtx ctx(&_eng, "r15_tag_concurrent", false, 0);
    const std::string a(256, 'A'), b(256, 'B');
    std::atomic<bool> start{false};
    std::atomic<bool> valid{true};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (int i = 0; i < 20000; ++i) {
            ctx.setUserTag(17, (i % 2 ? a : b).c_str());
            if (i % 3 == 0) ctx.eraseOrderTag(17);
        }
    });
    std::thread reader([&] {
        start.store(true, std::memory_order_release);
        for (int i = 0; i < 20000; ++i) {
            const std::string value = ctx.getOrderTag(17);
            if (!value.empty() && value != a && value != b)
                valid.store(false, std::memory_order_relaxed);
        }
    });
    writer.join();
    reader.join();
    EXPECT_TRUE(valid.load());
}

TEST_F(R15CtxTest, synchronous_provider_rejection_sees_registered_user_tag) {
    WTSSessionInfo* session = WTSSessionInfo::create("R15ALL", "R15ALL", 0);
    session->addTradingSection(0, 2400);
    _comm->setSessionInfo(session);
    session->release();
    if (!_comm->getSessionInfo()->isInTradingTime(WtHelper::getTime(), true))
        GTEST_SKIP() << "time is at strict session endpoint";
    _ct->setAltCode("ag2512");
    _ct->setVolumeLimits(100, 100, 1, 1);

    const std::string path = "r15_sync_policy.json";
    { std::ofstream config(path); config << R"({"default":{"order":[{"action":"open"}]}})"; }
    ActionPolicyMgr policy;
    ASSERT_TRUE(policy.init(path.c_str()));
    R15HftCtx ctx(&_eng, "r15_sync_tag", false, 0);
    R15SpyStra strategy;
    ctx.set_strategy(&strategy);
    ctx.on_init();
    TimingTraderApi api;
    TraderAdapter trader;
    const std::string id = "r15sync" + std::to_string(static_cast<long>(::getpid()));
    ASSERT_TRUE(trader.initExt(id.c_str(), &api, &_bdm, &policy));
    api.adapter = &trader;
    ctx.setTrader(&trader);
    trader.addSink(&ctx);
    ASSERT_TRUE(trader.run());

    // Positive control: a response after the API returns sees the registered tag.
    auto delayed = ctx.stra_buy(kAg, 7100, 1, "delayed-tag");
    ASSERT_EQ(delayed.size(), 1u);
    ASSERT_NE(api.pending, nullptr);
    api.fire();
    ASSERT_EQ(strategy.n_entrust, 1u);
    EXPECT_EQ(strategy.last_tag, "delayed-tag");

    // The same provider response before orderInsert returns must see this tag too.
    api.synchronous = true;
    auto synchronous = ctx.stra_buy(kAg, 7100, 1, "synchronous-tag");
    ASSERT_EQ(synchronous.size(), 1u);
    ASSERT_EQ(strategy.n_entrust, 2u);
    EXPECT_EQ(strategy.last_localid, synchronous[0]);
    EXPECT_EQ(strategy.last_tag, "synchronous-tag");
    EXPECT_STREQ(ctx.getOrderTag(synchronous[0]), "synchronous-tag");
}
