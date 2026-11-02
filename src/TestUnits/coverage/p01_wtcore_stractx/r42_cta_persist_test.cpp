/**
 * r42_cta_persist_test.cpp  (lane R42 / WtCore 策略上下文补测)
 *
 * 覆盖目标 src/WtCore/CtaStraBaseCtx.cpp 的持久化、调度、图表与查询面：
 *   init_outputs / save_data / load_data / save_userdata / load_userdata
 *   dump_chart_info / on_schedule / on_session_begin / on_session_end
 *   on_bar / add_chart_mark / register_index(_line) / add_index_baseline / set_index_value
 *   stra_get_* 查询族 与 stra_log_* 日志门面
 *
 * 全部通过公有 API 与测试内子类的 protected 包装驱动，不修改任何生产代码。
 * 依赖的替身（P01TempEnv/FakeBaseDataMgr/FakeHotMgr/P01CtaEngine）复用同目录 p01_cta_env.h。
 */
#include "p01_cta_env.h"

#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {

static const char* kR42Code = "SHFE.rb.2510";
static const char* kR42T1Code = "SHFE.ag.2512";

std::string r42_read_file(const std::string& path)
{
    std::ifstream ifs(path.c_str());
    if (!ifs.is_open())
        return std::string();
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool r42_write_file(const std::string& path, const std::string& content)
{
    std::filesystem::path p(path);
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
        return false;
    ofs << content;
    ofs.close();
    return true;
}

/**
 * 测试内子类：
 *   - 只补齐 CtaStraBaseCtx 的 4 个策略回调；
 *   - 暴露少量 protected 入口（main key / dump / do_set_position），用于驱动被测基类；
 *   - 在 on_calculate 内部按需调用图表接口，以便在 _is_in_schedule 为真时覆盖成功分支。
 */
class R42CtaCtx : public CtaStraBaseCtx
{
public:
    R42CtaCtx(WtCtaEngine* engine, const char* name, int32_t slippage = 0)
        : CtaStraBaseCtx(engine, name, slippage) {}

    virtual ~R42CtaCtx() {}

    void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)newTick;
        last_tick_code = stdCode;
        ++tick_updates;
    }

    void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        (void)newBar;
        last_bar_code = stdCode;
        last_bar_period = period;
        ++bar_closes;
    }

    void on_calculate(uint32_t curDate, uint32_t curTime) override
    {
        ++calc_times;
        last_calc = std::make_pair(curDate, curTime);
        if (emit_chart)
        {
            add_chart_mark(3500.0, "buy", "r42tag");
            idx_value_ok = set_index_value("r42idx", "ma", 1.5);
            // 已注册指标但不存在的线 -> 失败分支（调度中才有机会到达）
            idx_line_missing_ok = set_index_value("r42idx", "missing", 9.0);
        }
    }

    void on_condition_triggered(const char* stdCode, double target, double price, const char* usertag) override
    {
        (void)price;
        (void)usertag;
        last_cond_code = stdCode;
        last_cond_target = target;
        ++cond_fires;
    }

    // protected 包装
    void set_main_key(const char* key) { _main_key = key; }
    void dump_chart() { dump_chart_info(); }
    void stra_log_error(const char* message) override
    {
        ++errorCount;
        lastError = message ? message : "";
    }
    void stra_log_info(const char* message) override
    {
        const std::string text = message ? message : "";
        if (text.find("position ignored") != std::string::npos) positionIgnored = text;
        if (text.find("signal ignored") != std::string::npos) signalIgnored = text;
        if (text.find("Position confirmed,") != std::string::npos) positionConfirmed = text;
        if (text.find("untouched signal recovered") != std::string::npos) signalRecovered = text;
        if (text.find("Market data subscribed:") != std::string::npos) subscribed = text;
    }
    size_t position_count() const { return _pos_map.size(); }
    size_t signal_count() const { return _sig_map.size(); }
    size_t condition_count() const { return _condtions.size(); }
    double stored_position(const char* code) const
    {
        auto it = _pos_map.find(code);
        return it == _pos_map.end() ? 0.0 : it->second._volume;
    }
    double stored_signal(const char* code) const
    {
        auto it = _sig_map.find(code);
        return it == _sig_map.end() ? 0.0 : it->second._volume;
    }
    uint64_t condition_time() const { return _last_cond_min; }
    uint32_t last_bar() const { return _last_barno; }
    size_t errorCount = 0;
    std::string lastError;
    std::string positionIgnored, signalIgnored, positionConfirmed, signalRecovered, subscribed;

    uint32_t tick_updates = 0;
    uint32_t bar_closes = 0;
    uint32_t calc_times = 0;
    uint32_t cond_fires = 0;
    std::string last_tick_code;
    std::string last_bar_code;
    std::string last_bar_period;
    std::string last_cond_code;
    double last_cond_target = 0;
    std::pair<uint32_t, uint32_t> last_calc { 0, 0 };

    bool emit_chart = false;
    bool idx_value_ok = false;
    bool idx_line_missing_ok = false;
};

class R42ThrowingBaseDataMgr : public FakeBaseDataMgr
{
public:
    bool throwOnAg = false;
    bool acceptAnyCode = false;
    WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t date = 0) override
    {
        if (acceptAnyCode)
            return FakeBaseDataMgr::getContract("rb2510", "SHFE", date);
        if (throwOnAg && std::string(code) == "ag2512")
            throw std::runtime_error("injected contract lookup failure");
        return FakeBaseDataMgr::getContract(code, exchg, date);
    }
};

class R42CtaPersistTest : public ::testing::Test
{
protected:
    P01TempEnv env { "cta_persist" };
    R42ThrowingBaseDataMgr bd;
    FakeHotMgr hot;
    P01CtaEngine engine;
    std::unique_ptr<R42CtaCtx> ctx;

    void SetUp() override
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        ASSERT_NE(sess, nullptr);
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = p01MakeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        bd.regis(rb, p01MakeCt("SHFE", "rb", "rb2510"));

        WTSCommodityInfo* ag = p01MakeComm("SHFE", "ag", "TRADING", TM_LongT1);
        ag->setSessionInfo(sess);
        bd.regis(ag, p01MakeCt("SHFE", "ag", "ag2512"));

        WTSCommodityInfo* of = p01MakeComm("SZSE", "of", "TRADING", TM_LongT1);
        of->setSessionInfo(sess);
        bd.regis(of, p01MakeCt("SZSE", "of", "of159915"));

        engine.attach(&bd, &hot);
        ctx.reset(new R42CtaCtx(&engine, "r42_cta_persist", 0));
    }

    std::string data_file(const char* name) const
    {
        return std::string(WtHelper::getStraDataDir()) + name + ".json";
    }

    std::string ud_file(const char* name) const
    {
        return std::string(WtHelper::getStraUsrDatDir()) + "ud_" + name + ".json";
    }

    void feed(const char* code, double price, bool emit = false)
    {
        WTSTickData* tick = p01MakeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, price);
        ASSERT_NE(tick, nullptr);
        // 上下文在 on_tick 里会把信号/条件单落成持仓，并通过 handle_pos_change 回提交给引擎；
        // 引擎侧 get_cur_price 需要同一份价格，否则会落到 _data_mgr（本夹具为 NULL）上。
        engine.prime_price(code, price);
        ctx->on_tick(code, tick, emit);
        tick->release();
    }

    // 限价开多 -> 第一条 tick 触发条件单产生信号，第二条 tick 把信号落成持仓
    void open_long_by_limit(const char* code, double qty, const char* tag, double limit)
    {
        ctx->stra_enter_long(code, qty, tag, limit);
        feed(code, limit - 10);
        feed(code, limit - 5);
    }

    // 目标仓位条件单（SP）-> 同上两步触发
    void set_target_by_limit(const char* code, double qty, const char* tag, double limit)
    {
        ctx->stra_set_position(code, qty, tag, limit);
        feed(code, limit - 10);
        feed(code, limit - 5);
    }
};

// 覆盖 CtaStraBaseCtx.cpp L77-188 init_outputs、L246-493 load_data、L246-275 load_userdata
TEST_F(R42CtaPersistTest, OnInitLoadsPersistedStateAndPreparesOutputs)
{
    const std::string json = R"json({
  "fund": {"total_profit": 12.5, "total_dynprofit": 3.5, "total_fees": 1.25, "tdate": 20250110},
  "positions": [
    {"code": "SHFE.rb.2510", "volume": 2, "closeprofit": 0.5, "dynprofit": 3.5,
     "lastentertime": 100, "lastexittime": 50, "frozen": 1, "frozendate": 20250109,
     "details": [
       {"long": true, "price": 3000, "maxprice": 3100, "minprice": 2900, "volume": 2,
        "opentime": 100, "opentdate": 20250109, "profit": 3.5, "maxprofit": 4,
        "maxloss": -1, "opentag": "A", "openbarno": 3}
     ]},
    {"code": "SHFE.xx.9999", "volume": 4, "closeprofit": 9, "dynprofit": 9,
     "lastentertime": 1, "lastexittime": 2,
     "details": [{"long": true, "price": 1, "volume": 4, "opentime": 1,
                  "profit": 0, "maxprofit": 0, "maxloss": 0, "opentag": "X"}]}
  ],
  "conditions": {"settime": 20250110093000, "items": {
      "SHFE.rb.2510": [{"usertag": "cond", "field": 0, "alg": )json"
        + std::to_string((uint32_t)WCT_LargerOrEqual)
        + R"json(, "target": 3400.0, "qty": 5.0, "action": )json"
        + std::to_string((uint32_t)COND_ACTION_SP)
        + R"json(}],
      "SHFE.xx.9999": [{"usertag": "bad", "field": 0, "alg": )json"
        + std::to_string((uint32_t)WCT_LargerOrEqual)
        + R"json(, "target": 1.0, "qty": 1.0, "action": )json"
        + std::to_string((uint32_t)COND_ACTION_SP)
        + R"json(}]
  }},
  "signals": {"SHFE.xx.9999": {"usertag": "old", "volume": 1, "sigprice": 1.0, "gentime": 1}},
  "utils": {"lastbarno": 7}
})json";

    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), json));
    ASSERT_TRUE(r42_write_file(ud_file("r42_cta_persist"), R"json({"k1":"v1","k2":"v2"})json"));

    ctx->on_init();

    // 有效的持仓被恢复；过期合约的仓位被强制归零
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42Code), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42Code, true), 1.0); // 冻结 1 手
    EXPECT_DOUBLE_EQ(ctx->stra_get_position("SHFE.xx.9999"), 0.0);

    // 持仓明细与资金：load_data 用 positions 汇总覆盖 total_profit/total_dynprofit
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kR42Code), 3000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(1), 9.5);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(2), 3.5);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(3), 1.25);
    EXPECT_EQ(ctx->stra_get_last_entertime(kR42Code), 100u);
    EXPECT_EQ(ctx->stra_get_last_exittime(kR42Code), 50u);
    EXPECT_STREQ(ctx->stra_get_last_entertag(kR42Code), "A");

    // 用户数据
    EXPECT_STREQ(ctx->stra_load_user_data("k1"), "v1");
    EXPECT_STREQ(ctx->stra_load_user_data("k2"), "v2");
    EXPECT_STREQ(ctx->stra_load_user_data("nope", "def"), "def");

    // 条件单被恢复：价格突破 3400 后产生目标仓位 5 的信号
    std::string seen_code;
    double seen_target = -1;
    ctx->enum_position([&](const char* code, double target) {
        if (std::string(code) == kR42Code)
        {
            seen_code = code;
            seen_target = target;
        }
    });
    EXPECT_DOUBLE_EQ(seen_target, 2.0);

    feed(kR42Code, 3500.0);
    ctx->enum_position([&](const char* code, double target) {
        if (std::string(code) == kR42Code)
            seen_target = target;
    });
    EXPECT_EQ(seen_code, kR42Code);
    EXPECT_DOUBLE_EQ(seen_target, 5.0);

    // 输出文件已初始化
    EXPECT_FALSE(r42_read_file(std::string(WtHelper::getOutputDir()) + "r42_cta_persist/trades.csv").empty());

    // 再初始化一次：走 "文件已存在 -> seek_to_end" 分支
    ctx->on_init();
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42Code), 2.0);
}

// 覆盖 L495-643 save_data、L752-793 update_dyn_profit、L1073-1109 on_session_end、
// L1761-1964 查询族、L1897-1964 detail 查询
TEST_F(R42CtaPersistTest, SaveDataRoundTripAcrossContexts)
{
    ctx->on_init();
    open_long_by_limit(kR42Code, 2, "A", 3400.0);   // 成交价 3395
    set_target_by_limit(kR42Code, 5, "B", 3500.0);  // 成交价 3495
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kR42Code), 5.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kR42Code), (3395.0 * 2 + 3495.0 * 3) / 5.0);

    ctx->on_session_end(20250110);

    // 旧上下文落盘后，新上下文仅靠 load_data 恢复全部字段
    R42CtaCtx reloaded(&engine, "r42_cta_persist", 0);
    reloaded.on_init();

    EXPECT_DOUBLE_EQ(reloaded.stra_get_position(kR42Code), 5.0);
    EXPECT_DOUBLE_EQ(reloaded.stra_get_position_avgpx(kR42Code), (3395.0 * 2 + 3495.0 * 3) / 5.0);
    EXPECT_DOUBLE_EQ(reloaded.stra_get_last_enterprice(kR42Code), 3495.0);
    EXPECT_EQ(reloaded.stra_get_last_entertime(kR42Code), 202501100931ull);
    EXPECT_EQ(reloaded.stra_get_first_entertime(kR42Code), 202501100931ull);
    EXPECT_EQ(reloaded.stra_get_last_exittime(kR42Code), 0u);
    EXPECT_STREQ(reloaded.stra_get_last_entertag(kR42Code), "A");
    EXPECT_DOUBLE_EQ(reloaded.stra_get_detail_cost(kR42Code, "B"), 3495.0);
    EXPECT_EQ(reloaded.stra_get_detail_entertime(kR42Code, "B"), 202501100931ull);
    EXPECT_DOUBLE_EQ(reloaded.stra_get_detail_profit(kR42Code, "A", -1), 0.0);

    // 未知代码/标签的查询守卫
    EXPECT_EQ(reloaded.stra_get_first_entertime("SHFE.zz.1"), 0u);
    EXPECT_STREQ(reloaded.stra_get_last_entertag("SHFE.zz.1"), "");
    EXPECT_DOUBLE_EQ(reloaded.stra_get_detail_cost(kR42Code, "missing"), 0.0);
    EXPECT_EQ(reloaded.stra_get_detail_entertime(kR42Code, "missing"), 0u);

    // 资金/持仓日志已经落盘
    EXPECT_FALSE(r42_read_file(std::string(WtHelper::getOutputDir()) + "r42_cta_persist/funds.csv").empty());
    EXPECT_FALSE(r42_read_file(std::string(WtHelper::getOutputDir()) + "r42_cta_persist/positions.csv").empty());
}

TEST_F(R42CtaPersistTest, MalformedSnapshotsDoNotPublishAnyStateOrSubscriptions)
{
    const char* baseline = R"json({
      "fund":{"total_profit":1,"total_dynprofit":2,"total_fees":3,"tdate":20250110},
      "positions":[{"code":"SHFE.rb.2510","volume":2,"closeprofit":1,"dynprofit":2,
        "lastentertime":1,"lastexittime":0,"details":[{"long":true,"price":3000,"volume":2,
        "opentime":1,"profit":2,"maxprofit":3,"maxloss":-1,"opentag":"initial"}]}],
      "conditions":{"settime":111,"items":{"SHFE.rb.2510":[{"usertag":"initial",
        "field":0,"alg":0,"target":3500,"qty":2,"action":0}]}},
      "signals":{"SHFE.rb.2510":{"usertag":"initial","volume":3,"sigprice":3000,"gentime":2}},
      "utils":{"lastbarno":7}
    })json";
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), baseline));
    ctx->on_init();
    ASSERT_EQ(ctx->position_count(), 1u);
    ASSERT_EQ(ctx->signal_count(), 1u);
    ASSERT_EQ(ctx->condition_count(), 1u);
    ASSERT_TRUE(engine.has_tick_subscription(kR42Code));
    ASSERT_FALSE(engine.has_tick_subscription(kR42T1Code));

    const char* candidate = R"json({
      "fund":{"total_profit":8,"total_dynprofit":9,"total_fees":10,"tdate":20250111},
      "positions":[{"code":"SHFE.ag.2512","volume":4,"closeprofit":8,"dynprofit":9,
        "lastentertime":10,"lastexittime":11,"frozen":1,"frozendate":20250110,
        "details":[{"long":true,"price":3000,"volume":4,"opentime":10,"profit":9,
          "maxprofit":10,"maxloss":-1,"opentag":"candidate"}]}],
      "conditions":{"settime":222,"items":{"SHFE.ag.2512":[{"usertag":"candidate",
        "field":0,"alg":0,"target":3500,"qty":4,"action":0}]}},
      "signals":{"SHFE.ag.2512":{"usertag":"candidate","volume":5,"sigprice":3000,"gentime":2}},
      "utils":{"lastbarno":8}
    })json";
    struct InvalidCase
    {
        const char* name;
        const char* path;
        std::function<void(rapidjson::Document&)> mutate;
    };
    const std::vector<InvalidCase> invalid = {
        {"root_array", "$", [](auto& d) { d.SetArray(); }},
        {"missing_positions", "$.positions", [](auto& d) { d.RemoveMember("positions"); }},
        {"bad_positions", "$.positions", [](auto& d) { d["positions"].SetObject(); }},
        {"missing_conditions", "$.conditions", [](auto& d) { d.RemoveMember("conditions"); }},
        {"bad_fund", "$.fund", [](auto& d) { d["fund"].SetArray(); }},
        {"missing_fees", "$.fund.total_fees", [](auto& d) { d["fund"].RemoveMember("total_fees"); }},
        {"bad_position", "$.positions[0]", [](auto& d) { d["positions"][0].SetBool(true); }},
        {"empty_code", "$.positions[0].code", [](auto& d) { d["positions"][0]["code"].SetString("", d.GetAllocator()); }},
        {"nul_code", "$.positions[0].code", [](auto& d) {
            const char code[] = {'a', '\0', 'b'};
            d["positions"][0]["code"].SetString(code, 3, d.GetAllocator());
        }},
        {"missing_volume", "$.positions[0].volume", [](auto& d) { d["positions"][0].RemoveMember("volume"); }},
        {"bad_frozen_date", "$.positions[0].frozendate", [](auto& d) { d["positions"][0]["frozendate"].SetString("x", d.GetAllocator()); }},
        {"bad_details", "$.positions[0].details", [](auto& d) { d["positions"][0]["details"].SetObject(); }},
        {"bad_detail_price", "$.positions[0].details[0].price", [](auto& d) { d["positions"][0]["details"][0]["price"].SetBool(true); }},
        {"bad_zero_detail_field", "$.positions[0].details[0].price", [](auto& d) {
            d["positions"][0]["details"][0]["volume"].SetInt(0);
            d["positions"][0]["details"][0]["price"].SetBool(true);
        }},
        {"long_opentag", "$.positions[0].details[0].opentag", [](auto& d) {
            const std::string tag(32, 'x');
            d["positions"][0]["details"][0]["opentag"].SetString(tag.c_str(), tag.size(), d.GetAllocator());
        }},
        {"nul_opentag", "$.positions[0].details[0].opentag", [](auto& d) {
            const char tag[] = {'a', '\0', 'b'};
            d["positions"][0]["details"][0]["opentag"].SetString(tag, 3, d.GetAllocator());
        }},
        {"bad_condition_items", "$.conditions.items", [](auto& d) { d["conditions"]["items"].SetArray(); }},
        {"condition_code_at_limit", "$.conditions.items[ccccccccccccccccccccccccccccccc].<code>[0].alg", [](auto& d) {
            const std::string key(31, 'c');
            auto it = d["conditions"]["items"].MemberBegin();
            it->name.SetString(key.c_str(), key.size(), d.GetAllocator());
            it->value[0]["alg"].SetUint(99);
        }},
        {"condition_code_over_limit", "$.conditions.items.<code>", [](auto& d) {
            const std::string key(32, 'c');
            d["conditions"]["items"].MemberBegin()->name.SetString(key.c_str(), key.size(), d.GetAllocator());
        }},
        {"condition_code_very_long", "$.conditions.items.<code>", [](auto& d) {
            const std::string key(4096, 'c');
            d["conditions"]["items"].MemberBegin()->name.SetString(key.c_str(), key.size(), d.GetAllocator());
        }},
        {"bad_condition_field", ".field", [](auto& d) { d["conditions"]["items"]["SHFE.ag.2512"][0]["field"].SetUint(99); }},
        {"bad_condition_alg", ".alg", [](auto& d) { d["conditions"]["items"]["SHFE.ag.2512"][0]["alg"].SetUint(99); }},
        {"bad_condition_action", ".action", [](auto& d) { d["conditions"]["items"]["SHFE.ag.2512"][0]["action"].SetUint(99); }},
        {"long_condition_tag", ".usertag", [](auto& d) {
            const std::string tag(32, 'x');
            d["conditions"]["items"]["SHFE.ag.2512"][0]["usertag"].SetString(tag.c_str(), tag.size(), d.GetAllocator());
        }},
        {"bad_signal", "$.signals[SHFE.ag.2512].gentime", [](auto& d) { d["signals"]["SHFE.ag.2512"]["gentime"].SetString("x", d.GetAllocator()); }},
        {"signal_code_very_long", "$.signals[ssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss...].gentime", [](auto& d) {
            const std::string key(4096, 's');
            auto it = d["signals"].MemberBegin();
            it->name.SetString(key.c_str(), key.size(), d.GetAllocator());
            it->value["gentime"].SetString("x", d.GetAllocator());
        }},
        {"long_signal_tag", "$.signals[SHFE.ag.2512].usertag", [](auto& d) {
            const std::string tag(32, 'x');
            d["signals"]["SHFE.ag.2512"]["usertag"].SetString(tag.c_str(), tag.size(), d.GetAllocator());
        }},
        {"bad_utils", "$.utils.lastbarno", [](auto& d) { d["utils"]["lastbarno"].SetInt(-1); }}
    };
    for (const InvalidCase& tc : invalid)
    {
        SCOPED_TRACE(tc.name);
        rapidjson::Document d;
        d.Parse(candidate);
        ASSERT_FALSE(d.HasParseError());
        tc.mutate(d);
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        ASSERT_TRUE(d.Accept(writer));
        ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), buffer.GetString()));
        ctx->errorCount = 0;
        ctx->lastError.clear();
        ctx->on_init();
        ASSERT_EQ(ctx->errorCount, 1u);
        EXPECT_NE(ctx->lastError.find(tc.path), std::string::npos);
        EXPECT_LT(ctx->lastError.size(), 512u);
        EXPECT_EQ(ctx->position_count(), 1u);
        EXPECT_DOUBLE_EQ(ctx->stored_position(kR42Code), 2.0);
        EXPECT_EQ(ctx->signal_count(), 1u);
        EXPECT_DOUBLE_EQ(ctx->stored_signal(kR42Code), 3.0);
        EXPECT_EQ(ctx->condition_count(), 1u);
        EXPECT_EQ(ctx->condition_time(), 111u);
        EXPECT_EQ(ctx->last_bar(), 7u);
        EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(3), 3.0);
        EXPECT_FALSE(engine.has_tick_subscription(kR42T1Code));
    }
    const std::string malformedText[] = {
        "{bad JSON", std::string(candidate) + std::string("\0garbage", 8)
    };
    for (const std::string& input : malformedText)
    {
        ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), input));
        ctx->errorCount = 0;
        ctx->lastError.clear();
        ctx->on_init();
        ASSERT_EQ(ctx->errorCount, 1u);
        EXPECT_NE(ctx->lastError.find("Invalid persisted state"), std::string::npos);
        EXPECT_EQ(ctx->position_count(), 1u);
        EXPECT_DOUBLE_EQ(ctx->stored_position(kR42Code), 2.0);
        EXPECT_EQ(ctx->signal_count(), 1u);
        EXPECT_DOUBLE_EQ(ctx->stored_signal(kR42Code), 3.0);
        EXPECT_EQ(ctx->condition_count(), 1u);
        EXPECT_EQ(ctx->condition_time(), 111u);
        EXPECT_EQ(ctx->last_bar(), 7u);
        EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(3), 3.0);
        EXPECT_FALSE(engine.has_tick_subscription(kR42T1Code));
    }
}

TEST_F(R42CtaPersistTest, ExpiredPositionCodesHaveBoundedLogDisplay)
{
    auto load = [this](const std::string& code) {
        const std::string json = std::string(R"({"positions":[{"code":")") + code +
            R"(","volume":1,"closeprofit":0,"dynprofit":0,"lastentertime":0,"lastexittime":0,"details":null}],"conditions":null})";
        EXPECT_TRUE(r42_write_file(data_file("r42_cta_persist"), json));
        ctx->on_init();
    };
    load("SHFE.xx.9999");
    EXPECT_EQ(ctx->positionIgnored, "SHFE.xx.9999 not exists or expired, position ignored");
    const std::string code(4096, 'x');
    load(code);
    EXPECT_EQ(ctx->positionIgnored, std::string(128, 'x') + "... not exists or expired, position ignored");
    EXPECT_DOUBLE_EQ(ctx->stored_position(code.c_str()), 0.0);
    EXPECT_FALSE(engine.has_tick_subscription(code.c_str()));
}

TEST_F(R42CtaPersistTest, ExpiredSignalCodesHaveBoundedLogDisplay)
{
    auto load = [this](const std::string& code) {
        const std::string json = std::string(R"({"positions":[],"conditions":null,"signals":{")") + code +
            R"(":{"usertag":"a","volume":1,"sigprice":1,"gentime":1}}})";
        EXPECT_TRUE(r42_write_file(data_file("r42_cta_persist"), json));
        ctx->on_init();
    };
    load("SHFE.xx.9999");
    EXPECT_EQ(ctx->signalIgnored, "SHFE.xx.9999 not exists or expired, signal ignored");
    const std::string code(4096, 'x');
    load(code);
    EXPECT_EQ(ctx->signalIgnored, std::string(128, 'x') + "... not exists or expired, signal ignored");
    EXPECT_EQ(ctx->signal_count(), 0u);
    EXPECT_FALSE(engine.has_tick_subscription(code.c_str()));
}

TEST_F(R42CtaPersistTest, LongRecoveredPositionCodesKeepTheirFullIdentity)
{
    bd.acceptAnyCode = true;
    const std::string first = std::string(4095, 'x') + "a";
    const std::string second = std::string(4095, 'x') + "b";
    auto item = [](const std::string& code, int volume) {
        return std::string(R"({"code":")") + code + R"(","volume":)" + std::to_string(volume) +
            R"(,"closeprofit":0,"dynprofit":0,"lastentertime":0,"lastexittime":0,"details":[{"long":true,"price":1,"volume":1,"opentime":1,"profit":0,"maxprofit":0,"maxloss":0,"opentag":"a"}]})";
    };
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"),
        std::string(R"({"positions":[)") + item(first, 2) + "," + item(second, 3) + R"(],"conditions":null})"));
    ctx->on_init();
    EXPECT_DOUBLE_EQ(ctx->stored_position(first.c_str()), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stored_position(second.c_str()), 3.0);
    EXPECT_TRUE(engine.has_tick_subscription(first.c_str()));
    EXPECT_TRUE(engine.has_tick_subscription(second.c_str()));
    EXPECT_EQ(ctx->positionConfirmed, "Position confirmed," + std::string(128, 'x') + "... -> 3");
    EXPECT_EQ(ctx->subscribed, "Market data subscribed: " + std::string(128, 'x') + "...");
}

TEST_F(R42CtaPersistTest, LongRecoveredSignalCodesKeepTheirFullIdentity)
{
    bd.acceptAnyCode = true;
    const std::string first = std::string(4095, 'x') + "a";
    const std::string second = std::string(4095, 'x') + "b";
    auto item = [](const std::string& code, int volume) {
        return std::string("\"") + code + R"(":{"usertag":"a","volume":)" + std::to_string(volume) +
            R"(,"sigprice":1,"gentime":1})";
    };
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"),
        std::string(R"({"positions":[],"conditions":null,"signals":{)") + item(first, 2) + "," + item(second, 3) + "}}"));
    ctx->on_init();
    EXPECT_DOUBLE_EQ(ctx->stored_signal(first.c_str()), 2.0);
    EXPECT_DOUBLE_EQ(ctx->stored_signal(second.c_str()), 3.0);
    EXPECT_TRUE(engine.has_tick_subscription(first.c_str()));
    EXPECT_TRUE(engine.has_tick_subscription(second.c_str()));
    EXPECT_EQ(ctx->signalRecovered, std::string(128, 'x') + "... untouched signal recovered, target pos: 3");
    EXPECT_EQ(ctx->subscribed, "Market data subscribed: " + std::string(128, 'x') + "...");
}

TEST_F(R42CtaPersistTest, LongDirectTickSubscriptionsKeepTheirFullIdentity)
{
    ctx->stra_sub_ticks(kR42Code);
    EXPECT_EQ(ctx->subscribed, "Market data subscribed: SHFE.rb.2510");
    const std::string first = std::string(4095, 'x') + "a";
    const std::string second = std::string(4095, 'x') + "b";
    ctx->stra_sub_ticks(first.c_str());
    ctx->stra_sub_ticks(second.c_str());
    EXPECT_TRUE(engine.has_tick_subscription(first.c_str()));
    EXPECT_TRUE(engine.has_tick_subscription(second.c_str()));
    EXPECT_EQ(ctx->subscribed, "Market data subscribed: " + std::string(128, 'x') + "...");
}

TEST_F(R42CtaPersistTest, MaximumSupportedDetailAndSignalTagsRestoreAndExecute)
{
    const std::string tag(31, 't');
    const std::string json = std::string(R"json({
      "positions":[{"code":"SHFE.rb.2510","volume":2,"closeprofit":0,"dynprofit":0,
        "lastentertime":1,"lastexittime":0,"details":[{"long":true,"price":3000,"volume":2,
        "opentime":1,"profit":0,"maxprofit":0,"maxloss":0,"opentag":")json") + tag + R"json("},{"volume":0}]}],
      "conditions":{"settime":0,"items":{}},
      "signals":{"SHFE.rb.2510":{"usertag":")json" + tag + R"json(","volume":3,"sigprice":3000,"gentime":2}}
    })json";
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), json));
    ctx->on_init();
    ASSERT_EQ(ctx->errorCount, 0u);
    EXPECT_DOUBLE_EQ(ctx->stored_position(kR42Code), 2.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag(kR42Code), tag.c_str());
    feed(kR42Code, 3500.0);
    EXPECT_DOUBLE_EQ(ctx->stored_position(kR42Code), 3.0);
    EXPECT_STREQ(ctx->stra_get_last_entertag(kR42Code), tag.c_str());
}

TEST_F(R42CtaPersistTest, LookupFailureWhilePreparingSnapshotDoesNotPublishCandidate)
{
    const char* baseline = R"json({
      "fund":{"total_profit":1,"total_dynprofit":2,"total_fees":3,"tdate":20250110},
      "positions":[{"code":"SHFE.rb.2510","volume":2,"closeprofit":1,"dynprofit":2,
        "lastentertime":1,"lastexittime":0,"details":[{"long":true,"price":3000,"volume":2,
        "opentime":1,"profit":2,"maxprofit":3,"maxloss":-1,"opentag":"initial"}]}],
      "conditions":{"settime":111,"items":{"SHFE.rb.2510":[{"usertag":"initial",
        "field":0,"alg":0,"target":3500,"qty":2,"action":0}]}},
      "signals":{"SHFE.rb.2510":{"usertag":"initial","volume":3,"sigprice":3000,"gentime":2}},
      "utils":{"lastbarno":7}
    })json";
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), baseline));
    ctx->on_init();
    ASSERT_EQ(ctx->position_count(), 1u);
    ASSERT_TRUE(engine.has_tick_subscription(kR42Code));
    ASSERT_FALSE(engine.has_tick_subscription(kR42T1Code));

    // The first position is applied to the candidate before the second
    // contract lookup throws. Neither it nor the changed fund may be published.
    const char* candidate = R"json({
      "fund":{"total_profit":8,"total_dynprofit":9,"total_fees":10,"tdate":20250111},
      "positions":[
        {"code":"SHFE.rb.2510","volume":8,"closeprofit":8,"dynprofit":9,
         "lastentertime":10,"lastexittime":11,"details":[]},
        {"code":"SHFE.ag.2512","volume":4,"closeprofit":8,"dynprofit":9,
         "lastentertime":10,"lastexittime":11,"details":[]}],
      "conditions":{"settime":222,"items":{}},
      "signals":{"SHFE.ag.2512":{"usertag":"candidate","volume":5,"sigprice":3000,"gentime":2}},
      "utils":{"lastbarno":8}
    })json";
    ASSERT_TRUE(r42_write_file(data_file("r42_cta_persist"), candidate));
    bd.throwOnAg = true;
    EXPECT_THROW(ctx->on_init(), std::runtime_error);
    bd.throwOnAg = false;
    EXPECT_EQ(ctx->position_count(), 1u);
    EXPECT_DOUBLE_EQ(ctx->stored_position(kR42Code), 2.0);
    EXPECT_EQ(ctx->signal_count(), 1u);
    EXPECT_DOUBLE_EQ(ctx->stored_signal(kR42Code), 3.0);
    EXPECT_EQ(ctx->condition_count(), 1u);
    EXPECT_EQ(ctx->condition_time(), 111u);
    EXPECT_EQ(ctx->last_bar(), 7u);
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(3), 3.0);
    EXPECT_FALSE(engine.has_tick_subscription(kR42T1Code));
}

// 覆盖 L647-677 on_bar/on_init、L937-1016 on_schedule、L1966-2062 图表接口（成功分支）
TEST_F(R42CtaPersistTest, ScheduleEmitsCalculateAndChartWrites)
{
    ctx->on_init();

    ctx->register_index("r42idx", 1);
    EXPECT_TRUE(ctx->register_index_line("r42idx", "ma", 1));
    EXPECT_TRUE(ctx->add_index_baseline("r42idx", "zero", 0.0));
    ctx->set_chart_kline(kR42Code, "m1");
    ctx->set_main_key("SHFE.rb.2510#m1");
    ctx->emit_chart = true;

    ctx->stra_sub_bar_events(kR42Code, "m1");
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.close = 3500.0;
    // on_bar 的 period 参数是基础周期（引擎传入 "m" 与 times=1 组合成 key "m1"），
    // 传 "m1" 会被拼成 "m11"，与 _main_key/订阅表都对不上。
    ctx->on_bar(kR42Code, "m", 1, &bar);
    EXPECT_EQ(ctx->bar_closes, 1u);
    EXPECT_EQ(ctx->last_bar_period, "m1");

    EXPECT_TRUE(ctx->on_schedule(20250110, 931));
    EXPECT_EQ(ctx->calc_times, 1u);
    EXPECT_EQ(ctx->last_calc.first, 20250110u);
    EXPECT_EQ(ctx->last_calc.second, 931u);
    EXPECT_TRUE(ctx->idx_value_ok);
    EXPECT_FALSE(ctx->idx_line_missing_ok);

    // 主 K 线未闭合时调度直接中断，不再触发 on_calculate
    EXPECT_FALSE(ctx->on_schedule(20250110, 932));
    EXPECT_EQ(ctx->calc_times, 1u);

    // 重复调度在非主 K 线闭合时保持不触发
    for (int i = 0; i < 18; ++i)
        ctx->on_schedule(20250110, 933);

    const std::string out_dir = std::string(WtHelper::getOutputDir()) + "r42_cta_persist/";
    EXPECT_FALSE(r42_read_file(out_dir + "marks.csv").empty());
    EXPECT_FALSE(r42_read_file(out_dir + "indice.csv").empty());

    ctx->dump_chart();
    const std::string chart = r42_read_file(out_dir + "rtchart.json");
    EXPECT_FALSE(chart.empty());
    EXPECT_NE(chart.find("r42idx"), std::string::npos);
    EXPECT_NE(chart.find("\"ma\""), std::string::npos);
}

// 覆盖 L1972-1978 add_chart_mark(mark 非调度期)、L1998-2050 图表注册失败分支
TEST_F(R42CtaPersistTest, ChartApisRejectUseOutsideScheduleAndUnknownNames)
{
    // 非调度期：只记错误日志并返回
    ctx->add_chart_mark(3500.0, "buy", "late");
    EXPECT_FALSE(ctx->set_index_value("r42idx", "ma", 1.0));

    EXPECT_FALSE(ctx->register_index_line("no_such_index", "ma", 1));
    EXPECT_FALSE(ctx->add_index_baseline("no_such_index", "zero", 0.0));
    EXPECT_FALSE(ctx->set_index_value("no_such_index", "ma", 1.0));

    // 已注册指标但线不存在（仍在非调度期，先命中调度检查）
    ctx->register_index("r42idx", 1);
    EXPECT_FALSE(ctx->set_index_value("r42idx", "ma", 1.0));
}

// 覆盖 L219-244 save_userdata、L1018-1039 on_session_begin、L1746-1759 用户数据接口
TEST_F(R42CtaPersistTest, SessionBeginReleasesFrozenAndPersistsUserData)
{
    ctx->on_init();

    // T+1 品种：开仓即冻结，可用持仓为 0（限价条件单驱动，成交价 4995）
    open_long_by_limit(kR42T1Code, 3, "T1", 5000.0);
    ASSERT_DOUBLE_EQ(ctx->stra_get_position(kR42T1Code), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42T1Code, true), 0.0);

    ctx->stra_save_user_data("flag", "on");
    EXPECT_STREQ(ctx->stra_load_user_data("flag"), "on");

    // 新交易日释放昨日冻结，同时把脏用户数据落盘
    ctx->on_session_begin(20250111);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42T1Code, true), 3.0);

    const std::string ud = r42_read_file(ud_file("r42_cta_persist"));
    EXPECT_NE(ud.find("\"flag\""), std::string::npos);

    R42CtaCtx reloaded(&engine, "r42_cta_persist", 0);
    reloaded.on_init();
    EXPECT_STREQ(reloaded.stra_load_user_data("flag"), "on");
}

// 覆盖 L752-793 update_dyn_profit、L1867-1964 均价/盈亏/明细查询
TEST_F(R42CtaPersistTest, PositionAccountingQueriesReactToTicks)
{
    ctx->on_init();
    open_long_by_limit(kR42Code, 2, "A", 3400.0); // 成交价 3395
    feed(kR42Code, 3700.0);                        // 更新浮动盈亏与极值

    // (3700-3395)*2*10 = 6100
    EXPECT_DOUBLE_EQ(ctx->stra_get_position_profit(kR42Code), 6100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 0), 6100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 1), 6100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 2), 3700.0);

    set_target_by_limit(kR42Code, 5, "B", 3600.0); // 成交价 3595，仓位 3 手
    // 最后一跳 3595：A 的浮动盈亏回到 (3595-3395)*2*10 = 4000，极值仍保留
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 0), 4000.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 1), 6100.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", -1), 0.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 2), 3700.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", -2), 3395.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_detail_profit(kR42Code, "A", 9), 0.0);

    EXPECT_DOUBLE_EQ(ctx->stra_get_position_avgpx(kR42Code), (3395.0 * 2 + 3595.0 * 3) / 5.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42Code, false, "B"), 3.0);
    EXPECT_DOUBLE_EQ(ctx->stra_get_position(kR42Code, false, "none"), 0.0);

    // 资金接口全部分支
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(0),
        ctx->stra_get_fund_data(1) - ctx->stra_get_fund_data(3) + ctx->stra_get_fund_data(2));
    EXPECT_DOUBLE_EQ(ctx->stra_get_fund_data(7), 0.0);
}

// 覆盖 L1684-1744 元信息与日志门面、L1662-1682 订阅、L1332-1350 价格读取
TEST_F(R42CtaPersistTest, LogsSubscriptionsAndMetadataAccessors)
{
    ctx->on_init();
    feed(kR42Code, 3510.0);

    ctx->stra_log_info("r42 info");
    ctx->stra_log_debug("r42 debug");
    ctx->stra_log_warn("r42 warn");
    ctx->stra_log_error("r42 error");

    ctx->stra_sub_ticks(kR42Code);
    ctx->stra_sub_ticks(kR42Code);
    ctx->stra_sub_bar_events(kR42Code, "m5");

    EXPECT_DOUBLE_EQ(ctx->stra_get_price(kR42Code), 3510.0);

    WTSCommodityInfo* comm = ctx->stra_get_comminfo(kR42Code);
    ASSERT_NE(comm, nullptr);
    EXPECT_STREQ(comm->getProduct(), "rb");
    EXPECT_EQ(ctx->stra_get_rawcode(kR42Code), std::string());
    EXPECT_EQ(ctx->stra_get_tdate(), 20250110u);
    EXPECT_EQ(ctx->stra_get_date(), 20250110u);
    EXPECT_EQ(ctx->stra_get_time(), 931u);
}

} // namespace
