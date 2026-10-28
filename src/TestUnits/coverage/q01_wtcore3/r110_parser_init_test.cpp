/**
 * r110_parser_init_test.cpp — lane R110 / wave19
 *
 * 目标：src/WtCore/ParserAdapter.cpp 的 init 成功链（此前只有 initExt 成功与
 * init 失败分支被覆盖）。
 *
 * 覆盖行段：
 *   - init 装载真实 ParserUDP 插件成功、filter 为空/非空两条订阅分支：:58-83, 108-142,
 *     145-198, 209-211
 *   - handleQuote 的交易所过滤器早退：:239-245
 *
 * 接缝：把构建产物 libParserUDP.dylib 软链进临时目录的 parsers/ 子目录，并
 * WtHelper::setInstDir 指向该目录；ParserUDP::init 只解析 host/port 构造 endpoint，
 * 不建 socket、不起线程；本文件只调 init，不调 run()/connect()。
 * 临时目录在用例结束时整棵删除。
 */
#include <gtest/gtest.h>

#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "r90_env.h"
#include "../p02_wtcore_engines2/r42_core_fakes.h"

#include <WtCore/ParserAdapter.h>
#include <WTSUtils/WTSCfgLoader.h>

#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSVariant.hpp>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 临时模块根：<tmp>/r110_parser_module/parsers/libParserUDP.dylib */
class R110ParserModule
{
public:
    R110ParserModule()
    {
        const std::string bin = r90::find_bin_dir();
        if (bin.empty())
            return;
        _root = fs::path(r90::temp_root()) / "r110_parser_module";
        std::error_code ec;
        fs::create_directories(_root / "parsers", ec);
        if (ec)
            return;

        const fs::path src = fs::path(bin) / "libParserUDP.dylib";
        if (!fs::exists(src, ec))
            return;

        void* h = dlopen(src.c_str(), RTLD_NOW);
        if (h == nullptr)
            return;
        const bool symbols = dlsym(h, "createParser") != nullptr
            && dlsym(h, "deleteParser") != nullptr;
        dlclose(h);
        if (!symbols)
            return;

        fs::remove(_root / "parsers" / "libParserUDP.dylib", ec);
        fs::create_symlink(src, _root / "parsers" / "libParserUDP.dylib", ec);
        _ok = !ec;
    }

    ~R110ParserModule()
    {
        if (!_root.empty())
        {
            std::error_code ec;
            fs::remove_all(_root, ec);
        }
    }

    bool ok() const { return _ok; }
    const std::string& root() const { return _rootStr; }

    void publish()
    {
        _rootStr = _root.string() + "/";
        _oldInst = WtHelper::getInstDir();
        WtHelper::setInstDir(_rootStr.c_str());
    }

    void restore()
    {
        if (!_rootStr.empty())
            WtHelper::setInstDir(_oldInst.c_str());
    }

private:
    fs::path _root;
    std::string _rootStr;
    std::string _oldInst;
    bool _ok = false;
};

class R110ParserEnv
{
public:
    R110ParserEnv()
    {
        bd.regisSession("TRADING");
        WTSSessionInfo* sess = bd.session("TRADING");
        sess->addTradingSection(900, 1500);

        WTSCommodityInfo* rb = r42fakes::makeComm("SHFE", "rb", "TRADING", TM_Both);
        rb->setSessionInfo(sess);
        WTSContractInfo* rbct = r42fakes::makeCt("SHFE", "rb", "rb2510");
        rbct->setAltCode("rb2510");
        bd.regis(rb, rbct);

        WTSCommodityInfo* m = r42fakes::makeComm("DCE", "m", "TRADING", TM_Both);
        m->setSessionInfo(sess);
        WTSContractInfo* mct = r42fakes::makeCt("DCE", "m", "m2509");
        mct->setAltCode("m2509");
        bd.regis(m, mct);
    }

    /** 只记录转发次数，No-op 实现 IParserStub */
    class Stub : public IParserStub
    {
    public:
        uint32_t quotes = 0;
        std::string last_code;
        void handle_push_quote(WTSTickData* curTick) override
        {
            ++quotes;
            last_code = curTick->code();
        }
    };

    r42fakes::R42GenDir      gen;
    r42fakes::R42BaseDataMgr bd;
    r42fakes::R42HotMgr      hot;
    Stub                     stub;

    WTSVariant* cfg(const char* filter)
    {
        std::string json = "{\"module\":\"ParserUDP\",\"host\":\"127.0.0.1\","
            "\"sport\":19001,\"bport\":19002";
        if (filter != nullptr && filter[0] != '\0')
            json += std::string(",\"filter\":\"") + filter + "\"";
        json += "}";
        return WTSCfgLoader::load_from_content(json.c_str());
    }
};

//////////////////////////////////////////////////////////////////
// 插件装载 + filter 非空/为空两条合约订阅分支
// src/WtCore/ParserAdapter.cpp:58-83, 108-142, 145-198, 209-211
//////////////////////////////////////////////////////////////////
TEST(R110ParserInit, LoadsModuleAndSubscribesWithAndWithoutFilter)
{
    R110ParserModule mod;
    if (!mod.ok())
        GTEST_SKIP() << "libParserUDP.dylib with createParser not found";
    mod.publish();

    R110ParserEnv env;

    {
        ParserAdapter filtered;
        WTSVariant* c1 = env.cfg("SHFE,rb2510");
        ASSERT_NE(nullptr, c1);
        EXPECT_TRUE(filtered.init("r110_filtered", c1, &env.stub, &env.bd, &env.hot));
        c1->release();
        EXPECT_STREQ("r110_filtered", filtered.id());
        EXPECT_EQ(&env.bd, filtered.getBaseDataMgr());
        filtered.release();
    }

    {
        ParserAdapter plain;
        WTSVariant* c2 = env.cfg("");
        ASSERT_NE(nullptr, c2);
        EXPECT_TRUE(plain.init("r110_plain", c2, &env.stub, &env.bd, &env.hot));
        c2->release();
        EXPECT_STREQ("r110_plain", plain.id());
        plain.release();
    }

    mod.restore();
}

//////////////////////////////////////////////////////////////////
// handleQuote 交易所过滤早退，命中过滤后正常转发
// src/WtCore/ParserAdapter.cpp:239-245, 278-295
//////////////////////////////////////////////////////////////////
TEST(R110ParserInit, HandleQuoteHonoursExchangeFilter)
{
    R110ParserModule mod;
    if (!mod.ok())
        GTEST_SKIP() << "libParserUDP.dylib with createParser not found";
    mod.publish();

    R110ParserEnv env;
    ParserAdapter adapter;
    WTSVariant* c = env.cfg("DCE");
    ASSERT_NE(nullptr, c);
    ASSERT_TRUE(adapter.init("r110_quote_filter", c, &env.stub, &env.bd, &env.hot));
    c->release();

    // 非 DCE 交易所的行情被过滤器丢弃
    WTSTickData* shfe = r42fakes::makeTick("SHFE", "rb2510", 20250110, 20250110, 93100000, 3500.0);
    adapter.handleQuote(shfe, 0);
    shfe->release();
    EXPECT_EQ(0u, env.stub.quotes);

    // DCE 分月合约转标准代码后转发
    WTSTickData* dce = r42fakes::makeTick("DCE", "m2509", 20250110, 20250110, 93100000, 2800.0);
    adapter.handleQuote(dce, 0);
    dce->release();
    EXPECT_EQ(1u, env.stub.quotes);
    EXPECT_EQ("DCE.m.2509", env.stub.last_code);

    adapter.release();
    mod.restore();
}

}  // namespace
