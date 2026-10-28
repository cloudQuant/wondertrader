/*
 * r124_porter_hft_orders_test.cpp  (lane R124 · wave20)
 *
 * 靶子：WtPorter 剩余的两块公开面
 *   - WtRtRunner.cpp:981-1042  initExecuters 的 diff / arbi / dist 分支，
 *     其中 diff/arbi 在配置里没有 trader 时必须走告警支（L989-991 / L1016-1018）。
 *   - WtPorter.cpp:2131-2139 / 2167-2175 / 2203-2211
 *     hft_cancel_all / hft_buy / hft_sell 的 OrderIDs 非空拼串与尾逗号裁剪。
 *
 * 既有 R82 只覆盖 local/diff/arbi 指向「不存在 trader」的支；R113 只覆盖
 * WtBtPorter 的同类拼串。本文件用公开 TraderAdapter::initExt 注入一个
 * 纯本地 ITraderApi（orderInsert/orderAction 立即返回 0，无连接、无线程），
 * 因此 hft 三条导出函数拿得到非空 localid，且不需要真实柜台。
 *
 * 边界：不开线程、不跑回测；adapter 的日志目录经 WtHelper::setGenerateDir
 * 指向系统临时目录，并在用例结束删除；不触碰 src 生产目录。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>

#include <unistd.h>

#include <boost/noncopyable.hpp>

#include "WtCore/ActionPolicyMgr.h"
#include "WtCore/TraderAdapter.h"
#include "WtCore/WtHelper.h"
#include "WtPorter/ExpHftContext.h"
#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtPorter.h"
#include "WtPorter/WtRtRunner.h"

#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	std::string prepare_dir(const char* tag)
	{
		stdfs::path p = stdfs::temp_directory_path()
			/ (std::string("r124_porter_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(p, ec);
		stdfs::create_directories(p, ec);
		return p.string();
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	WtRtRunner* new_runner(const std::string& dir)
	{
		/* 堆上构造、永不 delete：见 BUG-R46-001（构造列表漏初始化 _config/_engine）。 */
		WtRtRunner* r = new WtRtRunner();
		r->init((dir + "/r124_absent_logcfg.yaml").c_str(), true, dir.c_str());
		return r;
	}

	void reset_singleton()
	{
		WtRtRunner& r = getRunner();
		r.~WtRtRunner();
		new (&r) WtRtRunner();
	}

	/* 只实现导出层需要的三件事：init 成功、发委托号、下单/撤单返回 0。
	 * 不连接任何柜台，也不创建线程。 */
	class R124FakeTraderApi : public ITraderApi
	{
	public:
		bool init(WTSVariant*) override { return true; }

		bool makeEntrustID(char* buffer, int length) override
		{
			if (buffer == NULL || length <= 0)
				return false;
			std::snprintf(buffer, (std::size_t)length, "r124.%u", ++_seq);
			return true;
		}

		int orderInsert(WTSEntrust*) override { return 0; }
		int orderAction(WTSEntrustAction*) override { return 0; }

	private:
		unsigned int _seq = 0;
	};

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":0,\"to\":2359}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

	/* default 组同时给出 open/close，供 buy 与 sell 分别命中。 */
	const char* kActions =
		"{\"default\":{\"order\":[{\"action\":\"open\"},{\"action\":\"close\"}]}}";
}

/*
 * 生产: WtRtRunner.cpp:981-1042（diff/arbi 无 trader 告警 + dist 独立分支）
 * 既有 R82 的 diff/arbi 用例都带 trader 字段（unknown trader 支），
 * 这里刻意省略 trader，覆盖 strlen(tid)==0 的两条告警行。
 */
TEST(R124PorterExecuter, DiffArbiDistWithoutTraderAreAccepted)
{
	const std::string dir = prepare_dir("exec");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", "{}");

	WtRtRunner* r = new_runner(dir);
	ASSERT_NE(r, nullptr);

	const std::string cfg = "{\"basefiles\":{},"
		"\"env\":{\"name\":\"cta\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\","
		"\"executers\":["
		"{\"active\":true,\"id\":\"r124_exec_diff\",\"name\":\"diff\"},"
		"{\"active\":true,\"id\":\"r124_exec_arbi\",\"name\":\"arbi\"},"
		"{\"active\":true,\"id\":\"r124_exec_dist\",\"name\":\"dist\"},"
		"{\"active\":false,\"id\":\"r124_exec_skipped\",\"name\":\"local\"}"
		"]}";

	EXPECT_TRUE(r->config(cfg.c_str(), false));
	EXPECT_NE(r->getEngine(), nullptr);

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/*
 * 生产: WtPorter.cpp:2131-2139（cancel_all 非空拼串）
 *       WtPorter.cpp:2167-2175（buy 非空 OrderIDs）
 *       WtPorter.cpp:2203-2211（sell 非空 OrderIDs）
 *
 * 前置：global runner 以 hft 环境配置好 session/commodity/contract；
 * 用公开 initExt 把本地 fake API 接到 context 的 _trader 上。
 * 买入后向 adapter 回灌一笔长仓，使 sell 的 close 规则有可平量；
 * 再回灌买入委托的 WTSOrderInfo，使 cancel_all 能枚举到该 localid。
 */
TEST(R124PorterHftOrders, BuySellAndCancelAllProduceLocalIds)
{
	const std::string dir = prepare_dir("hftord");
	ASSERT_FALSE(dir.empty());
	write_text(dir + "/actions.json", kActions);
	write_text(dir + "/sessions.json", kSessions);
	write_text(dir + "/commodities.json", kCommodities);
	write_text(dir + "/contracts.json", kContracts);

	WtHelper::setGenerateDir(dir.c_str());
	reset_singleton();
	getRunner().init((dir + "/r124_absent_logcfg.yaml").c_str(), true, dir.c_str());

	const std::string cfg = "{\"basefiles\":{"
		"\"session\":\"" + dir + "/sessions.json\","
		"\"commodity\":\"" + dir + "/commodities.json\","
		"\"contract\":\"" + dir + "/contracts.json\"},"
		"\"env\":{\"name\":\"hft\"},\"data\":{},"
		"\"bspolicy\":\"" + dir + "/actions.json\"}";
	ASSERT_TRUE(getRunner().config(cfg.c_str(), false));

	const CtxHandler h = create_hft_context("r124_hft_ord", "R124_TRADER_ABSENT", false, 0);
	ASSERT_NE(h, 0U);
	HftContextPtr base = getRunner().getHftContext(h);
	ASSERT_TRUE(base != nullptr);
	ExpHftContext* ctx = dynamic_cast<ExpHftContext*>(base.get());
	ASSERT_NE(ctx, nullptr);

	IBaseDataMgr* bdMgr = getRunner().getEngine()->get_basedata_mgr();
	ASSERT_NE(bdMgr, nullptr);
	WTSContractInfo* ct = bdMgr->getContract("rb2510", "SHFE");
	ASSERT_NE(ct, nullptr);

	ActionPolicyMgr policy;
	ASSERT_TRUE(policy.init((dir + "/actions.json").c_str()));

	R124FakeTraderApi api;
	{
		TraderAdapter adapter(NULL);
		ASSERT_TRUE(adapter.initExt("r124_tr", &api, bdMgr, &policy));
		ctx->setTrader(&adapter);

		const char* code = "SHFE.rb.2510";

		std::string buy = hft_buy(h, code, 4500.0, 2.0, "r124", 0);
		ASSERT_FALSE(buy.empty());
		const uint32_t buyId = (uint32_t)std::strtoul(buy.c_str(), NULL, 10);
		ASSERT_NE(buyId, 0U);

		/* 给 adapter 回灌一笔多头可用持仓，sell 的 close 规则才有量。 */
		WTSArray* ayp = WTSArray::create();
		WTSPositionItem* pos = WTSPositionItem::create("rb2510", "CNY", "SHFE");
		pos->setContractInfo(ct);
		pos->setDirection(WDT_LONG);
		pos->setNewPosition(2.0);
		pos->setAvailNewPos(2.0);
		ayp->append(pos);
		pos->release();
		adapter.onRspPosition(ayp);
		ayp->release();

		std::string sell = hft_sell(h, code, 4500.0, 2.0, "r124", 0);
		ASSERT_FALSE(sell.empty());
		const uint32_t sellId = (uint32_t)std::strtoul(sell.c_str(), NULL, 10);
		ASSERT_NE(sellId, 0U);
		EXPECT_NE(buyId, sellId);

		/* 回灌买入委托，userTag 采用 doEntrust 写入的 otp.<id>.<localid> 形态。 */
		WTSOrderInfo* oi = WTSOrderInfo::create();
		oi->setContractInfo(ct);
		oi->setDirection(WDT_LONG);
		oi->setOffsetType(WOT_OPEN);
		oi->setPrice(4500.0);
		oi->setVolume(2.0);
		oi->setVolLeft(2.0);
		oi->setVolTraded(0.0);
		oi->setOrderID("r124_oid_1");
		char tag[64] = { 0 };
		std::snprintf(tag, sizeof(tag), "otp.r124_tr.%u", buyId);
		oi->setUserTag(tag);
		oi->setOrderState(WOS_NotTraded_Queuing);
		adapter.onPushOrder(oi);

		std::string canceled = hft_cancel_all(h, code, true);
		EXPECT_EQ(std::to_string(buyId), canceled);

		ctx->setTrader(nullptr);
	}

	reset_singleton();

	/* adapter 的 trades/orders 日志可能落在 getBaseDir()/traders/r124_tr。 */
	const std::string baseDir = WtHelper::getBaseDir();
	std::error_code ec;
	stdfs::remove_all(stdfs::path(baseDir) / "traders" / "r124_tr", ec);
	stdfs::remove_all(dir, ec);
}
