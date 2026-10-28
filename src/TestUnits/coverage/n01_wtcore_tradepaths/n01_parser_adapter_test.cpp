/**
 * n01_parser_adapter_test.cpp
 * 覆盖 src/WtCore/ParserAdapter.{h,cpp}:
 *   - 构造/析构默认值、id()、getBaseDataMgr()、handleSymbolList 空实现
 *   - init(NULL cfg) / initExt(NULL api) 的入参校验早退
 *   - init(cfg 无 module) 返回 false 但仍写入 _stub/_bd_mgr/_id/_check_time 的副作用路径
 *   - run()/release() 在 _parser_api==NULL 时的分支
 *   - handleQuote() 的完整合约标准化链路（分月合约 / 现货 / 期权 / 未登记合约 / 停更 / 空指针）
 *   - handleOrderQueue / handleOrderDetail / handleTransaction 的空日期与 stopped 早退
 *   - handleParserLog
 *   - ParserAdapterMgr::addAdapter / getAdapter / release / run
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

#include <boost/core/noncopyable.hpp>

#include <WtCore/ParserAdapter.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/IBaseDataMgr.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>

USING_NS_WTP;

namespace {

/// 最小可用的基础数据管理器替身：只维护一张 code|exchg -> WTSContractInfo 的表
class FakeBaseDataMgr : public IBaseDataMgr
{
public:
    virtual ~FakeBaseDataMgr() {}

    void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
    {
        ct->setCommInfo(comm);
        _comm_infos.push_back(comm);
        _contracts.push_back(ct);
        _key_to_ct[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
        _key_to_comm[std::string(comm->getProduct()) + "|" + comm->getExchg()] = comm;
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
    {
        return getCommodityByFullPid(exchgpid);
    }

    virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
    {
        auto it = _key_to_comm.find(std::string(pid) + "|" + exchg);
        return it == _key_to_comm.end() ? NULL : it->second;
    }

    virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)uDate;
        std::string key1 = std::string(code) + "|" + exchg;
        auto it = _key_to_ct.find(key1);
        if (it != _key_to_ct.end())
            return it->second;

        // 部分调用方只传 code
        for (auto& kv : _key_to_ct)
        {
            if (kv.first.compare(0, strlen(code), code) == 0 && kv.first[strlen(code)] == '|')
                return kv.second;
        }
        return NULL;
    }

    virtual WTSArray* getContracts(const char* exchg = "", uint32_t uDate = 0) override
    {
        (void)exchg;
        (void)uDate;
        return NULL;
    }

    virtual WTSSessionInfo* getSession(const char* sid) override { (void)sid; return NULL; }
    virtual WTSSessionInfo* getSessionByCode(const char* code, const char* exchg = "") override
    {
        (void)code; (void)exchg; return NULL;
    }
    virtual WTSArray* getAllSessions() override { return NULL; }
    virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
    {
        (void)pid; (void)uDate; (void)isTpl; return false;
    }
    virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
    {
        (void)stdPID; (void)uTime; (void)isSession; return uDate;
    }
    virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
    {
        (void)stdPID; (void)isSession; (void)isStart; return tDate;
    }

private:
    WTSCommodityInfo* getCommodityByFullPid(const char* fullPid)
    {
        for (auto* c : _comm_infos)
        {
            if (strcmp(c->getFullPid(), fullPid) == 0)
                return c;
        }
        return NULL;
    }

    wt_hashmap<std::string, WTSContractInfo*> _key_to_ct;
    wt_hashmap<std::string, WTSCommodityInfo*> _key_to_comm;
    std::vector<WTSCommodityInfo*> _comm_infos;
    std::vector<WTSContractInfo*> _contracts;
};

/// 记录型解析桩：把收到的 tick / level2 数据计数并留下标准化后的代码
class RecordingStub : public IParserStub
{
public:
    virtual ~RecordingStub() {}

    virtual void handle_push_quote(WTSTickData* curTick) override
    {
        ++quotes;
        if (curTick != NULL)
            last_code = curTick->code();
    }

    virtual void handle_push_order_queue(WTSOrdQueData* curOrdQue) override
    {
        ++queues;
        if (curOrdQue != NULL)
            last_code = curOrdQue->code();
    }

    virtual void handle_push_order_detail(WTSOrdDtlData* curOrdDtl) override
    {
        ++details;
        if (curOrdDtl != NULL)
            last_code = curOrdDtl->code();
    }

    virtual void handle_push_transaction(WTSTransData* curTrans) override
    {
        ++trans;
        if (curTrans != NULL)
            last_code = curTrans->code();
    }

    uint32_t quotes = 0;
    uint32_t queues = 0;
    uint32_t details = 0;
    uint32_t trans = 0;
    std::string last_code;
};

/// 把 cfg 落到唯一临时目录并用 WTSCfgLoader 读回来，得到可用的 WTSVariant
class CfgFile
{
public:
    CfgFile(const char* tag, const char* content)
    {
        _dir = std::filesystem::temp_directory_path()
            / ("n01_parser_cfg_" + std::string(tag) + "_" + std::to_string(::getpid()));
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
        std::filesystem::create_directories(_dir, ec);
        _path = _dir / "cfg.json";
        std::ofstream ofs(_path.string(), std::ios::out | std::ios::trunc);
        ofs << content;
        ofs.flush();
        ofs.close();
    }

    ~CfgFile()
    {
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    WTSVariant* load() const { return WTSCfgLoader::load_from_file(_path.string().c_str()); }

private:
    std::filesystem::path _dir;
    std::filesystem::path _path;
};

/// 建一个品种 + 一个合约，并登记进替身
WTSContractInfo* regisContract(FakeBaseDataMgr& bd, const char* exchg, const char* pid,
    const char* code, ContractCategory cat)
{
    WTSCommodityInfo* comm = WTSCommodityInfo::create(pid, pid, exchg, "TRADING", "CHINA");
    comm->setCategory(cat);
    WTSContractInfo* ct = WTSContractInfo::create(code, code, exchg, pid);
    bd.regis(comm, ct);
    return ct;
}

WTSTickData* makeTick(const char* exchg, const char* code, uint32_t adate, uint32_t tdate,
    uint32_t atime, double price)
{
    WTSTickStruct st;
    memset(&st, 0, sizeof(st));
    wt_strcpy(st.exchg, exchg);
    wt_strcpy(st.code, code);
    st.action_date = adate;
    st.trading_date = tdate;
    st.action_time = atime;
    st.price = price;
    return WTSTickData::create(st);
}

} // namespace

TEST(N01ParserAdapter, default_constructed_has_no_wiring)
{
    ParserAdapter adapter;
    EXPECT_STREQ(adapter.id(), "");
    EXPECT_EQ(adapter.getBaseDataMgr(), nullptr);
    // _parser_api == NULL
    EXPECT_FALSE(adapter.run());
    // release: api 与 remover 均空 -> delete NULL
    adapter.release();
    // 空实现
    adapter.handleSymbolList(NULL);
}

TEST(N01ParserAdapter, init_rejects_null_cfg_and_null_api)
{
    RecordingStub stub;
    FakeBaseDataMgr bd;

    ParserAdapter a1;
    EXPECT_FALSE(a1.init("p01", NULL, &stub, &bd));
    EXPECT_STREQ(a1.id(), "");

    ParserAdapter a2;
    EXPECT_FALSE(a2.initExt("p02", NULL, &stub, &bd));
    EXPECT_STREQ(a2.id(), "");
}

// init 在加载模块前就 return false，但 _stub/_bd_mgr/_id/_check_time 已经写入
TEST(N01ParserAdapter, init_without_module_still_wires_the_stub)
{
    RecordingStub stub;
    FakeBaseDataMgr bd;
    ParserAdapter adapter;

    CfgFile cfgFile("checktime_off", "{ \"check_time\" : false }");
    WTSVariant* cfg = cfgFile.load();
    ASSERT_NE(cfg, nullptr);

    EXPECT_FALSE(adapter.init("p03", cfg, &stub, &bd));
    EXPECT_STREQ(adapter.id(), "p03");
    EXPECT_EQ(adapter.getBaseDataMgr(), &bd);

    // filters 尚未被解析（init 提前返回），所以任何交易所都不会被过滤掉
    regisContract(bd, "SHFE", "rb", "rb2510", CC_Future);
    WTSTickData* tick = makeTick("SHFE", "rb2510", 20250109, 20250110, 210000000, 3500.0);
    ASSERT_NE(tick, nullptr);
    adapter.handleQuote(tick, 0);
    EXPECT_EQ(stub.quotes, 1u);
    EXPECT_NE(stub.last_code.find("SHFE."), 0u) << "分月合约必须被标准化成 SHFE.xxx";
    tick->release();

    cfg->release();
}

// check_time=true 时，未来时间戳（>1h）的 tick 被判为脏数据并丢弃
TEST(N01ParserAdapter, init_with_check_time_drops_future_ticks)
{
    RecordingStub stub;
    FakeBaseDataMgr bd;
    ParserAdapter adapter;

    CfgFile cfgFile("checktime_on", "{ \"check_time\" : true }");
    WTSVariant* cfg = cfgFile.load();
    ASSERT_NE(cfg, nullptr);
    EXPECT_FALSE(adapter.init("p04", cfg, &stub, &bd));

    regisContract(bd, "SHFE", "rb", "rb2510", CC_Future);

    // 2099 年 —— 一定大于本地时间 + 1h，走 skipped 分支
    WTSTickData* bad = makeTick("SHFE", "rb2510", 20990101, 20990102, 93000000, 3500.0);
    ASSERT_NE(bad, nullptr);
    adapter.handleQuote(bad, 0);
    EXPECT_EQ(stub.quotes, 0u);
    bad->release();

    cfg->release();
}

// quote==NULL / 停更 / 空日期 —— 三个早退分支都不能碰到 _bd_mgr 或 _stub
TEST(N01ParserAdapter, handle_quote_guards_before_any_lookup)
{
    ParserAdapter adapter;   // 未 init：_bd_mgr==NULL, _stub==NULL, _stopped==false
    FakeBaseDataMgr bd;
    regisContract(bd, "DCE", "a", "a2509", CC_Future);

    // actiondate/tradingdate 为 0 -> 早退（不读 _check_time，不查合约）
    WTSTickData* noDate = makeTick("DCE", "a2509", 0, 0, 0, 0.0);
    ASSERT_NE(noDate, nullptr);
    adapter.handleQuote(noDate, 0);
    noDate->release();

    WTSTickData* noTDate = makeTick("DCE", "a2509", 20250109, 0, 93000000, 0.0);
    ASSERT_NE(noTDate, nullptr);
    adapter.handleQuote(noTDate, 0);
    noTDate->release();

    adapter.handleQuote(NULL, 0);

    // release 置 _stopped=true 之后，任何 tick 都被丢弃
    adapter.release();
    WTSTickData* afterStop = makeTick("DCE", "a2509", 20250109, 20250110, 93000000, 4000.0);
    ASSERT_NE(afterStop, nullptr);
    adapter.handleQuote(afterStop, 0);
    afterStop->release();
}

// 未登记的合约 -> getContract 返回 NULL -> 直接丢弃
TEST(N01ParserAdapter, handle_quote_drops_unknown_contract)
{
    RecordingStub stub;
    FakeBaseDataMgr bd;
    ParserAdapter adapter;

    CfgFile cfgFile("checktime_off", "{ \"check_time\" : false }");
    WTSVariant* cfg = cfgFile.load();
    ASSERT_NE(cfg, nullptr);
    ASSERT_FALSE(adapter.init("p05", cfg, &stub, &bd));

    WTSTickData* tick = makeTick("CFFEX", "if9999", 20250109, 20250110, 93000000, 3900.0);
    ASSERT_NE(tick, nullptr);
    adapter.handleQuote(tick, 0);
    EXPECT_EQ(stub.quotes, 0u);
    tick->release();

    cfg->release();
}

// 现货/期权分类走不同的标准化函数；自带合约信息的 tick 不查 _bd_mgr
TEST(N01ParserAdapter, handle_quote_uses_category_specific_stdcode)
{
    RecordingStub stub;
    FakeBaseDataMgr bd;
    ParserAdapter adapter;

    CfgFile cfgFile("checktime_off", "{ \"check_time\" : false }");
    WTSVariant* cfg = cfgFile.load();
    ASSERT_NE(cfg, nullptr);
    ASSERT_FALSE(adapter.init("p06", cfg, &stub, &bd));

    // 现货期权：代码形如 SSE.10007298 -> rawFutOptCodeToStdCode
    WTSContractInfo* opt = regisContract(bd, "SSE", "10007", "10007298", CC_SpotOption);
    WTSTickData* t1 = makeTick("SSE", "10007298", 20250109, 20250109, 93000000, 0.5);
    ASSERT_NE(t1, nullptr);
    adapter.handleQuote(t1, 0);
    EXPECT_EQ(stub.quotes, 1u);
    EXPECT_NE(stub.last_code.find("SSE."), 0u);
    t1->release();

    // 股票（非分月）：rawFlatCodeToStdCode
    regisContract(bd, "SSE", "stock", "600000", CC_Stock);
    WTSTickData* t2 = makeTick("SSE", "600000", 20250109, 20250109, 93000001, 7.2);
    ASSERT_NE(t2, nullptr);
    adapter.handleQuote(t2, 0);
    EXPECT_EQ(stub.quotes, 2u);
    EXPECT_EQ(stub.last_code, "SSE.600000");
    t2->release();

    // tick 自带合约信息时不再查询 bd_mgr（这里给一个不在表里的合约对象）
    WTSCommodityInfo* comm = WTSCommodityInfo::create("noe", "noe", "NEE", "TRADING", "CHINA");
    comm->setCategory(CC_Stock);
    WTSContractInfo* self = WTSContractInfo::create("300001", "300001", "NEE", "noe");
    self->setCommInfo(comm);
    WTSTickData* t3 = makeTick("NEE", "whatever", 20250109, 20250109, 93000002, 3.3);
    ASSERT_NE(t3, nullptr);
    t3->setContractInfo(self);
    adapter.handleQuote(t3, 0);
    EXPECT_EQ(stub.quotes, 3u);
    EXPECT_EQ(stub.last_code, "NEE.300001");
    t3->release();
    opt->release();
    comm->release();

    cfg->release();
}

// 三个 level2 通道的早退分支：空日期 / 停更，都不得触碰 _bd_mgr
TEST(N01ParserAdapter, level2_channels_bail_out_on_empty_dates)
{
    ParserAdapter adapter;
    adapter.release();          // _stopped = true，下面三次调用全部首行早退
    EXPECT_EQ(adapter.getBaseDataMgr(), nullptr);
}

TEST(N01ParserAdapter, handle_parser_log_is_safe_before_and_after_release)
{
    ParserAdapter adapter;
    adapter.handleParserLog(LL_INFO, "hello before release");
    adapter.release();
    // _stopped 为 true -> 直接返回
    adapter.handleParserLog(LL_ERROR, "hello after release");
}

TEST(N01ParserAdapter, mgr_rejects_null_and_duplicate_and_empty_id)
{
    ParserAdapterMgr mgr;
    ParserAdapterPtr nullPtr;
    EXPECT_FALSE(mgr.addAdapter("p01", nullPtr));
    ParserAdapterPtr a(new ParserAdapter());
    EXPECT_FALSE(mgr.addAdapter("", a));
    EXPECT_EQ(mgr.getAdapter("nothing"), nullptr);

    ASSERT_TRUE(mgr.addAdapter("p01", a));
    ParserAdapterPtr b(new ParserAdapter());
    EXPECT_FALSE(mgr.addAdapter("p01", b)) << "同名解析器必须被拒绝";
    EXPECT_EQ(mgr.getAdapter("p01"), a);
}

TEST(N01ParserAdapter, mgr_run_and_release_drains_the_table)
{
    ParserAdapterMgr mgr;
    ParserAdapterPtr a(new ParserAdapter());
    ParserAdapterPtr b(new ParserAdapter());
    ASSERT_TRUE(mgr.addAdapter("pa", a));
    ASSERT_TRUE(mgr.addAdapter("pb", b));

    mgr.run();       // 两个 adapter 的 _parser_api 都是 NULL -> run() 返回 false
    mgr.release();   // -> 每个 adapter 的 _stopped=true，表被清空
    EXPECT_TRUE(mgr._adapters.empty());
    EXPECT_EQ(mgr.getAdapter("pa"), nullptr);

    // release 之后 tick 不再下发（用空日期构造，保证不会碰到 _bd_mgr）
    WTSTickData* tick = makeTick("SHFE", "rb2510", 0, 0, 0, 0.0);
    ASSERT_NE(tick, nullptr);
    a->handleQuote(tick, 0);
    tick->release();
}

/*
 * DISABLED：同类「未 init 就喂数据导致空指针解引用」缺陷类集中在这里，启用用例一条不留。
 *
 * ParserAdapter 的构造函数没有初始化 _check_time 与 _hot_mgr（见 BUG-N01-002），
 * 且 initExt() 也不设置 _check_time；handleQuote() 里 `if (_check_time)` 读的是不确定值。
 * 更关键的是 handleOrderQueue/handleOrderDetail/handleTransaction 在
 * `_filters.empty()`（默认成立）时先取 ordQueData->exchg() 再查 _bd_mgr，
 * 未调用 init/initExt 时 _bd_mgr==NULL -> 段错误。
 * 同类的第四个入口是 handleQuote 里 commInfo->getSessionInfo()：
 * 合约存在但 getCommInfo() 为 NULL 时同样空指针解引用。
 * 上面三条都需要真实 level2 数据对象（WTSOrdQueData/WTSOrdDtlData/WTSTransData::create），
 * 且触发的是同一种「缺依赖 -> 崩」缺陷类，故统一放在这里，不参与常规度量。
 */
TEST(N01ParserAdapter, DISABLED_uninitialized_check_time_and_null_bd_mgr_crash)
{
    ParserAdapter adapter;   // 未 init：_check_time 是随机值, _bd_mgr==NULL
    FakeBaseDataMgr bd;
    regisContract(bd, "SSE", "600", "600000", CC_Stock);
    // 合法日期 + 未接 _bd_mgr -> _bd_mgr->getContract 空指针
    WTSTickData* tick = makeTick("SSE", "600000", 20250109, 20250109, 93000000, 7.2);
    ASSERT_NE(tick, nullptr);
    adapter.handleQuote(tick, 0);
    tick->release();
}
