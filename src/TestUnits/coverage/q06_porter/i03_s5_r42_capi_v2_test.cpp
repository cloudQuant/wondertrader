/*
 * i03_s5_r42_capi_v2_test.cpp (迭代03 s5-runners, BUG-R42-002)
 *
 * 版本化数量查询 hft_get_position_v2 / hft_get_undone_v2：未知句柄、非法参数、
 * 未绑定交易通道均返回可区分状态并把输出置为 NaN，已绑定上下文返回 OK 与
 * 合法零值。旧 hft_get_position/hft_get_undone 签名与行为不变。只用本地
 * fake：交易通道为不加载模块的 TraderAdapter，不连接柜台、不下单。
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

#include "Includes/ExecuteDefs.h"
#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSVariant.hpp"
#include "WtCore/ITrdNotifySink.h"
#include "WtCore/IExecCommand.h"
#include "WtCore/ExecCommandQuiesce.h"
#include "WtCore/ExecUnitInitStatus.h"
#include "WtCore/WtExecuterFactory.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/EventNotifier.h"
#include "WtCore/CtaStrategyMgr.h"
#include "WtCore/HftStrategyMgr.h"
#include "WtCore/SelStrategyMgr.h"
#include "WtCore/WtCtaEngine.h"
#include "WtCore/WtHftEngine.h"
#include "WtCore/WtSelEngine.h"
#include "WtCore/WtLocalExecuter.h"
#include "WtCore/WtDiffExecuter.h"
#include "WtCore/WtArbiExecuter.h"
#include "WtCore/WtDistExecuter.h"
#include "WtCore/WtExecMgr.h"
#include "WtCore/ParserAdapter.h"
#include "WtCore/WtDtMgr.h"
#include "WtCore/ActionPolicyMgr.h"
#include "WTSTools/WTSHotMgr.h"
#include "WTSTools/WTSBaseDataMgr.h"
#include "WtPorter/PorterDefs.h"
#include "Includes/ILogHandler.h"
#include "Includes/IDataReader.h"

#define private public
#include "../../../WtPorter/WtRtRunner.h"
#undef private
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/WtPorter.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();
TraderAdapterPtr i03s5_register_test_trader(WtRtRunner& runner, const char* id);

namespace
{
namespace stdfs = std::filesystem;

class CapiDir
{
public:
	CapiDir()
	{
		_path = stdfs::temp_directory_path() / ("i03s5_r42_capi_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(_path, ec);
		stdfs::create_directories(_path, ec);
		write("actions.json", "{\"default\":{}}");
		write("sessions.yaml", "ALLDAY:\n    name: AllDay\n    offset: 0\n    sections:\n"
			"        - from: 0\n          to: 0\n");
	}
	~CapiDir()
	{
		std::error_code ec;
		stdfs::remove_all(_path, ec);
	}
	std::string str() const { return _path.generic_string(); }
	std::string file(const char* name) const { return (_path / name).generic_string(); }
	void write(const char* name, const std::string& body) const
	{
		std::ofstream ofs(_path / name, std::ios::binary | std::ios::trunc);
		ofs << body;
	}
	std::string hft_config() const
	{
		return "{\"basefiles\":{\"session\":\"" + file("sessions.yaml") + "\"},"
			"\"env\":{\"name\":\"hft\",\"product\":{\"session\":\"ALLDAY\"}},"
			"\"data\":{},\"bspolicy\":\"" + file("actions.json") + "\","
			"\"parsers\":[],\"traders\":[]}";
	}

private:
	stdfs::path _path;
};
}

TEST(I03S5R42HftQuantity, CApiV2QueriesReportUnknownHandleAndInvalidArguments)
{
	// The C API resolves handles through the process porter; an unknown handle
	// must not be reported as a legal zero quantity.
	double qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_UNKNOWN_CONTEXT_V2, hft_get_position_v2(0, "SHFE.rb.2510", false, &qty));
	EXPECT_TRUE(std::isnan(qty));
	qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_UNKNOWN_CONTEXT_V2, hft_get_undone_v2(0xFFFFFFF0u, "SHFE.rb.2510", &qty));
	EXPECT_TRUE(std::isnan(qty));
	EXPECT_EQ(WT_HFT_QTY_INVALID_ARGUMENT_V2, hft_get_position_v2(0, "SHFE.rb.2510", false, nullptr));
	qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_INVALID_ARGUMENT_V2, hft_get_undone_v2(0, nullptr, &qty));
	EXPECT_TRUE(std::isnan(qty));
	// Legacy API keeps its old contract for an unknown handle (0).
	EXPECT_EQ(0.0, hft_get_position(0, "SHFE.rb.2510", false));
}

namespace
{
void reset_process_porter()
{
	WtRtRunner& runner = getRunner();
	runner.WtRtRunner::~WtRtRunner();
	new (&runner) WtRtRunner();
}
}

TEST(I03S5R42HftQuantity, CApiV2QueriesSucceedForBoundContextAndReportUnboundContext)
{
	CapiDir dir;
	reset_process_porter();
	WtRtRunner& runner = getRunner();
	runner.init(dir.file("absent_logcfg.yaml").c_str(), true, dir.str().c_str());
	ASSERT_TRUE(runner.config(dir.hft_config().c_str(), false));
	ASSERT_TRUE(i03s5_register_test_trader(runner, "i03s5_capi_trader") != nullptr);
	const CtxHandler bound = create_hft_context("i03s5_capi_bound", "i03s5_capi_trader", false, 0);
	ASSERT_NE(0u, bound);
	double qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_OK_V2, hft_get_position_v2(bound, "SHFE.rb.2510", false, &qty));
	EXPECT_EQ(0.0, qty); // the module-less adapter holds no position: a legal zero
	qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_OK_V2, hft_get_undone_v2(bound, "SHFE.rb.2510", &qty));
	EXPECT_EQ(0.0, qty);

	// An unbound export context is unreachable through createHftContext (R42
	// gate); install one directly to prove the versioned query reports it.
	ExpHftContext* unboundCtx = new ExpHftContext(&runner._hft_engine, "i03s5_capi_unbound", false, 0);
	const uint32_t unbound = unboundCtx->id();
	runner._hft_engine.addContext(HftContextPtr(unboundCtx));
	qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_TRADER_UNBOUND_V2, hft_get_position_v2(unbound, "SHFE.rb.2510", false, &qty));
	EXPECT_TRUE(std::isnan(qty));
	qty = 123.0;
	EXPECT_EQ(WT_HFT_QTY_TRADER_UNBOUND_V2, hft_get_undone_v2(unbound, "SHFE.rb.2510", &qty));
	EXPECT_TRUE(std::isnan(qty));
	runner.release();
	reset_process_porter();
}
