/*
 * 迭代03 K57（BUG-R134-004，用户裁定 2026-11-06）：实盘 WtPorter 不支持外部 tick 加载。
 *
 * 合同：
 *  - register_ext_data_loader 的 tickLoader 参数为 ABI 兼容保留；传非空值时打 WARN，说明实盘不支持、
 *    永不调用；传 NULL 时不打该告警。注册本身不调用任何加载器。
 *  - 实盘 feed_raw_ticks 打 ERROR，写明“实盘不支持”和丢弃的条数，丢弃数据，不崩溃（含 NULL/0）。
 * 回测（WtBtPorter）的 tick 加载器不受影响。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "Includes/ILogHandler.h"
#include "Includes/WTSStruct.h"
#include "WTSTools/WTSLogger.h"
#include "WtPorter/WtPorter.h"

USING_NS_WTP;

namespace i03k57 {

std::atomic<int> g_tick_loader_calls{0};
std::atomic<int> g_bar_loader_calls{0};

bool PORTER_FLAG tickLoader(const char*, uint32_t)
{
	++g_tick_loader_calls;
	return true;
}

bool PORTER_FLAG fnlBarLoader(const char*, const char*)
{
	++g_bar_loader_calls;
	return true;
}

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

} // namespace i03k57

using namespace i03k57;

TEST(I03K57LiveTickLoader, NonNullTickLoaderIsWarnedAndNeverCalled)
{
	LogCapture logs;
	register_ext_data_loader(NULL, NULL, NULL, NULL);
	EXPECT_EQ(0u, logs.count(LL_WARN, "tick loader is not supported")) << logs.dump();

	register_ext_data_loader(&fnlBarLoader, NULL, NULL, &tickLoader);
	EXPECT_EQ(1u, logs.count(LL_WARN, "tick loader is not supported")) << logs.dump();
	EXPECT_EQ(0, g_tick_loader_calls.load());
	EXPECT_EQ(0, g_bar_loader_calls.load());
	register_ext_data_loader(NULL, NULL, NULL, NULL);
}

TEST(I03K57LiveTickLoader, FeedRawTicksReportsUnsupportedAndDropCount)
{
	LogCapture logs;
	WTSTickStruct ticks[3];
	memset(ticks, 0, sizeof(ticks));
	feed_raw_ticks(ticks, 3);
	EXPECT_EQ(1u, logs.count(LL_ERROR, "feed_raw_ticks is not supported by the live runner")) << logs.dump();
	EXPECT_EQ(1u, logs.count(LL_ERROR, "3 ticks dropped")) << logs.dump();

	feed_raw_ticks(NULL, 0);
	EXPECT_EQ(2u, logs.count(LL_ERROR, "feed_raw_ticks is not supported by the live runner")) << logs.dump();
	EXPECT_EQ(1u, logs.count(LL_ERROR, "0 ticks dropped")) << logs.dump();
}
