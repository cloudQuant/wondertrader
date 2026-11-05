/**
 * 迭代03 第五会话主线程：卡6 6b=A（BUG-L01-014）——账户恢复的显式命令入口（恢复请求文件）+ 审计。
 *
 * - AccountResumeInbox：每个 `<trader>.json` 只处理一次（先改名认领再解析），按结果改名 .done/.rejected/.invalid，
 *   非 .json 文件不动；缺操作员/原因/审阅代次的请求不交给门禁。
 * - 与真实 AccountTradingGate 联动：暂停后按审阅代次恢复成功并写审计；过期代次被拒绝且保持暂停。
 * - WtRunner::processAccountResumeRequests：未知交易通道的请求被拒绝并归档。
 */
#include "r71_wtcore_env.h"

#include <WtCore/AccountResumeInbox.hpp>
#include <WtCore/AccountTradingGate.h>

#include <fstream>
#include <map>
#include <unistd.h>

using namespace wtp;

namespace {

namespace l01fs = std::filesystem;

l01fs::path l01014_dir(const char* tag)
{
	const l01fs::path dir = l01fs::temp_directory_path() /
		(std::string("s5_l01014_") + tag + "_" + std::to_string(static_cast<long>(::getpid())));
	std::error_code ec;
	l01fs::remove_all(dir, ec);
	l01fs::create_directories(dir, ec);
	return dir;
}

void l01014_write(const l01fs::path& file, const std::string& body)
{
	std::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
	out << body;
}

std::map<std::string, std::string> l01014_listing(const l01fs::path& dir)
{
	std::map<std::string, std::string> names;	// original request name -> final suffix
	std::error_code ec;
	for (const auto& entry : l01fs::directory_iterator(dir, ec))
	{
		const std::string name = entry.path().filename().string();
		const std::size_t json = name.find(".json");
		if (json == std::string::npos)
		{
			names[name] = "";
			continue;
		}
		const std::size_t last = name.rfind('.');
		names[name.substr(0, json + 5)] = name.substr(last + 1);
	}
	return names;
}

std::string l01014_request(const char* op, const char* reason, const char* generation)
{
	std::string body = "{";
	bool first = true;
	auto add = [&](const char* key, const std::string& value) {
		body += (first ? "" : ",");
		body += "\"" + std::string(key) + "\":" + value;
		first = false;
	};
	if (op != NULL) add("operator", std::string("\"") + op + "\"");
	if (reason != NULL) add("reason", std::string("\"") + reason + "\"");
	if (generation != NULL) add("generation", generation);
	add("reference", "\"TICKET-1\"");
	return body + "}";
}

} // namespace

TEST(S5MainL01014ResumeInbox, EachRequestIsTakenExactlyOnceAndArchivedByOutcome)
{
	const l01fs::path dir = l01014_dir("inbox");
	l01014_write(dir / "acct_ok.json", l01014_request("alice", "reconciled", "3"));
	l01014_write(dir / "acct_stale.json", l01014_request("bob", "reconciled", "2"));
	l01014_write(dir / "no_operator.json", l01014_request(NULL, "reconciled", "3"));
	l01014_write(dir / "no_generation.json", l01014_request("carol", "reconciled", NULL));
	l01014_write(dir / "broken.json", "{not json");
	l01014_write(dir / "notes.txt", "ignored");

	std::vector<std::string> calls;
	AccountTradingGate::ResumeRequest seen;
	auto resumer = [&](const std::string& trader, const AccountTradingGate::ResumeRequest& request, std::string& diagnostic) {
		calls.push_back(trader);
		if (trader == "acct_ok")
		{
			seen = request;
			return AccountTradingGate::ResumeStatus::Ok;
		}
		diagnostic = "stale";
		return AccountTradingGate::ResumeStatus::StaleGeneration;
	};
	const std::vector<AccountResumeOutcome> outcomes = AccountResumeInbox::process(dir.string(), resumer);
	ASSERT_EQ(5u, outcomes.size());
	EXPECT_EQ((std::vector<std::string>{ "acct_ok", "acct_stale" }), calls) << "malformed requests never reach the gate";
	EXPECT_EQ("alice", seen.operator_id);
	EXPECT_EQ("reconciled", seen.reason);
	EXPECT_EQ(3u, seen.expected_generation);
	EXPECT_EQ("TICKET-1", seen.reconciliation_reference);

	std::map<std::string, std::string> files = l01014_listing(dir);
	EXPECT_EQ("done", files["acct_ok.json"]);
	EXPECT_EQ("rejected", files["acct_stale.json"]);
	EXPECT_EQ("invalid", files["no_operator.json"]);
	EXPECT_EQ("invalid", files["no_generation.json"]);
	EXPECT_EQ("invalid", files["broken.json"]);
	EXPECT_EQ("", files["notes.txt"]);
	EXPECT_FALSE(l01fs::exists(dir / "acct_ok.json"));

	// 已处理的请求不会再次交给门禁
	calls.clear();
	EXPECT_TRUE(AccountResumeInbox::process(dir.string(), resumer).empty());
	EXPECT_TRUE(calls.empty());
	// 目录不存在不是错误
	EXPECT_TRUE(AccountResumeInbox::process((dir / "missing").string(), resumer).empty());
	std::error_code ec;
	l01fs::remove_all(dir, ec);
}

TEST(S5MainL01014ResumeInbox, RealGateResumesOnlyWithTheReviewedGenerationAndAudits)
{
	const l01fs::path dir = l01014_dir("gate");
	const l01fs::path inbox = dir / "resume";
	l01fs::create_directories(inbox);
	AccountTradingGate gate("acct-l01014");
	std::string error;
	ASSERT_TRUE(gate.configureAudit((dir / "audit.log").string(), error)) << error;
	const uint64_t generation = gate.pause("test", "unresolvable option code report");
	ASSERT_TRUE(gate.paused());

	auto resumer = [&gate](const std::string&, const AccountTradingGate::ResumeRequest& request, std::string& diagnostic) {
		return gate.resume(request, &diagnostic);
	};

	// 审阅后又发生一次暂停：用旧代次的请求被拒绝，账户保持暂停
	const uint64_t later = gate.pause("test", "second unresolved report");
	ASSERT_GT(later, generation);
	l01014_write(inbox / "acct.json", l01014_request("alice", "reconciled", std::to_string(generation).c_str()));
	std::vector<AccountResumeOutcome> stale = AccountResumeInbox::process(inbox.string(), resumer);
	ASSERT_EQ(1u, stale.size());
	EXPECT_EQ(AccountTradingGate::ResumeStatus::StaleGeneration, stale[0].status);
	EXPECT_TRUE(gate.paused());

	l01014_write(inbox / "acct.json", l01014_request("alice", "reconciled", std::to_string(later).c_str()));
	std::vector<AccountResumeOutcome> ok = AccountResumeInbox::process(inbox.string(), resumer);
	ASSERT_EQ(1u, ok.size());
	EXPECT_EQ(AccountTradingGate::ResumeStatus::Ok, ok[0].status) << ok[0].diagnostic;
	EXPECT_FALSE(gate.paused());

	std::ifstream audit((dir / "audit.log").string());
	const std::string text((std::istreambuf_iterator<char>(audit)), std::istreambuf_iterator<char>());
	EXPECT_NE(std::string::npos, text.find("resume")) << "the gate records the resume durably";
	EXPECT_NE(std::string::npos, text.find("alice"));
	std::error_code ec;
	l01fs::remove_all(dir, ec);
}
