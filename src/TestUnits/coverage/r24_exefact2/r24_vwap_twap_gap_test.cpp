// R24 lane（第二批）：WtVWapExeUnit / WtTWapExeUnit 的残余分支
// 目标文件: src/WtExeFact/WtVWapExeUnit.cpp（本目标基线残余 missed=46/408）
//           src/WtExeFact/WtTWapExeUnit.cpp（残余 missed=46/363）
//
// 普查结论（先 grep 过 n05/r7 的用例头与 manifest R7_exefact.md / R17_exefact.md）：
//  R7 已覆盖 VWAP 的 init 文件解析 / do_calc 早退链 / 分桶量 / 涨跌停 / on_channel_ready 三分支 /
//  on_order 撤单计数 / fire_at_once 的 mode1 与涨停收敛；TWAP 的均分切片 / 卖向符号 / 尾差归整 /
//  价格模式与偏移 / 撤单重发价。本轮只打**残余**：
//   VWAP: L108-109(_sess_info retain) L239-240(首笔非交易时段过滤) L262-271(挂单超时扫描)
//         L179-182(未撤完订单的错误返回) L280-282(on_trade 空体) L301-303(CalcFlag 重入)
//         L416-417(最新价 0 回落 preclose) L450-451(fire_at_once 0 量) L465-468/L478-483/L488-489
//         (fire_at_once 的对手价/跌停/卖向) L501-502(set_position 判等)
//   TWAP: L142-144 L351-352 L394-412 L204-210 L471-472 L514-531 L547-549 L694-701 L732-735
//  两边的 L349-362 / L635-653（清仓兜底）是 BUG-R7-004 的死代码（L319/L319-320 先一步 return），
//  不重复投入，见 manifest 的留白表。
//
// 挂单超时测试通过 fake context clock 跨过正常 1 秒阈值，不 sleep、不依赖挂钟。
//
// 引用计数纪律沿用 R7（见 r7_vwap_unit_test.cpp:7-11）：
//  凡喂进 on_tick 且被单元接手的 tick，用例侧不再 release（单元 dtor 会放）；
//  只有被 L228/L239-240 过滤掉、或代码不匹配的 tick 由用例自己 release。
//  WTSSessionInfo 不放进任何单元的释放路径（四个单元的 dtor 都没放 _sess_info），
//  所以夹具多 retain 一次抵消，收尾再 release 一次，净 0 不踩池。
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "Includes/ExecuteDefs.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "WtExeFact/WtVWapExeUnit.h"
#include "WtExeFact/WtTWapExeUnit.h"

using namespace wtp;

namespace
{
const uint32_t kStickySeconds = 1u;

WTSSessionInfo* mkVSess();

class VFakeCtx : public ExecuteContext
{
public:
	struct Rec { std::string code; double price; double qty; };

	~VFakeCtx()
	{
		if (defaultSess != NULL)
			defaultSess->release();
	}

	WTSTickSlice* getTicks(const char*, uint32_t, uint64_t) override { return NULL; }
	WTSTickData* grabLastTick(const char*) override { return NULL; }
	// I03-NEW-074：与 r7_twap 替身同理，按 flag 返回一致的多/空/净分解（多 >= 0、空 <= 0、净 = 多 + 空）。
	double getPosition(const char*, bool = true, int32_t flag = 3) override
	{
		if (flag == 1)
			return pos > 0 ? pos : 0.0;
		if (flag == 2)
			return pos < 0 ? pos : 0.0;
		return pos;
	}
	OrderMap* getOrders(const char*) override { return NULL; }
	double getUndoneQty(const char*) override { return undone; }

	OrderIDs buy(const char* stdCode, double price, double qty, bool) override
	{
		buys.push_back({ stdCode, price, qty });
		if (onAfterBuy)
			onAfterBuy();
		OrderIDs ids{ nextId++ };
		return ids;
	}
	OrderIDs sell(const char* stdCode, double price, double qty, bool) override
	{
		sells.push_back({ stdCode, price, qty });
		OrderIDs ids{ nextId++ };
		return ids;
	}
	bool cancel(uint32_t localid) override { byidCancels.push_back(localid); return cancelByIdRet; }
	OrderIDs cancel(const char* stdCode, bool isBuy, double = 0) override
	{
		bydirCancels.push_back({ stdCode, isBuy });
		OrderIDs ids;
		for (uint32_t i = 0; i < cancelBydirRetCnt; i++)
			ids.push_back(nextId++);
		return ids;
	}
	void writeLog(const char* log) override { logs.push_back(log ? log : ""); }
	WTSCommodityInfo* getCommodityInfo(const char*) override { return comm; }
	WTSSessionInfo* getSessionInfo(const char*) override
	{
		if (sess != NULL)
			return sess;
		if (defaultSess == NULL)
			defaultSess = mkVSess();
		return defaultSess;
	}
	uint64_t getCurTime() override { return curTime; }

	double pos = 0;
	double undone = 0;
	uint64_t curTime = 20240101093000000ull;
	uint32_t nextId = 9000;
	uint32_t cancelBydirRetCnt = 2;
	bool cancelByIdRet = true;
	WTSCommodityInfo* comm = NULL;
	WTSSessionInfo* sess = NULL;
	WTSSessionInfo* defaultSess = NULL; // context owns the fallback session reference
	std::function<void()> onAfterBuy;              // 重入钩子（CalcFlag 用例）
	std::vector<Rec> buys;
	std::vector<Rec> sells;
	std::vector<uint32_t> byidCancels;
	std::vector<std::pair<std::string, bool>> bydirCancels;
	std::vector<std::string> logs;

	bool hasLog(const std::string& sub) const { return countLog(sub) > 0; }
	uint32_t countLog(const std::string& sub) const
	{
		uint32_t n = 0;
		for (const std::string& l : logs)
			if (l.find(sub) != std::string::npos)
				n++;
		return n;
	}
};

inline std::string commName(const char* prefix)
{
	return std::string(prefix) + std::to_string((long long)getpid());
}

// WtVWapExeUnit.cpp L125-127：文件名 = "Vwap_" + comm->getName() + ".txt"（相对进程 CWD）
class VwapFile
{
public:
	explicit VwapFile(const std::string& commName, double bucketVal, uint32_t rows = 241)
	{
		_path = std::string("Vwap_") + commName + ".txt";
		std::ofstream f(_path.c_str());
		for (uint32_t i = 0; i < rows; i++)
		{
			if (i)
				f << ",";
			f << bucketVal;
		}
		f.close();
	}
	~VwapFile() { std::remove(_path.c_str()); }

private:
	std::string _path;
};

WTSCommodityInfo* mkVComm(const std::string& name)
{
	WTSCommodityInfo* c = WTSCommodityInfo::create("tv", name.c_str(), "CFFEX", "R24DAY", "CHN");
	c->setVolScale(1);
	c->setPriceTick(0.01);
	c->setLotsTick(1.0);
	c->setMinLots(1.0);
	c->setCategory(CC_Future);
	c->setCoverMode(CM_OpenCover);
	c->setPriceMode(PM_Both);
	c->setTradingMode(TM_Both);
	return c;
}

// 日盘两段：930-1130 / 1300-1500，offset 0
// isInTradingTime(900) -> timeToMinutes 落不到任何 section -> INVALID_UINT32 -> false
WTSSessionInfo* mkVSess()
{
	WTSSessionInfo* s = WTSSessionInfo::create("R24DAY", "R24日盘", 0);
	s->addTradingSection(930, 1130);
	s->addTradingSection(1300, 1500);
	return s;
}

WTSTickData* mkVTick(const char* code, uint32_t actTime, double lastPx,
	double bidPx, double askPx, double upper = 0, double lower = 0)
{
	WTSTickStruct ts;
	snprintf(ts.code, sizeof(ts.code), "%s", code);
	ts.action_date = 20240101;
	ts.trading_date = 20240101;
	ts.action_time = actTime;
	ts.price = lastPx;
	ts.pre_close = 10.0;
	ts.bid_prices[0] = bidPx;
	ts.bid_qty[0] = 100;
	ts.ask_prices[0] = askPx;
	ts.ask_qty[0] = 80;
	ts.upper_limit = upper;
	ts.lower_limit = lower;
	return WTSTickData::create(ts);
}

// L110-119 实际读取的键；minOpenLots 传 NAN 表示「不写这个键」（此时 _min_open_lots 未被初始化，
// 见 BUG-R24-002，本轮所有用例都显式给值，不依赖未初始化读）
WTSVariant* mkVCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 0, uint32_t beginTime = 930, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("price_offset", priceOffset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	return cfg;
}

const char* const kVCode = "tv2403";
const char* const kTCode = "tw2403";

// WtTWapExeUnit 的 init（L150-167）：_total_secs 先按 cfg 读一次，L163 又被 calTmSecs(930,1500)=19800
// 覆盖，L167 才用 (_total_secs - _tail_secs)/_total_times 算 _fire_span。
// 默认 tailSecs=19800 让 _fire_span=0，于是 on_tick 非首笔分支的间隔闸门（L415）恒开；
// 需要「闸门挡住第二轮」的差分只要改这个参数即可（和 VWAP 的 L120 顺序缺陷不同，这里是先算后覆）。
WTSVariant* mkTCfg(uint32_t totalTimes = 5, uint32_t priceMode = 0, uint32_t priceOffset = 0,
	double minOpenLots = 1.0, double lots = 10.0, uint32_t ordSticky = 0,
	uint32_t tailSecs = 19800, uint32_t beginTime = 930, uint32_t endTime = 1500)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("begin_time", beginTime);
	cfg->append("end_time", endTime);
	cfg->append("ord_sticky", ordSticky);
	cfg->append("tail_secs", tailSecs);
	cfg->append("total_times", totalTimes);
	cfg->append("price_mode", priceMode);
	cfg->append("price_offset", priceOffset);
	cfg->append("lots", lots);
	cfg->append("minopenlots", minOpenLots);
	cfg->append("total_secs", 19800);
	return cfg;
}

void bootV(WtVWapExeUnit& unit, VFakeCtx& ctx, double target, WTSVariant* cfg)
{
	unit.init(&ctx, kVCode, cfg);
	cfg->release();
	unit.set_position(kVCode, target);
	unit.on_channel_ready();
}

void bootT(WtTWapExeUnit& unit, VFakeCtx& ctx, double target, WTSVariant* cfg)
{
	unit.init(&ctx, kTCode, cfg);
	cfg->release();
	unit.set_position(kTCode, target);
	unit.on_channel_ready();
}
}

//////////////////////////////////////////////////////////////////////////
// VWAP：_sess_info 非空时 init 会 retain（L108-109），首笔 tick 不在交易时段则整笔丢弃（L239-240）
// 可证伪差分：9:00 的 tick 之后不得有单；同一目标在 10:00 的 tick 必须下 25 手
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, session_info_retained_and_pre_session_first_tick_dropped)
{
	const std::string name = commName("R24VSESS");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	ctx.sess = mkVSess();
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg());

	WTSTickData* early = mkVTick(kVCode, 90000000, 10.50, 10.49, 10.51);
	unit.on_tick(early);                                 // L239-240：900 不在 930-1130
	EXPECT_TRUE(ctx.buys.empty());
	early->release();                                    // 被过滤的 tick 没被单元接手

	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51));   // 1000 在时段内
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.comm->release();
	ctx.sess->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：price_mode=0 且最新价为 0 -> L416-417 回落
// 断言委托价 = preclose(10.0)，而不是 0：这条回落只存在于 do_calc，fire_at_once 没有（BUG-R7-014）
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, calc_zero_last_price_falls_back_to_preclose)
{
	const std::string name = commName("R24VZERO");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg(5, 0));
	unit.on_tick(mkVTick(kVCode, 100000000, 0.0, 0.0, 0.0));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.0);           // L417 的 preclose 分支
	EXPECT_NE(ctx.buys[0].price, 0.0);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：没有版本化订单身份时，超时只记录日志、禁用后续自动扫描并保留监控单。
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, expired_order_without_verified_identity_fails_closed)
{
	const std::string name = commName("R24VSTICKY");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg(5, 0, 0, 1.0, 10.0, kStickySeconds));
	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // 本地单 9000 进 OMS
	ctx.undone = ctx.buys[0].qty;                        // fake 回填活单未完成量，避免模拟出重复发单
	EXPECT_TRUE(ctx.byidCancels.empty());

	ctx.curTime += 999;
	unit.on_tick(mkVTick(kVCode, 100100000, 10.60, 10.59, 10.61));   // 非首笔 -> L258 else
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 0u);
	ctx.curTime += 1;
	unit.on_tick(mkVTick(kVCode, 100200000, 10.70, 10.69, 10.71));
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);                      // 仅等待订单状态，不因身份未验证而重发

	ctx.curTime += 1000;
	unit.on_tick(mkVTick(kVCode, 100300000, 10.80, 10.79, 10.81));
	EXPECT_TRUE(ctx.byidCancels.empty());                // 超时项仍受监控，但自动扫描已禁用
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：撤单指令还没走完就收到「未撤销」的订单回报 -> 记错误并返回（L179-182）
// 前置用 on_channel_ready 的外部挂单全撤（L189-204）抬起 _cancel_cnt=2
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, on_order_with_pending_cancel_cnt_returns_error)
{
	const std::string name = commName("R24VERR");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	ctx.undone = 5.0;
	ctx.cancelBydirRetCnt = 2;
	WtVWapExeUnit unit;
	unit.init(&ctx, kVCode, mkVCfg());
	unit.set_position(kVCode, 100);
	unit.on_channel_ready();                             // 撤 9001/9002，_cancel_cnt=2
	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51)); // 通道撤单在首个有效行情到达时协调
	ASSERT_EQ(ctx.bydirCancels.size(), 1u);
	EXPECT_TRUE(ctx.hasLog("cancelcnt -> 2"));

	unit.on_order(9001, kVCode, true, 5, 10.50, false);  // 未撤完 + 非撤单回报 -> L179
	EXPECT_EQ(ctx.countLog("hasn't canceled, error will be return"), 1u);
	EXPECT_TRUE(ctx.buys.empty());                       // L181 直接 return，不会走到发单
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：on_trade 是空实现（L280-282）；set_position 目标相等时早退（L501-502），
// 且不重置 _fired_times —— 用「第几次发单」的日志序号做可证伪差分
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, on_trade_noop_and_equal_target_set_position_keeps_fire_count)
{
	const std::string name = commName("R24VNOOP");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg(5, 0, 0, 1.0, 10.0, 0, 14400, 930, 1500)); // span=0，测试连续发单计数
	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);

	unit.on_trade(9000, kVCode, true, 5, 10.50);         // L280-282 空体
	unit.on_trade(9000, kVCode, true, 5, 10.50);
	EXPECT_EQ(ctx.buys.size(), 1u);                      // 不因为成交回报加仓
	EXPECT_EQ(ctx.sells.size(), 0u);

	unit.set_position(kVCode, 100);                      // 与 _target_pos 相等 -> L501-502
	unit.on_tick(mkVTick(kVCode, 100100000, 10.60, 10.59, 10.61));
	ASSERT_EQ(ctx.buys.size(), 2u);
	// 若 set_position 没有早退而是重跑 L504-508，_fired_times 会被打回 0 -> 日志会是「第 1 次」
	EXPECT_EQ(ctx.countLog("第 2 次发单"), 1u);
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 1u);

	// 换目标则必须重置计数（L504-508）
	unit.set_position(kVCode, 200);
	unit.on_tick(mkVTick(kVCode, 100200000, 10.70, 10.69, 10.71));
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_EQ(ctx.countLog("第 1 次发单"), 2u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：do_calc 重入被 CalcFlag 挡在锁外（L301-303）
// 在 buy() 回调内喂入有效的后续 tick，真实触发嵌套 on_tick -> do_calc；外层仍只发一单。
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, calc_flag_blocks_reentrant_do_calc)
{
	const std::string name = commName("R24VRE");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	ctx.sess = mkVSess();
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg());
	EXPECT_TRUE(unit.initializationSucceeded());
	int hits = 0;
	uint32_t nestedTicks = 0;
	ctx.onAfterBuy = [&]() {
		if (++hits == 1)
		{
			WTSTickData* nested = mkVTick(kVCode, 104800000, 10.55, 10.54, 10.56);
			++nestedTicks;
			unit.on_tick(nested);                        // 10:48 在 R24DAY session 内，触发里层 do_calc
			nested->release();                           // 单元保留缓存引用
		}
	};
	WTSTickData* first = mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51);
	unit.on_tick(first);
	first->release();
	EXPECT_EQ(nestedTicks, 1u);
	EXPECT_EQ(hits, 1);
	// 后续 tick 已通过日期、session 和执行窗口校验；嵌套 do_calc 不得重复发单。
	EXPECT_FALSE(ctx.hasLog("VWAP tick action time 104800000 rejected"));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.onAfterBuy = nullptr;
	ctx.comm->release();
	ctx.sess->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：撤单后在新鲜行情上重算切片；可执行量为 0 时不重复发单。
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, canceled_retry_with_zero_vwap_slice_does_not_send_order)
{
	const std::string name = commName("R24VQ0");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg(5, 0, 0, 0.0));         // minopenlots = 0
	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // curQty = max(25,0)*1 = 25
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);

	ctx.pos = 40;                                        // 撤单后按实际持仓重算，本轮可执行量为 0
	unit.on_order(9000, kVCode, true, 5, 10.50, true);   // 撤单重试延迟到新鲜时段内行情
	EXPECT_TRUE(ctx.hasLog("re_fire deferred until a fresh in-session tick"));
	EXPECT_EQ(ctx.buys.size(), 1u);                      // 收到新行情前不重发
	unit.on_tick(mkVTick(kVCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.buys.size(), 1u);                      // 切片量为0时仅保留原始订单记录，不产生重发
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// VWAP：fire_at_once 的卖出侧（L465-468 对手价取 bid、L478-483 跌停收敛、L488-489 sell）
// minopenlots=-5（产品不校验负值）使 max(-5, 25-40) = -5 -> 负量 -> 走卖
//////////////////////////////////////////////////////////////////////////
TEST(r24_vwap, fire_at_once_sell_uses_counter_price_and_lower_limit)
{
	const std::string name = commName("R24VSELL");
	VwapFile vf(name, 25.0);
	VFakeCtx ctx;
	ctx.comm = mkVComm(name);
	WtVWapExeUnit unit;
	bootV(unit, ctx, 100, mkVCfg(5, 2, 0, 1.0));         // 合法的非负 minopenlots
	ASSERT_TRUE(unit.initializationSucceeded());
	unit.on_tick(mkVTick(kVCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                      // 买向：mode2 -> ask 10.51
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.51);

	ctx.pos = 140;                                       // 实仓超过目标，撤单后的新差值为负
	unit.on_order(9000, kVCode, true, 5, 10.51, true);
	EXPECT_TRUE(ctx.hasLog("re_fire deferred until a fresh in-session tick"));
	EXPECT_TRUE(ctx.sells.empty());                    // 撤单重试需等待新鲜时段内行情
	unit.on_tick(mkVTick(kVCode, 100100000, 10.60, 10.49, 10.61, 0, 10.50));
	// 合法 retryQty 为负：isBuy=false -> bid 10.49；撤单加一跳后低于跌停，收敛到 10.50。
	ASSERT_EQ(ctx.sells.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.sells[0].qty, 1.0);
	EXPECT_DOUBLE_EQ(ctx.sells[0].price, 10.50);
	EXPECT_TRUE(ctx.hasLog("modified to lower limit price"));
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：_sess_info 非空时 init retain（L142-144），首笔非交易时段 tick 被丢（L351-352）
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, session_retained_and_first_tick_outside_section_dropped)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TSESS"));
	ctx.sess = mkVSess();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));

	WTSTickData* early = mkVTick(kTCode, 90000000, 10.50, 10.49, 10.51);
	unit.on_tick(early);
	EXPECT_TRUE(ctx.buys.empty());
	early->release();

	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	ctx.undone = ctx.buys[0].qty;                         // fake 回填活单未完成量，避免模拟出重复发单
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);             // max(1, round(100/4))
	ctx.comm->release();
	ctx.sess->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：全额成交回报复位 _cancel_times（L204-210）
// 差分口径：撤单重发的加点是 L508 的 priceTick*_cancel_times，而 _cancel_times 在 L226
// 先自增再重发 —— 所以「复位过」的第一次重发只加 1 跳（10.60 -> 10.61），
// 若 L207 的复位没生效（计数攒到 2）就会加 2 跳（10.62）。
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, full_fill_report_resets_cancel_times_before_refire)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TFILL"));
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);                       // 9000 @10.50, _this_target=25

	// 第一次撤单重发：_cancel_times 0 -> 1，加点 1 跳
	unit.on_order(9000, kTCode, true, 5, 10.50, true);
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.51);
	EXPECT_TRUE(ctx.hasLog("re_fire will be done"));       // L223

	// 全额成交回报 -> L204-210 复位 _cancel_times
	unit.on_order(9001, kTCode, true, 0, 10.51, false);
	EXPECT_EQ(ctx.countLog("has filled"), 1u);             // L209
	unit.on_order(9001, kTCode, true, 0, 10.51, false);    // 已被 L193 摘除 -> L186-187
	EXPECT_EQ(ctx.countLog("has filled"), 1u);

	unit.on_tick(mkVTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	// leftTimes = 4-1 = 3 -> curQty = max(1, round(100/3)) = 33
	ASSERT_EQ(ctx.buys.size(), 3u);
	EXPECT_DOUBLE_EQ(ctx.buys[2].qty, 33.0);
	EXPECT_DOUBLE_EQ(ctx.buys[2].price, 10.60);            // 非重发：不带加点

	unit.on_order(9002, kTCode, true, 3, 10.60, true);     // 撤单回报 -> 再重发
	ASSERT_EQ(ctx.buys.size(), 4u);
	// 复位生效 -> _cancel_times 只有 1；不生效则是 10.62
	EXPECT_DOUBLE_EQ(ctx.buys[3].price, 10.61);
	EXPECT_NE(ctx.buys[3].price, 10.62);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：无身份令牌的超时单留在监控中，不再通过裸 localid 自动撤单。
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, expired_order_without_verified_identity_fails_closed)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TSTICKY"));
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0, 0, 1.0, 10.0, kStickySeconds));
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	ctx.undone = ctx.buys[0].qty;                         // fake 回填活单，避免下一 tick 重复发单

	ctx.curTime += 999;
	unit.on_tick(mkVTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 0u);
	ctx.curTime += 1;
	unit.on_tick(mkVTick(kTCode, 100200000, 10.70, 10.69, 10.71));
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);                       // 等待订单状态，不能用裸 localid 超时撤单
	ctx.curTime += 1000;
	unit.on_tick(mkVTick(kTCode, 100300000, 10.80, 10.79, 10.81));
	EXPECT_TRUE(ctx.byidCancels.empty());
	EXPECT_EQ(ctx.countLog("Expired order 9000 of"), 1u);
	EXPECT_EQ(ctx.buys.size(), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：尾桶打板（L694-701 量取 diffQty、L732-735 价 +5 跳）
// 关键：_fired_times 不能被 set_position 打回（那是 R7 同名用例失败的原因）
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, last_slice_showhand_adds_five_ticks)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TSHOW"));
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(1));                    // total_times=1
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	// leftTimes = 1-0 = 1 -> max(1, round(100/1)) = 100，非打板
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);

	unit.on_tick(mkVTick(kTCode, 100100000, 10.60, 10.59, 10.61));
	// leftTimes = 1-1 = 0 且 diffQty=100!=0 -> curQty=max(1,100)=100，价 10.60+0.01*5
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].qty, 100.0);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.65);
	EXPECT_EQ(ctx.countLog("第 2 次发单"), 1u);
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：fire_at_once 的涨停收敛（L514-519）
// 撤单回报后 _cancel_times=1，10.50+0.01 超过涨停 10.505 -> 收敛为 10.505
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, fire_at_once_caps_buy_at_upper_limit)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TCAP"));
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4, 0, 0, 1.0));
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51, 10.505, 0));
	// do_calc 未加偏移（price_offset=0），10.50 < 涨停 10.505 -> 不收敛
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].price, 10.50);

	unit.on_order(9000, kTCode, true, 5, 10.50, true);   // 重发：10.50 + 0.01*1 = 10.51 > 10.505
	ASSERT_EQ(ctx.buys.size(), 2u);
	EXPECT_DOUBLE_EQ(ctx.buys[1].price, 10.505);          // L516-518
	EXPECT_EQ(ctx.countLog("modified to upper limit price"), 1u);

	// 实仓涨过本轮目标 -> max(minopenlots=1, _this_target-realPos) 仍为 1，量不为 0，
	// 所以把 minopenlots 设成 0 的那条 0 量路径单独覆盖（下一条用例）
	ctx.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：fire_at_once 的 0 量早退（L471-472）与卖出侧（L520-531）
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, fire_at_once_zero_qty_and_sell_side_floor)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TSELL"));
	WtTWapExeUnit unit;
	// 第一轮：minopenlots=0，25 手目标 -> _this_target=25；实仓抬到 40 -> 重发量 = max(0,-15)=0
	bootT(unit, ctx, 100, mkTCfg(4, 0, 0, 0.0));
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	ctx.pos = 40;
	unit.on_order(9000, kTCode, true, 5, 10.50, true);
	// R7-012（有界重发）：实仓已超过本轮目标时买单撤单不重发（保持方向、量为 0），不再记录
	// “re_fire will be done”（改前期望记录该日志后在 fire_at_once 内 0 量早退）。
	EXPECT_FALSE(ctx.hasLog("re_fire will be done"));
	EXPECT_TRUE(ctx.buys.size() == 1u);
	EXPECT_TRUE(ctx.sells.empty());
	ctx.comm->release();

	// 第二轮：minopenlots=-8 -> max(-8, 25-40) = -8 -> 卖 8 手，跌停 10.49 收敛
	const std::string name2 = commName("R24TSELL2");
	VFakeCtx ctx2;
	ctx2.comm = mkVComm(name2);
	WtTWapExeUnit unit2;
	bootT(unit2, ctx2, 100, mkTCfg(4, 2, 0, -8.0));
	unit2.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51, 0, 10.49));
	// R7-012：负的 minopenlots 不是合法的最小开仓量，有界数量计算返回 0，首轮也不下单
	// （改前期望：首轮按 ask 买入 25 手）。
	EXPECT_TRUE(ctx2.buys.empty());
	ctx2.pos = 40;
	unit2.on_order(9000, kTCode, true, 5, 10.51, true);
	// R7-012：撤单重发不得反向（买单撤单不能变成卖单），负的 minopenlots 也不能把重发量变成反向数量
	// （改前期望：fire_at_once(-8) 反向卖 8 手并按跌停价收敛）。
	EXPECT_TRUE(ctx2.sells.empty());
	EXPECT_EQ(ctx2.countLog("modified to lower limit price"), 0u);
	EXPECT_TRUE(ctx2.buys.empty());
	ctx2.comm->release();
}

//////////////////////////////////////////////////////////////////////////
// TWAP：do_calc 重入被 CalcFlag 挡在 L548-549（buy 已经在 flag 之内，钩子挂 buy）
//////////////////////////////////////////////////////////////////////////
TEST(r24_twap, calc_flag_blocks_reentrant_do_calc)
{
	VFakeCtx ctx;
	ctx.comm = mkVComm(commName("R24TRE"));
	ctx.sess = mkVSess();
	WtTWapExeUnit unit;
	bootT(unit, ctx, 100, mkTCfg(4));
	EXPECT_TRUE(unit.initializationSucceeded());
	int hits = 0;
	uint32_t readyLogsBeforeNested = 0;
	ctx.onAfterBuy = [&]() {
		if (++hits == 1)
		{
			readyLogsBeforeNested = ctx.countLog("Unrecognized condition while channle ready");
			unit.on_channel_ready();
		}
	};
	unit.on_tick(mkVTick(kTCode, 100000000, 10.50, 10.49, 10.51));
	EXPECT_EQ(hits, 1);
	// 里层 on_channel_ready：undone==0 且 OMS 里还没有单（buy 还没回 id）-> 只多一条 L303 日志
	ASSERT_EQ(ctx.buys.size(), 1u);
	EXPECT_DOUBLE_EQ(ctx.buys[0].qty, 25.0);
	EXPECT_EQ(ctx.countLog("Unrecognized condition while channle ready"), readyLogsBeforeNested + 1);
	ctx.onAfterBuy = nullptr;
	ctx.comm->release();
	ctx.sess->release();
}
