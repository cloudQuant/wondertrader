/*!
 * \file r18_parser_adapter_test.cpp
 * \brief WtDtCore/ParserAdapter.cpp + ParserAdapter.h 覆盖用例（迭代01 R18 lane）
 *
 * 覆盖目标（全部为纯内存分支，不起线程 / 不 sleep / 不 socket / 不 shm / 不加载插件 DLL）：
 *  - ParserAdapter 构造/析构、id()、getBaseDataMgr()
 *  - initExt：api==NULL 拒绝、api->init 失败分支、api->init 成功后用 bd_mgr 全合约拼订阅集合
 *  - release：无 api 空操作、有 api 时 release+delete（_remover 恒为 NULL 的 initExt 路径）
 *  - run：无 api false / 有 api 调 connect
 *  - init：cfg==NULL、module 为空、模块加载失败、_cfg 已存在时二次拒绝
 *  - handleQuote / handleOrderQueue / handleOrderDetail / handleTransaction 的四段守卫
 *    （_stopped、actiondate/tradingdate 为 0、合约查不到、合约已预置不重查）
 *  - handleSymbolList 空实现、handleParserLog 的 _stopped 守卫
 *  - ParserAdapterMgr：addAdapter 空指针/空 id/重名、getAdapter 命中与未命中、run、release
 * 观测手法说明：
 *  1) 拒绝分支的“可证伪性”靠把 DataManager* 置 NULL —— 一旦守卫漏掉而落到
 *     `_dt_mgr->writeXxx(...)`（ParserAdapter.cpp:249/266/283/304）就是空指针解引用，
 *     用例必然崩；因此“跑完没崩 + getContractInfo() 仍为 NULL”即等价于走到了 return。
 *  2) 放行分支靠 setContractInfo 的副作用（ParserAdapter.cpp:247/264/281/298）可观测。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/ParserAdapter.h>
#include <WtDtCore/DataManager.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

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

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

struct FakeParserApiStats
{
	int initCnt { 0 };
	int subCnt { 0 };
	bool initResult { true };
};

/*
 *	纯内存的 IParserApi 替身：只计数，不碰任何 IO。
 *	_releaseCnt/_connectCnt 等用于证明 ParserAdapter 转发了哪个调用；
 *	_destroyed 指向用例自己持有的 bool，用于在对象被释放后安全判断析构是否已发生。
 */
class FakeParserApi : public IParserApi
{
public:
	explicit FakeParserApi(bool* destroyed = nullptr, bool initRet = true, FakeParserApiStats* stats = nullptr)
		: _destroyed(destroyed), _initRet(initRet), _stats(stats) {}

	virtual ~FakeParserApi() override
	{
		if (_destroyed != nullptr)
			*_destroyed = true;
	}

	virtual bool init(WTSVariant* config) override
	{
		_initCnt++;
		_cfgSeen = config;
		if (_stats != nullptr)
		{
			_stats->initCnt++;
			_stats->initResult = _initRet;
		}
		return _initRet;
	}

	virtual void release() override
	{
		_releaseCnt++;
	}

	virtual bool connect() override { _connectCnt++; return true; }

	virtual bool disconnect() override { _disconnectCnt++; return true; }

	virtual bool isConnected() override { return _connected; }

	virtual void subscribe(const CodeSet& setCodes) override
	{
		_subCnt++;
		_subbed = setCodes;
		if (_stats != nullptr)
			_stats->subCnt++;
	}

	virtual void unsubscribe(const CodeSet& setCodes) override
	{
		_unsubCnt++;
		_unsubbedSize = setCodes.size();
	}

	virtual void registerSpi(IParserSpi* spi) override
	{
		_spiRegCnt++;
		_spi = spi;
	}

	bool*		_destroyed { nullptr };
	bool		_initRet { true };
	FakeParserApiStats* _stats { nullptr };
	bool		_connected { false };
	int			_initCnt { 0 };
	int			_spiRegCnt { 0 };
	int			_releaseCnt { 0 };
	int			_connectCnt { 0 };
	int			_disconnectCnt { 0 };
	int			_subCnt { 0 };
	int			_unsubCnt { 0 };
	std::size_t	_unsubbedSize { 0 };
	WTSVariant*	_cfgSeen { nullptr };
	IParserSpi*	_spi { nullptr };
	CodeSet		_subbed;
};

class R18ParserAdapterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r18_pa");
		// 把模块目录指向一个必然没有 parser 插件的空目录，
		// 这样 init() 稳定走 DLLHelper::load_library 失败分支（ParserAdapter.cpp:111-116）
		_binDir = _dir / "bin";
		fs::create_directories(_binDir);
		WtHelper::set_module_dir((_binDir / "/").string().c_str());

		write_file(_dir / "sessions.json",
			R"JSON({
				"DAY0": {"name": "日盘", "offset": 0,
					"auction": {"from": 915, "to": 925},
					"sections": [{"from": 930, "to": 1500}]}
			})JSON");
		write_file(_dir / "commodities.json",
			R"JSON({
				"SSE": {"STK": {"name": "股票", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 0.01, "volscale": 1, "category": 0, "covermode": 0,
					"pricemode": 0, "trademode": 1, "lotstick": 1, "minlots": 1}},
				"SHFE": {"rb": {"name": "螺纹钢", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 1, "volscale": 10, "covermode": 0, "pricemode": 1}}
			})JSON");
		// 恰好 2 个合约，订阅集合的期望值可以直接手算
		write_file(_dir / "contracts.json",
			R"JSON({
				"SSE": {"600000": {"name": "浦发银行", "exchg": "SSE", "product": "STK",
					"altcode": "SH600000", "opendate": 20200101, "expiredate": 20500101}},
				"SHFE": {"rb2410": {"name": "螺纹2410", "exchg": "SHFE", "product": "rb",
					"opendate": 20230110, "expiredate": 20241115}}
			})JSON");
		write_file(_dir / "holidays.json", R"JSON({"CHINA": [20240101]})JSON");

		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadHolidays((_dir / "holidays.json").string().c_str()));

		_ctSse = _bdMgr.getContract("600000", "SSE");
		_ctShfe = _bdMgr.getContract("rb2410", "SHFE");
		ASSERT_NE(_ctSse, nullptr);
		ASSERT_NE(_ctShfe, nullptr);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	static WTSTickData* make_tick(const char* code, const char* exchg,
		uint32_t adate, uint32_t tdate)
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, code);
		strcpy(s.exchg, exchg);
		s.action_date = adate;
		s.trading_date = tdate;
		s.price = 12.34;
		return WTSTickData::create(s);
	}

	static WTSOrdQueData* make_ordque(const char* code, const char* exchg,
		uint32_t adate, uint32_t tdate)
	{
		WTSOrdQueStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, code);
		strcpy(s.exchg, exchg);
		s.action_date = adate;
		s.trading_date = tdate;
		return WTSOrdQueData::create(s);
	}

	static WTSOrdDtlData* make_orddtl(const char* code, const char* exchg,
		uint32_t adate, uint32_t tdate)
	{
		WTSOrdDtlStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, code);
		strcpy(s.exchg, exchg);
		s.action_date = adate;
		s.trading_date = tdate;
		return WTSOrdDtlData::create(s);
	}

	static WTSTransData* make_trans(const char* code, const char* exchg,
		uint32_t adate, uint32_t tdate)
	{
		WTSTransStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, code);
		strcpy(s.exchg, exchg);
		s.action_date = adate;
		s.trading_date = tdate;
		return WTSTransData::create(s);
	}

	fs::path		_dir;
	fs::path		_binDir;
	WTSBaseDataMgr	_bdMgr;
	DataManager		_dm;			// 从未 init，_writer==NULL：所有 writeXxx 静默返回 false
	WTSContractInfo* _ctSse { nullptr };
	WTSContractInfo* _ctShfe { nullptr };
};

TEST_F(R18ParserAdapterTest, FreshAdapterIsIdleAndExposesBaseDataMgr)
{
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);

	// 未 init 前 _id 是空串（ParserAdapter.cpp:89 之前没人写过）
	EXPECT_STREQ(pa.id(), "");
	// 无 api 时 run() 必须在 ParserAdapter.cpp:223-224 返回 false，且不崩
	EXPECT_FALSE(pa.run());
	// 构造参数被原样带回（ParserAdapter.cpp:319-322）
	EXPECT_EQ(pa.getBaseDataMgr(), static_cast<IBaseDataMgr*>(&_bdMgr));
	// 空实现（ParserAdapter.cpp:230-233）
	pa.handleSymbolList(nullptr);
	// 未停止：走 :316 的 log_raw_by_cat("parser", ...)
	pa.handleParserLog(LL_INFO, "r18 parser log while running");
	pa.release();
	// 停止后：:313-314 的 _stopped 守卫返回；同时覆盖 release 的
	// 「无 api -> _remover 为空 -> delete NULL」尾段（:210-218）
	pa.handleParserLog(LL_ERROR, "r18 parser log after stop");
	pa.release();
}

TEST_F(R18ParserAdapterTest, InitExtReturnsFalseWhenApiInitFails)
{
	bool destroyed = false;
	FakeParserApiStats stats;
	FakeParserApi* api = new FakeParserApi(&destroyed, false, &stats);
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);

	// IParserApi::init 返回 false 时，适配器初始化也必须报告失败。
	EXPECT_FALSE(pa.initExt("p1", api));
	EXPECT_EQ(stats.initCnt, 1);
	EXPECT_FALSE(stats.initResult);
	EXPECT_EQ(stats.subCnt, 0);			// 初始化失败不应订阅

	pa.release();
	// 失败后的 API 所有权策略尚未约定：允许 initExt 已清理，也允许仍由调用方清理。
	if (!destroyed)
	{
		api->release();
		delete api;
	}
}

TEST_F(R18ParserAdapterTest, InitExtSubscribesEveryContractFullCode)
{
	FakeParserApi* api = new FakeParserApi(nullptr, true);
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);
	ASSERT_TRUE(pa.initExt("p2", api));

	// ParserAdapter.cpp:60-72 用 getFullCode()（形如 EXCHG.code），不是 altcode
	EXPECT_EQ(api->_subCnt, 1);
	ASSERT_EQ(api->_subbed.size(), 2u);
	EXPECT_NE(api->_subbed.find("SSE.600000"), api->_subbed.end());
	EXPECT_NE(api->_subbed.find("SHFE.rb2410"), api->_subbed.end());
	EXPECT_EQ(api->_subbed.find("SSE.SH600000"), api->_subbed.end());

	pa.release();
}

TEST_F(R18ParserAdapterTest, InitRejectsNullCfgEmptyModuleAndMissingModule)
{
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);

	// ParserAdapter.cpp:86-87
	EXPECT_FALSE(pa.init("p3", nullptr));

	write_file(_dir / "no_module.json", R"JSON({"filter": "SSE"})JSON");
	WTSVariant* noModule = WTSCfgLoader::load_from_file((_dir / "no_module.json").string().c_str());
	ASSERT_NE(noModule, nullptr);
	// ParserAdapter.cpp:99-100：module 为空直接 false
	EXPECT_FALSE(pa.init("p3", noModule));
	noModule->release();

	write_file(_dir / "bad_module.json", R"JSON({"module": "r18_no_such_parser_module"})JSON");
	WTSVariant* badModule = WTSCfgLoader::load_from_file((_dir / "bad_module.json").string().c_str());
	ASSERT_NE(badModule, nullptr);
	// ParserAdapter.cpp:104-116：模块文件不存在 -> 拼 get_module_dir()+"parsers/" 再试 -> 仍失败
	EXPECT_FALSE(pa.init("p3", badModule));
	// L89 在守卫之前就写了 _id
	EXPECT_STREQ(pa.id(), "p3");
	EXPECT_FALSE(pa.run());	// 仍然没有 api

	// _cfg 已在 L94-95 被 retain，第二次 init 必须走 L91-92 的重复初始化守卫
	EXPECT_FALSE(pa.init("p3", badModule));
	badModule->release();
}

TEST_F(R18ParserAdapterTest, StoppedAdapterNeverFillsContractInfo)
{
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);
	WTSTickData* tick = make_tick("600000", "SSE", 20240102, 20240102);
	// 未 release：合约命中并写回（ParserAdapter.cpp:294-299）
	pa.handleQuote(tick, 1);
	EXPECT_EQ(tick->getContractInfo(), _ctSse);
	tick->release();

	WTSTickData* tick2 = make_tick("600000", "SSE", 20240102, 20240102);
	pa.release();
	pa.handleQuote(tick2, 1);
	// _stopped 守卫（L288-289）先返回，所以 _bd_mgr 那条查询根本没跑
	EXPECT_EQ(tick2->getContractInfo(), nullptr);
	tick2->release();
}

TEST_F(R18ParserAdapterTest, HandleQuoteRejectedWhenDatesZeroOrContractUnknown)
{
	// _dt_mgr 传 NULL：一旦漏过 actiondate/tradingdate 或 contract==NULL 的守卫
	// （ParserAdapter.cpp:291/301），落到 L304 就是空指针解引用，用例必崩
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);

	WTSTickData* zeroAction = make_tick("600000", "SSE", 0, 20240102);
	pa.handleQuote(zeroAction, 0);
	EXPECT_EQ(zeroAction->getContractInfo(), nullptr);
	zeroAction->release();

	WTSTickData* zeroTrading = make_tick("600000", "SSE", 20240102, 0);
	pa.handleQuote(zeroTrading, 0);
	EXPECT_EQ(zeroTrading->getContractInfo(), nullptr);
	zeroTrading->release();

	WTSTickData* unknown = make_tick("999999", "SSE", 20240102, 20240102);
	pa.handleQuote(unknown, 0);
	EXPECT_EQ(unknown->getContractInfo(), nullptr);
	unknown->release();
}

TEST_F(R18ParserAdapterTest, HandleQuoteKeepsPresetContractInsteadOfLookingUp)
{
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);
	// 行情代码指向 SSE.600000，但合约信息预置成 SHFE.rb2410
	WTSTickData* tick = make_tick("600000", "SSE", 20240102, 20240102);
	tick->setContractInfo(_ctShfe);
	pa.handleQuote(tick, 1);
	// ParserAdapter.cpp:294-299 的 if 不成立 -> 不重查、不覆写
	EXPECT_EQ(tick->getContractInfo(), _ctShfe);
	tick->release();

	// 查找命中路径会把合约写回（L297-298），且 writeTick 因 _writer==NULL 返回 false，
	// 因此 L307-308 的 _idx_fact->handle_quote 不会执行（传 nullptr 也不崩）
	WTSTickData* tick2 = make_tick("rb2410", "SHFE", 20240102, 20240102);
	pa.handleQuote(tick2, 1);
	EXPECT_EQ(tick2->getContractInfo(), _ctShfe);
	tick2->release();
}

TEST_F(R18ParserAdapterTest, ThreeLevel2HandlersRejectBeforeDataManager)
{
	// _dt_mgr==NULL：证明 L240-245 / L257-262 / L274-279 的三道守卫全部生效
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);

	WTSOrdQueData* q0 = make_ordque("600000", "SSE", 0, 20240102);
	pa.handleOrderQueue(q0);
	EXPECT_EQ(q0->getContractInfo(), nullptr);
	q0->release();

	WTSOrdDtlData* d0 = make_orddtl("600000", "SSE", 20240102, 0);
	pa.handleOrderDetail(d0);
	EXPECT_EQ(d0->getContractInfo(), nullptr);
	d0->release();

	WTSTransData* t0 = make_trans("600000", "SSE", 0, 0);
	pa.handleTransaction(t0);
	EXPECT_EQ(t0->getContractInfo(), nullptr);
	t0->release();

	WTSOrdQueData* qBad = make_ordque("NOPE", "SSE", 20240102, 20240102);
	pa.handleOrderQueue(qBad);
	EXPECT_EQ(qBad->getContractInfo(), nullptr);
	qBad->release();

	WTSOrdDtlData* dBad = make_orddtl("NOPE", "SHFE", 20240102, 20240102);
	pa.handleOrderDetail(dBad);
	EXPECT_EQ(dBad->getContractInfo(), nullptr);
	dBad->release();

	WTSTransData* tBad = make_trans("NOPE", "SSE", 20240102, 20240102);
	pa.handleTransaction(tBad);
	EXPECT_EQ(tBad->getContractInfo(), nullptr);
	tBad->release();
}

TEST_F(R18ParserAdapterTest, ThreeLevel2HandlersResolveContractAndDelegateWrite)
{
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);

	WTSOrdQueData* q = make_ordque("600000", "SSE", 20240102, 20240102);
	pa.handleOrderQueue(q);
	EXPECT_EQ(q->getContractInfo(), _ctSse);	// L281
	q->release();

	WTSOrdDtlData* d = make_orddtl("rb2410", "SHFE", 20240103, 20240103);
	pa.handleOrderDetail(d);
	EXPECT_EQ(d->getContractInfo(), _ctShfe);	// L264
	d->release();

	WTSTransData* t = make_trans("600000", "SSE", 20240104, 20240104);
	pa.handleTransaction(t);
	EXPECT_EQ(t->getContractInfo(), _ctSse);	// L247
	t->release();
}

TEST_F(R18ParserAdapterTest, MgrAddAndGetGuards)
{
	ParserAdapterMgr mgr;
	EXPECT_EQ(mgr.size(), 0u);

	ParserAdapterPtr empty;
	// ParserAdapter.cpp:339-340：adapter==NULL -> false（&& 短路，空 id 不会先被 strlen）
	EXPECT_FALSE(mgr.addAdapter("", empty));
	EXPECT_FALSE(mgr.addAdapter("x1", empty));

	ParserAdapterPtr a = std::make_shared<ParserAdapter>(&_bdMgr, &_dm, nullptr);
	ASSERT_TRUE(mgr.addAdapter("a1", a));
	EXPECT_EQ(mgr.size(), 1u);
	EXPECT_EQ(mgr.getAdapter("a1"), a);
	// ParserAdapter.cpp:354-363：未命中返回空 ParserAdapterPtr
	EXPECT_EQ(mgr.getAdapter("nope"), nullptr);

	// 重名拒绝（L342-347），原对象不被覆盖
	ParserAdapterPtr dup = std::make_shared<ParserAdapter>(&_bdMgr, &_dm, nullptr);
	EXPECT_FALSE(mgr.addAdapter("a1", dup));
	EXPECT_EQ(mgr.size(), 1u);
	EXPECT_EQ(mgr.getAdapter("a1"), a);
}

TEST_F(R18ParserAdapterTest, MgrRunAndReleaseDriveEveryAdapter)
{
	ParserAdapterMgr mgr;

	bool destroyed1 = false;
	bool destroyed2 = false;
	FakeParserApi* api1 = new FakeParserApi(&destroyed1, true);
	FakeParserApi* api2 = new FakeParserApi(&destroyed2, true);

	ParserAdapterPtr a1 = std::make_shared<ParserAdapter>(&_bdMgr, &_dm, nullptr);
	ParserAdapterPtr a2 = std::make_shared<ParserAdapter>(&_bdMgr, &_dm, nullptr);
	ASSERT_TRUE(a1->initExt("m1", api1));
	ASSERT_TRUE(a2->initExt("m2", api2));
	ASSERT_TRUE(mgr.addAdapter("m1", a1));
	ASSERT_TRUE(mgr.addAdapter("m2", a2));
	ASSERT_EQ(mgr.size(), 2u);

	// ParserAdapter.cpp:365-373：逐个 run，_parser_api 非空则调 connect（L226-227）
	mgr.run();
	EXPECT_EQ(api1->_connectCnt, 1);
	EXPECT_EQ(api2->_connectCnt, 1);

	// L327-335：逐个 release 后清空
	mgr.release();
	EXPECT_EQ(api1->_releaseCnt, 1);
	EXPECT_EQ(api2->_releaseCnt, 1);
	EXPECT_TRUE(destroyed1);
	EXPECT_TRUE(destroyed2);
	EXPECT_EQ(mgr.size(), 0u);
	EXPECT_EQ(mgr.getAdapter("m1"), nullptr);

	// 表已清空，二次 release 不再触碰任何适配器（不会二次 delete）
	mgr.release();
	EXPECT_EQ(api1->_releaseCnt, 1);
}

TEST_F(R18ParserAdapterTest, DISABLED_HandleQuoteCrashesWhenDataManagerIsNull)
{
	ParserAdapter pa(&_bdMgr, nullptr, nullptr);
	WTSTickData* tick = make_tick("600000", "SSE", 20240102, 20240102);
	pa.handleQuote(tick, 1);	// 落到 L304 的 _dt_mgr->writeTick -> 空指针解引用
	tick->release();
}

/*
 * 触发条件（禁用原因）：ParserAdapter::release()（L207-219）不调 api 后不置空
 * _parser_api/_remover，第二次 release() 会对同一对象再次 `delete`（L218）
 * ——double free，gwp-asan/`MallocDoubleFree` 直接崩。
 * 缺陷登记：bugs/R18_wtdtcore.md BUG-R18-003。
 */
TEST_F(R18ParserAdapterTest, DISABLED_DoubleReleaseDoubleDelete)
{
	bool destroyed = false;
	FakeParserApi* api = new FakeParserApi(&destroyed, false);
	ParserAdapter pa(&_bdMgr, &_dm, nullptr);
	ASSERT_TRUE(pa.initExt("dup", api));
	pa.release();
	EXPECT_TRUE(destroyed);
	pa.release();	// 第二次 delete
}

/*
 * 空字符串是 addAdapter 明确检查的拒绝条件；普通唯一 ID 应成功登记。
 * 这是安全边界与正向路径对照，不推断该 API 接受 NULL ID。
 */
TEST_F(R18ParserAdapterTest, MgrRejectsEmptyIdAndAcceptsUniqueId)
{
	ParserAdapterMgr mgr;
	ParserAdapterPtr a = std::make_shared<ParserAdapter>(&_bdMgr, &_dm, nullptr);

	EXPECT_FALSE(mgr.addAdapter("", a));
	EXPECT_EQ(mgr.size(), 0u);

	ASSERT_TRUE(mgr.addAdapter("r18_unique_parser", a));
	EXPECT_EQ(mgr.size(), 1u);
	EXPECT_EQ(mgr.getAdapter("r18_unique_parser"), a);

	mgr.release();
	EXPECT_EQ(mgr.size(), 0u);
}

}  // namespace
