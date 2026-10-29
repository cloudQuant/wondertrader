/*!
 * \file r44_parser_adapter_edges_test.cpp
 * \brief WtDtCore/ParserAdapter 边界补测（迭代01 第十波 R44）
 *
 * 与既有 r18_parser_adapter_test.cpp 的分工：r18 已覆盖 initExt 的正常/失败、
 * 三个 level2 handler 的日期/合约守卫与放行写回、init 的 null/空 module/加载失败、
 * release/run 的基本路径。本文件只补：
 *  - initExt(id, NULL) 的早期返回（ParserAdapter.cpp:46-49）
 *  - initExt 在「合约表为空」时的 subscribe(空集合)（L60-72 循环 0 次）
 *  - 单次 initExt 后 release 的清理；重复 initExt 语义待公开合同明确
 *  - release 之后三个 level2 handler 的 _stopped 守卫（L237-238/254-255/271-272）
 *  - 构造时 bd_mgr==NULL 仍可日志/释放（L319-322 仅回传，不解引用）
 *  - ParserAdapterMgr 空表的 run/release（L327-335/365-373 循环 0 次）
 *  - init 的 module 文件存在但 dlopen 失败（L104-116 的 exists 为真分支）
 * 全部为纯内存/临时目录，不加载任何真实解析插件、不连柜台、不起线程。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/ParserAdapter.h>
#include <WtDtCore/DataManager.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

namespace fs = std::filesystem;

namespace {

fs::path r44_make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void r44_write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

// 纯内存 IParserApi 替身：只计数，不碰 IO
class R44FakeParserApi : public IParserApi
{
public:
	explicit R44FakeParserApi(bool* destroyed = nullptr, int* releaseCnt = nullptr)
		: _destroyed(destroyed), _releaseCntExt(releaseCnt) {}
	virtual ~R44FakeParserApi() override
	{
		if (_destroyed != nullptr)
			*_destroyed = true;
	}

	virtual bool init(WTSVariant* /*config*/) override
	{
		_initCnt++;
		return true;
	}
	virtual void release() override
	{
		_releaseCnt++;
		if (_releaseCntExt != nullptr)
			++(*_releaseCntExt);
	}
	virtual bool connect() override { _connectCnt++; return true; }
	virtual void registerSpi(IParserSpi* spi) override { _spi = spi; }
	virtual void subscribe(const CodeSet& setCodes) override
	{
		_subCnt++;
		_subbed = setCodes;
	}

	bool*		_destroyed { nullptr };
	int*		_releaseCntExt { nullptr };
	int			_initCnt { 0 };
	int			_releaseCnt { 0 };
	int			_connectCnt { 0 };
	int			_subCnt { 0 };
	IParserSpi*	_spi { nullptr };
	CodeSet		_subbed;
};

class R44ParserAdapterEdgesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r44_make_tmp_dir("r44_pa");

		r44_write_file(_dir / "sessions.json",
			R"JSON({
				"DAY0": {"name": "日盘", "offset": 0,
					"sections": [{"from": 930, "to": 1500}]}
			})JSON");
		r44_write_file(_dir / "commodities.json",
			R"JSON({
				"SSE": {"STK": {"name": "股票", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 0.01, "volscale": 1, "covermode": 0,
					"pricemode": 0, "trademode": 1, "lotstick": 1, "minlots": 1}}
			})JSON");
		r44_write_file(_dir / "contracts.json",
			R"JSON({
				"SSE": {"600000": {"name": "浦发银行", "exchg": "SSE", "product": "STK",
					"altcode": "SH600000", "opendate": 20200101, "expiredate": 20500101}}
			})JSON");
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
		ASSERT_NE(_bdMgr.getContract("600000", "SSE"), nullptr);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	static WTSOrdQueData* makeOrdQue()
	{
		WTSOrdQueStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSOrdQueData::create(s);
	}

	static WTSOrdDtlData* makeOrdDtl()
	{
		WTSOrdDtlStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSOrdDtlData::create(s);
	}

	static WTSTransData* makeTrans()
	{
		WTSTransStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = 20240102;
		s.trading_date = 20240102;
		return WTSTransData::create(s);
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
};

// ParserAdapter.cpp:46-49 —— api==NULL 时不写 _id、不注册 SPI，直接 false
TEST_F(R44ParserAdapterEdgesTest, InitExtRejectsNullApiWithoutTouchingId)
{
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	EXPECT_FALSE(pa.initExt("r44null", nullptr));
	EXPECT_STREQ(pa.id(), "");
	EXPECT_FALSE(pa.run());
	pa.release();
}

// ParserAdapter.cpp:60-72 —— 空合约表时 getContracts 返回空数组，循环 0 次后仍 subscribe 一次空集合
TEST_F(R44ParserAdapterEdgesTest, InitExtWithEmptyContractTableSubscribesEmptySet)
{
	WTSBaseDataMgr emptyBd;	// 未 loadContracts，_code_list 为空
	// release() 会 delete _parser_api，故替身必须堆分配
	bool destroyed = false;
	R44FakeParserApi* api = new R44FakeParserApi(&destroyed);
	ParserAdapter pa(&emptyBd, nullptr, nullptr);
	ASSERT_TRUE(pa.initExt("r44empty", api));

	EXPECT_EQ(api->_initCnt, 1);
	EXPECT_EQ(api->_subCnt, 1);
	EXPECT_TRUE(api->_subbed.empty());
	EXPECT_EQ(api->_spi, static_cast<IParserSpi*>(&pa));
	EXPECT_STREQ(pa.id(), "r44empty");
	pa.release();
	EXPECT_TRUE(destroyed);
}

// 单次有效 initExt 仍独立覆盖正常初始化与 release 清理路径。
TEST_F(R44ParserAdapterEdgesTest, SingleInitExtReleasesApi)
{
	bool destroyed = false;
	int released = 0;
	R44FakeParserApi* api = new R44FakeParserApi(&destroyed, &released);

	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	ASSERT_TRUE(pa.initExt("r44single", api));
	EXPECT_EQ(api->_initCnt, 1);

	pa.release();
	EXPECT_EQ(released, 1);
	EXPECT_TRUE(destroyed);
}

// 重复 initExt 的替换、合并或拒绝语义尚未由公开合同定义，暂不将当前实现行为固化为测试预期。
TEST_F(R44ParserAdapterEdgesTest, RepeatedInitExtNeedsContract)
{
	GTEST_SKIP() << "Repeated initExt semantics are unspecified; define the ownership and replacement contract before enabling this scenario.";
}

// ParserAdapter.cpp:237-238 / 254-255 / 271-272 —— release 置 _stopped 后三个 level2 回调直接返回，
// 不再查合约、不写 DataManager（此处 _dt_mgr==NULL，若守卫失效会崩）
TEST_F(R44ParserAdapterEdgesTest, ReleasedAdapterDropsLevel2Callbacks)
{
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	pa.release();

	WTSOrdQueData* q = makeOrdQue();
	WTSOrdDtlData* d = makeOrdDtl();
	WTSTransData* t = makeTrans();
	ASSERT_NE(q, nullptr);
	ASSERT_NE(d, nullptr);
	ASSERT_NE(t, nullptr);

	pa.handleOrderQueue(q);
	pa.handleOrderDetail(d);
	pa.handleTransaction(t);

	EXPECT_EQ(q->getContractInfo(), nullptr);
	EXPECT_EQ(d->getContractInfo(), nullptr);
	EXPECT_EQ(t->getContractInfo(), nullptr);

	q->release();
	d->release();
	t->release();
}

// ParserAdapter.cpp:319-322 + 311-317 —— 允许 bd_mgr==NULL 的实例：getBaseDataMgr 原样回传，
// 日志接口不被 _bd_mgr 影响，release 对空 api 安全
TEST_F(R44ParserAdapterEdgesTest, NullBaseDataMgrStillAllowsLogAndRelease)
{
	ParserAdapter pa(nullptr, nullptr, nullptr);
	EXPECT_EQ(pa.getBaseDataMgr(), nullptr);
	pa.handleParserLog(LL_INFO, "r44 parser log with null bd mgr");
	pa.release();
	pa.handleParserLog(LL_ERROR, "r44 parser log after release");
	pa.release();
	EXPECT_STREQ(pa.id(), "");
}

// ParserAdapter.cpp:327-335 / 365-373 —— 空适配器表的 run/release 只走 0 次循环
TEST_F(R44ParserAdapterEdgesTest, EmptyAdapterMgrRunAndReleaseAreNoops)
{
	ParserAdapterMgr mgr;
	EXPECT_EQ(mgr.size(), 0u);
	mgr.run();
	EXPECT_EQ(mgr.getAdapter("none"), nullptr);
	mgr.release();
	EXPECT_EQ(mgr.size(), 0u);
	mgr.release();
}

// ParserAdapter.cpp:102-116 —— module 文件在 parsers/ 下真实存在但不是 Mach-O，
// 走 dlopen 失败 return false；_cfg 已被 retain，二次 init 命中重复初始化守卫
TEST_F(R44ParserAdapterEdgesTest, InitFailsOnExistingButUnloadableModuleFile)
{
	const fs::path modDir = _dir / "mod";
	fs::create_directories(modDir / "parsers");
	r44_write_file(modDir / "parsers" / "libr44bad.dylib", "not a Mach-O library");
	WtHelper::set_module_dir((modDir.string() + "/").c_str());

	r44_write_file(_dir / "bad.json", R"JSON({"module": "r44bad"})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "bad.json").string().c_str());
	ASSERT_NE(cfg, nullptr);

	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	EXPECT_FALSE(pa.init("r44bad", cfg));
	EXPECT_STREQ(pa.id(), "r44bad");
	// 第二次 init 在 L91-92 因 _cfg 非空直接拒绝，不再尝试 dlopen
	EXPECT_FALSE(pa.init("r44bad", cfg));
	cfg->release();
	pa.release();
}

/*
 * 触发条件（禁用原因）：ParserAdapter::release()（L207-219）只 delete 不置空 _parser_api，
 * run()（L221-228）又只看 _parser_api==NULL、不看 _stopped，于是 release 后 run 会对已释放
 * 对象做虚调用 -> use-after-free。缺陷登记：bugs/R44.md BUG-R44-004。
 */
TEST_F(R44ParserAdapterEdgesTest, DISABLED_RunAfterReleaseUsesFreedApi)
{
	R44FakeParserApi* api = new R44FakeParserApi(nullptr);
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	ASSERT_TRUE(pa.initExt("r44uaf", api));
	pa.release();
	EXPECT_TRUE(pa.run());	// 对已 delete 的 _parser_api 调 connect()
}

}  // namespace
