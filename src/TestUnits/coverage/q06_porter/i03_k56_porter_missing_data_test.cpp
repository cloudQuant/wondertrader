/*
 * 迭代03 K56（BUG-R6-006，用户裁定 2026-11-06）：WtPorter（WtRtRunner）配置缺 `data` 段时拒绝启动，
 * 与 WtRunner 一致。
 *
 * 合同：
 *  - basefiles/env/bspolicy 齐全但完全没有 `data` 段：config 返回 false 并打 ERROR，随后 run 安全拒绝。
 *  - 有 `data` 段但不配 `store`（由 wtpy 扩展数据加载器供数）仍然合法：config 返回 true。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include "Includes/ILogHandler.h"
#include "WTSTools/WTSLogger.h"
#include "WtPorter/WtRtRunner.h"

USING_NS_WTP;

namespace i03k56 {

class LogCapture : public ILogHandler
{
public:
	LogCapture()
	{
		static const char* cfg = R"({"root":{"level":"debug","async":false,"sinks":[]}})";
		WTSLogger::init(cfg, false, NULL);
		WTSLogger::registerHandler(this);
	}
	~LogCapture() { WTSLogger::registerHandler(NULL); }
	void handleLogAppend(WTSLogLevel level, const char* message) override
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_items.emplace_back(level, message == NULL ? "" : message);
	}
	size_t count(WTSLogLevel level, const char* text)
	{
		std::lock_guard<std::mutex> lock(_mutex);
		size_t n = 0;
		for (const auto& item : _items)
			if (item.first == level && item.second.find(text) != std::string::npos)
				++n;
		return n;
	}
	std::string dump()
	{
		std::lock_guard<std::mutex> lock(_mutex);
		std::string out;
		for (const auto& item : _items)
			out += std::to_string((int)item.first) + ": " + item.second + "\n";
		return out;
	}
private:
	std::mutex _mutex;
	std::vector<std::pair<WTSLogLevel, std::string>> _items;
};

std::string prepare_dir(const char* tag)
{
	std::filesystem::path p = std::filesystem::temp_directory_path()
		/ (std::string("i03_k56_") + tag + "_" + std::to_string(::getpid()));
	std::error_code ec;
	std::filesystem::remove_all(p, ec);
	std::filesystem::create_directories(p, ec);
	return p.string();
}

void write_text(const std::string& path, const std::string& body)
{
	std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
	ofs << body;
}

void write_basefiles(const std::string& dir)
{
	write_text(dir + "/sessions.json",
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,\"sections\":[{\"from\":900,\"to\":1500}]}}");
	write_text(dir + "/commodity.json",
		"{\"SHFE\":{\"au\":{\"name\":\"Gold\",\"session\":\"TRADING\",\"holiday\":\"CHINA\","
		"\"pricetick\":0.02,\"volscale\":1000,\"covermode\":0,\"pricemode\":0}}}");
	write_text(dir + "/contract.json",
		"{\"SHFE\":{\"au2406\":{\"name\":\"au2406\",\"product\":\"au\",\"exchg\":\"SHFE\"}}}");
	write_text(dir + "/holidays.json", "{\"CHINA\":[]}");
	write_text(dir + "/actions.json", "{\"default\":{}}");
}

std::string config_json(const std::string& dir, const char* dataSection)
{
	std::string cfg = "{\"basefiles\":{\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodity.json\","
		"\"contract\":\"" + dir + "/contract.json\","
		"\"holiday\":\"" + dir + "/holidays.json\"},"
		"\"env\":{\"name\":\"cta\",\"product\":{\"session\":\"TRADING\"}},";
	if (dataSection != NULL)
		cfg += std::string("\"data\":") + dataSection + ",";
	cfg += "\"bspolicy\":\"" + dir + "/actions.json\"}";
	return cfg;
}

/* 堆上构造、永不 delete（同 r69，见 BUG-R46-001）。 */
WtRtRunner* new_runner(const std::string& dir)
{
	WtRtRunner* r = new WtRtRunner();
	r->init((dir + "/absent_logcfg.yaml").c_str(), true, dir.c_str());
	return r;
}

} // namespace i03k56

using namespace i03k56;

TEST(I03K56PorterMissingData, MissingDataSectionFailsConfigAndRunIsRefused)
{
	LogCapture logs;
	const std::string dir = prepare_dir("missing");
	write_basefiles(dir);
	WtRtRunner* r = new_runner(dir);

	ASSERT_FALSE(r->config(config_json(dir, NULL).c_str(), false)) << logs.dump();
	EXPECT_GE(logs.count(LL_ERROR, "data"), 1u) << logs.dump();

	const size_t refusedBefore = logs.count(LL_ERROR, "refusing to run");
	r->run(true);
	EXPECT_EQ(refusedBefore + 1, logs.count(LL_ERROR, "refusing to run")) << logs.dump();

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

TEST(I03K56PorterMissingData, EmptyDataSectionWithoutStoreStillConfigures)
{
	LogCapture logs;
	const std::string dir = prepare_dir("empty");
	write_basefiles(dir);
	WtRtRunner* r = new_runner(dir);

	EXPECT_TRUE(r->config(config_json(dir, "{}").c_str(), false)) << logs.dump();
	EXPECT_NE(nullptr, r->getEngine());

	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

/*
 * 迭代03 K54（BUG-R211-001）：WtPorter 侧同一合同。env.riskmon.active=true 而模块加载失败时
 * config 返回 false，随后 run 拒绝；active=false 时照常配置。
 */
namespace i03k56 {
std::string config_with_riskmon(const std::string& dir, const char* riskmon)
{
	std::string cfg = config_json(dir, "{}");
	const std::string anchor = "\"product\":{\"session\":\"TRADING\"}";
	const size_t pos = cfg.find(anchor);
	if (pos != std::string::npos)
		cfg.insert(pos + anchor.size(), std::string(",\"riskmon\":") + riskmon);
	return cfg;
}
} // namespace i03k56

TEST(I03K54PorterRiskmon, ActiveRiskmonLoadFailureFailsConfigAndRunIsRefused)
{
	LogCapture logs;
	const std::string dir = prepare_dir("risk_bad");
	write_basefiles(dir);
	WtRtRunner* r = new_runner(dir);
	ASSERT_FALSE(r->config(config_with_riskmon(dir,
		"{\"active\":true,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}").c_str(), false)) << logs.dump();
	const size_t refusedBefore = logs.count(LL_ERROR, "refusing to run");
	r->run(true);
	EXPECT_EQ(refusedBefore + 1, logs.count(LL_ERROR, "refusing to run")) << logs.dump();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}

TEST(I03K54PorterRiskmon, InactiveRiskmonStillConfigures)
{
	LogCapture logs;
	const std::string dir = prepare_dir("risk_off");
	write_basefiles(dir);
	WtRtRunner* r = new_runner(dir);
	EXPECT_TRUE(r->config(config_with_riskmon(dir,
		"{\"active\":false,\"module\":\"i03_k54_no_such_riskmon\",\"name\":\"x\"}").c_str(), false)) << logs.dump();
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
}
