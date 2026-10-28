/*!
 * \file r8_parseradapter_test.cpp
 * \brief WtDtServo/ParserAdapter 覆盖用例（迭代01 第七波 R8）
 *
 * 覆盖目标（只针对 src/WtDtServo/ParserAdapter.cpp，不碰 WtDtCore 的同名异形状类）：
 *  - init 的四条拒绝分支：cfg==NULL(:84) / _cfg 已存在(:89) / module 为空(:97) / 模块加载失败(:110)
 *  - initExt 的 api==NULL 拒绝分支(:46)
 *  - run 在 _parser_api==NULL 时的 false 分支(:221)
 *  - release 在全空成员上的安全路径(:205-217) 与停表后各 handle* 的早退
 *  - handleQuote 的 NULL / 日期为 0 / _dt_runner==NULL 三档过滤(:278-282)
 *  - handleTransaction / handleOrderDetail / handleOrderQueue 的停表、日期为 0、
 *    空基础数据（getContract==NULL）三档过滤(:235-272)
 *  - handleParserLog 停表(:287) 与未停表(:290) 两分支、getBaseDataMgr 透传(:295)
 *  - ParserAdapterMgr：addAdapter 空指针/空 id/重名(:313-321)、getAdapter 未命中(:336)、
 *    run(:339-347) 与 release(:301-309) 的扇出
 * 不加载任何插件 DLL（模块目录指向空目录），不建线程，不碰 socket/shm。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include <filesystem>

#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSStruct.h>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WtDtServo/ParserAdapter.h>
#include <WtDtServo/WtHelper.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

class R8ParserAdapterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r8_pa");
		// 把模块目录钉在必然没有 parsers/*.dylib 的空目录，保证走加载失败分支
		WtHelper::set_module_dir((_dir / "bin/").string().c_str());
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* loadCfg(const char* name, const std::string& body)
	{
		fs::path p = _dir / name;
		std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
		ofs << body;
		ofs.close();
		WTSVariant* ret = WTSCfgLoader::load_from_file(p.string().c_str());
		_cfgs.push_back(ret);
		return ret;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	std::vector<WTSVariant*> _cfgs;
};

// ------------------------------------------------------------------
// ParserAdapter::init 的拒绝分支
// ------------------------------------------------------------------

TEST_F(R8ParserAdapterTest, InitWithNullCfgRejectsAndKeepsIdEmpty)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// ParserAdapter.cpp:84-85  cfg==NULL 立即 return false，_id 尚未赋值
	EXPECT_FALSE(pa.init("pa_null", nullptr));
	// ParserAdapter.cpp:87  _id=id 排在 cfg 判空之后，故此时 id() 仍是默认空串
	EXPECT_STREQ(pa.id(), "");
	// ParserAdapter.cpp:293-296  构造入参原样透传
	EXPECT_EQ(pa.getBaseDataMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));
}

TEST_F(R8ParserAdapterTest, InitWithEmptyModuleRejectsButStillBindsIdAndCfg)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// 无 module 字段 -> getString("module").empty() -> ParserAdapter.cpp:97-98 return false
	WTSVariant* cfg = loadCfg("nomodule.json", R"JSON({"id":"r8a", "active":true})JSON");
	ASSERT_NE(cfg, nullptr);

	EXPECT_FALSE(pa.init("pa_nomod", cfg));
	// ParserAdapter.cpp:87-93：_id 赋值与 _cfg 暂存都发生在 module 判空之前
	EXPECT_STREQ(pa.id(), "pa_nomod");

	// 第二次 init 走 _cfg!=NULL 的拒绝分支（:89-90），
	// 但该分支同样在 _id=id 之后，于是 id 被覆盖成第二个名字（见 BUG-R8-002）
	WTSVariant* cfg2 = loadCfg("nomodule2.json", R"JSON({"id":"r8b"})JSON");
	ASSERT_NE(cfg2, nullptr);
	EXPECT_FALSE(pa.init("pa_second", cfg2));
	EXPECT_STREQ(pa.id(), "pa_second");

	// _cfg!=NULL 但 _parser_api 仍为 NULL -> run 只能 false（:221）
	EXPECT_FALSE(pa.run());
}

TEST_F(R8ParserAdapterTest, InitWithUnloadableModuleReturnsFalse)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// module 存在但文件不存在：StdFile::exists(:102) 为假 -> 退化到
	// <moduledir>/parsers/lib...（:104-107），该目录为空 -> load_library 失败（:110-113）
	WTSVariant* cfg = loadCfg("badmodule.json",
		R"JSON({"id":"r8bad", "module":"r8_no_such_parser"})JSON");
	ASSERT_NE(cfg, nullptr);

	EXPECT_FALSE(pa.init("pa_badmod", cfg));
	EXPECT_STREQ(pa.id(), "pa_badmod");
	EXPECT_FALSE(pa.run());
	// 未创建解析器时，release 走 delete NULL（:213-216），不应崩溃
	pa.release();
}

// ------------------------------------------------------------------
// initExt / run / release
// ------------------------------------------------------------------

TEST_F(R8ParserAdapterTest, InitExtWithNullApiIsRejected)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// ParserAdapter.cpp:46-47  api==NULL -> return false，_id 不赋值
	EXPECT_FALSE(pa.initExt("ext_null", nullptr));
	EXPECT_STREQ(pa.id(), "");
}

TEST_F(R8ParserAdapterTest, RunOnFreshAdapterIsFalseAndReleaseIsSafe)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// :221  _parser_api==NULL -> false
	EXPECT_FALSE(pa.run());
	// :205-217  _parser_api/_remover 皆 NULL，release 不应崩溃
	pa.release();
	EXPECT_FALSE(pa.run());
	EXPECT_STREQ(pa.id(), "");
}

// ------------------------------------------------------------------
// handleQuote：三档过滤
// ------------------------------------------------------------------

TEST_F(R8ParserAdapterTest, HandleQuoteIgnoresNullAndZeroDateTicks)
{
	ParserAdapter pa(&_bdMgr, nullptr);

	// :278  quote==NULL 直接返回
	pa.handleQuote(nullptr, 0);

	WTSTickData* tick = WTSTickData::create("IC2204");
	ASSERT_NE(tick, nullptr);
	WTSTickStruct& ts = tick->getTickStruct();
	// 新建 tick 的 action_date/trading_date 为 0 -> :278 中段条件早退
	EXPECT_EQ(ts.action_date, 0u);
	EXPECT_EQ(ts.trading_date, 0u);
	pa.handleQuote(tick, 0);

	// 日期补齐后 _dt_runner==NULL -> :281 判空，不转发，tick 内容不得被改写
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	ts.price = 123.45;
	const std::string codeCopy(ts.code, strnlen(ts.code, sizeof(ts.code)));
	pa.handleQuote(tick, 0);
	EXPECT_EQ(std::string(ts.code, strnlen(ts.code, sizeof(ts.code))), codeCopy);
	EXPECT_EQ(tick->getContractInfo(), nullptr);
	EXPECT_DOUBLE_EQ(ts.price, 123.45);

	tick->release();
}

TEST_F(R8ParserAdapterTest, HandleQuoteAfterReleaseStopsForwarding)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	WTSTickData* tick = WTSTickData::create("IC2204");
	ASSERT_NE(tick, nullptr);
	WTSTickStruct& ts = tick->getTickStruct();
	ts.action_date = 20240102;
	ts.trading_date = 20240102;

	pa.release();  // _stopped = true（ParserAdapter.cpp:207）
	const std::string codeCopy(ts.code, strnlen(ts.code, sizeof(ts.code)));
	pa.handleQuote(tick, 0);
	// 停表分支（:278 中段 _stopped）必须原样返回，代码串不动
	EXPECT_EQ(std::string(ts.code, strnlen(ts.code, sizeof(ts.code))), codeCopy);

	// 已停表时三类明细回调先判 _stopped，即使传 NULL 也不会解引用（:235/:250/:264/:287）
	pa.handleParserLog(LL_INFO, "r8 stopped parser log");
	pa.handleTransaction(nullptr);
	pa.handleOrderDetail(nullptr);
	pa.handleOrderQueue(nullptr);
	EXPECT_TRUE(true);
	tick->release();
}

TEST_F(R8ParserAdapterTest, HandleParserLogAliveBranchIsSafe)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// :285-291 未停表 -> WTSLogger::log_raw_by_cat("parser", ...)，只要求不崩且不改状态
	pa.handleParserLog(LL_WARN, "r8 alive parser log");
	pa.handleSymbolList(nullptr);
	EXPECT_FALSE(pa.run());
}

// ------------------------------------------------------------------
// handleTransaction / handleOrderDetail / handleOrderQueue
// ------------------------------------------------------------------

TEST_F(R8ParserAdapterTest, DetailHandlersReturnOnZeroDates)
{
	ParserAdapter pa(&_bdMgr, nullptr);

	WTSTransData* trans = WTSTransData::create("IC2204");
	ASSERT_NE(trans, nullptr);
	// 新建明细 action_date 为 0 -> ParserAdapter.cpp:239-240 早退
	EXPECT_EQ(trans->actiondate(), 0u);
	pa.handleTransaction(trans);

	WTSOrdDtlData* ordDtl = WTSOrdDtlData::create("IC2204");
	ASSERT_NE(ordDtl, nullptr);
	EXPECT_EQ(ordDtl->actiondate(), 0u);
	pa.handleOrderDetail(ordDtl);

	WTSOrdQueData* ordQue = WTSOrdQueData::create("IC2204");
	ASSERT_NE(ordQue, nullptr);
	EXPECT_EQ(ordQue->actiondate(), 0u);
	pa.handleOrderQueue(ordQue);

	trans->release();
	ordDtl->release();
	ordQue->release();
}

TEST_F(R8ParserAdapterTest, DetailHandlersReturnOnUnknownContract)
{
	ParserAdapter pa(&_bdMgr, nullptr);
	// _bdMgr 未加载任何合约 -> getContract 返回 NULL -> :242-244 / :256-258 / :270-272

	WTSTransStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.code, "IC2204");
	wt_strcpy(ts.exchg, "CFFEX");
	ts.action_date = 20240102;
	ts.trading_date = 20240102;
	WTSTransData* trans = WTSTransData::create(ts);
	ASSERT_NE(trans, nullptr);
	pa.handleTransaction(trans);
	EXPECT_EQ(_bdMgr.getContract("IC2204", "CFFEX"), nullptr);
	trans->release();

	WTSOrdDtlStruct ds;
	memset(&ds, 0, sizeof(ds));
	wt_strcpy(ds.code, "IC2204");
	wt_strcpy(ds.exchg, "CFFEX");
	ds.action_date = 20240102;
	ds.trading_date = 20240102;
	WTSOrdDtlData* ordDtl = WTSOrdDtlData::create(ds);
	ASSERT_NE(ordDtl, nullptr);
	pa.handleOrderDetail(ordDtl);
	ordDtl->release();

	WTSOrdQueStruct qs;
	memset(&qs, 0, sizeof(qs));
	wt_strcpy(qs.code, "IC2204");
	wt_strcpy(qs.exchg, "CFFEX");
	qs.action_date = 20240102;
	qs.trading_date = 20240102;
	WTSOrdQueData* ordQue = WTSOrdQueData::create(qs);
	ASSERT_NE(ordQue, nullptr);
	pa.handleOrderQueue(ordQue);
	ordQue->release();
}

// ------------------------------------------------------------------
// ParserAdapterMgr
// ------------------------------------------------------------------

TEST_F(R8ParserAdapterTest, AddAdapterRejectsNullPtrAndEmptyId)
{
	ParserAdapterMgr mgr;
	ParserAdapterPtr nullPtr;
	// ParserAdapter.cpp:313  adapter==NULL -> false
	EXPECT_FALSE(mgr.addAdapter("x", nullPtr));
	EXPECT_EQ(mgr.size(), 0u);

	ParserAdapterPtr pa(new ParserAdapter(&_bdMgr, nullptr));
	// id 长度为 0 -> 同一条 :313 的 strlen(id)==0 分支
	EXPECT_FALSE(mgr.addAdapter("", pa));
	EXPECT_EQ(mgr.size(), 0u);
	// 未登记的 id 查询返回空指针（:336）
	EXPECT_EQ(mgr.getAdapter("nothing").get(), nullptr);
}

TEST_F(R8ParserAdapterTest, AddAdapterKeepsFirstOnDuplicateId)
{
	ParserAdapterMgr mgr;
	ParserAdapterPtr first(new ParserAdapter(&_bdMgr, nullptr));
	ParserAdapterPtr second(new ParserAdapter(&_bdMgr, nullptr));

	ASSERT_TRUE(mgr.addAdapter("dup", first));
	// 同名 -> :317-321 报 "Same name of parsers" 并拒绝
	EXPECT_FALSE(mgr.addAdapter("dup", second));
	EXPECT_EQ(mgr.size(), 1u);
	// getAdapter 必须还回第一个，而不是第二个
	EXPECT_EQ(mgr.getAdapter("dup").get(), first.get());
	EXPECT_NE(mgr.getAdapter("dup").get(), second.get());

	// run/release 遍历已登记项，release 后表清空（:301-309 / :339-347）
	mgr.run();
	mgr.release();
	EXPECT_EQ(mgr.size(), 0u);
	EXPECT_EQ(mgr.getAdapter("dup").get(), nullptr);
}

TEST_F(R8ParserAdapterTest, ManagerReleaseOnEmptyMapIsSafe)
{
	ParserAdapterMgr mgr;
	// 空表：size()==0，run 走 "{} parsers started" 日志分支（:346），release 空转
	EXPECT_EQ(mgr.size(), 0u);
	mgr.run();
	mgr.release();
	EXPECT_EQ(mgr.size(), 0u);

	ParserAdapterPtr pa(new ParserAdapter(&_bdMgr, nullptr));
	ASSERT_TRUE(mgr.addAdapter("solo", pa));
	mgr.run();
	mgr.release();
	EXPECT_EQ(mgr.size(), 0u);
}

}  // namespace
