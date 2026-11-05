// WP1 order identity / account gate fixture (decision cards 5 and 6, 2026-11-04).
// Uses the real TraderAdapter with in-process fake providers only; never
// connects to a broker. Build with -DWP1_BASELINE to compile only the tests
// that use pre-change public APIs (red controls against the baseline copy).
#include "../../coverage/p11_wtcore_traderadapter/r59_trader_fixture.h"

#include <Share/CodeHelper.hpp>
#include <WtCore/UnresolvedReportJournal.h>
#if !defined(WP1_BASELINE) && !defined(WP1_BASELINE2)
#include <WtCore/WtExecuterFactory.h>
#include <Includes/IExecPluginV2.h>
#endif
#include <sys/stat.h>

#include <atomic>
#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>

extern char** environ;

using namespace wtp;
using namespace r59x;

namespace {
std::string g_argv0;

typedef void (*PushBackendFn)(ITraderApi*);

std::string tagFor(const std::string& id, uint32_t localid)
{
    return "otp." + id + "." + std::to_string(localid);
}

class Wp1Test : public R59TraderTest
{
protected:
    std::string oldInstall_;
    void* plugin_ = nullptr;
    PushBackendFn push_ = nullptr;
    ActionPolicyMgr pm_;

    void SetUp() override
    {
        R59TraderTest::SetUp();
        oldInstall_ = WtHelper::getInstDir();
        const char* plugin = std::getenv("WP1_FAKE_PLUGIN");
        ASSERT_NE(nullptr, plugin);
        const auto install = tmp_ / "install";
        std::filesystem::create_directories(install / "traders");
        const auto copy = install / "traders" / "libWp1Fake.dylib";
        ASSERT_TRUE(std::filesystem::copy_file(plugin, copy));
        WtHelper::setInstDir((install.string() + "/").c_str());
        plugin_ = dlopen(copy.c_str(), RTLD_NOW);
        ASSERT_NE(nullptr, plugin_);
        push_ = (PushBackendFn)dlsym(plugin_, "wp1PushBackend");
        ASSERT_NE(nullptr, push_);
        ASSERT_TRUE(loadPolicy(pm_, "wp1_policy.json", R"json({"default":{"order":[{"action":"open"}]}})json"));
    }
    void TearDown() override
    {
        WtHelper::setInstDir(oldInstall_.c_str());
        R59TraderTest::TearDown();
        // plugin_ stays loaded: adapters created by the test may still be
        // destroyed after TearDown by the fixture destructor chain.
    }

    // init() through the loadable fake plugin, then log in (no wall clock).
    void initPlugin(TraderAdapter& adapter, ITraderApi* backend, const char* id,
        const std::function<void(WTSVariant*)>& extra)
    {
        push_(backend);
        WTSVariant* cfg = WTSVariant::createObject();
        cfg->append("module", "Wp1Fake");
        if (extra)
            extra(cfg);
        ASSERT_TRUE(adapter.init(id, cfg, &bd_, &pm_));
        cfg->release();
        ASSERT_TRUE(adapter.run());
        adapter.onLoginResult(true, "wp1", 20240510);
    }

    void pushOrder(TraderAdapter& adapter, const std::string& id, uint32_t localid, WTSOrderState st,
        double vol, double traded, WTSDirectionType dir = WDT_LONG, WTSOffsetType off = WOT_OPEN)
    {
        WTSOrderInfo* o = makeOrder(ct_, dir, off, st, vol, traded,
            ("OID" + std::to_string(localid)).c_str(), tagFor(id, localid).c_str());
        adapter.onPushOrder(o);
        o->release();
    }

    void pushTrade(TraderAdapter& adapter, const std::string& id, uint32_t localid, double vol, const char* tid)
    {
        WTSTradeInfo* t = makeTrade(ct_, WDT_LONG, WOT_OPEN, vol, 7000, tid,
            ("OID" + std::to_string(localid)).c_str(), tagFor(id, localid).c_str());
        adapter.onPushTrade(t);
        t->release();
    }

    void quarantineUnknown(TraderAdapter& adapter)
    {
        WTSTradeInfo* bad = WTSTradeInfo::create("unknown", "SHFE");
        bad->setTradeID("bad-1");
        bad->setVolume(1);
        adapter.onPushTrade(bad);
        bad->release();
    }

    void rspOrders(TraderAdapter& adapter, std::initializer_list<WTSOrderInfo*> items)
    {
        WTSArray* arr = WTSArray::create();
        for (WTSOrderInfo* o : items)
        {
            arr->append(o, false);
        }
        adapter.onRspOrders(arr);
        arr->release();
    }
};

//////////////////////////////////////////////////////////////////////////
// Slice 1: account-level gate (card 6a/6b, L01-014/R54-001)

TEST_F(Wp1Test, SharedAccountPauseClosesEverySiblingAdapter)
{
    p11fix::FakeTraderApi a, b, c;
    TraderAdapter A, B, C;
    initPlugin(A, &a, "wp1a", [](WTSVariant* cfg) { cfg->append("account_id", "ACC-1"); });
    initPlugin(B, &b, "wp1b", [](WTSVariant* cfg) { cfg->append("account_id", "ACC-1"); });
    initPlugin(C, &c, "wp1c", [](WTSVariant* cfg) { cfg->append("account_id", "ACC-2"); });
    ASSERT_NE(UINT_MAX, B.openLong(kStdCode, 7000, 1, 0, ct_));
    ASSERT_EQ(1u, b.n_insert_);

    quarantineUnknown(A);
    EXPECT_TRUE(A.isUnresolvedReportPaused());
    EXPECT_TRUE(B.isUnresolvedReportPaused()) << "sibling adapter on the same account must be paused";
    EXPECT_EQ(UINT_MAX, B.openLong(kStdCode, 7000, 1, 0, ct_));
    EXPECT_TRUE(B.buy(kStdCode, 7000, 1, 0, false, ct_).empty());
    EXPECT_TRUE(B.sell(kStdCode, 7000, 1, 0, false, ct_).empty());
    EXPECT_EQ(1u, b.n_insert_);

    EXPECT_FALSE(C.isUnresolvedReportPaused());
    EXPECT_NE(UINT_MAX, C.openLong(kStdCode, 7000, 1, 0, ct_));
    EXPECT_EQ(1u, c.n_insert_);
}

// User decision 2026-11-04 (supersedes the WP1 "all cancels blocked" oracle):
// while paused, cancels of live orders whose identity is verified are allowed;
// unverifiable cancels and every new order stay rejected.
TEST_F(Wp1Test, PausedAccountCancelsOnlyVerifiedIdentities)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    const uint32_t mine = adapter.openLong(kStdCode, 7000, 2, 0, ct_);
    const uint32_t mine2 = adapter.openLong(kStdCode, 7000, 1, 0, ct_);
    ASSERT_NE(UINT_MAX, mine);
    ASSERT_NE(UINT_MAX, mine2);
    pushOrder(adapter, id_, mine, WOS_NotTraded_Queuing, 2, 0);
    pushOrder(adapter, id_, mine2, WOS_NotTraded_Queuing, 1, 0);
    // Own-tagged but never sent by this adapter boot (e.g. a previous session):
    // its identity cannot be verified.
    const uint32_t foreign = mine + 50000;
    pushOrder(adapter, id_, foreign, WOS_NotTraded_Queuing, 1, 0);
    quarantineUnknown(adapter);
    ASSERT_TRUE(adapter.isUnresolvedReportPaused());
    const uint32_t inserts = api2_.n_insert_;
    EXPECT_EQ(UINT_MAX, adapter.openLong(kStdCode, 7000, 1, 0, ct_));
    EXPECT_TRUE(adapter.buy(kStdCode, 7000, 1, 0, false, ct_).empty());
    EXPECT_EQ(inserts, api2_.n_insert_) << "new orders stay rejected while paused";
    EXPECT_FALSE(adapter.cancel(foreign)) << "unverifiable identity stays rejected while paused";
    EXPECT_EQ(0u, api2_.n_action_);
    EXPECT_TRUE(adapter.cancel(mine)) << "verified live identity may be cancelled while paused";
    EXPECT_EQ(1u, api2_.n_action_);
    OrderIDs batch = adapter.cancel(kStdCode, true, 0);
    EXPECT_EQ(std::count(batch.begin(), batch.end(), foreign), 0);
    EXPECT_EQ(std::count(batch.begin(), batch.end(), mine2), 1);
    EXPECT_EQ(1u + batch.size(), api2_.n_action_);
}

//////////////////////////////////////////////////////////////////////////
// Slice 2: month code contract (card 6c, L01-016)

TEST_F(Wp1Test, MonthCodeContractKeepsThreeToSixDigitsAndRejectsOthers)
{
    EXPECT_EQ("CZCE.AP.2405", CodeHelper::rawMonthCodeToStdCode("AP405", "CZCE"));
    EXPECT_EQ("CZCE.AP.1905", CodeHelper::rawMonthCodeToStdCode("AP905", "CZCE"));
    EXPECT_EQ("SHFE.ag.2512", CodeHelper::rawMonthCodeToStdCode("ag2512", "SHFE"));
    EXPECT_EQ("SHFE.ag.25123", CodeHelper::rawMonthCodeToStdCode("ag25123", "SHFE"));
    EXPECT_EQ("SHFE.ag.251234", CodeHelper::rawMonthCodeToStdCode("ag251234", "SHFE"));
    EXPECT_EQ(std::string("SHFE.ag.251234").size(), CodeHelper::rawMonthCodeToStdCode("ag251234", "SHFE").size());
    EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag2512345", "SHFE").empty()) << "7 digits exceed the contract";
    EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag25", "SHFE").empty());
    EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag19x2", "SHFE").empty()) << "non-digit month";
    EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag12-3", "SHFE").empty());
    EXPECT_TRUE(CodeHelper::rawMonthCodeToStdCode("ag1 23", "SHFE").empty());
    EXPECT_EQ("SHFE.ag", CodeHelper::rawMonthCodeToStdCode("ag", "SHFE", true));
}

TEST_F(Wp1Test, NonDigitMonthTradeIsQuarantinedAndLongMonthKeptIntact)
{
    WTSContractInfo* bad = WTSContractInfo::create("ag19x2", "ag19x2", kExchg, "ag");
    bad->setCommInfo(comm_);
    WTSContractInfo* longMonth = WTSContractInfo::create("ag25123", "ag25123", kExchg, "ag");
    longMonth->setCommInfo(comm_);
    {
        TraderAdapter adapter;
        build(adapter, &pm_);
        WTSTradeInfo* t = makeTrade(longMonth, WDT_LONG, WOT_OPEN, 2, 7000, "T5", "O5", "ext");
        adapter.onPushTrade(t);
        t->release();
        EXPECT_DOUBLE_EQ(2.0, adapter.getPosition("SHFE.ag.25123", false));
        EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("SHFE.ag.2123", false));
        EXPECT_FALSE(adapter.isUnresolvedReportPaused());

        t = makeTrade(bad, WDT_LONG, WOT_OPEN, 3, 7000, "T6", "O6", "ext");
        adapter.onPushTrade(t);
        t->release();
        EXPECT_TRUE(adapter.isUnresolvedReportPaused()) << "unparseable month code must pause the account";
        EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("SHFE.ag.19x2", false));
        EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("", false));
        EXPECT_EQ(UINT_MAX, adapter.openLong(kStdCode, 7000, 1, 0, ct_));
    }
    longMonth->release();
    bad->release();
}

//////////////////////////////////////////////////////////////////////////
// Slice 3: per-order ledger (R3-008 duplicate/late async reports)

TEST_F(Wp1Test, DirectOrderReservesAndDuplicateCancelReleasesOnce)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    const uint32_t localid = adapter.openLong(kStdCode, 7000, 2, 0, ct_);
    ASSERT_NE(UINT_MAX, localid);
    EXPECT_DOUBLE_EQ(2.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, localid, WOS_Canceled, 2, 0);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, localid, WOS_Canceled, 2, 0);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode)) << "duplicate cancel must not release twice";
}

TEST_F(Wp1Test, BuySplitDuplicateTradeLateTradeAndDuplicateCancelAreIdempotent)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    OrderIDs ids = adapter.buy(kStdCode, 7000, 5, 0, false, ct_);
    ASSERT_EQ(1u, ids.size());
    const uint32_t localid = ids[0];
    EXPECT_DOUBLE_EQ(5.0, adapter.getUndoneQty(kStdCode));
    pushTrade(adapter, id_, localid, 2, "T1");
    EXPECT_DOUBLE_EQ(3.0, adapter.getUndoneQty(kStdCode));
    pushTrade(adapter, id_, localid, 2, "T1");
    EXPECT_DOUBLE_EQ(3.0, adapter.getUndoneQty(kStdCode)) << "duplicate trade id must not shrink undone twice";
    pushOrder(adapter, id_, localid, WOS_PartTraded_Queuing, 5, 2);
    EXPECT_DOUBLE_EQ(3.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, localid, WOS_Canceled, 5, 2);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, localid, WOS_Canceled, 5, 2);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    pushTrade(adapter, id_, localid, 1, "T2");
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode)) << "late trade after terminal must not go negative";
}

TEST_F(Wp1Test, RejectionOfUnknownLocalIdDoesNotConsumeOtherOrders)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    OrderIDs ids = adapter.buy(kStdCode, 7000, 5, 0, false, ct_);
    ASSERT_EQ(1u, ids.size());
    ASSERT_DOUBLE_EQ(5.0, adapter.getUndoneQty(kStdCode));
    WTSEntrust* e = WTSEntrust::create(kRawCode, 5, 7000, kExchg);
    e->setContractInfo(ct_);
    e->setDirection(WDT_LONG);
    e->setOffsetType(WOT_OPEN);
    e->setUserTag(tagFor(id_, ids[0] + 1000).c_str());
    WTSError* err = WTSError::create(WEC_ORDERINSERT, "rejected");
    adapter.onRspEntrust(e, err);
    EXPECT_DOUBLE_EQ(5.0, adapter.getUndoneQty(kStdCode)) << "unknown order rejection must not touch undone";
    e->setUserTag(tagFor(id_, ids[0]).c_str());
    adapter.onRspEntrust(e, err);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    adapter.onRspEntrust(e, err);
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    size_t failures = 0;
    for (const auto& rec : sink_.entrusts_)
        failures += rec.ok ? 0 : 1;
    EXPECT_EQ(1u, failures);
    err->release();
    e->release();
}

//////////////////////////////////////////////////////////////////////////
// Slice 4: epoch/version snapshot reconciliation (card 5d, NEW-154)

TEST_F(Wp1Test, StaleSnapshotRowDoesNotResurrectNewerCancel)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    const uint32_t localid = adapter.openLong(kStdCode, 7000, 2, 0, ct_);
    ASSERT_NE(UINT_MAX, localid);
    pushOrder(adapter, id_, localid, WOS_NotTraded_Queuing, 2, 0);
    adapter.onRspPosition(static_cast<const WTSArray*>(nullptr));	// issues queryOrders (snapshot epoch)
    ASSERT_EQ(1u, api2_.n_qorders_);
    pushOrder(adapter, id_, localid, WOS_Canceled, 2, 0);			// realtime, newer than the query
    rspOrders(adapter, { makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 2, 0,
        "OIDX", tagFor(id_, localid).c_str()) });
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    EXPECT_FALSE(adapter.cancel(localid)) << "stale snapshot must not re-register a canceled order";
    EXPECT_EQ(0u, api2_.n_action_);
}

TEST_F(Wp1Test, AbsentOrderBecomesRevivableTombstone)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    const uint32_t localid = adapter.openLong(kStdCode, 7000, 2, 0, ct_);
    ASSERT_NE(UINT_MAX, localid);
    pushOrder(adapter, id_, localid, WOS_NotTraded_Queuing, 2, 0);
    adapter.onRspPosition(static_cast<const WTSArray*>(nullptr));
    rspOrders(adapter, {});
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, localid, WOS_PartTraded_Queuing, 2, 1);
    EXPECT_DOUBLE_EQ(1.0, adapter.getUndoneQty(kStdCode)) << "newer realtime report revives an absent tombstone";
}

// Follow-up (NEW-154, 5d for positions): a realtime trade newer than the
// position query wins over the stale snapshot row of the same contract.
TEST_F(Wp1Test, StalePositionSnapshotRowDoesNotOverwriteNewerTrade)
{
    TraderAdapter adapter;
    build(adapter, &pm_);	// onLoginResult issues queryPositions (position epoch)
    ASSERT_EQ(1u, api2_.n_qpositions_);
    WTSTradeInfo* t = makeTrade(ct_, WDT_LONG, WOT_OPEN, 2, 7000, "TP1", "OP1", "ext");
    adapter.onPushTrade(t);
    t->release();
    ASSERT_DOUBLE_EQ(2.0, adapter.getPosition(kStdCode, false));
    seedPos(adapter, { makePos(ct_, WDT_LONG, 0, 0, 0, 0), makePos(ct2_, WDT_LONG, 0, 0, 4, 4) });
    EXPECT_DOUBLE_EQ(2.0, adapter.getPosition(kStdCode, false)) << "stale snapshot row must not erase the newer trade";
    EXPECT_DOUBLE_EQ(4.0, adapter.getPosition(kStdAu, false)) << "rows without newer realtime events still apply";
}

TEST_F(Wp1Test, FailedSnapshotKeepsLedger)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    ASSERT_NE(UINT_MAX, adapter.openLong(kStdCode, 7000, 2, 0, ct_));
    adapter.onRspPosition(static_cast<const WTSArray*>(nullptr));
    adapter.onRspOrders(static_cast<const WTSArray*>(nullptr));
    EXPECT_DOUBLE_EQ(2.0, adapter.getUndoneQty(kStdCode));
}

#ifndef WP1_BASELINE
//////////////////////////////////////////////////////////////////////////
// New public contracts (green only; baseline has no such API)

TEST_F(Wp1Test, SnapshotLedgerStatesAndEpoch)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    const uint32_t a = adapter.openLong(kStdCode, 7000, 2, 0, ct_);
    const uint32_t b = adapter.openLong(kStdCode, 7000, 3, 0, ct_);
    pushOrder(adapter, id_, a, WOS_NotTraded_Queuing, 2, 0);
    pushOrder(adapter, id_, b, WOS_AllTraded, 3, 3);
    adapter.onRspPosition(static_cast<const WTSArray*>(nullptr));
    const uint64_t epoch = adapter.ordersSnapshotVersion();
    rspOrders(adapter, { makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 3, 0, "OB", tagFor(id_, b).c_str()) });
    EXPECT_GT(adapter.ordersSnapshotVersion(), epoch);
    EXPECT_EQ(TraderAdapter::OrderLedgerState::Absent, adapter.orderLedger(a).state);
    EXPECT_EQ(TraderAdapter::OrderLedgerState::Terminal, adapter.orderLedger(b).state) << "terminal tombstone wins over snapshot";
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
}

TEST_F(Wp1Test, AuditedManualResumeContract)
{
    const std::string audit = (tmp_ / "audit.log").string();
    p11fix::FakeTraderApi a, b, n;
    TraderAdapter A, B, N;
    initPlugin(A, &a, "wp1ra", [&](WTSVariant* cfg) { cfg->append("account_id", "ACC-R"); cfg->append("account_audit_log", audit.c_str()); });
    initPlugin(B, &b, "wp1rb", [&](WTSVariant* cfg) { cfg->append("account_id", "ACC-R"); });
    initPlugin(N, &n, "wp1rn", [&](WTSVariant* cfg) { cfg->append("account_id", "ACC-NOAUDIT"); });
    EXPECT_EQ(A.accountGate(), B.accountGate());
    EXPECT_EQ(AccountTradingGate::ResumeStatus::NotPaused,
        A.resumeAccountTrading({ "ops", "nothing", A.accountPauseGeneration(), "" }));

    quarantineUnknown(B);
    ASSERT_TRUE(A.isUnresolvedReportPaused());
    const uint64_t gen = A.accountPauseGeneration();
    EXPECT_EQ(AccountTradingGate::ResumeStatus::InvalidRequest, A.resumeAccountTrading({ "", "r", gen, "" }));
    EXPECT_EQ(AccountTradingGate::ResumeStatus::StaleGeneration, A.resumeAccountTrading({ "ops", "r", gen - 1, "" }));
    quarantineUnknown(A);	// a new pause after the operator's review
    EXPECT_EQ(AccountTradingGate::ResumeStatus::StaleGeneration, A.resumeAccountTrading({ "ops", "r", gen, "" }));
    const uint64_t gen2 = A.accountPauseGeneration();
    A.accountGate()->failAuditWritesForTest(true);
    EXPECT_EQ(AccountTradingGate::ResumeStatus::AuditWriteFailed, A.resumeAccountTrading({ "ops", "r", gen2, "" }));
    EXPECT_TRUE(B.isUnresolvedReportPaused());
    A.accountGate()->failAuditWritesForTest(false);
    EXPECT_EQ(AccountTradingGate::ResumeStatus::Ok, B.resumeAccountTrading({ "ops", "reconciled ticket", gen2, "TICKET-1" }));
    EXPECT_FALSE(A.isUnresolvedReportPaused());
    EXPECT_FALSE(B.isUnresolvedReportPaused());
    EXPECT_NE(UINT_MAX, B.openLong(kStdCode, 7000, 1, 0, ct_));
    EXPECT_EQ(1u, b.n_insert_);

    quarantineUnknown(N);
    EXPECT_EQ(AccountTradingGate::ResumeStatus::AuditUnavailable,
        N.resumeAccountTrading({ "ops", "r", N.accountPauseGeneration(), "" }));
    EXPECT_TRUE(N.isUnresolvedReportPaused());

    std::ifstream in(audit);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(std::string::npos, text.find("\"event\":\"pause\""));
    EXPECT_NE(std::string::npos, text.find("\"event\":\"resume_rejected\""));
    EXPECT_NE(std::string::npos, text.find("\"event\":\"resume_requested\""));
    EXPECT_NE(std::string::npos, text.find("\"event\":\"resumed\""));
    EXPECT_NE(std::string::npos, text.find("TICKET-1"));
    EXPECT_NE(std::string::npos, text.find("\"account\":\"account:ACC-R\""));
}

TEST_F(Wp1Test, ResumeAcknowledgesUnresolvedJournal)
{
    const std::string audit = (tmp_ / "audit.log").string();
    auto dir = tmp_ / "journal";
    std::filesystem::create_directory(dir);
    ::chmod(dir.c_str(), 0700);
    const std::string journalPath = (dir / "reports.wal").string();
    { UnresolvedReportJournal journal; ASSERT_EQ(UnresolvedReportJournal::Status::Ok, journal.createNew(journalPath, "wp1")); }
    {
        p11fix::FakeTraderApi a;
        TraderAdapter A;
        initPlugin(A, &a, "wp1j", [&](WTSVariant* cfg) {
            cfg->append("account_id", "ACC-J"); cfg->append("account_audit_log", audit.c_str());
            cfg->append("unresolved_report_journal", journalPath.c_str()); });
        ASSERT_TRUE(A.isUnresolvedReportPaused()) << "startup reconciliation pause";
        quarantineUnknown(A);
        ASSERT_TRUE(A.unresolvedTradeReports().at(0).persisted);
        EXPECT_EQ(AccountTradingGate::ResumeStatus::Ok,
            A.resumeAccountTrading({ "ops", "reconciled", A.accountPauseGeneration(), "REC-7" }));
        EXPECT_FALSE(A.isUnresolvedReportPaused());
        EXPECT_NE(UINT_MAX, A.openLong(kStdCode, 7000, 1, 0, ct_));
    }
    UnresolvedReportJournal reopened;
    ASSERT_EQ(UnresolvedReportJournal::Status::Ok, reopened.open(journalPath));
    std::vector<UnresolvedReportJournal::Report> pending;
    ASSERT_EQ(UnresolvedReportJournal::Status::Ok, reopened.pending(pending));
    EXPECT_TRUE(pending.empty());
    std::vector<UnresolvedReportJournal::Acknowledgement> acks;
    ASSERT_EQ(UnresolvedReportJournal::Status::Ok, reopened.acknowledgements(acks));
    ASSERT_EQ(1u, acks.size());
    EXPECT_EQ("ops", acks[0].operator_id);
}

TEST_F(Wp1Test, IdentityJournalReservesMonotonicIdsAndVerifiedCancel)
{
    const std::string wal = (tmp_ / "identity.wal").string();
    p11fix::FakeTraderApi a;
    TraderAdapter A;
    initPlugin(A, &a, "wp1i", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-I"); cfg->append("order_identity_journal", wal.c_str());
        cfg->append("local_order_id_floor", (uint32_t)1000); cfg->append("local_order_id_floor_evidence", "fixture: empty account"); });
    ASSERT_FALSE(A.isTradingAdmissionClosed());
    const uint32_t first = A.openLong(kStdCode, 7000, 2, 0, ct_);
    const uint32_t second = A.openLong(kStdCode, 7000, 1, 0, ct_);
    EXPECT_EQ(1001u, first);
    EXPECT_EQ(1002u, second);
    EXPECT_EQ(tagFor("wp1i", 1002), a.last_usertag_);
    TraderAdapter::OrderIdentityToken t1, t2;
    ASSERT_TRUE(A.orderIdentity(first, t1));
    ASSERT_TRUE(A.orderIdentity(second, t2));
    EXPECT_NE(t1.intent_id, t2.intent_id);
    pushOrder(A, "wp1i", first, WOS_NotTraded_Queuing, 2, 0);
    pushOrder(A, "wp1i", second, WOS_NotTraded_Queuing, 1, 0);
    TraderAdapter::OrderIdentityToken forged = t1;
    forged.intent_id = t2.intent_id;
    EXPECT_FALSE(A.cancelVerified(forged));
    EXPECT_EQ(0u, a.n_action_);
    EXPECT_TRUE(A.cancelVerified(t1));
    EXPECT_EQ(1u, a.n_action_);
    pushOrder(A, "wp1i", first, WOS_Canceled, 2, 0);
    EXPECT_FALSE(A.cancelVerified(t1)) << "terminal identity can never be cancelled again";
    EXPECT_EQ(1u, a.n_action_);
    EXPECT_DOUBLE_EQ(1.0, A.getUndoneQty(kStdCode));
}

TEST_F(Wp1Test, IdentityJournalWithoutFloorFailsClosed)
{
    const std::string wal = (tmp_ / "identity.wal").string();
    p11fix::FakeTraderApi a;
    TraderAdapter A;
    initPlugin(A, &a, "wp1f", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-F"); cfg->append("order_identity_journal", wal.c_str()); });
    EXPECT_TRUE(A.isUnresolvedReportPaused());
    EXPECT_EQ(UINT_MAX, A.openLong(kStdCode, 7000, 1, 0, ct_));
    EXPECT_EQ(0u, a.n_insert_);
}

// Crash/restart through a real child process killed with SIGKILL.
class CrashBackend : public p11fix::FakeTraderApi
{
public:
    bool killInInsert = false;
    int orderInsert(WTSEntrust* entrust) override
    {
        if (killInInsert)
            std::raise(SIGKILL);	// the intent is already durable; the send never completes
        return p11fix::FakeTraderApi::orderInsert(entrust);
    }
};

TEST_F(Wp1Test, ChildScenario)
{
    const char* mode = std::getenv("WP1_CHILD_MODE");
    if (mode == nullptr)
        GTEST_SKIP() << "only runs as a spawned child";
    const std::string wal = std::getenv("WP1_CHILD_WAL");
    CrashBackend backend;
    backend.killInInsert = std::string(mode) == "crash_in_insert";
    TraderAdapter A;
    initPlugin(A, &backend, "wp1k", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-K"); cfg->append("order_identity_journal", wal.c_str());
        cfg->append("local_order_id_floor", (uint32_t)5000); cfg->append("local_order_id_floor_evidence", "fixture: empty account"); });
    ASSERT_FALSE(A.isTradingAdmissionClosed());
    const uint32_t localid = A.openLong(kStdCode, 7000, 2, 0, ct_);
    ASSERT_EQ(5001u, localid);
    std::raise(SIGKILL);	// crash after a successful send
}

int runChild(const std::string& mode, const std::string& wal)
{
    std::string filter = "--gtest_filter=Wp1Test.ChildScenario";
    std::vector<std::string> envs;
    for (char** e = environ; *e; ++e)
        envs.push_back(*e);
    envs.push_back("WP1_CHILD_MODE=" + mode);
    envs.push_back("WP1_CHILD_WAL=" + wal);
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(&e[0]);
    envp.push_back(nullptr);
    std::vector<char*> argv = { &g_argv0[0], &filter[0], nullptr };
    pid_t pid = 0;
    if (posix_spawn(&pid, g_argv0.c_str(), nullptr, nullptr, argv.data(), envp.data()) != 0)
        return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -2;
    return WIFSIGNALED(status) ? WTERMSIG(status) : 1000 + WEXITSTATUS(status);
}

TEST_F(Wp1Test, CrashDuringSendIsNeverResentAndPausesAccount)
{
    const std::string wal = (tmp_ / "crash.wal").string();
    ASSERT_EQ(SIGKILL, runChild("crash_in_insert", wal));
    const std::string audit = (tmp_ / "audit.log").string();
    p11fix::FakeTraderApi api;
    TraderAdapter A;
    initPlugin(A, &api, "wp1k", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-K"); cfg->append("order_identity_journal", wal.c_str());
        cfg->append("account_audit_log", audit.c_str()); });
    EXPECT_TRUE(A.isIdentityRecoveryPending());
    EXPECT_TRUE(A.isTradingAdmissionClosed());
    EXPECT_EQ(UINT_MAX, A.openLong(kStdCode, 7000, 2, 0, ct_));
    A.onRspPosition(static_cast<const WTSArray*>(nullptr));
    rspOrders(A, {});	// provider snapshot has no trace of 5001
    EXPECT_FALSE(A.isIdentityRecoveryPending());
    EXPECT_TRUE(A.isUnresolvedReportPaused()) << "unrecoverable intent pauses the account";
    EXPECT_EQ(UINT_MAX, A.openLong(kStdCode, 7000, 2, 0, ct_));
    EXPECT_EQ(0u, api.n_insert_) << "never auto-resend";
    ASSERT_EQ(AccountTradingGate::ResumeStatus::Ok,
        A.resumeAccountTrading({ "ops", "5001 confirmed absent at broker", A.accountPauseGeneration(), "REC-5001" }));
    const uint32_t next = A.openLong(kStdCode, 7000, 1, 0, ct_);
    EXPECT_EQ(5002u, next) << "reserved id 5001 is never reused";
    EXPECT_EQ(1u, api.n_insert_);
}

TEST_F(Wp1Test, CrashAfterSendIsRecoveredFromSnapshot)
{
    const std::string wal = (tmp_ / "crash2.wal").string();
    ASSERT_EQ(SIGKILL, runChild("crash_after_send", wal));
    p11fix::FakeTraderApi api;
    TraderAdapter A;
    initPlugin(A, &api, "wp1k", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-K"); cfg->append("order_identity_journal", wal.c_str()); });
    EXPECT_TRUE(A.isIdentityRecoveryPending());
    A.onRspPosition(static_cast<const WTSArray*>(nullptr));
    rspOrders(A, { makeOrder(ct_, WDT_LONG, WOT_OPEN, WOS_NotTraded_Queuing, 2, 0, "OID5001", tagFor("wp1k", 5001).c_str()) });
    EXPECT_FALSE(A.isIdentityRecoveryPending());
    EXPECT_FALSE(A.isUnresolvedReportPaused());
    EXPECT_DOUBLE_EQ(2.0, A.getUndoneQty(kStdCode));
    TraderAdapter::OrderIdentityToken token;
    ASSERT_TRUE(A.orderIdentity(5001, token));
    EXPECT_TRUE(A.cancelVerified(token));
    EXPECT_EQ(5002u, A.openLong(kStdCode, 7000, 1, 0, ct_));
}

// Concurrency smoke for TSan: order entry, reports, snapshots and gate
// pause/resume racing on one adapter.
TEST_F(Wp1Test, ConcurrentEntryReportsSnapshotsAndGate)
{
    const std::string audit = (tmp_ / "audit.log").string();
    TraderAdapter adapter;
    build(adapter, &pm_);
    std::string error;
    ASSERT_TRUE(adapter.bindAccount("ACC-T", audit, &error)) << error;
    std::mutex idsMutex;
    std::vector<uint32_t> ids;
    std::atomic<bool> stop{false};
    std::thread sender([&]() {
        for (int i = 0; i < 300; ++i)
        {
            uint32_t id = adapter.openLong(kStdCode, 7000, 1, 0, ct_);
            if (id != UINT_MAX) { std::lock_guard<std::mutex> lock(idsMutex); ids.push_back(id); }
        }
        stop = true;
    });
    std::thread reporter([&]() {
        size_t done = 0;
        while (!stop || done < ids.size())
        {
            uint32_t id = 0;
            { std::lock_guard<std::mutex> lock(idsMutex); if (done < ids.size()) id = ids[done++]; }
            if (id == 0) { std::this_thread::yield(); continue; }
            pushOrder(adapter, id_, id, (id % 2) ? WOS_Canceled : WOS_NotTraded_Queuing, 1, 0);
            pushOrder(adapter, id_, id, WOS_Canceled, 1, 0);
        }
    });
    std::thread reader([&]() {
        while (!stop) { (void)adapter.getUndoneQty(kStdCode); adapter.onRspOrders(static_cast<const WTSArray*>(nullptr)); }
    });
    std::thread gate([&]() {
        while (!stop)
        {
            adapter.accountGate()->pause("test", "flip");
            adapter.resumeAccountTrading({ "ops", "flip", adapter.accountPauseGeneration(), "" });
        }
    });
    sender.join(); reporter.join(); reader.join(); gate.join();
    double sum = 0;
    for (uint32_t id : ids)
    {
        auto view = adapter.orderLedger(id);
        ASSERT_TRUE(view.found);
        EXPECT_EQ(TraderAdapter::OrderLedgerState::Terminal, view.state);
        sum += view.accounted;
    }
    EXPECT_DOUBLE_EQ(sum, adapter.getUndoneQty(kStdCode));
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty(kStdCode));
}
#endif

#if !defined(WP1_BASELINE) && !defined(WP1_BASELINE2)
//////////////////////////////////////////////////////////////////////////
// Follow-up: owner routing (5c), non-shared mode (5b), verified cancel while paused

namespace {
TraderAdapter::OwnerRoutingCapabilities allCaps()
{
    TraderAdapter::OwnerRoutingCapabilities caps;
    caps.durable_journal = caps.local_order_id_floor = true;
    caps.provider_user_tag_round_trip = caps.stable_trade_id_across_push_and_query = true;
    return caps;
}
size_t ordersFor(const p11fix::RecordingSink& s, uint32_t id)
{
    return (size_t)std::count_if(s.orders_.begin(), s.orders_.end(), [id](const p11fix::RecordingSink::OrderRec& r) { return r.localid == id; });
}
size_t tradesFor(const p11fix::RecordingSink& s, uint32_t id)
{
    return (size_t)std::count_if(s.trades_.begin(), s.trades_.end(), [id](const p11fix::RecordingSink::TradeRec& r) { return r.localid == id; });
}
}

TEST_F(Wp1Test, OwnerRoutingIsolatesReportsCancelsAndUndone)
{
    TraderAdapter adapter;
    build(adapter, &pm_);	// sink_ stays a non-owner observer
    ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(allCaps()));
    p11fix::RecordingSink a, b;
    EXPECT_TRUE(adapter.declareOwnerSink(&a, "exec:a"));
    EXPECT_TRUE(adapter.declareOwnerSink(&b, "exec:b"));
    adapter.addSink(&a);
    adapter.addSink(&b);
    OrderIDs ia = adapter.buyAsOwner("exec:a", true, kStdCode, 7000, 2, 0, false);
    OrderIDs ib = adapter.buyAsOwner("exec:b", true, kStdCode, 7000, 3, 0, false);
    ASSERT_EQ(1u, ia.size());
    ASSERT_EQ(1u, ib.size()) << "5c: same account+contract shared by two capable owners once routing exists";
    EXPECT_DOUBLE_EQ(2.0, adapter.getUndoneQtyForOwner(kStdCode, "exec:a"));
    EXPECT_DOUBLE_EQ(3.0, adapter.getUndoneQtyForOwner(kStdCode, "exec:b"));
    EXPECT_DOUBLE_EQ(5.0, adapter.getUndoneQty(kStdCode));
    pushOrder(adapter, id_, ia[0], WOS_NotTraded_Queuing, 2, 0);
    pushOrder(adapter, id_, ib[0], WOS_NotTraded_Queuing, 3, 0);
    EXPECT_EQ(1u, ordersFor(a, ia[0]));
    EXPECT_EQ(0u, ordersFor(a, ib[0]));
    EXPECT_EQ(1u, ordersFor(b, ib[0]));
    EXPECT_EQ(0u, ordersFor(b, ia[0]));
    EXPECT_EQ(1u, ordersFor(sink_, ia[0]));
    EXPECT_EQ(1u, ordersFor(sink_, ib[0]));
    pushTrade(adapter, id_, ib[0], 1, "TB1");
    EXPECT_EQ(1u, tradesFor(b, ib[0]));
    EXPECT_EQ(0u, tradesFor(a, ib[0]));
    EXPECT_DOUBLE_EQ(2.0, adapter.getUndoneQtyForOwner(kStdCode, "exec:b"));
    OrderMap* mapA = adapter.getOrdersForOwner(kStdCode, "exec:a");
    ASSERT_NE(nullptr, mapA);
    EXPECT_EQ(1u, mapA->size());
    EXPECT_NE(nullptr, mapA->get(ia[0]));
    mapA->release();

    EXPECT_FALSE(adapter.cancelForOwner(ia[0], "exec:b")) << "never cancel another owner's order";
    EXPECT_FALSE(adapter.cancel(ia[0])) << "bare cancel cannot touch an owned order";
    EXPECT_EQ(0u, api2_.n_action_);
    EXPECT_TRUE(adapter.cancelForOwner(ia[0], "exec:a"));
    EXPECT_EQ(1u, api2_.n_action_);
    OrderIDs cb = adapter.cancelForOwner(kStdCode, true, 0, "exec:b");
    ASSERT_EQ(1u, cb.size());
    EXPECT_EQ(ib[0], cb[0]);

    // Paused account: the owner's verified live order may still be cancelled.
    quarantineUnknown(adapter);
    ASSERT_TRUE(adapter.isUnresolvedReportPaused());
    EXPECT_TRUE(adapter.buyAsOwner("exec:a", true, kStdCode, 7000, 1, 0, false).empty());
    EXPECT_TRUE(adapter.cancelForOwner(ib[0], "exec:b"));
    EXPECT_FALSE(adapter.cancelForOwner(ib[0], "exec:a"));
    pushOrder(adapter, id_, ia[0], WOS_Canceled, 2, 0);
    EXPECT_FALSE(adapter.cancelForOwner(ia[0], "exec:a")) << "terminal identity";
    EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQtyForOwner(kStdCode, "exec:a"));
}

TEST_F(Wp1Test, NonSharedOwnerModeForOldAbiOrMissingRouting)
{
    TraderAdapter routed;
    build(routed, &pm_);
    ASSERT_TRUE(routed.configureOwnerRoutingCapabilities(allCaps()));
    // 5b: an old-identity-ABI unit (not shared capable) owns the contract exclusively.
    ASSERT_EQ(1u, routed.buyAsOwner("exec:old", false, kStdCode, 7000, 1, 0, false).size());
    const uint32_t inserts = api2_.n_insert_;
    EXPECT_TRUE(routed.buyAsOwner("exec:new", true, kStdCode, 7000, 1, 0, false).empty());
    EXPECT_EQ(inserts, api2_.n_insert_);
    EXPECT_EQ(1u, routed.buyAsOwner("exec:new", true, kStdAu, 400, 1, 0, false).size()) << "other contract is free";
    routed.releaseOwner("exec:old");
    EXPECT_EQ(1u, routed.buyAsOwner("exec:new", true, kStdCode, 7000, 1, 0, false).size());

    // 5c: without owner routing two capable owners cannot share a contract.
    p11fix::FakeTraderApi api;
    TraderAdapter plain;
    ASSERT_TRUE(plain.initExt("wp1plain", &api, &bd_, &pm_));
    ASSERT_TRUE(plain.run());
    plain.onLoginResult(true, "wp1", 20240510);
    ASSERT_FALSE(plain.isOwnerRoutingEnabled());
    ASSERT_EQ(1u, plain.buyAsOwner("exec:x", true, kStdCode, 7000, 1, 0, false).size());
    EXPECT_TRUE(plain.buyAsOwner("exec:y", true, kStdCode, 7000, 1, 0, false).empty());
    EXPECT_EQ(1u, api.n_insert_);
}

TEST_F(Wp1Test, ContractClaimsSpanAdaptersOfOneAccount)
{
    p11fix::FakeTraderApi apiY;
    TraderAdapter X, Y;
    build(X, &pm_);
    ASSERT_TRUE(Y.initExt("wp1claimy", &apiY, &bd_, &pm_));
    ASSERT_TRUE(Y.run());
    Y.onLoginResult(true, "wp1", 20240510);
    ASSERT_TRUE(X.bindAccount("ACC-CLAIM", ""));
    ASSERT_TRUE(Y.bindAccount("ACC-CLAIM", ""));
    ASSERT_EQ(1u, X.buyAsOwner("exec:a", false, kStdCode, 7000, 1, 0, false).size());
    EXPECT_TRUE(Y.buyAsOwner("exec:b", true, kStdCode, 7000, 1, 0, false).empty());
    EXPECT_EQ(0u, apiY.n_insert_);
}

class ReadyProbeSink : public p11fix::RecordingSink
{
public:
    std::function<void()> onReady;
    void on_channel_ready() override { RecordingSink::on_channel_ready(); if (onReady) onReady(); }
};

TEST_F(Wp1Test, DeclaredOwnerIsRegisteredBeforeReadyIsDelivered)
{
    TraderAdapter adapter;
    build(adapter, &pm_);
    ASSERT_TRUE(adapter.configureOwnerRoutingCapabilities(allCaps()));
    p11fix::RecordingSink b;
    adapter.declareOwnerSink(&b, "exec:b");
    adapter.addSink(&b);
    OrderIDs ib = adapter.buyAsOwner("exec:b", true, kStdCode, 7000, 1, 0, false);
    ASSERT_EQ(1u, ib.size());
    makeReady(adapter);
    ReadyProbeSink a;
    a.onReady = [&]() { pushOrder(adapter, id_, ib[0], WOS_NotTraded_Queuing, 1, 0); };
    adapter.declareOwnerSink(&a, "exec:a");
    adapter.addSink(&a);	// READY is delivered synchronously from addSink
    ASSERT_EQ(1u, a.n_ready_);
    EXPECT_EQ(0u, ordersFor(a, ib[0])) << "owner a was routed before READY, so it never sees owner b's order";
    EXPECT_EQ(1u, ordersFor(b, ib[0]));
}

TEST_F(Wp1Test, ManagerResumeCommandIsAuditedAndGenerationChecked)
{
    const std::string audit = (tmp_ / "audit.log").string();
    p11fix::FakeTraderApi api;
    TraderAdapterPtr adapter(new TraderAdapter());
    initPlugin(*adapter, &api, "wp1mgr", [&](WTSVariant* cfg) {
        cfg->append("account_id", "ACC-MGR"); cfg->append("account_audit_log", audit.c_str()); });
    TraderAdapterMgr mgr;
    ASSERT_TRUE(mgr.addAdapter("wp1mgr", adapter));
    quarantineUnknown(*adapter);
    const uint64_t gen = mgr.accountPauseGeneration("wp1mgr");
    ASSERT_GT(gen, 0u);
    EXPECT_EQ(0u, mgr.accountPauseGeneration("nope"));
    std::string diagnostic;
    EXPECT_EQ(AccountTradingGate::ResumeStatus::InvalidRequest,
        mgr.resumeAccountTrading("nope", { "ops", "r", gen, "" }, &diagnostic));
    EXPECT_NE(std::string::npos, diagnostic.find("unknown trader adapter"));
    EXPECT_EQ(AccountTradingGate::ResumeStatus::StaleGeneration, mgr.resumeAccountTrading("wp1mgr", { "ops", "r", gen + 1, "" }));
    EXPECT_TRUE(adapter->isUnresolvedReportPaused());
    EXPECT_EQ(AccountTradingGate::ResumeStatus::Ok, mgr.resumeAccountTrading("wp1mgr", { "ops", "reconciled", gen, "MGR-1" }));
    EXPECT_FALSE(adapter->isUnresolvedReportPaused());
}

TEST_F(Wp1Test, ExecFactoryInjectsHostIdentityKeyOnlyForCapablePluginAndVerifyingHost)
{
    const char* paths[2] = { std::getenv("WP1_FAKE_EXEC_CAP"), std::getenv("WP1_FAKE_EXEC_OLD") };
    for (int which = 0; which < 2; ++which)
    {
        ASSERT_NE(nullptr, paths[which]);
        const bool capable = which == 0;
        const auto dir = tmp_ / (capable ? "exec_cap" : "exec_old");
        std::filesystem::create_directories(dir);
        const auto copy = dir / (capable ? "libWp1FakeExecCap.dylib" : "libWp1FakeExecOld.dylib");
        ASSERT_TRUE(std::filesystem::copy_file(paths[which], copy));
        WtExecuterFactory factory;
        ASSERT_TRUE(factory.loadFactories((dir.string() + "/").c_str()));
        void* handle = dlopen(copy.c_str(), RTLD_NOW);
        ASSERT_NE(nullptr, handle);
        typedef int64_t (*LastKeyFn)();
        LastKeyFn lastKey = (LastKeyFn)dlsym(handle, "wp1FakeExecLastHostKey");
        ASSERT_NE(nullptr, lastKey);
        for (int verifying = 0; verifying < 2; ++verifying)
        {
            ExecuteUnitPtr unit = factory.createExeUnit("Wp1FakeExec.unit");
            ASSERT_NE(nullptr, unit.get());
            WTSVariant* cfg = WTSVariant::createObject();
            cfg->append(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY, (uint32_t)1);	// user cannot force it
            std::string error;
            ASSERT_TRUE(unit->initialize(nullptr, kStdCode, cfg, error, verifying == 1)) << error;
            EXPECT_EQ((capable && verifying == 1) ? 1 : 0, lastKey()) << "capable=" << capable << " verifying=" << verifying;
            EXPECT_EQ(capable, unit->hostOrderIdentitySupported());
            EXPECT_EQ(1u, cfg->getUInt32(WT_EXEC_PLUGIN_V2_HOST_ORDER_IDENTITY_KEY)) << "shared policy node is never mutated";
            cfg->release();
            unit->requestQuiesce();
            ASSERT_TRUE(unit->waitQuiesced());
        }
    }
}
#endif
} // namespace

int main(int argc, char** argv)
{
    g_argv0 = argv[0];
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
