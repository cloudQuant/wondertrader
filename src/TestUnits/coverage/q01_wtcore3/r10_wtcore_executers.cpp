/**
 * r10_wtcore_executers.cpp —— lane R10 / WtArbiExecuter.cpp + WtDiffExecuter.cpp 可达面
 *
 * 关键结论（详见 bugs/R10_wtcore.md BUG-R10-002）：
 *   WtExecuterFactory::_factories 只能由 loadFactories() 扫描目录里的动态库填充，没有进程内
 *   注册 IExecuterFact 的接口 ⇒ create*ExeUnit 恒返回空，getUnit() 恒返回空 ExecuteUnitPtr，
 *   两个执行器「单元创建成功之后」的主体（价差撮合、价量约束、部分成交、超时撤单）不可达。
 *   本文件因此只打：配置解析、组合/目标仓位记账、通道状态机、ExecuteContext 转发、
 *   旧主力自动清理判定矩阵、diff 目标仓位的 json 落盘读回。
 *
 * 公开接缝（不碰产品代码）：
 *   IExecCommand::setStub(IExecuterStub*)；ctor 的 IDataManager 与 IBaseDataMgr 形参；
 *   WtHelper::setGenerateDir（经 Q01TempEnv）驱动 load_data/save_data。
 *   poolsize 一律 0：boost::threadpool 会 spawn 线程，本 lane 禁止起线程。
 *
 * 覆盖映射（行号为实际源码）：
 *   WtArbiExecuter.cpp  L33-53 / L60-66 / L74-154 / L163-202 / L217-391 / L404-540
 *                       L548-698 / L707-801 / L821-889
 *   WtDiffExecuter.cpp  L41-105 / L113-172 / L180-232 / L243-286 / L304-481
 *                       L501-845 / L897-900
 */
#include "q01_sel_env.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include <rapidjson/document.h>

#include <Includes/IDataManager.h>
#include <Includes/WTSVariant.hpp>
#include <WtCore/IExecCommand.h>
#include <WtCore/WtArbiExecuter.h>
#include <WtCore/WtDiffExecuter.h>
#include <WtCore/WtExecuterFactory.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kStdAg = "SHFE.ag.2512";
static const char* kStdAu = "SHFE.au.2212";
static const char* kStdStock = "SSE.600000";

using DiffPositionMap = std::map<std::string, double>;

bool parseDiffPositionSection(const rapidjson::Value& section, const char* valueKey,
    DiffPositionMap& out)
{
    if (!section.IsArray())
        return false;

    for (const rapidjson::Value& item : section.GetArray())
    {
        if (!item.IsObject() || !item.HasMember("code") || !item["code"].IsString() ||
            item["code"].GetStringLength() == 0 || !item.HasMember(valueKey) ||
            !item[valueKey].IsNumber())
            return false;

        const std::string code(item["code"].GetString(), item["code"].GetStringLength());
        if (!out.emplace(code, item[valueKey].GetDouble()).second)
            return false;
    }
    return true;
}

bool parseDiffSnapshot(const std::string& content, DiffPositionMap& targets, DiffPositionMap& diffs)
{
    rapidjson::Document root;
    root.Parse(content.data(), content.size());
    if (root.HasParseError() || !root.IsObject() || !root.HasMember("targets") ||
        !root.HasMember("diffs"))
        return false;

    return parseDiffPositionSection(root["targets"], "target", targets) &&
        parseDiffPositionSection(root["diffs"], "diff", diffs);
}

/** 上一期主力“就是本合约”：只为打通 on_position 的自动清理判定 */
class PrevHotSelf : public HotMgr
{
public:
    uint32_t n_prev_raw = 0;

    virtual const char* getPrevRawCode(const char* exchg, const char* pid, uint32_t dt) override
    {
        (void)dt;
        ++n_prev_raw;
        if (strcmp(exchg, "SHFE") == 0 && strcmp(pid, "ag") == 0)
            return "ag2512";
        return "";
    }
};

/** 执行器存根替身：getCommodityInfo/getSessionInfo/getCurTime/get_hot_mon 全靠它 */
class R10Stub : public IExecuterStub
{
public:
    R10Stub() {}

    virtual uint64_t get_real_time() override { return _realTime; }

    virtual WTSCommodityInfo* get_comm_info(const char* stdCode) override
    {
        auto it = _comms.find(stdCode);
        return it == _comms.end() ? NULL : it->second;
    }

    virtual WTSSessionInfo* get_sess_info(const char* stdCode) override
    {
        auto it = _sess.find(stdCode);
        return it == _sess.end() ? NULL : it->second;
    }

    virtual IHotMgr* get_hot_mon() override { ++n_hot_mon; return _hot; }
    virtual uint32_t get_trading_day() override { return _tradingDay; }

    uint64_t _realTime = 20250110093115000ull;
    uint32_t _tradingDay = 20250110;
    uint32_t n_hot_mon = 0;
    IHotMgr* _hot = NULL;
    wt_hashmap<std::string, WTSCommodityInfo*> _comms;
    wt_hashmap<std::string, WTSSessionInfo*> _sess;
};

/** IDataManager 替身：只证明转发次数与入参，绝不返回未持有的引用计数 */
class FakeDataMgr : public IDataManager
{
public:
    virtual WTSTickSlice* get_tick_slice(const char* stdCode, uint32_t count, uint64_t etime = 0) override
    {
        (void)stdCode; (void)etime;
        ++n_tick_slice;
        last_count = count;
        return NULL;
    }

    virtual WTSTickData* grab_last_tick(const char* stdCode) override
    {
        ++n_grab;
        last_code = stdCode;
        if (_tick)
            _tick->retain();
        return _tick;
    }

    virtual uint32_t get_adjusting_flag() override { return 7u; }

    uint32_t n_tick_slice = 0;
    uint32_t n_grab = 0;
    uint32_t last_count = 0;
    std::string last_code;
    WTSTickData* _tick = NULL;
};

/** 只放 stdCode -> 品种/时段 的直连表，避免替身里再做代码拆分 */
inline void regisStub(R10Stub& stub, BaseDataMgr& bdm, const char* stdCode,
    WTSCommodityInfo* comm, const char* sid)
{
    stub._comms[stdCode] = comm;
    stub._sess[stdCode] = bdm.session(sid);
}

class Cfg
{
public:
    Cfg() { _root = WTSVariant::createObject(); }
    WTSVariant* root() { return _root; }

    Cfg& str(const char* k, const char* v) { _root->append(k, v); return *this; }
    Cfg& real(const char* k, double v) { _root->append(k, v); return *this; }
    Cfg& u32(const char* k, uint32_t v) { _root->append(k, v); return *this; }
    Cfg& boolean(const char* k, bool v) { _root->append(k, v); return *this; }

    /** 在 parent 下挂一个子对象并返回其裸指针 */
    static WTSVariant* addObject(WTSVariant* parent, const char* name)
    {
        WTSVariant* obj = WTSVariant::createObject();
        parent->append(name, obj);
        return obj;
    }

    static WTSVariant* addArray(WTSVariant* parent, const char* name)
    {
        WTSVariant* arr = WTSVariant::createArray();
        parent->append(name, arr);
        return arr;
    }

private:
    WTSVariant* _root;
};

class R10ExecuterTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r10exe"));

        _bdm.regisSession("TRADING");
        _sess = _bdm.session("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _comm2 = makeComm("SHFE", "au", "TRADING", TM_Long, CC_Future);
        _ct2 = makeCt("SHFE", "au", "au2212");
        _bdm.regis(_comm2, _ct2);

        _stub._hot = &_hot;
        regisStub(_stub, _bdm, kStdAg, _comm, "TRADING");
        regisStub(_stub, _bdm, kStdAu, _comm2, "TRADING");
    }

    void TearDown() override { _env.reset(); }

    /**
     * policy.default.name 必须存在：getUnit() 在 policy 里既没有 commID 也没有
     * "default" 时会 cfg->getCString 解引用 NULL（BUG-R10-001），
     * 所以本 lane 所有启用用例都显式给出 default。
     *
     * 卡8b=A（eager 全量校验，BUG-R10-001/P02-002 已落地）：init 前校验 scale 为有限正数、
     * policy 条目 name 严格为 "<factory>.<unit>"。原夹具缺 scale（旧实现读成 0，目标被清零）
     * 且 name 无工厂段，按新合同补齐；工厂仍未加载，单元创建照旧失败。
     */
    WTSVariant* policyCfg(const char* unitName = "R10Fact.R10Unit")
    {
        Cfg c;
        WTSVariant* policy = Cfg::addObject(c.root(), "policy");
        WTSVariant* def = Cfg::addObject(policy, "default");
        def->append("name", unitName);
        c.real("scale", 1.0);
        return c.root();
    }

    WTSVariant* policyCfgWithScale(double scale, const char* unitName = "R10Fact.R10Unit")
    {
        Cfg c;
        WTSVariant* policy = Cfg::addObject(c.root(), "policy");
        WTSVariant* def = Cfg::addObject(policy, "default");
        def->append("name", unitName);
        c.real("scale", scale);
        return c.root();
    }

    /** exec 数据目录：WtHelper::getExecDataDir() 由 Q01TempEnv 的重定向派生 */
    std::string execFile(const char* name)
    {
        std::string dir = WtHelper::getExecDataDir();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(dir), ec);
        return dir + name + ".json";
    }

    static std::string slurp(const std::string& path)
    {
        std::ifstream in(path.c_str());
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    std::unique_ptr<Q01TempEnv> _env;
    BaseDataMgr _bdm;
    PrevHotSelf _hot;
    FakeDataMgr _dm;
    R10Stub _stub;
    WtExecuterFactory _factory;
    WTSSessionInfo* _sess = NULL;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
    WTSCommodityInfo* _comm2 = NULL;
    WTSContractInfo* _ct2 = NULL;
};

} // namespace

/* ===================== WtArbiExecuter ===================== */

TEST_F(R10ExecuterTest, arbi_init_null_and_null_trader_guards)
{
    WtArbiExecuter exe(&_factory, "arbi_guard", NULL);
    EXPECT_FALSE(exe.init(NULL));   // L76-77

    // _data_mgr==NULL（L217-237）：两个行情接口都早退，不会 deref
    EXPECT_EQ(exe.getTicks(kStdAg, 10), nullptr);
    EXPECT_EQ(exe.grabLastTick(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_tick_slice, 0u);

    // _trader==NULL（L247-281）
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_DOUBLE_EQ(exe.getUndoneQty(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);

    // 通道未就绪（L292-345）：不碰 _trader
    EXPECT_EQ(exe.buy(kStdAg, 7100.0, 2).size(), 0u);
    EXPECT_EQ(exe.sell(kStdAg, 7100.0, 2).size(), 0u);
    EXPECT_FALSE(exe.cancel(1u));
    EXPECT_EQ(exe.cancel(kStdAg, true, 0).size(), 0u);
}

TEST_F(R10ExecuterTest, arbi_stub_backed_context_queries)
{
    WtArbiExecuter exe(&_factory, "arbi_stub", &_dm);
    exe.setStub(&_stub);

    ASSERT_TRUE(exe.init(policyCfg()));

    EXPECT_EQ(exe.getCommodityInfo(kStdAg), _comm);
    EXPECT_EQ(exe.getCommodityInfo(kStdStock), nullptr);
    EXPECT_EQ(exe.getSessionInfo(kStdAg), _sess);
    EXPECT_EQ(exe.getSessionInfo(kStdAu), _sess);
    // L387-389：getCurTime 就是 stub->get_real_time()，可精确算出
    EXPECT_EQ(exe.getCurTime(), 20250110093115000ull);

    // _data_mgr!=NULL 的转发（L219-223 / L233-236）
    EXPECT_EQ(exe.getTicks(kStdAg, 33), nullptr);
    EXPECT_EQ(_dm.n_tick_slice, 1u);
    EXPECT_EQ(_dm.last_count, 33u);
    EXPECT_EQ(exe.grabLastTick(kStdAu), nullptr);
    EXPECT_EQ(_dm.n_grab, 1u);
    EXPECT_EQ(_dm.last_code, std::string(kStdAu));
}

TEST_F(R10ExecuterTest, arbi_getunit_creates_nothing_and_callbacks_early_return)
{
    WtArbiExecuter exe(&_factory, "arbi_unit", &_dm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93100, 7100.0);
    ASSERT_NE(tk, nullptr);

    // 空工厂 => createExeUnit 返回空（L186-196）；四个回报入口在 unit==NULL 处早退
    exe.on_tick(kStdAg, tk);
    exe.on_trade(1001u, kStdAg, true, 1, 7100.0);
    exe.on_order(1001u, kStdAg, true, 2, 1, 7100.0, false);
    exe.on_entrust(1001u, kStdAg, true, "ok");
    exe.on_position_changed(kStdAg, 5);
    tk->release();

    // 没有任何单元 => 既不下单也不查行情，仓位账本保持空
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_grab, 0u);
    EXPECT_EQ(_dm.n_tick_slice, 0u);
}

TEST_F(R10ExecuterTest, arbi_set_position_group_math_emits_nothing_without_unit)
{
    WtArbiExecuter exe(&_factory, "arbi_grp", &_dm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10Unit");
    // groups.spread: 两条腿各 2 手，目标 5/5 => 组合可凑 1 组，余 3/3
    WTSVariant* groups = Cfg::addObject(c.root(), "groups");
    WTSVariant* spread = Cfg::addObject(groups, "spread");
    spread->append(kStdAg, 2.0);
    spread->append(kStdAu, 2.0);
    c.real("scale", 1.0);
    ASSERT_TRUE(exe.init(c.root()));

    wt_hashmap<std::string, double> targets;
    targets[kStdAg] = 5;
    targets[kStdAu] = 5;
    exe.set_position(targets);

    // 组合命中后仍走 getUnit==NULL 的 continue（L448-450），不得产生任何委托
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(exe.getOrders(kStdAu), nullptr);
    EXPECT_DOUBLE_EQ(exe.getUndoneQty(kStdAg), 0.0);
    EXPECT_EQ(_dm.n_grab, 0u);
}

TEST_F(R10ExecuterTest, arbi_strict_sync_scans_channel_holds)
{
    WtArbiExecuter exe(&_factory, "arbi_sync", &_dm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10Unit");
    c.boolean("strict_sync", true);
    WTSVariant* clear = Cfg::addObject(c.root(), "clear");
    clear->append("active", false);      // 只登记通道持仓，不做清理
    c.real("scale", 1.0);                // 卡8b=A：scale 必填
    ASSERT_TRUE(exe.init(c.root()));

    // on_position 先写 _channel_holds（L823），再因 _auto_clear==false 早退（L830-831）
    exe.on_position(kStdAg, true, 0, 0, 2, 2, 20250110);
    exe.on_position(kStdAu, false, 0, 0, 1, 1, 20250110);
    EXPECT_EQ(_hot.n_prev_raw, 0u);      // 自动清理被关掉，绝不问主力规则
    EXPECT_EQ(_stub.n_hot_mon, 0u);

    // 空目标 => 第二个循环无事可做，strict_sync 巡检 _channel_holds（L513-525）
    wt_hashmap<std::string, double> empty;
    exe.set_position(empty);
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
}

TEST_F(R10ExecuterTest, arbi_channel_events_over_empty_unit_map)
{
    WtArbiExecuter exe(&_factory, "arbi_chan", &_dm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));

    // 没有单元时三个广播循环只遍历空 map（L707-801）
    exe.on_channel_ready();
    exe.on_channel_lost();
    exe.on_account("CNY", 1000, 1100, 1200, 900, 50, 100, 200, 5, 100, 0);
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_grab, 0u);
}

TEST_F(R10ExecuterTest, arbi_autoclear_matrix_not_monthly_code)
{
    WtArbiExecuter exe(&_factory, "arbi_ac1", &_dm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));     // _auto_clear 默认 true（ctor L39）

    // 非分月期货代码 => L834-835 早退，不查主力
    exe.on_position(kStdStock, true, 0, 0, 10, 10, 20250110);
    EXPECT_EQ(_stub.n_hot_mon, 0u);
    EXPECT_EQ(_hot.n_prev_raw, 0u);
}

TEST_F(R10ExecuterTest, arbi_autoclear_matrix_not_prev_hot)
{
    WtArbiExecuter exe(&_factory, "arbi_ac2", &_dm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));

    // SHFE.au 的上一期主力替身返回 ""，与 "au2212" 不等 => L843-844 早退
    exe.on_position(kStdAu, true, 0, 0, 3, 3, 20250110);
    EXPECT_EQ(_stub.n_hot_mon, 1u);
    EXPECT_EQ(_hot.n_prev_raw, 1u);
    EXPECT_EQ(exe.getOrders(kStdAu), nullptr);
}

TEST_F(R10ExecuterTest, arbi_autoclear_matrix_exclude_list)
{
    WtArbiExecuter exe(&_factory, "arbi_ac3", &_dm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10Unit");
    WTSVariant* clear = Cfg::addObject(c.root(), "clear");
    clear->append("active", true);
    // excludes 用字符串形态（L117-118）
    clear->append("excludes", "SHFE.ag");
    c.real("scale", 1.0);                // 卡8b=A：scale 必填
    ASSERT_TRUE(exe.init(c.root()));

    // ag2512 就是上一期主力 => 命中排除列表，L853-858 返回
    exe.on_position(kStdAg, true, 0, 0, 4, 4, 20250110);
    EXPECT_EQ(_hot.n_prev_raw, 1u);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
}

TEST_F(R10ExecuterTest, arbi_autoclear_matrix_include_list_miss)
{
    WtArbiExecuter exe(&_factory, "arbi_ac4", &_dm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10Unit");
    WTSVariant* clear = Cfg::addObject(c.root(), "clear");
    clear->append("active", true);
    // includes 用数组形态（L119-124），且不含 SHFE.ag
    WTSVariant* inc = Cfg::addArray(clear, "includes");
    inc->append("CFFEX.IF");
    inc->append("DCE.m");
    c.real("scale", 1.0);                // 卡8b=A：scale 必填
    ASSERT_TRUE(exe.init(c.root()));

    exe.on_position(kStdAg, true, 0, 0, 4, 4, 20250110);
    EXPECT_EQ(_hot.n_prev_raw, 1u);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
}

TEST_F(R10ExecuterTest, arbi_autoclear_matrix_will_clear_but_no_unit)
{
    WtArbiExecuter exe(&_factory, "arbi_ac5", &_dm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10Unit");
    WTSVariant* clear = Cfg::addObject(c.root(), "clear");
    clear->append("active", true);
    WTSVariant* inc = Cfg::addArray(clear, "includes");
    inc->append("SHFE.ag");          // 命中包含列表 => 走到 L873-888
    c.real("scale", 1.0);                // 卡8b=A：scale 必填
    ASSERT_TRUE(exe.init(c.root()));

    exe.on_position(kStdAg, true, 0, 0, 4, 4, 20250110);
    EXPECT_EQ(_hot.n_prev_raw, 1u);
    // unit 为空 => 不清仓、不下单
    EXPECT_DOUBLE_EQ(exe.getUndoneQty(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
}

TEST_F(R10ExecuterTest, arbi_write_log_and_set_trader_null)
{
    WtArbiExecuter exe(&_factory, "arbi_log", NULL);
    exe.setStub(&_stub);
    exe.setTrader(NULL);             // L62-65：NULL 时不动 _channel_ready
    EXPECT_FALSE(exe.cancel(7u));    // 仍然未就绪
    exe.writeLog("hello arbi");      // L352-358
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
}

/* ===================== WtDiffExecuter ===================== */

TEST_F(R10ExecuterTest, diff_init_null_and_guards_without_trader_or_data)
{
    WtDiffExecuter exe(&_factory, "diff_guard", NULL, &_bdm);
    EXPECT_FALSE(exe.init(NULL));    // L86-87

    EXPECT_EQ(exe.getTicks(kStdAg, 5), nullptr);
    EXPECT_EQ(exe.grabLastTick(kStdAg), nullptr);
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_DOUBLE_EQ(exe.getUndoneQty(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(exe.buy(kStdAg, 7000.0, 1).size(), 0u);
    EXPECT_EQ(exe.sell(kStdAg, 7000.0, 1).size(), 0u);
    EXPECT_FALSE(exe.cancel(3u));
    EXPECT_EQ(exe.cancel(kStdAg, false, 1).size(), 0u);
    EXPECT_EQ(_dm.n_grab, 0u);
}

TEST_F(R10ExecuterTest, diff_stub_queries_and_data_forwarding)
{
    WtDiffExecuter exe(&_factory, "diff_stub", &_dm, &_bdm);
    exe.setStub(&_stub);

    Cfg c;
    WTSVariant* policy = Cfg::addObject(c.root(), "policy");
    WTSVariant* def = Cfg::addObject(policy, "default");
    def->append("name", "R10Fact.R10DiffUnit");
    c.real("scale", 3.0);
    ASSERT_TRUE(exe.init(c.root()));

    EXPECT_EQ(exe.getCommodityInfo(kStdAg), _comm);
    EXPECT_EQ(exe.getCommodityInfo("SSE.600000"), nullptr);
    EXPECT_EQ(exe.getSessionInfo(kStdAu), _sess);
    EXPECT_EQ(exe.getCurTime(), 20250110093115000ull);

    _dm._tick = makeTick("SHFE", "ag2512", 20250110, 20250110, 93130, 7111.0);
    ASSERT_NE(_dm._tick, nullptr);
    WTSTickData* got = exe.grabLastTick(kStdAg);
    ASSERT_NE(got, nullptr);
    EXPECT_DOUBLE_EQ(got->getTickStruct().price, 7111.0);
    got->release();
    EXPECT_EQ(_dm.n_grab, 1u);
    EXPECT_EQ(_dm.last_code, std::string(kStdAg));

    exe.writeLog("hello diff");
    exe.on_position(kStdAg, true, 0, 0, 1, 1, 20250110);   // L897-900 空实现
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
}

TEST_F(R10ExecuterTest, diff_load_data_then_set_position_roundtrip)
{
    const std::string file = execFile("diff_load");
    {
        std::ofstream out(file.c_str());
        // 一条合法（品种/合约已登记）、一条非法（合约查不到 => L143-147 continue）
        out << "{\"targets\":[{\"code\":\"SHFE.ag.2512\",\"target\":7.0},"
            << "{\"code\":\"DCE.rr.2509\",\"target\":9.0}],"
            << "\"diffs\":[{\"code\":\"SHFE.ag.2512\",\"diff\":-2.5},"
            << "{\"code\":\"DCE.rr.2509\",\"diff\":4.0}]}";
    }

    WtDiffExecuter exe(&_factory, "diff_load", &_dm, &_bdm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));

    // 空目标集合 => 第一循环跳过；第二循环命中「不在目标里且 pos!=0」(L598-616)，
    // getUnit 仍为空 => continue；最后 save_data 把内存账本原样写回（L641）
    wt_hashmap<std::string, double> empty;
    exe.set_position(empty);

    const std::string saved = slurp(file);
    // 读回的证据：合法代码仍在账本里，且非法代码被 load_data 丢弃
    EXPECT_NE(saved.find("SHFE.ag.2512"), std::string::npos);
    EXPECT_NE(saved.find("targets"), std::string::npos);
    EXPECT_NE(saved.find("diffs"), std::string::npos);
    EXPECT_EQ(saved.find("DCE.rr.2509"), std::string::npos);
    EXPECT_EQ(_dm.n_grab, 0u);
}

TEST_F(R10ExecuterTest, diff_set_position_on_missing_file_persists_target_without_unit)
{
    const std::string file = execFile("diff_fresh");
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(file), ec);
    ASSERT_FALSE(std::filesystem::exists(std::filesystem::path(file)));

    WtDiffExecuter exe(&_factory, "diff_fresh", &_dm, &_bdm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfgWithScale(1.0)));   // A missing snapshot is a valid first-run state.
    ASSERT_EQ(_factory.createDiffExeUnit("R10Fact.R10Unit"), nullptr);

    wt_hashmap<std::string, double> targets;
    targets[kStdAg] = 4;
    exe.set_position(targets);

    ASSERT_TRUE(std::filesystem::exists(std::filesystem::path(file)));
    const std::string saved = slurp(file);
    DiffPositionMap savedTargets;
    DiffPositionMap savedDiffs;
    ASSERT_TRUE(parseDiffSnapshot(saved, savedTargets, savedDiffs));
    ASSERT_EQ(savedTargets.size(), 1u);
    ASSERT_EQ(savedDiffs.size(), 1u);
    EXPECT_DOUBLE_EQ(savedTargets.at(kStdAg), 4.0);
    EXPECT_DOUBLE_EQ(savedDiffs.at(kStdAg), 4.0);

    // The snapshot records the requested state even when there is no unit to dispatch it.
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_grab, 0u);
    EXPECT_EQ(_dm.n_tick_slice, 0u);
}

// 迭代02 起 Diff 快照先整体按 schema 校验再发布（HEAD ccaad010 WtDiffExecuter::load_data
// “Rejected Diff snapshot”），与 R15 SEL 快照“损坏快照拒绝”同一合同，亦符合卡13c=A
// “未知/损坏格式 fail-closed、不猜测”。原用例期待语法损坏与空文件被静默忽略并以空账本覆盖，
// 会丢失可能仍有效的目标/差额；按已落地合同改为：init 拒绝、不保留配置、不改写原文件。
TEST_F(R10ExecuterTest, diff_load_data_broken_json_and_empty_file_are_rejected_without_overwrite)
{
    const std::string badPayload = "{\"targets\": [ {\"code\"";   // 语法不完整 => HasParseError
    {
        std::ofstream bad(execFile("diff_bad").c_str());
        bad << badPayload;
    }
    {
        std::ofstream empt(execFile("diff_empty").c_str());
        empt << "";                                 // 空内容同样无法通过 schema 校验
    }

    WtDiffExecuter exeBad(&_factory, "diff_bad", &_dm, &_bdm);
    exeBad.setStub(&_stub);
    WTSVariant* badCfg = policyCfg();
    EXPECT_FALSE(exeBad.init(badCfg));

    WtDiffExecuter exeEmpty(&_factory, "diff_empty", &_dm, &_bdm);
    exeEmpty.setStub(&_stub);
    WTSVariant* emptyCfg = policyCfg();
    EXPECT_FALSE(exeEmpty.init(emptyCfg));

    // 拒绝后的宿主不持有配置，也不得用空账本覆盖待人工核对的原快照。
    wt_hashmap<std::string, double> empty;
    exeBad.set_position(empty);
    exeEmpty.set_position(empty);
    EXPECT_EQ(slurp(execFile("diff_bad")), badPayload);
    EXPECT_EQ(slurp(execFile("diff_empty")), std::string());
    EXPECT_EQ(badCfg->retainCount(), 1u);
    EXPECT_EQ(emptyCfg->retainCount(), 1u);
    badCfg->release();
    emptyCfg->release();
}

// I03-NEW-070：初始化因快照损坏失败后，非空目标与增量都不得被接受，也不得覆盖被拒绝的快照。
TEST_F(R10ExecuterTest, diff_failed_init_rejects_nonempty_targets_and_keeps_rejected_snapshot)
{
    const std::string badPayload = "{\"targets\": [ {\"code\"";
    {
        std::ofstream bad(execFile("diff_bad_nonempty").c_str());
        bad << badPayload;
    }

    WtDiffExecuter exe(&_factory, "diff_bad_nonempty", &_dm, &_bdm);
    exe.setStub(&_stub);
    WTSVariant* cfg = policyCfg();
    EXPECT_FALSE(exe.init(cfg));

    wt_hashmap<std::string, double> targets;
    targets.emplace(kStdAg, 3.0);
    exe.set_position(targets);
    exe.on_position_changed(kStdAg, 2.0);

    EXPECT_EQ(slurp(execFile("diff_bad_nonempty")), badPayload);
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_EQ(_dm.n_grab, 0u);
    cfg->release();
}

TEST_F(R10ExecuterTest, diff_channel_ready_recovers_loaded_diffs)
{
    const std::string file = execFile("diff_chan");
    {
        std::ofstream out(file.c_str());
        out << "{\"targets\":[],\"diffs\":[{\"code\":\"SHFE.ag.2512\",\"diff\":6.0}]}";
    }

    WtDiffExecuter exe(&_factory, "diff_chan", &_dm, &_bdm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfg()));

    exe.on_channel_ready();     // _channel_ready=true + 遍历 _diff_pos 尝试恢复（L773-817）
    exe.on_channel_lost();      // L824-845
    exe.on_account("CNY", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);  // L862-884

    // 单元为空 => 恢复循环只会 continue，绝不委托
    EXPECT_DOUBLE_EQ(exe.getPosition(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_grab, 0u);
    // on_channel_ready 之后通道已就绪，但 _trader 仍为 NULL，
    // 因此这里不调用 buy/sell（会解引用空指针），只验证状态切换不产生副作用
}

TEST_F(R10ExecuterTest, diff_callbacks_without_unit_early_return)
{
    WtDiffExecuter exe(&_factory, "diff_cb", &_dm, &_bdm);
    exe.setStub(&_stub);
    ASSERT_TRUE(exe.init(policyCfgWithScale(1.0)));

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93140, 7120.0);
    ASSERT_NE(tk, nullptr);
    exe.on_tick(kStdAg, tk);
    exe.on_trade(2001u, kStdAg, true, 2, 7120.0);
    exe.on_order(2001u, kStdAg, true, 5, 3, 7120.0, false);
    exe.on_entrust(2001u, kStdAg, false, "no unit");
    exe.on_position_changed(kStdAg, 3);
    tk->release();

    // The position callback records target 3 even though the factory supplies no unit.
    const std::string file = execFile("diff_cb");
    const std::string before = slurp(file);
    DiffPositionMap beforeTargets;
    DiffPositionMap beforeDiffs;
    ASSERT_TRUE(parseDiffSnapshot(before, beforeTargets, beforeDiffs));
    ASSERT_EQ(beforeTargets.size(), 1u);
    ASSERT_EQ(beforeDiffs.size(), 1u);
    EXPECT_DOUBLE_EQ(beforeTargets.at(kStdAg), 3.0);
    EXPECT_DOUBLE_EQ(beforeDiffs.at(kStdAg), 3.0);

    // An empty target request clears the persisted target; zero-valued ledger rows remain valid.
    wt_hashmap<std::string, double> empty;
    exe.set_position(empty);
    const std::string after = slurp(file);
    DiffPositionMap afterTargets;
    DiffPositionMap afterDiffs;
    ASSERT_TRUE(parseDiffSnapshot(after, afterTargets, afterDiffs));
    ASSERT_EQ(afterTargets.size(), 1u);
    ASSERT_EQ(afterDiffs.size(), 1u);
    EXPECT_DOUBLE_EQ(afterTargets.at(kStdAg), 0.0);
    EXPECT_DOUBLE_EQ(afterDiffs.at(kStdAg), 0.0);
    EXPECT_NE(before, after);
    EXPECT_DOUBLE_EQ(exe.getUndoneQty(kStdAg), 0.0);
    EXPECT_EQ(exe.getOrders(kStdAg), nullptr);
    EXPECT_EQ(_dm.n_grab, 0u);
    EXPECT_EQ(_dm.n_tick_slice, 0u);
}
