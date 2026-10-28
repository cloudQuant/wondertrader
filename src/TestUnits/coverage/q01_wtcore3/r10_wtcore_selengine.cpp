/**
 * r10_wtcore_selengine.cpp —— lane R10 / src/WtCore/WtSelEngine.cpp 可达面
 *
 * 注入方式（全部是被测类自己的 protected/public 成员，不用 #define private public）：
 *   - attachEx() 直接写 WtEngine 的 protected _base_data_mgr/_hot_mgr/_data_mgr
 *     _data_mgr 挂一个真实 WtDtMgr（_reader 恒 NULL，行为由 r10_wtcore_dtmgr.cpp 逐条测过），
 *     这样 WtSelEngine::on_tick 的 L109 不再解引用空指针；
 *   - subTick()/subBar() 写 protected 的 _tick_sub_map/_bar_sub_map（SubOpt 是
 *     std::pair<uint32_t,uint32_t>，first=contextid、second=复权选项 0/1/2）；
 *   - addContext()/getContext() 是公开注册口，_ctx_map 由它填充；
 *   - 替身 ctx 把 virtual on_tick/on_bar 截下来，只证明「引擎按订阅表分发」，
 *     不去跑 SelStraBaseCtx 自己的历史数据链路（那是别的 lane 的文件）。
 *
 * 覆盖映射：
 *   L45-57 release（含基类版本，构造/析构即触发）
 *   L59-83 on_session_end/on_session_begin(_ready=true)/on_init/handle_push_quote
 *   L85-103 on_bar：空订阅表 ⇒ 只走日志；有订阅 ⇒ 分发到 ctx
 *   L105-222 on_tick：WtEngine::on_tick + _data_mgr->handle_push_quote + _exec_mgr 转发
 *                     + _ready 开关下的 0/1/2 复权分发（SUFFIX_QFQ='-'、SUFFIX_HFQ='+'）
 *   L224-337 on_minute_end：nextTime 匹配、上次执行时间、节假日、TPT_None/Minute/Monthly/
 *                     Weekly/Yearly 的 bIgnore 判定（bHasHoliday 的两条子分支也打到）
 *   L382-425 init 之外的 addContext/getContext 三个分支
 *   L427-488 handle_pos_change（过滤器放行 → _pos_map 建条目 → append_signal）
 *   L490-509 get_comm_info/get_sess_info/get_real_time
 *
 * 不可达 / 崩溃类（见 bugs/R10_wtcore.md）：
 *   L328-335 任务派发：StdThreadPtr thrd(new StdThread(...)) 是局部对象，std::thread 析构时
 *            仍处于 joinable ⇒ std::terminate（TPT_Daily 到点必炸，已收 DISABLED_）
 *   L339-380 run()：_cfg/_data_mgr->reader()/WtSelRtTicker/_adapter_mgr 全依赖 init + 插件
 *   L382-388 init：转调 WtEngine::init（线程 + DLL 缝，R3 已登记，本 lane 不硬闯）
 */
#include "q01_sel_env.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <Includes/IHotMgr.h>
#include <Includes/IBaseDataMgr.h>
#include <Includes/WTSVariant.hpp>
#include <WtCore/SelStraBaseCtx.h>
#include <WtCore/WtDtMgr.h>
#include <WtCore/WtSelEngine.h>

using namespace q01sel;

namespace {

USING_NS_WTP;

static const char* kAg = "SHFE.ag.2512";
static const char* kAu = "SHFE.au.2212";
static const char* kStock = "SSE.600000";

/** 任务派发用的截流替身：只记录回调，不跑 SelStraBaseCtx 的历史数据链路 */
class DispatchCtx : public SelStraBaseCtx
{
public:
    DispatchCtx(WtSelEngine* e, const char* name) : SelStraBaseCtx(e, name, 0) {}
    virtual ~DispatchCtx() {}

    virtual void on_bar_close(const char* stdCode, const char* period, WTSBarStruct* newBar) override
    {
        (void)stdCode; (void)period; (void)newBar;
        ++n_bar_close;
    }

    virtual void on_tick_updated(const char* stdCode, WTSTickData* newTick) override
    {
        (void)stdCode; (void)newTick;
        ++n_tick_upd;
    }

    virtual void on_strategy_schedule(uint32_t curDate, uint32_t curTime) override
    {
        (void)curDate; (void)curTime;
        ++n_sched;
    }

    virtual void on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar) override
    {
        (void)times; (void)newBar;
        ++n_on_bar;
        _bar_key = std::string(stdCode) + "-" + period;
    }

    virtual void on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy = true) override
    {
        (void)newTick; (void)bEmitStrategy;
        ++n_on_tick;
        _tick_code = stdCode;
    }

    uint32_t n_bar_close = 0;
    uint32_t n_tick_upd = 0;
    uint32_t n_sched = 0;
    uint32_t n_on_bar = 0;
    uint32_t n_on_tick = 0;
    std::string _bar_key;
    std::string _tick_code;
};

/** 节假日替身：H0 恒为假日；H1 只有 20250109 是假日（用来打「顺延」分支） */
class HolidayBdm : public BaseDataMgr
{
public:
    virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
    {
        (void)isTpl;
        ++n_holiday;
        std::string tpl(pid);
        if (tpl == "H0")
            return true;
        if (tpl == "H1")
            return uDate == 20250109;
        return false;
    }
    uint32_t n_holiday = 0;
};

/** 把 WtEngine 的 protected 订阅表/数据管理器/就绪位开出来 */
class R10SelEngine : public WtSelEngine
{
public:
    void attachEx(IBaseDataMgr* bd, IHotMgr* hot, WtDtMgr* dm)
    {
        _base_data_mgr = bd;
        _hot_mgr = hot;
        _data_mgr = dm;
        set_date_time(20250110, 931, 0, 0);
        set_trading_date(20250110);
    }

    void subTick(uint32_t sid, const char* stdCode, uint32_t opt)
    {
        _tick_sub_map[stdCode][sid] = SubOpt(sid, opt);
    }

    void subBar(uint32_t sid, const char* stdCode, const char* period, uint32_t times, uint32_t opt)
    {
        std::string key = std::string(stdCode) + "-" + period + "-" + std::to_string(times);
        _bar_sub_map[key][sid] = SubOpt(sid, opt);
    }

    void setReady(bool v) { _ready = v; }
    bool isReady() { return _ready; }

    bool hasPos(const char* stdCode) { return _pos_map.find(stdCode) != _pos_map.end(); }

    double sigVolume(const char* stdCode)
    {
        auto it = _sig_map.find(stdCode);
        return it == _sig_map.end() ? -1.0 : it->second._volume;
    }

    uint64_t priceOf(const char* stdCode)
    {
        auto it = _price_map.find(stdCode);
        return it == _price_map.end() ? 0 : (uint64_t)(it->second * 100);
    }
};

class R10SelEngineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        _env.reset(new Q01TempEnv("r10sel"));
        _bdm.regisSession("TRADING");
        _sess = _bdm.session("TRADING");
        _comm = makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future);
        _ct = makeCt("SHFE", "ag", "ag2512");
        _bdm.regis(_comm, _ct);
        _comm2 = makeComm("SHFE", "au", "TRADING", TM_Long, CC_Future);
        _ct2 = makeCt("SHFE", "au", "au2212");
        _bdm.regis(_comm2, _ct2);

        _dtmgr.reset(new WtDtMgr());
        _eng.attachEx(&_bdm, &_hot, _dtmgr.get());
    }

    void TearDown() override
    {
        _ctx.reset();
        _dtmgr.reset();
        _env.reset();
    }

    /** 注册一个截流 ctx，返回它的裸指针（所有权：引擎 _ctx_map + 本夹具的 _ctx 共享同一个引用） */
    DispatchCtx* addCtx(const char* name, uint32_t day, uint32_t time, TaskPeriodType period,
        const char* trdtpl = "CHINA", const char* session = "TRADING")
    {
        DispatchCtx* raw = new DispatchCtx(&_eng, name);
        SelContextPtr keep(raw);      // 只有一个引用计数族，避免双shared_ptr重复释放
        _ctx = keep;
        _eng.addContext(keep, day, time, period, true, trdtpl, session);
        return raw;
    }

    std::unique_ptr<Q01TempEnv> _env;
    std::unique_ptr<WtDtMgr> _dtmgr;
    BaseDataMgr _bdm;
    HotMgr _hot;
    R10SelEngine _eng;
    SelContextPtr _ctx;
    WTSSessionInfo* _sess = NULL;
    WTSCommodityInfo* _comm = NULL;
    WTSContractInfo* _ct = NULL;
    WTSCommodityInfo* _comm2 = NULL;
    WTSContractInfo* _ct2 = NULL;
};

} // namespace

TEST_F(R10SelEngineTest, not_ready_until_session_begin)
{
    EXPECT_FALSE(_eng.isReady());
    _eng.on_init();                 // L73-77：无监听者，只走一次判空
    EXPECT_FALSE(_eng.isReady());
    _eng.on_session_end();          // L59-63：无监听者
    EXPECT_FALSE(_eng.isReady());
    _eng.on_session_begin();        // L65-71：_ready = true
    EXPECT_TRUE(_eng.isReady());
    // _tm_ticker 为空 ⇒ handle_push_quote 只走判空（L79-83）
    _eng.handle_push_quote(NULL);
    EXPECT_TRUE(_eng.isReady());
}

TEST_F(R10SelEngineTest, comm_sess_and_real_time_from_engine_members)
{
    EXPECT_EQ(_eng.get_comm_info(kAg), _comm);
    EXPECT_EQ(_eng.get_comm_info(kAu), _comm2);
    EXPECT_EQ(_eng.get_comm_info(kStock), nullptr);   // 未登记的品种

    // get_sess_info：未知品种返回 NULL，已知品种返回品种挂着的时段对象
    EXPECT_EQ(_eng.get_sess_info(kStock), nullptr);
    EXPECT_EQ(_eng.get_sess_info(kAg), _comm->getSessionInfo());

    // L506-509：makeTime(_cur_date, _cur_raw_time*100000 + _cur_secs)
    // attachEx 里 raw=0、secs=0 ⇒ 秒字段全 0
    EXPECT_EQ(_eng.get_real_time(), 20250110000000000ull);
    _eng.set_date_time(20250110, 931, 45, 931);
    // 931*100000 + 45 = 93100045 ⇒ 20250110 093100045
    EXPECT_EQ(_eng.get_real_time(), 20250110093100045ull);
}

TEST_F(R10SelEngineTest, add_context_registers_and_duplicate_is_rejected)
{
    // ctx==NULL 早退（L392-393）
    _eng.addContext(SelContextPtr(), 20250110, 931, TPT_None);
    EXPECT_EQ(_eng.getContext(99999u), nullptr);

    DispatchCtx* raw = addCtx("s_add", 20250110, 931, TPT_None);
    ASSERT_GT(raw->id(), 0u);
    SelContextPtr got = _eng.getContext(raw->id());
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got.get(), raw);      // _ctx_map[sid] 由 addContext 写入（L415）

    // 同一个 id 重复注册 => L396-400 报重复并直接返回
    _eng.addContext(SelContextPtr(raw), 20250110, 940, TPT_None);
    SelContextPtr again = _eng.getContext(raw->id());
    EXPECT_EQ(again.get(), raw);
}

TEST_F(R10SelEngineTest, on_bar_without_subscription_only_logs)
{
    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.time = 202501100931;
    bar.open = 7100.0;
    bar.high = 7110.0;
    bar.low = 7090.0;
    bar.close = 7105.0;
    bar.vol = 120;

    // 订阅表为空 => 循环体不进，只打 L102 的日志（period[0]=='m' ⇒ 取 bar->time）
    _eng.on_bar(kAg, "m", 1, &bar);
    EXPECT_EQ(_eng.getContext(1u), nullptr);
}

TEST_F(R10SelEngineTest, on_bar_dispatches_only_to_subscribed_context)
{
    DispatchCtx* raw = addCtx("s_bar", 20250110, 931, TPT_None);
    // 引擎用 "{}-{}-{}" 组 key，这里必须用完全一致的 key 订阅
    _eng.subBar(raw->id(), kAg, "m", 1, 0);

    WTSBarStruct bar;
    memset(&bar, 0, sizeof(bar));
    bar.time = 202501100932;
    bar.date = 20250110;
    bar.close = 7111.0;
    bar.vol = 10;

    _eng.on_bar(kAg, "m", 1, &bar);
    EXPECT_EQ(raw->n_on_bar, 1u);
    EXPECT_EQ(raw->_bar_key, std::string("SHFE.ag.2512-m"));

    // 日线的日志分支取 bar->date（L102）
    _eng.on_bar(kAg, "d", 1, &bar);
    EXPECT_EQ(raw->n_on_bar, 1u);   // 没订阅 "…-d-1" => 不再分发

    // 订阅过但 ctx 已被摘掉的情形：用未注册的 sid
    _eng.subBar(888888u, kAg, "m", 1, 0);
    _eng.on_bar(kAg, "m", 1, &bar);
    EXPECT_EQ(raw->n_on_bar, 1u);
}

TEST_F(R10SelEngineTest, on_tick_pushes_data_and_dispatches_raw_code)
{
    DispatchCtx* raw = addCtx("s_tick", 20250110, 931, TPT_None);
    _eng.subTick(raw->id(), kAg, 0);
    _eng.setReady(true);

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93100, 7123.0);
    ASSERT_NE(tk, nullptr);
    _eng.on_tick(kAg, tk);
    EXPECT_EQ(raw->n_on_tick, 1u);
    EXPECT_EQ(raw->_tick_code, std::string(kAg));   // opt==0 ⇒ 原始代码直通（L160-163）

    // tick 已被 WtEngine::on_tick 记进 _price_map，并被 _data_mgr 缓存
    EXPECT_GT(_eng.priceOf(kAg), 0ull);
    WTSTickData* cached = _dtmgr->grab_last_tick(kAg);
    ASSERT_NE(cached, nullptr);
    EXPECT_DOUBLE_EQ(cached->getTickStruct().price, 7123.0);
    cached->release();

    // 未订阅的合约：命中 _tick_sub_map.find==end，不分发（L143-144）
    _eng.on_tick(kAu, tk);
    EXPECT_EQ(raw->n_on_tick, 1u);
    tk->release();
}

TEST_F(R10SelEngineTest, on_tick_adjusting_suffixes_for_qfq_and_hfq)
{
    DispatchCtx* raw = addCtx("s_adj", 20250110, 931, TPT_None);
    _eng.subTick(raw->id(), kAg, 1);       // 前复权
    _eng.subTick(raw->id(), kAu, 2);       // 后复权
    _eng.setReady(true);

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93101, 7124.0);
    ASSERT_NE(tk, nullptr);
    _eng.on_tick(kAg, tk);
    // SUFFIX_QFQ 是字符 '-'（WtSelEngine.cpp L167 的三元式两边都是 char）
    EXPECT_EQ(raw->_tick_code, std::string(kAg) + std::string(1, '-'));

    WTSTickData* tkAu = makeTick("SHFE", "au2212", 20250110, 20250110, 93102, 480.0);
    ASSERT_NE(tkAu, nullptr);
    _eng.on_tick(kAu, tkAu);
    // 后复权分支：临时 newTick 已 release，ctx 只留下带 '+' 的代码（L172-212）
    // SUFFIX_HFQ 是字符 '+'
    EXPECT_EQ(raw->_tick_code, std::string(kAu) + std::string(1, '+'));
    EXPECT_EQ(raw->n_on_tick, 2u);
    tk->release();
    tkAu->release();
}

TEST_F(R10SelEngineTest, on_tick_ignored_before_ready_flag)
{
    DispatchCtx* raw = addCtx("s_notready", 20250110, 931, TPT_None);
    _eng.subTick(raw->id(), kAg, 0);
    EXPECT_FALSE(_eng.isReady());

    WTSTickData* tk = makeTick("SHFE", "ag2512", 20250110, 20250110, 93103, 7125.0);
    ASSERT_NE(tk, nullptr);
    _eng.on_tick(kAg, tk);            // L141 的 _ready 为 false ⇒ 整个订阅块跳过
    EXPECT_EQ(raw->n_on_tick, 0u);
    // 但行情仍然入库了（L109）
    WTSTickData* cached = _dtmgr->grab_last_tick(kAg);
    ASSERT_NE(cached, nullptr);
    EXPECT_DOUBLE_EQ(cached->getTickStruct().price, 7125.0);
    cached->release();
    tk->release();
}

TEST_F(R10SelEngineTest, minute_end_skips_tasks_by_time_and_holiday)
{
    DispatchCtx* raw = addCtx("s_time", 20250110, 931, TPT_None);
    // curTime=930 => nextTime=931；_time=1400 的任务先被 L236-237 跳过
    _eng.addContext(SelContextPtr(new DispatchCtx(&_eng, "s_other")), 20250110, 1400, TPT_Daily);

    // 931 命中的任务：TPT_None => bIgnore 保持 true（L263-264）
    _eng.on_minute_end(20250110, 930);
    EXPECT_EQ(raw->n_sched, 0u);

    // 已经执行过同一分钟 => L240-241 跳过
    _eng.on_minute_end(20250110, 930);
    EXPECT_EQ(raw->n_sched, 0u);
}

TEST_F(R10SelEngineTest, minute_end_holiday_task_is_skipped)
{
    HolidayBdm bdm;
    bdm.regisSession("TRADING");
    bdm.regis(makeComm("SHFE", "ag", "TRADING", TM_Both, CC_Future), makeCt("SHFE", "ag", "ag2512"));
    R10SelEngine eng;
    WtDtMgr dm;
    eng.attachEx(&bdm, &_hot, &dm);

    DispatchCtx* raw = new DispatchCtx(&eng, "s_hol");
    SelContextPtr keep(raw);
    // trdtpl="H0" => isHoliday 恒真 ⇒ L243-244 continue
    eng.addContext(keep, 20250110, 931, TPT_Daily, true, "H0", "TRADING");
    eng.on_minute_end(20250110, 930);
    EXPECT_EQ(raw->n_sched, 0u);
    EXPECT_GE(bdm.n_holiday, 1u);

    // "H1" 只有 20250109 是假日 ⇒ 走 while 顺延块（L247-256）+ TPT_None 不触发
    DispatchCtx* raw2 = new DispatchCtx(&eng, "s_hol2");
    SelContextPtr keep2(raw2);
    eng.addContext(keep2, 20250110, 935, TPT_None, true, "H1", "TRADING");
    eng.on_minute_end(20250110, 934);
    EXPECT_EQ(raw2->n_sched, 0u);
    EXPECT_GE(bdm.n_holiday, 3u);   // 至少 243、247、250 三处查询
}

TEST_F(R10SelEngineTest, minute_end_period_branches_that_do_not_fire)
{
    // _time 全部设成 931（nextTime=931 才会往下走），_day 取永不相等的值
    DispatchCtx* minute = addCtx("s_min", 999999, 931, TPT_Minute);
    EXPECT_EQ(minute->n_sched, 0u);

    // 同一引擎里再挂月度/周度/年度任务
    DispatchCtx* m = new DispatchCtx(&_eng, "s_mon");
    SelContextPtr keepM(m);
    _eng.addContext(keepM, 999999, 931, TPT_Monthly, true, "CHINA", "TRADING");
    DispatchCtx* w = new DispatchCtx(&_eng, "s_wk");
    SelContextPtr keepW(w);
    _eng.addContext(keepW, 99, 931, TPT_Weekly, true, "CHINA", "TRADING");
    DispatchCtx* y = new DispatchCtx(&_eng, "s_yr");
    SelContextPtr keepY(y);
    _eng.addContext(keepY, 9999, 931, TPT_Yearly, true, "CHINA", "TRADING");

    // addContext 只保留最后一个 ctx（_ctx_map 按 id 覆盖），这里逐个把订阅表填上
    _eng.on_minute_end(20250110, 930);
    EXPECT_EQ(m->n_sched, 0u);
    EXPECT_EQ(w->n_sched, 0u);
    EXPECT_EQ(y->n_sched, 0u);
    EXPECT_EQ(minute->n_sched, 0u);
}

TEST_F(R10SelEngineTest, handle_pos_change_creates_pos_and_signal)
{
    // 无过滤器 => is_filtered_by_strategy 返回 false，走到记账（L430-437）
    _eng.handle_pos_change("s1", kAg, 3);
    EXPECT_TRUE(_eng.hasPos(kAg));          // L461-464 建条目分支
    EXPECT_DOUBLE_EQ(_eng.sigVolume(kAg), 3.0);   // 0 + 3，append_signal 记的是目标仓位

    // 再进一笔 -1 => _pos_map 里 _volume 仍是 0（这里只累加信号，不落仓）
    _eng.handle_pos_change("s1", kAg, -1);
    EXPECT_DOUBLE_EQ(_eng.sigVolume(kAg), 2.0);

    // 另一合约走「查不到条目 ⇒ 新建」分支
    _eng.handle_pos_change("s2", kAu, 5);
    EXPECT_TRUE(_eng.hasPos(kAu));
    EXPECT_DOUBLE_EQ(_eng.sigVolume(kAu), 5.0);
    EXPECT_DOUBLE_EQ(_eng.sigVolume(kAg), 2.0);
}

// 触发条件：任务到期（TPT_Daily 且 _time==nextTime、非假日、未执行过）时，
// L330 的 StdThreadPtr thrd(new StdThread(...)) 是循环体内的局部对象，
// std::thread 析构时仍处于 joinable ⇒ std::terminate 直接 abort 整个测试进程。
// 规则 2 禁止起线程，故整条 DISABLED_，缺陷登记为 BUG-R10-006。
TEST_F(R10SelEngineTest, DISABLED_minute_end_fires_task_thread_terminates_process)
{
    DispatchCtx* raw = addCtx("s_fire", 20250110, 931, TPT_Daily);
    _eng.on_minute_end(20250110, 930);   // ⇒ std::terminate
    EXPECT_EQ(raw->n_sched, 1u);
}
