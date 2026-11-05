/*
 * i03_s5_test_trader_support.cpp (迭代03 s5-runners, BUG-R42-002)
 *
 * R42-002 已定门禁：createHftContext / create_hft_context 对未知或空 trader
 * 返回 0，不登记上下文。原先以“不存在的 trader”造 HFT 上下文、只为驱动
 * 回调/转发的旧夹具前提因此失效。本文件提供唯一的测试辅助：向 runner 的
 * 交易通道管理器登记一个不加载任何模块、不连接柜台的本地 TraderAdapter，
 * 让这些夹具改为绑定一个已登记通道（门禁的合法正分支）。
 *
 * TraderAdapter 在 _trader_api 为空时 run() 直接返回 false、release() 不调用
 * 任何 provider，因此不会触发真实下单、撤单或结算。
 */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <new>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

USING_NS_WTP;

// Registers a module-less local TraderAdapter under `id` and returns it. An
// adapter already registered under `id` (for example on the shared process
// porter) is returned as is. Returns null only for an empty id or a failed add.
TraderAdapterPtr i03s5_register_test_trader(WtRtRunner& runner, const char* id)
{
	if (id == NULL || id[0] == '\0')
		return TraderAdapterPtr();
	if (TraderAdapterPtr existing = runner._traders.getAdapter(id))
		return existing;
	TraderAdapterPtr trader(new TraderAdapter());
	if (!runner._traders.addAdapter(id, trader))
		return TraderAdapterPtr();
	return trader;
}


#include "WtPorter/ExpCtaContext.h"
#include "WtPorter/ExpSelContext.h"
#include "WtPorter/ExpHftContext.h"

// I03-NEW-064: createCta/Sel/HftContext now refuse an engine the runner did not
// configure (a sibling engine has no base data or ticker; NEW-130 crashed
// there). Forwarding-only fixtures that intentionally exercise ExpXxxContext
// callbacks without a configured engine install the context directly; they
// must not call engine paths that need configuration.
uint32_t i03s5_inject_cta_context(WtRtRunner& runner, const char* name, int32_t slippage)
{
	ExpCtaContext* ctx = new ExpCtaContext(&runner._cta_engine, name, slippage);
	runner._cta_engine.addContext(CtaContextPtr(ctx));
	return ctx->id();
}

uint32_t i03s5_inject_sel_context(WtRtRunner& runner, const char* name, uint32_t date, uint32_t time,
	TaskPeriodType period, int32_t slippage)
{
	ExpSelContext* ctx = new ExpSelContext(&runner._sel_engine, name, slippage);
	runner._sel_engine.addContext(SelContextPtr(ctx), date, time, period, true, "CHINA", "TRADING");
	return ctx->id();
}

uint32_t i03s5_inject_hft_context(WtRtRunner& runner, const char* name, const char* trader, int32_t slippage)
{
	TraderAdapterPtr adapter = runner._traders.getAdapter(trader);
	if (!adapter)
		return 0;
	ExpHftContext* ctx = new ExpHftContext(&runner._hft_engine, name, false, slippage);
	ctx->setTrader(adapter.get());
	runner._hft_engine.addContext(HftContextPtr(ctx));
	adapter->addSink(ctx);
	return ctx->id();
}

extern WtRtRunner& getRunner();

// I03-NEW-064: tests of the C entry points configure the process porter with the
// engine they exercise (configuration is one-shot, card 9c, so the porter object
// is replaced first, like the v2 fixture does).
bool i03s5_fresh_configured_porter(const char* engine)
{
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() /
		("i03s5_porter_" + std::string(engine) + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::create_directories(dir, ec);
	{ std::ofstream actions(dir / "actions.json", std::ios::trunc); actions << "{\"default\":{}}"; }
	{
		std::ofstream sessions(dir / "sessions.yaml", std::ios::trunc);
		sessions << "ALLDAY:\n    name: AllDay\n    offset: 0\n    sections:\n"
			"        - from: 0\n          to: 0\n";
	}
	WtRtRunner& runner = getRunner();
	runner.WtRtRunner::~WtRtRunner();
	new (&runner) WtRtRunner();
	runner.init((dir / "absent_logcfg.yaml").string().c_str(), true, dir.string().c_str());
	const std::string cfg = "{\"basefiles\":{\"session\":\"" + (dir / "sessions.yaml").generic_string() + "\"},"
		"\"env\":{\"name\":\"" + std::string(engine) + "\",\"product\":{\"session\":\"ALLDAY\"}},"
		"\"data\":{},\"bspolicy\":\"" + (dir / "actions.json").generic_string() + "\","
		"\"parsers\":[],\"traders\":[],\"executers\":[]}";
	return runner.config(cfg.c_str(), false);
}
