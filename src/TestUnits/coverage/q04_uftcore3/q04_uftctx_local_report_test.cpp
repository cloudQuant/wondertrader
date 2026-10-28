/*!
 * \file q04_uftctx_local_report_test.cpp
 * \brief UftStraContext 本地块装载与成交/委托回报推进(迭代01 第五波 q04)
 *
 * 覆盖: on_channel_ready -> load_local_data 的 position/order/trade/round 四个 membin
 * 块建立(含 mannual.yaml 手工持仓导入与改名)、on_trade 的净头寸推进与回合盈亏、
 * on_order/on_entrust/on_position 的本单认领、stra_* 下单与查询转发、
 * 交易日切换时的过期块清理。
 * 落盘只发生在 temp_directory_path() 下的唯一根目录, 用例结束删除。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/WtUftDtMgr.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

//WtHelper::getOutputDir() 把路径缓存成函数内静态, 所以根目录一次定死,
//每个用例再用唯一的策略名区分自己的子目录
const std::string& ctxRoot()
{
	static std::string root =
		(std::filesystem::temp_directory_path() / "q04_uftctx_root").string() + "/";
	std::error_code ec;
	std::filesystem::remove_all(std::filesystem::temp_directory_path() / "q04_uftctx_root", ec);
	std::filesystem::create_directories(root, ec);
	WtHelper::setGenerateDir(root.c_str());
	return root;
}

struct CtxEnv
{
	q04::TempRoot			_tmp{ "ctx" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	TraderAdapter			_adapter;
	WtUftDtMgr				_dtMgr;
	WtUftEngine				_engine;
	UftStraContext			_ctx;
	std::string				_name;

	explicit CtxEnv(const char* name)
		: _ctx(&_engine, name)
	{
		_name = name;
		ctxRoot();
		_bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover, TM_Both, 1);
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		_engine.init(nullptr, &_bd, nullptr, nullptr);
		EXPECT_TRUE(_adapter.initExt(name, &_api, &_bd, &_policy));
		_adapter.run();
		//交易日必须先写好, 后续回报才会带正确的 _trading_day
		_adapter.onLoginResult(true, "ok", 20240315);
		_adapter.addSink(&_ctx);
		_ctx.setTrader(&_adapter);
	}

	std::string straFolder()
	{
		return ctxRoot() + "outputs/" + _name + "/";
	}

	std::string straFile(const char* suffix)
	{
		return straFolder() + suffix;
	}

	void removeOwnFolder()
	{
		std::error_code ec;
		std::filesystem::remove_all(ctxRoot() + "outputs/" + _name, ec);
	}
};

}	// namespace

TEST(q04_uftctx_local, channel_ready_builds_the_four_local_blocks)
{
	CtxEnv ctx("q04ctx_ready");
	ctx.removeOwnFolder();

	//_tradingday 为 0 时 load_local_data 直接早退, 不建任何文件
	ctx._ctx.on_position(kAg, true, 1, 1, 1, 1, 20240315);
	EXPECT_FALSE(std::filesystem::exists(ctx.straFile("position.membin")));

	ctx._ctx.on_channel_ready(20240315);
	EXPECT_TRUE(std::filesystem::exists(ctx.straFile("position.membin")));
	EXPECT_TRUE(std::filesystem::exists(ctx.straFile("order.membin")));
	EXPECT_TRUE(std::filesystem::exists(ctx.straFile("trade.membin")));
	EXPECT_TRUE(std::filesystem::exists(ctx.straFile("round.membin")));

	//本地无历史持仓
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_posprofit(kAg));
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_closeprofit(kAg));

	//同一天重复就绪不再重建
	ctx._ctx.on_channel_ready(20240315);
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_position(kAg));
	ctx.removeOwnFolder();
}

TEST(q04_uftctx_local, manual_positions_are_imported_then_file_is_renamed)
{
	CtxEnv ctx("q04ctx_manual");
	ctx.removeOwnFolder();

	std::error_code ec;
	std::filesystem::create_directories(ctx.straFolder(), ec);

	//手工持仓表: 一条可识别, 一条合约不存在(应被跳过)
	ctx._tmp.write(ctx.straFolder() + "mannual.yaml", R"JSON({
	"details": [
		{"exchg":"SHFE","code":"ag2406","direct":0,"volume":3,"openprice":5000},
		{"exchg":"DCE","code":"m2509","direct":1,"volume":1,"openprice":3000}
	]
})JSON");

	ctx._ctx.on_channel_ready(20240315);

	//导入的 3 手多头进入本地净持仓, 成本 3*5000*1
	EXPECT_DOUBLE_EQ(3.0, ctx._ctx.stra_get_local_position(kAg));
	//原文件被改名, 不会二次生效
	long renamed = 0;
	for (auto& e : std::filesystem::directory_iterator(ctx.straFolder(), ec))
	{
		if (e.path().string().find("mannual.yaml.") != std::string::npos)
			renamed++;
	}
	EXPECT_EQ(1L, renamed);
	EXPECT_FALSE(std::filesystem::exists(ctx.straFolder() + "mannual.yaml"));
	ctx.removeOwnFolder();
}

TEST(q04_uftctx_local, trade_reports_advance_position_and_round_profit)
{
	CtxEnv ctx("q04ctx_trade");
	ctx.removeOwnFolder();
	ctx._ctx.on_channel_ready(20240315);

	//开多 4 手
	uint32_t idOpen = ctx._ctx.stra_enter_long(kAg, 5000.0, 4.0, 0);
	EXPECT_NE(UINT_MAX, idOpen);
	//委托回报(此时订单尚未登记, on_order 只落块)
	ctx._ctx.on_order(idOpen, kAg, true, 0, 4.0, 4.0, 5000.0, false);
	//非本策略的单子要被拒掉
	ctx._ctx.on_trade(idOpen + 100000, kAg, true, 0, 1.0, 5000.0);
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_position(kAg));
	//委托回报同样只认自己的单
	ctx._ctx.on_entrust(idOpen + 100000, kAg, false, "no");
	ctx._ctx.on_entrust(idOpen, kAg, true, "accepted");

	ctx._ctx.on_trade(idOpen, kAg, true, 0, 4.0, 5000.0);
	EXPECT_DOUBLE_EQ(4.0, ctx._ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_closeprofit(kAg));

	//平多 4 手: 净头寸归零, 回合盈亏 (5100-5000)*4*1 = 400
	uint32_t idClose = ctx._ctx.stra_exit_long(kAg, 5100.0, 4.0, false, 0);
	EXPECT_NE(UINT_MAX, idClose);
	ctx._ctx.on_trade(idClose, kAg, true, 1, 4.0, 5100.0);
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(400.0, ctx._ctx.stra_get_local_closeprofit(kAg));
	//浮动盈亏只在盯市时更新, 成交推进不动它
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_local_posprofit(kAg));

	//反向: 先开空 2, 再买 5 -> 平空 2 + 开多 3
	uint32_t idS = ctx._ctx.stra_enter_short(kAg, 5200.0, 2.0, 0);
	ctx._ctx.on_trade(idS, kAg, false, 0, 2.0, 5200.0);
	EXPECT_DOUBLE_EQ(-2.0, ctx._ctx.stra_get_local_position(kAg));
	uint32_t idB = ctx._ctx.stra_exit_short(kAg, 5150.0, 5.0, false, 0);
	ctx._ctx.on_trade(idB, kAg, false, 1, 5.0, 5150.0);
	//空头被平 2 手获利 (5200-5150)*2 = 100, 剩余 3 手开多
	EXPECT_DOUBLE_EQ(3.0, ctx._ctx.stra_get_local_position(kAg));
	EXPECT_DOUBLE_EQ(500.0, ctx._ctx.stra_get_local_closeprofit(kAg));
	ctx.removeOwnFolder();
}

TEST(q04_uftctx_local, order_and_cancel_forward_through_the_channel)
{
	CtxEnv ctx("q04ctx_cancel");
	ctx.removeOwnFolder();
	ctx._ctx.on_channel_ready(20240315);

	// stra_buy/stra_sell 走动作规则(default 组只有 open), 并登记 localid
	OrderIDs buys = ctx._ctx.stra_buy(kAg, 5000.0, 2.0, 0);
	ASSERT_EQ(1u, buys.size());
	OrderIDs sells = ctx._ctx.stra_sell(kAg, 5000.0, 3.0, 0);
	ASSERT_EQ(1u, sells.size());
	EXPECT_EQ(2u, ctx._api._entrusts.size());
	EXPECT_EQ((int)WDT_LONG, ctx._api._entrusts[0]._dir);
	EXPECT_EQ((int)WDT_SHORT, ctx._api._entrusts[1]._dir);

	//查询转发
	EXPECT_DOUBLE_EQ(0.0, ctx._ctx.stra_get_position(kAg, true, 3));
	EXPECT_DOUBLE_EQ(5.0, ctx._ctx.stra_get_undone(kAg));
	EXPECT_EQ(0u, ctx._ctx.stra_get_infos(kAg));
	EXPECT_NE(nullptr, ctx._ctx.stra_get_comminfo(kAg));
	EXPECT_EQ(nullptr, ctx._ctx.stra_get_comminfo("SHFE.unknown"));

	//没有活单时撤单/撤全部都返回空
	EXPECT_FALSE(ctx._ctx.stra_cancel(buys[0]));
	EXPECT_EQ(0u, ctx._ctx.stra_cancel_all("").size());

	//把第一笔委托按活单回报推进通道(cancelAll 需要 ContractInfo, 见 BUG-Q04-008)
	const q04::FakeTraderApi::EntrustSnap& snap = ctx._api._entrusts[0];
	WTSOrderInfo* ord = q04::orderFromSnap(snap, "CTX-1");
	ord->setContractInfo(ctx._bd.getContract("ag2406", "SHFE"));
	WTSArray* ay = WTSArray::create();
	ay->append(ord);
	ctx._adapter.onRspOrders(ay);
	ay->release();
	ord->release();

	//通道里的活单会经 sink 回到 ctx.on_order
	ctx._ctx.on_order(buys[0], kAg, true, 0, 2.0, 2.0, 5000.0, false);
	EXPECT_TRUE(ctx._ctx.stra_cancel(buys[0]));
	ASSERT_EQ(1u, ctx._api._actions.size());
	EXPECT_EQ("CTX-1", ctx._api._actions[0]._code);

	//撤全部: 空代码撤掉所有活单
	OrderIDs all = ctx._ctx.stra_cancel_all("");
	EXPECT_EQ(1u, all.size());
	//带标准代码的过滤在 cancelAll 里按 fullcode 比较, 可以命中
	OrderIDs byCode = ctx._ctx.stra_cancel_all(kAg);
	EXPECT_EQ(1u, byCode.size());

	//通道持仓经 stra_enum_position 回调到本 ctx(on_position 是空实现, 不推进本地块)
	WTSArray* posAy = WTSArray::create();
	WTSPositionItem* pItem = q04::posItem("SHFE", "ag2406", WDT_LONG, 2, 2, 0, 0);
	posAy->append(pItem);
	ctx._adapter.onRspPosition(posAy);
	posAy->release();
	pItem->release();
	EXPECT_DOUBLE_EQ(2.0, ctx._ctx.stra_enum_position(kAg));
	EXPECT_DOUBLE_EQ(2.0, ctx._ctx.stra_get_position(kAg, false, 1));
	ctx._ctx.on_channel_lost();
	ctx.removeOwnFolder();
}

TEST(q04_uftctx_local, trading_day_switch_rebuilds_stale_blocks)
{
	CtxEnv ctx("q04ctx_switch");
	ctx.removeOwnFolder();

	ctx._ctx.on_channel_ready(20240315);
	uint32_t idOpen = ctx._ctx.stra_enter_long(kAg, 5000.0, 4.0, 0);
	ctx._ctx.on_trade(idOpen, kAg, true, 0, 4.0, 5000.0);
	ASSERT_DOUBLE_EQ(4.0, ctx._ctx.stra_get_local_position(kAg));

	//切到新交易日: 明细文件按 _date 判定过期并清理
	ctx._ctx.on_channel_ready(20240318);
	//清理只作用于块内数据, 内存里的净持仓不会被回退(见 BUG-Q04-007 待裁定)
	EXPECT_DOUBLE_EQ(8.0, ctx._ctx.stra_get_local_position(kAg));
	EXPECT_TRUE(std::filesystem::exists(ctx.straFile("position.membin")));
	ctx.removeOwnFolder();
}

//崩溃/阻塞类输入整类收在这里
TEST(DISABLED_q04_uftctx_local, uninitialized_trader_and_shared_param_class)
{
	CtxEnv ctx("q04ctx_disabled");

	//1) _trader 由 setTrader 装配, 未装配时 8 个下单/查询接口直接解引用(BUG-P06-004)
	UftStraContext bare(&ctx._engine, "q04ctx_bare");
	bare.stra_get_position(kAg, true, 3);
	bare.stra_get_undone(kAg);
	bare.stra_get_infos(kAg);
	bare.stra_cancel(1);
	bare.stra_buy(kAg, 5000.0, 1.0, 0);

	//2) 未 on_channel_ready(块指针为 NULL) 时推成交: _pos_blk._block 解引用
	ctx._ctx.on_channel_ready(20240315);
	uint32_t id = ctx._ctx.stra_enter_long(kAg, 5000.0, 1.0, 0);
	ctx._ctx.on_trade(id, "SHFE.unknown", true, 0, 1.0, 5000.0);

	//3) watch_param/read_param/sync_param 需要共享内存段(BUG-P06 同类)
	ctx._ctx.watch_param("k", 1.0);
	ctx._ctx.read_param("k", 1.0);
	ctx._ctx.sync_param("k", 1.0);
	ctx._ctx.commit_param_watcher();
}
