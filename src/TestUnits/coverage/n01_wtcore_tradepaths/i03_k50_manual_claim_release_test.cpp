/**
 * 迭代03 K50（I03-NEW-089，用户裁定 2026-11-06）：人工（manual）合约归属用带审计的显式命令释放。
 *
 * 合同：
 *  - owner 路由开启时，人工/裸 API 下单让账户的 `manual` 独占该合约（K44），之后执行器/HFT 在该
 *    账户上交易这个合约被拒绝，直到显式释放。
 *  - 释放命令必须带合约、操作员、原因；先持久写入账户审计日志（claim_released）再释放；
 *    该合约仍有未完成的人工委托时拒绝（LiveOrders，审计 claim_release_rejected），归属保留；
 *    审计不可用 / 写失败时拒绝，归属保留；没有该归属时 NotClaimed。
 *  - 入口：TraderAdapter / TraderAdapterMgr::releaseManualClaim；WtPorter `release_manual_claim`；
 *    WtRunner 请求文件目录（ManualClaimInbox，见本文件 inbox 用例与 m_WtRunner 用例）。
 */
#include "r71_wtcore_env.h"

#include <WtCore/AccountTradingGate.h>
#include <WtCore/ManualClaimInbox.hpp>

#include <climits>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>

using namespace wtp;

namespace {

namespace k50fs = std::filesystem;

TraderAdapter::OwnerRoutingCapabilities k50Capabilities()
{
	TraderAdapter::OwnerRoutingCapabilities caps;
	caps.durable_journal = true;
	caps.local_order_id_floor = true;
	caps.provider_user_tag_round_trip = true;
	caps.stable_trade_id_across_push_and_query = true;
	return caps;
}

// fill=true：每笔委托同步成交（委托随即结束）；fill=false：委托一直在途
class K50Api : public r71::R71FakeTraderApi
{
public:
	ITraderSpi* spi = nullptr;
	r71::R71BaseDataMgr* bd = nullptr;
	bool fill = true;
	uint32_t trade_seq = 0;

	void registerSpi(ITraderSpi* listener) override { spi = listener; }

	int orderInsert(WTSEntrust* entrust) override
	{
		r71::R71FakeTraderApi::orderInsert(entrust);
		if (!fill || spi == nullptr || bd == nullptr)
			return 0;
		WTSContractInfo* ct = bd->getContract(entrust->getCode(), entrust->getExchg());
		WTSTradeInfo* trade = WTSTradeInfo::create(entrust->getCode(), entrust->getExchg());
		trade->setContractInfo(ct);
		trade->setUserTag(entrust->getUserTag());
		const std::string tid = "K50-T" + std::to_string(++trade_seq);
		trade->setTradeID(tid.c_str());
		trade->setRefOrder(tid.c_str());
		trade->setVolume(entrust->getVolume());
		trade->setPrice(entrust->getPrice());
		trade->setDirection(entrust->getDirection());
		trade->setOffsetType(entrust->getOffsetType());
		trade->setTradeDate(20250110);
		trade->setTradeTime(93100000);
		spi->onPushTrade(trade);
		trade->release();

		// 与真实柜台一致：成交后推送委托终态（全部成交），台账中的这笔人工委托随之结束
		WTSOrderInfo* order = WTSOrderInfo::create();
		order->setEntrustID(entrust->getEntrustID());
		order->setContractInfo(ct);
		order->setDirection(entrust->getDirection());
		order->setOffsetType(entrust->getOffsetType());
		order->setPrice(entrust->getPrice());
		order->setVolume(entrust->getVolume());
		order->setVolTraded(entrust->getVolume());
		order->setVolLeft(0.0);
		order->setUserTag(entrust->getUserTag());
		order->setOrderID(tid.c_str());
		order->setOrderState(WOS_AllTraded);
		spi->onPushOrder(order);
		order->release();
		return 0;
	}
};

std::string readAll(const k50fs::path& file)
{
	std::ifstream in(file.string(), std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

k50fs::path k50Dir(const char* tag)
{
	const k50fs::path dir = k50fs::temp_directory_path() /
		(std::string("i03_k50_") + tag + "_" + std::to_string(static_cast<long>(::getpid())));
	std::error_code ec;
	k50fs::remove_all(dir, ec);
	k50fs::create_directories(dir, ec);
	return dir;
}

AccountTradingGate::ClaimReleaseRequest k50Request(const char* code, const char* op = "alice",
	const char* reason = "manual hedge closed")
{
	AccountTradingGate::ClaimReleaseRequest request;
	request.code = code;
	request.operator_id = op;
	request.reason = reason;
	request.reference = "TICKET-K50";
	return request;
}

const char* const kAg = "SHFE.ag.2512";

class I03K50ManualClaimRelease : public ::testing::Test
{
protected:
	r71::R71GenEnv env;
	r71::R71TraderEnv traderEnv;
	K50Api api;
	k50fs::path dir;

	void SetUp() override
	{
		traderEnv.bd.regisSession("TRADING");
		WTSSessionInfo* sess = traderEnv.bd.session("TRADING");
		ASSERT_NE(sess, nullptr);
		sess->addTradingSection(900, 1500);
		WTSCommodityInfo* ag = r71::makeComm("SHFE", "ag", "TRADING", TM_Both);
		ag->setSessionInfo(sess);
		traderEnv.bd.regis(ag, r71::makeCt("SHFE", "ag", "ag2512"));
		api.bd = &traderEnv.bd;
		dir = k50Dir(::testing::UnitTest::GetInstance()->current_test_info()->name());
	}

	void TearDown() override
	{
		std::error_code ec;
		k50fs::remove_all(dir, ec);
	}

	bool build(TraderAdapter& trader, const char* id, bool withAudit)
	{
		if (!traderEnv.policy_ok() || !trader.initExt(id, &api, &traderEnv.bd, traderEnv.policy()) || !trader.run())
			return false;
		trader.onLoginResult(true, "k50", 20250110);
		trader.onRspPosition(static_cast<const WTSArray*>(nullptr));
		trader.onRspOrders(static_cast<const WTSArray*>(nullptr));
		trader.onRspTrades(static_cast<const WTSArray*>(nullptr));
		trader.onRspAccount(static_cast<WTSArray*>(nullptr));
		if (trader.state() != TraderAdapter::AS_ALLREADY)
			return false;
		if (!trader.configureOwnerRoutingCapabilities(k50Capabilities()))
			return false;
		if (withAudit)
		{
			std::string error;
			if (!trader.accountGate()->configureAudit((dir / "account_audit.log").string(), error))
				return false;
		}
		return true;
	}
};

} // namespace

TEST_F(I03K50ManualClaimRelease, AuditedCommandReleasesTheManualClaimForExecuters)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k50_release", true));
	ASSERT_NE(UINT_MAX, trader.openLong(kAg, 3500.0, 1.0, 0));		// 人工单，立即成交
	ASSERT_TRUE(trader.accountGate()->hasClaim(kAg, "manual"));
	ASSERT_TRUE(trader.buyAsOwner("exec:k50", true, kAg, 3500.0, 1.0, 0, false).empty())
		<< "before the release an executer may not trade the manual contract";

	std::string diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::InvalidRequest,
		trader.releaseManualClaim(k50Request(kAg, ""), &diagnostic)) << diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::InvalidRequest,
		trader.releaseManualClaim(k50Request(kAg, "alice", ""), &diagnostic)) << diagnostic;
	EXPECT_TRUE(trader.accountGate()->hasClaim(kAg, "manual"));

	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::Ok,
		trader.releaseManualClaim(k50Request(kAg), &diagnostic)) << diagnostic;
	EXPECT_FALSE(trader.accountGate()->hasClaim(kAg, "manual"));
	EXPECT_EQ(1u, trader.buyAsOwner("exec:k50", true, kAg, 3500.0, 1.0, 0, false).size())
		<< "after the release the executer may claim and trade the contract";

	const std::string audit = readAll(dir / "account_audit.log");
	EXPECT_NE(std::string::npos, audit.find("\"event\":\"claim_released\"")) << audit;
	EXPECT_NE(std::string::npos, audit.find("\"actor\":\"alice\"")) << audit;
	EXPECT_NE(std::string::npos, audit.find("manual hedge closed")) << audit;
	EXPECT_NE(std::string::npos, audit.find("TICKET-K50")) << audit;

	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::NotClaimed,
		trader.releaseManualClaim(k50Request(kAg), &diagnostic)) << diagnostic;
	trader.release();
}

TEST_F(I03K50ManualClaimRelease, LiveManualOrderBlocksTheRelease)
{
	api.fill = false;
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k50_live", true));
	ASSERT_NE(UINT_MAX, trader.openLong(kAg, 3500.0, 1.0, 0));		// 人工单，一直在途
	ASSERT_TRUE(trader.accountGate()->hasClaim(kAg, "manual"));

	std::string diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::LiveOrders,
		trader.releaseManualClaim(k50Request(kAg), &diagnostic)) << diagnostic;
	EXPECT_TRUE(trader.accountGate()->hasClaim(kAg, "manual"));
	EXPECT_TRUE(trader.buyAsOwner("exec:k50", true, kAg, 3500.0, 1.0, 0, false).empty());
	const std::string audit = readAll(dir / "account_audit.log");
	EXPECT_NE(std::string::npos, audit.find("\"event\":\"claim_release_rejected\"")) << audit;
	EXPECT_EQ(std::string::npos, audit.find("\"event\":\"claim_released\"")) << audit;
	trader.release();
}

TEST_F(I03K50ManualClaimRelease, ReleaseWithoutDurableAuditKeepsTheClaim)
{
	TraderAdapter trader;
	ASSERT_TRUE(build(trader, "k50_no_audit", false));
	ASSERT_NE(UINT_MAX, trader.openLong(kAg, 3500.0, 1.0, 0));
	std::string diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::AuditUnavailable,
		trader.releaseManualClaim(k50Request(kAg), &diagnostic)) << diagnostic;
	EXPECT_TRUE(trader.accountGate()->hasClaim(kAg, "manual"));
	trader.release();
}

TEST(I03K50AccountGate, FailedAuditWriteKeepsTheClaimAndOtherOwnersAreUntouched)
{
	const k50fs::path dir = k50Dir("gate");
	AccountTradingGate gate("acct-k50");
	std::string error;
	ASSERT_TRUE(gate.configureAudit((dir / "audit.log").string(), error)) << error;
	ASSERT_TRUE(gate.claimContract("SHFE.ag.2512", "manual", false, error)) << error;
	ASSERT_TRUE(gate.claimContract("SHFE.au.2212", "exec:a", true, error)) << error;

	gate.failAuditWritesForTest(true);
	std::string diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::AuditWriteFailed,
		gate.releaseContractClaim("manual", k50Request("SHFE.ag.2512"), false, &diagnostic)) << diagnostic;
	EXPECT_TRUE(gate.hasClaim("SHFE.ag.2512", "manual"));
	gate.failAuditWritesForTest(false);

	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::NotClaimed,
		gate.releaseContractClaim("manual", k50Request("SHFE.au.2212"), false, &diagnostic)) << diagnostic;
	EXPECT_EQ(AccountTradingGate::ClaimReleaseStatus::Ok,
		gate.releaseContractClaim("manual", k50Request("SHFE.ag.2512"), false, &diagnostic)) << diagnostic;
	EXPECT_FALSE(gate.hasClaim("SHFE.ag.2512", "manual"));
	EXPECT_TRUE(gate.hasClaim("SHFE.au.2212", "exec:a")) << "only the requested owner/contract is released";
	std::error_code ec;
	k50fs::remove_all(dir, ec);
}

TEST(I03K50ManualClaimInbox, EachRequestIsTakenOnceAndArchivedByOutcome)
{
	const k50fs::path dir = k50Dir("inbox");
	auto write = [&](const char* name, const std::string& body) {
		std::ofstream out((dir / name).string(), std::ios::binary | std::ios::trunc);
		out << body;
	};
	write("a_ok.json", R"({"trader":"t1","code":"SHFE.ag.2512","operator":"alice","reason":"closed","reference":"T-1"})");
	write("b_live.json", R"({"trader":"t1","code":"SHFE.au.2212","operator":"bob","reason":"closed"})");
	write("c_no_code.json", R"({"trader":"t1","operator":"carol","reason":"closed"})");
	write("d_no_operator.json", R"({"trader":"t1","code":"SHFE.ag.2512","reason":"closed"})");
	write("e_broken.json", "{not json");
	write("notes.txt", "ignored");

	std::vector<std::string> seen;
	auto releaser = [&](const std::string& trader, const AccountTradingGate::ClaimReleaseRequest& request, std::string& diagnostic) {
		seen.push_back(trader + "|" + request.code + "|" + request.operator_id + "|" + request.reference);
		diagnostic = "handled";
		return request.code == "SHFE.au.2212" ? AccountTradingGate::ClaimReleaseStatus::LiveOrders
			: AccountTradingGate::ClaimReleaseStatus::Ok;
	};
	const std::vector<ManualClaimOutcome> outcomes = ManualClaimInbox::process(dir.string(), releaser);
	ASSERT_EQ(5u, outcomes.size());
	ASSERT_EQ(2u, seen.size()) << "malformed requests never reach the account gate";
	EXPECT_EQ("t1|SHFE.ag.2512|alice|T-1", seen[0]);
	EXPECT_EQ("t1|SHFE.au.2212|bob|", seen[1]);

	std::map<std::string, std::string> suffix;	// original name -> final suffix
	std::error_code ec;
	for (const auto& entry : k50fs::directory_iterator(dir, ec))
	{
		const std::string name = entry.path().filename().string();
		const std::size_t json = name.find(".json");
		suffix[json == std::string::npos ? name : name.substr(0, json + 5)] =
			json == std::string::npos ? "" : name.substr(name.rfind('.') + 1);
	}
	EXPECT_EQ("done", suffix["a_ok.json"]);
	EXPECT_EQ("rejected", suffix["b_live.json"]);
	EXPECT_EQ("invalid", suffix["c_no_code.json"]);
	EXPECT_EQ("invalid", suffix["d_no_operator.json"]);
	EXPECT_EQ("invalid", suffix["e_broken.json"]);
	EXPECT_EQ("", suffix["notes.txt"]);

	// 已归档的请求不会被再次处理
	EXPECT_TRUE(ManualClaimInbox::process(dir.string(), releaser).empty());
	EXPECT_EQ(2u, seen.size());
	k50fs::remove_all(dir, ec);
}
