/**
 * n03_idxfactory_worker_test.cpp
 *
 * 覆盖 src/WtIndexFactory:
 *   WtIndexFactory.cpp : WtIdxFactory::get_name/create_worker/delete_worker
 *                        + extern "C" create_index_factory/delete_index_factory
 *   WtBlkIdxWorker.cpp : get_fact_name / init / handle_quote / generate_tick
 *
 * 全部为纯计算路径: 用假的 IIndexContext / IBaseDataMgr / IHotMgr 注入,
 * timeout 固定为 0(不启动 _thrd_trigger 线程), trigger 用 "time" 或 fullcode,
 * 断言只做指数算式的确定性校验与 action_time 的单调性, 不断言墙钟时间/时区.
 *
 * 跳过项(需要真实共享内存/行情回放, 见 manifest):
 *   - handle_quote 里 _timeout>0 的 _thrd_trigger 线程分支(会创建常驻线程并 wait)
 */
#include <gtest/gtest.h>

#include "WtIndexFactory/WtIndexFactory.h"
#include "WtIndexFactory/WtBlkIdxWorker.h"

#include "Includes/IIndexDefs.h"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"

#include <cstring>
#include <string>
#include <vector>

USING_NS_WTP;

// 插件入口是 extern "C"（src/WtIndexFactory/WtIndexFactory.cpp L9-L21），必须在文件作用域声明：
// C++ 不允许在函数体内写 extern "C"，上一版把它们放进 TEST 里，直接编译失败。
// delete 的实参是按值传指针，不是引用，所以调用后调用方变量不会被置空。
extern "C" IIndexWorkerFact* create_index_factory();
extern "C" void delete_index_factory(IIndexWorkerFact* fact);

namespace {

//////////////////////////////////////////////////////////////////////////
// 假的基础数据管理器: 只维护 code.exchg -> WTSContractInfo
class N03BaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		auto it = _comms.find(exchgpid);
		return it == _comms.end() ? nullptr : it->second;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		return getCommodity(fmt::format("{}.{}", exchg, pid).c_str());
	}

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		auto it = _contracts.find(fmt::format("{}.{}", exchg, code));
		return it == _contracts.end() ? nullptr : it->second;
	}

	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return nullptr; }
	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

	void addContract(WTSContractInfo* cInfo) { _contracts[cInfo->getFullCode()] = cInfo; }
	void addCommodity(const std::string& key, WTSCommodityInfo* comm) { _comms[key] = comm; }

private:
	wt_hashmap<std::string, WTSContractInfo*> _contracts;
	wt_hashmap<std::string, WTSCommodityInfo*> _comms;
};

//////////////////////////////////////////////////////////////////////////
// 假的热点合约管理器: 不识别任何 ruletag, 使 extractStdCode 走普通合约分支
class N03HotMgr : public IHotMgr
{
public:
	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t = 0) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t = 0) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t = 0) override { return 1.0; }
};

//////////////////////////////////////////////////////////////////////////
// 假的指数上下文: 记录订阅请求与 push 出来的指数快照
class N03IdxContext : public IIndexContext
{
public:
	N03IdxContext(N03BaseDataMgr* bdMgr, N03HotMgr* hotMgr) : _bdMgr(bdMgr), _hotMgr(hotMgr) {}

	IHotMgr* get_hot_mgr() override { return _hotMgr; }
	IBaseDataMgr* get_bd_mgr() override { return _bdMgr; }

	// 成分合约的基线快照: 未登记的返回 NULL, 走 "无基线数据" 分支
	WTSTickData* sub_ticks(const char* fullCode) override
	{
		_subscribes.emplace_back(fullCode);
		auto it = _baselines.find(fullCode);
		if (it == _baselines.end())
			return nullptr;

		WTSTickData* tick = WTSTickData::create(it->second);
		return tick;
	}

	WTSTickData* get_tick(const char*, const char*) override { return nullptr; }

	void push_tick(WTSTickData* newTick) override
	{
		// newTick 马上会被 release 归还对象池, 这里必须按值拷贝
		_pushed.emplace_back(newTick->getTickStruct());
	}

	void output_log(WTSLogLevel, const char* message) override { _logs.emplace_back(message); }

	std::vector<std::string> _subscribes;
	std::vector<WTSTickStruct> _pushed;
	std::vector<std::string> _logs;
	wt_hashmap<std::string, WTSTickStruct> _baselines;

private:
	N03BaseDataMgr* _bdMgr;
	N03HotMgr* _hotMgr;
};

//////////////////////////////////////////////////////////////////////////
WTSTickStruct makeTick(const char* exchg, const char* code, double price,
	uint32_t vol = 0, double turnover = 0.0, double hold = 0.0,
	uint32_t actDate = 20240301, uint32_t actTime = 93000000)
{
	WTSTickStruct t;
	memset(&t, 0, sizeof(WTSTickStruct));
	wt_strcpy(t.exchg, exchg);
	wt_strcpy(t.code, code);
	t.trading_date = actDate;
	t.action_date = actDate;
	t.action_time = actTime;
	t.price = price;
	t.open = price;
	t.high = price;
	t.low = price;
	t.total_volume = vol;
	t.total_turnover = turnover;
	t.open_interest = hold;
	return t;
}

// 每个用例独立的运行环境
class N03BlkIdxEnv
{
public:
	N03BlkIdxEnv() : ctx(&bdMgr, &hotMgr)
	{
		fact = new WtIdxFactory();
	}

	~N03BlkIdxEnv()
	{
		if (worker)
			fact->delete_worker(worker);
		delete fact;
	}

	IIndexWorker* create(const char* name, const char* id)
	{
		worker = fact->create_worker(name, id);
		if (worker)
			worker->set_factory(&ctx);
		return worker;
	}

	WTSContractInfo* regContract(const char* exchg, const char* code)
	{
		WTSContractInfo* cInfo = WTSContractInfo::create(code, code, exchg, code);
		bdMgr.addContract(cInfo);
		_owned.push_back(cInfo);
		return cInfo;
	}

	N03BaseDataMgr	bdMgr;
	N03HotMgr		hotMgr;
	N03IdxContext	ctx;
	IIndexWorkerFact* fact;
	IIndexWorker*	worker = nullptr;
	std::vector<WTSContractInfo*> _owned;
};

WTSVariant* baseConfig(N03BlkIdxEnv& env, uint32_t weightAlg, const char* trigger)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("exchg", "SSE");
	cfg->append("code", "BLK001");
	cfg->append("trigger", trigger);
	cfg->append("timeout", (uint32_t)0);
	cfg->append("weight_alg", weightAlg);

	WTSVariant* codes = WTSVariant::createArray();
	codes->append("SSE.600000");
	codes->append("SSE.600519");
	cfg->append("codes", codes);
	env.ctx._baselines["SSE.600000"] = makeTick("SSE", "600000", 10.0, 100, 1000.0, 500.0);
	env.ctx._baselines["SSE.600519"] = makeTick("SSE", "600519", 20.0, 200, 2000.0, 600.0);
	codes->release();
	return cfg;
}

void feedTick(N03BlkIdxEnv& env, WTSContractInfo* cInfo, WTSTickStruct& t)
{
	WTSTickData* tick = WTSTickData::create(t);
	tick->setContractInfo(cInfo);
	env.worker->handle_quote(tick);
	tick->release();
}

} // namespace

//////////////////////////////////////////////////////////////////////////
TEST(n03_idxfactory, factory_name_and_worker_lookup)
{
	WtIdxFactory fact;
	EXPECT_STREQ("WtIndexFactory", fact.get_name());

	IIndexWorker* w = fact.create_worker("WtBuiltinIndex", "blk01");
	ASSERT_NE(nullptr, w);
	EXPECT_STREQ("WtIndexFactory", w->get_fact_name());
	EXPECT_TRUE(fact.delete_worker(w));

	// 未注册的名字返回 NULL
	EXPECT_EQ(nullptr, fact.create_worker("NoSuchIndex", "blk02"));
	// delete NULL 视为成功
	EXPECT_TRUE(fact.delete_worker(nullptr));
}

TEST(n03_idxfactory, extern_c_entry_points)
{
	IIndexWorkerFact* fact = create_index_factory();
	ASSERT_NE(nullptr, fact);
	EXPECT_STREQ("WtIndexFactory", fact->get_name());

	IIndexWorker* w = fact->create_worker("WtBuiltinIndex", "blk03");
	ASSERT_NE(nullptr, w);
	EXPECT_TRUE(fact->delete_worker(w));
	delete_index_factory(fact);
	// 按值传指针的 C 入口不会把调用方的变量置空（src 内不存在任何按引用删除的插件入口，
	// 已用两种正则各扫一遍确认），所以这里只断言「删除不崩溃」，不断言 fact 变 nullptr。
	(void)fact;

	// 传入 NULL 不应崩溃
	IIndexWorkerFact* nullFact = nullptr;
	delete_index_factory(nullFact);
}

TEST(n03_idxfactory, delete_worker_rejects_foreign_fact)
{
	WtIdxFactory fact;

	// 一个不属于本工厂的 worker
	struct ForeignWorker : public IIndexWorker
	{
		ForeignWorker() : IIndexWorker("foreign") {}
		const char* get_fact_name() override { return "OtherFactory"; }
		bool init(WTSVariant*) override { return true; }
		void handle_quote(WTSTickData*) override {}
	};

	ForeignWorker* fw = new ForeignWorker();
	EXPECT_FALSE(fact.delete_worker(fw));
	// 返回 false 表示未删除, 由调用方负责释放
	delete fw;
}

//////////////////////////////////////////////////////////////////////////
TEST(n03_blkidx, init_rejects_null_config)
{
	N03BlkIdxEnv env;
	IIndexWorker* w = env.create("WtBuiltinIndex", "blk04");
	ASSERT_NE(nullptr, w);
	EXPECT_FALSE(w->init(nullptr));
}

TEST(n03_blkidx, fixed_weight_index_and_high_low)
{
	N03BlkIdxEnv env;
	WTSContractInfo* c1 = env.regContract("SSE", "600000");
	[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
	ASSERT_NE(nullptr, c1);
	ASSERT_NE(nullptr, c2);

	IIndexWorker* w = env.create("WtBuiltinIndex", "blk05");
	ASSERT_NE(nullptr, w);
	WTSVariant* cfg = baseConfig(env, 0, "time");
	ASSERT_TRUE(w->init(cfg));

	// 两个成分都订阅过
	EXPECT_EQ(2u, env.ctx._subscribes.size());

	// 基线已有数据, 第一笔行情进来即可出指数: 固定权重 total_base=1
	// index = (10*1 + 20*1) / 1 / 2 = 15
	WTSTickStruct t1 = makeTick("SSE", "600000", 10.0, 100, 1000.0, 500.0);
	feedTick(env, c1, t1);
	ASSERT_EQ(1u, env.ctx._pushed.size());
	EXPECT_DOUBLE_EQ(15.0, env.ctx._pushed[0].price);
	EXPECT_DOUBLE_EQ(15.0, env.ctx._pushed[0].open);
	EXPECT_DOUBLE_EQ(15.0, env.ctx._pushed[0].high);
	EXPECT_DOUBLE_EQ(15.0, env.ctx._pushed[0].low);
	EXPECT_STREQ("SSE", env.ctx._pushed[0].exchg);
	EXPECT_STREQ("BLK001", env.ctx._pushed[0].code);
	EXPECT_EQ(20240301u, env.ctx._pushed[0].trading_date);
	EXPECT_DOUBLE_EQ(300.0, env.ctx._pushed[0].total_volume);
	EXPECT_DOUBLE_EQ(1100.0, env.ctx._pushed[0].open_interest);
	EXPECT_DOUBLE_EQ(3000.0, env.ctx._pushed[0].total_turnover);

	// 下跌修正 low, high 保持
	WTSTickStruct t2 = makeTick("SSE", "600000", 5.0, 100, 1000.0, 500.0);
	feedTick(env, c1, t2);
	ASSERT_EQ(2u, env.ctx._pushed.size());
	EXPECT_DOUBLE_EQ(12.5, env.ctx._pushed[1].price);
	EXPECT_DOUBLE_EQ(15.0, env.ctx._pushed[1].high);
	EXPECT_DOUBLE_EQ(12.5, env.ctx._pushed[1].low);

	// 上涨修正 high
	WTSTickStruct t3 = makeTick("SSE", "600000", 30.0, 100, 1000.0, 500.0);
	feedTick(env, c1, t3);
	ASSERT_EQ(3u, env.ctx._pushed.size());
	EXPECT_DOUBLE_EQ(25.0, env.ctx._pushed[2].price);
	EXPECT_DOUBLE_EQ(25.0, env.ctx._pushed[2].high);
	EXPECT_DOUBLE_EQ(12.5, env.ctx._pushed[2].low);
	// 不断言绝对时间, 只做单调性校验
	EXPECT_GE(env.ctx._pushed[2].action_time, env.ctx._pushed[0].action_time);

	cfg->release();
}

TEST(n03_blkidx, dynamic_interest_and_dynamic_volume_algs)
{
	// 动态总持: base = ΣOI, value = Σ(OI*price*w)
	{
		N03BlkIdxEnv env;
		WTSContractInfo* c1 = env.regContract("SSE", "600000");
		[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
		IIndexWorker* w = env.create("WtBuiltinIndex", "blk06");
		WTSVariant* cfg = baseConfig(env, 1, "time");
		ASSERT_TRUE(w->init(cfg));

		WTSTickStruct t1 = makeTick("SSE", "600000", 10.0, 100, 1000.0, 500.0);
		feedTick(env, c1, t1);
		ASSERT_EQ(1u, env.ctx._pushed.size());
		// (500*10 + 600*20) / 1100 / 2 = 17000/1100/2
		EXPECT_DOUBLE_EQ(17000.0 / 1100.0 / 2.0, env.ctx._pushed[0].price);
		cfg->release();
	}

	// 动态成交量: base = Σvol, value = Σ(vol*price*w)
	{
		N03BlkIdxEnv env;
		WTSContractInfo* c1 = env.regContract("SSE", "600000");
		[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
		IIndexWorker* w = env.create("WtBuiltinIndex", "blk07");
		WTSVariant* cfg = baseConfig(env, 2, "time");
		ASSERT_TRUE(w->init(cfg));

		WTSTickStruct t1 = makeTick("SSE", "600000", 10.0, 100, 1000.0, 500.0);
		feedTick(env, c1, t1);
		ASSERT_EQ(1u, env.ctx._pushed.size());
		// (100*10 + 200*20) / 300 / 2
		EXPECT_DOUBLE_EQ(5000.0 / 300.0 / 2.0, env.ctx._pushed[0].price);
		cfg->release();
	}
}

TEST(n03_blkidx, stand_scale_scales_index)
{
	N03BlkIdxEnv env;
	WTSContractInfo* c1 = env.regContract("SSE", "600000");
	[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
	IIndexWorker* w = env.create("WtBuiltinIndex", "blk08");

	WTSVariant* cfg = baseConfig(env, 0, "time");
	cfg->append("stand_scale", 0.5);
	ASSERT_TRUE(w->init(cfg));

	WTSTickStruct t1 = makeTick("SSE", "600000", 10.0);
	feedTick(env, c1, t1);
	ASSERT_EQ(1u, env.ctx._pushed.size());
	// 15 * 0.5
	EXPECT_DOUBLE_EQ(7.5, env.ctx._pushed[0].price);
	cfg->release();
}

TEST(n03_blkidx, unknown_contract_is_skipped)
{
	N03BlkIdxEnv env;
	// 只注册一个合约, 另一个成分合约在基础数据里不存在 -> 走 error log + continue
	WTSContractInfo* c1 = env.regContract("SSE", "600000");
	IIndexWorker* w = env.create("WtBuiltinIndex", "blk09");
	WTSVariant* cfg = baseConfig(env, 0, "time");
	ASSERT_TRUE(w->init(cfg));

	// 数据不全(600519 没有基线), 不出指数
	WTSTickStruct t1 = makeTick("SSE", "600000", 10.0);
	feedTick(env, c1, t1);
	EXPECT_TRUE(env.ctx._pushed.empty());
	EXPECT_EQ(1u, env.ctx._logs.size());
	cfg->release();
}

TEST(n03_blkidx, non_participant_quote_ignored)
{
	N03BlkIdxEnv env;
	WTSContractInfo* c1 = env.regContract("SSE", "600000");
	[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
	WTSContractInfo* c3 = env.regContract("SSE", "600777");
	IIndexWorker* w = env.create("WtBuiltinIndex", "blk10");
	WTSVariant* cfg = baseConfig(env, 0, "time");
	ASSERT_TRUE(w->init(cfg));

	// 不在成分列表里的合约, handle_quote 直接返回
	WTSTickStruct t3 = makeTick("SSE", "600777", 99.0);
	feedTick(env, c3, t3);
	EXPECT_TRUE(env.ctx._pushed.empty());

	WTSTickStruct t1 = makeTick("SSE", "600000", 10.0);
	feedTick(env, c1, t1);
	EXPECT_EQ(1u, env.ctx._pushed.size());
	cfg->release();
}

TEST(n03_blkidx, trigger_code_filters_first_quote)
{
	N03BlkIdxEnv env;
	WTSContractInfo* c1 = env.regContract("SSE", "600000");
	[[maybe_unused]] WTSContractInfo* c2 = env.regContract("SSE", "600519");
	IIndexWorker* w = env.create("WtBuiltinIndex", "blk11");

	// trigger 指定为某个成分合约, 只有它进来才触发重算
	WTSVariant* cfg = baseConfig(env, 0, "SSE.600519");
	ASSERT_TRUE(w->init(cfg));

	WTSTickStruct t1 = makeTick("SSE", "600000", 10.0);
	feedTick(env, c1, t1);
	EXPECT_TRUE(env.ctx._pushed.empty());

	WTSTickStruct t2 = makeTick("SSE", "600519", 20.0);
	feedTick(env, c2, t2);
	EXPECT_EQ(1u, env.ctx._pushed.size());
	cfg->release();
}

TEST(n03_blkidx, commodities_branch_without_codes)
{
	N03BlkIdxEnv env;
	// 走 commodities 分支: 品种不存在 -> continue; 品种存在但无成分列表 -> 空循环
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("exchg", "SSE");
	cfg->append("code", "BLK002");
	cfg->append("trigger", "time");
	cfg->append("timeout", (uint32_t)0);

	WTSVariant* comms = WTSVariant::createArray();
	comms->append("SHFE.rb");
	WTSVariant* item = WTSVariant::createObject();
	item->append("code", "SHFE.cu");
	item->append("weight", 0.0);	// 0 权重会被修正为 1
	comms->append(item);
	cfg->append("commodities", comms);
	comms->release();

	WTSCommodityInfo* comm = WTSCommodityInfo::create("cu", "铜", "SHFE", "SHFE.day", "none");
	ASSERT_NE(nullptr, comm);
	env.bdMgr.addCommodity("SHFE.cu", comm);

	IIndexWorker* w = env.create("WtBuiltinIndex", "blk12");
	ASSERT_TRUE(w->init(cfg));
	EXPECT_TRUE(env.ctx._subscribes.empty());

	WTSTickStruct t = makeTick("SHFE", "cu2404", 10.0);
	WTSTickData* tick = WTSTickData::create(t);
	WTSContractInfo* cInfo = WTSContractInfo::create("cu2404", "cu2404", "SHFE", "SHFE.cu");
	tick->setContractInfo(cInfo);
	env.worker->handle_quote(tick);
	tick->release();
	// 没有任何成分被订阅, 不会产生指数
	EXPECT_TRUE(env.ctx._pushed.empty());

	comm->release();
	cInfo->release();
	cfg->release();
}

//////////////////////////////////////////////////////////////////////////
// 缺陷类整体隔离: WtBlkIdxWorker::init 末行用 WEIGHT_ALGS[_weight_alg] 打印权重算法名,
// WEIGHT_ALGS 只有 3 个元素且 weight_alg 未做区间校验。任意 >=3 的取值都会越界读取
// 悬垂指针, 属于同一个崩溃类, 因此整组放进同一个 DISABLED_ 用例, 默认不执行。
// 详见 docs/迭代计划/迭代01/bugs/n03_uftcore_index.md
TEST(DISABLED_n03_blkidx, weight_alg_out_of_range_class)
{
	const uint32_t badAlgs[] = { 3u, 4u, 10u, 99u, 0xFFFFFFFFu };
	for (uint32_t alg : badAlgs)
	{
		N03BlkIdxEnv env;
		env.regContract("SSE", "600000");
		env.regContract("SSE", "600519");
		IIndexWorker* w = env.create("WtBuiltinIndex", "blk13");
		WTSVariant* cfg = baseConfig(env, alg, "time");
		w->init(cfg);
		cfg->release();
	}
}
