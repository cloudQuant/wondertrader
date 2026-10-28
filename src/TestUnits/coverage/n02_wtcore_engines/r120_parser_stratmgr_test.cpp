/**
 * r120_parser_stratmgr_test.cpp — lane R120 / wave20
 *
 * 目标：src/WtCore/ParserAdapter.cpp 的模块装载/过滤器装配，以及
 *       src/WtCore/HftStrategyMgr.cpp / SelStrategyMgr.cpp 的目录扫描与未命中分支。
 *
 * 覆盖行段：
 *   - ParserAdapter::init 过滤器解析与订阅 :145-198
 *   - ParserAdapter::init 模块可装载但缺 createParser 的拒绝 :125-132
 *   - HftStrategyMgr::loadFactories 目录/扩展名/dlopen/符号缺失 :30-91；
 *     createStrategy/getStrategy 未命中 :94-131
 *   - SelStrategyMgr::loadFactories 同上 :30-92；createStrategy/getStrategy :95-132
 *
 * 只 dlopen 构建产物（libParserUDP / libWtDataStorage），不连接任何行情端口。
 */
#include <TestUnits/coverage/q01_wtcore3/q01_sel_env.h>
#include <TestUnits/coverage/n01_wtcore_tradepaths/r71_wtcore_env.h>

#include <WTSUtils/WTSCfgLoader.h>

#include <WtCore/HftStrategyMgr.h>
#include <WtCore/ParserAdapter.h>
#include <WtCore/SelStrategyMgr.h>
#include <WtCore/WtHelper.h>

#include <Includes/WTSCollection.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wtp;

namespace {

namespace fs = std::filesystem;

/** 返回单个合约的 IBaseDataMgr 替身 */
class R120ParserBd : public q01sel::BaseDataMgr
{
public:
    WTSContractInfo* ct = NULL;

    WTSArray* getContracts(const char* = "", uint32_t = 0) override
    {
        WTSArray* ret = WTSArray::create();
        if (ct != NULL)
        {
            // 调用方（ParserAdapter::init）对返回的数组调用 release()，
            // WTSArray::release() 会 clear() 并逐个 release 元素；
            // 因此这里必须按容器约定 retain，否则合约会在 init 内被提前 delete，
            // 用例末尾再 release 就是 use-after-free（R135 修复）。
            ret->append(ct);
        }
        return ret;
    }
};

class R120ParserStub : public IParserStub
{
public:
    void handle_push_quote(WTSTickData*) override {}
};

/** 把某个构建产物按指定模块名软链到 <root>/<subdir>/ 下 */
bool link_module(const std::string& root, const char* subdir,
                 const char* srcModule, const char* dstModule)
{
    std::error_code ec;
    fs::create_directories(fs::path(root) / subdir, ec);
    const fs::path src = fs::path(r71::bin_dir()) / DLLHelper::wrap_module(srcModule, "lib");
    const fs::path dst = fs::path(root) / subdir / DLLHelper::wrap_module(dstModule, "lib");
    fs::remove(dst, ec);
    if (!fs::exists(src, ec))
        return false;
    fs::create_symlink(src, dst, ec);
    return !ec && fs::exists(dst, ec);
}

class R120ParserTest : public ::testing::Test
{
protected:
    r71::R71GenEnv env;
    std::string oldInst;
    std::string root;
    q01sel::HotMgr hot;

    void SetUp() override
    {
        oldInst = WtHelper::getInstDir();
        root = r71::temp_root() + "r120_parser_modules/";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
        WtHelper::setInstDir(root.c_str());
    }

    void TearDown() override
    {
        WtHelper::setInstDir(oldInst.c_str());
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

//////////////////////////////////////////////////////////////////
// ParserAdapter::init 成功装载 ParserUDP：过滤器装配 + 订阅
// src/WtCore/ParserAdapter.cpp:125-198
//////////////////////////////////////////////////////////////////
TEST_F(R120ParserTest, InitParsesFilterAndSubscribesContracts)
{
    ASSERT_TRUE(link_module(root, "parsers", "ParserUDP", "ParserUDP"));

    R120ParserBd bd;
    bd.regisSessionWithTime("TRADING", 900, 1500);
    WTSCommodityInfo* comm = q01sel::makeComm("SHFE", "rb", "TRADING", TM_Both);
    WTSContractInfo* ct = q01sel::makeCt("SHFE", "rb", "rb2510");
    ct->setAltCode("rb2510");
    bd.regis(comm, ct);
    bd.ct = ct;

    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"ParserUDP\",\"host\":\"127.0.0.1\",\"bport\":19996,"
        "\"sport\":19995,\"filter\":\"SHFE\"}");
    ASSERT_NE(nullptr, cfg);

    R120ParserStub stub;
    ParserAdapter adapter;
    const bool ok = adapter.init("r120_parser", cfg, &stub, &bd, &hot);
    cfg->release();
    adapter.release();

    EXPECT_TRUE(ok);
    EXPECT_STREQ("r120_parser", adapter.id());

    comm->release();
    ct->release();
}

//////////////////////////////////////////////////////////////////
// 模块可 dlopen 但缺 createParser：init 拒绝
// src/WtCore/ParserAdapter.cpp:125-132
//////////////////////////////////////////////////////////////////
TEST_F(R120ParserTest, InitRejectsModuleWithoutCreateParser)
{
    ASSERT_TRUE(link_module(root, "parsers", "WtDataStorage", "NoParserHere"));

    R120ParserBd bd;
    WTSVariant* cfg = WTSCfgLoader::load_from_content(
        "{\"module\":\"NoParserHere\",\"filter\":\"\"}");
    ASSERT_NE(nullptr, cfg);

    R120ParserStub stub;
    ParserAdapter adapter;
    const bool ok = adapter.init("r120_noparser", cfg, &stub, &bd, NULL);
    cfg->release();
    adapter.release();

    EXPECT_FALSE(ok);
}

/** 一个只含非 dylib 与一个无策略工厂符号 dylib 的目录 */
std::string make_strategy_dir(const std::string& root, const char* tag)
{
    std::string dir = root + tag + "/";
    std::error_code ec;
    fs::create_directories(dir, ec);
    {
        std::ofstream ofs((dir + "readme.txt").c_str(), std::ios::trunc);
        ofs << "not a module";
    }
    fs::create_directories(dir + "subdir", ec);
    link_module(dir, "", "WtDataStorage", "WtBogusFact");
    return dir;
}

//////////////////////////////////////////////////////////////////
// 两个策略管理器的目录扫描与未命中分支（无策略插件可用）
// src/WtCore/HftStrategyMgr.cpp:30-131；src/WtCore/SelStrategyMgr.cpp:30-132
//////////////////////////////////////////////////////////////////
TEST_F(R120ParserTest, StrategyManagersScanDirectoriesAndMiss)
{
    std::error_code ec;
    const std::string emptyDir = root + "empty_fact/";
    fs::create_directories(emptyDir, ec);

    const std::string bogusDir = make_strategy_dir(root, "bogus_fact");

    // ---------------- HFT ----------------
    HftStrategyMgr hft;
    EXPECT_FALSE(hft.loadFactories((root + "not_exists").c_str()));
    EXPECT_TRUE(hft.loadFactories(emptyDir.c_str()));
    EXPECT_TRUE(hft.loadFactories(bogusDir.c_str()));

    EXPECT_EQ(HftStrategyPtr(), hft.createStrategy("nodot", "r120h1"));
    EXPECT_EQ(HftStrategyPtr(), hft.createStrategy("WtBogusFact.unit", "r120h2"));
    EXPECT_EQ(HftStrategyPtr(), hft.createStrategy("WtBogusFact", "unit", "r120h3"));
    EXPECT_EQ(HftStrategyPtr(), hft.getStrategy("r120h1"));

    // ---------------- SEL ----------------
    SelStrategyMgr sel;
    EXPECT_FALSE(sel.loadFactories((root + "not_exists").c_str()));
    EXPECT_TRUE(sel.loadFactories(emptyDir.c_str()));
    EXPECT_TRUE(sel.loadFactories(bogusDir.c_str()));

    EXPECT_EQ(SelStrategyPtr(), sel.createStrategy("nodot", "r120s1"));
    EXPECT_EQ(SelStrategyPtr(), sel.createStrategy("WtBogusFact.unit", "r120s2"));
    EXPECT_EQ(SelStrategyPtr(), sel.createStrategy("WtBogusFact", "unit", "r120s3"));
    EXPECT_EQ(SelStrategyPtr(), sel.getStrategy("r120s1"));

    fs::remove_all(root + "bogus_fact", ec);
}

}  // namespace
