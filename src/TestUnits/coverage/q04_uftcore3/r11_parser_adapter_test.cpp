/*!
 * \file r11_parser_adapter_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - ParserAdapter 状态机与拒绝分支
 *
 * 覆盖: init() 的 cfg 为空 / module 为空 / 模块加载失败三条拒绝路径 + `_cfg!=NULL` 二次拒绝,
 * initExt() 的 api 为空拒绝 + 正常注册订阅路径 + api->init 失败分支,
 * run() 的无 api / 有 api 两条分支,
 * handleQuote/handleOrderQueue/handleOrderDetail/handleTransaction 的四段式拒绝
 * (stopped、日期为0、合约不存在)与成功透传, handleParserLog/handleSymbolList,
 * 以及 ParserAdapterMgr 的 addAdapter/getAdapter/run/release。
 *
 * 不装载任何动态库、不起线程、不开 socket/shm: 解析模块用纯内存替身。
 * 注意 ParserAdapter::release() 在 _remover 为空时会 `delete _parser_api`,
 * 所以替身 new 到堆上、并把计数写回用例持有的 ApiStats, 释放后仍可断言。
 */
#include <boost/noncopyable.hpp>

#include "WtUftCore/ParserAdapter.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "q04_trader_fixture.h"

using namespace wtp;

namespace {

//替身的计数回写到调用方持有的结构, 这样 release() 里 delete 掉替身后仍可断言
struct ApiStats
{
	uint32_t _initCnt = 0;
	uint32_t _releaseCnt = 0;
	uint32_t _connectCnt = 0;
	uint32_t _disconnectCnt = 0;
	uint32_t _subCnt = 0;
	uint32_t _regSpiCnt = 0;
	size_t	 _subbedSize = 0;
	std::string _firstSubbed;
	bool	 _lastSpiIsAdapter = false;
};

class FakeParserApi : public IParserApi
{
public:
	explicit FakeParserApi(ApiStats* stats) : _stats(stats) {}

	bool _initRet = true;

	bool init(WTSVariant* /*config*/) override { _stats->_initCnt++; return _initRet; }
	void release() override { _stats->_releaseCnt++; }
	bool connect() override { _stats->_connectCnt++; return true; }
	bool disconnect() override { _stats->_disconnectCnt++; return true; }
	bool isConnected() override { return _stats->_connectCnt > 0; }

	void subscribe(const CodeSet& setCodes) override
	{
		_stats->_subCnt++;
		_stats->_subbedSize = setCodes.size();
		_stats->_firstSubbed = setCodes.empty() ? "" : *setCodes.begin();
	}

	void unsubscribe(const CodeSet& setCodes) override { _stats->_subCnt += (uint32_t)setCodes.size(); }

	void registerSpi(IParserSpi* spi) override
	{
		_stats->_regSpiCnt++;
		_stats->_lastSpiIsAdapter = (spi == _expectSpi);
	}

	void setExpectSpi(IParserSpi* spi) { _expectSpi = spi; }

private:
	ApiStats*	_stats;
	IParserSpi*	_expectSpi = nullptr;
};

class FakeStub : public IParserStub
{
public:
	uint32_t _quoteCnt = 0;
	uint32_t _oqCnt = 0;
	uint32_t _odCnt = 0;
	uint32_t _trCnt = 0;
	std::string _lastCode;
	std::string _lastOqCode;
	std::string _lastOdCode;
	std::string _lastTrCode;

	void handle_push_quote(WTSTickData* curTick) override
	{
		_quoteCnt++;
		_lastCode = curTick->code();
	}
	void handle_push_order_queue(WTSOrdQueData* curOrdQue) override
	{
		_oqCnt++;
		_lastOqCode = curOrdQue->code();
	}
	void handle_push_order_detail(WTSOrdDtlData* curOrdDtl) override
	{
		_odCnt++;
		_lastOdCode = curOrdDtl->code();
	}
	void handle_push_transaction(WTSTransData* curTrans) override
	{
		_trCnt++;
		_lastTrCode = curTrans->code();
	}
};

struct Env
{
	q04::FakeBaseDataMgr	_bd;
	FakeStub				_stub;
	Env() { _bd.add("SHFE", "ag", "ag2406"); }
};

//api 由 adapter 拥有; 用例结束不调 release 时手工 delete, 调了 release 就别再 delete
inline FakeParserApi* newApi(ApiStats* st, ParserAdapter* owner, FakeStub* stub)
{
	FakeParserApi* api = new FakeParserApi(st);
	api->setExpectSpi(owner);
	(void)stub;
	return api;
}

WTSTickData* makeTick(const char* exchg, const char* code, uint32_t actDate, uint32_t actTime)
{
	WTSTickStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = actDate;
	st.action_time = actTime;
	st.trading_date = actDate;
	st.price = 7000.5;
	return WTSTickData::create(st);
}

//////////////////////////////////////////////////////////////////////////
TEST(R11ParserAdapter, initRejectsNullConfig)
{
	Env env;
	ParserAdapter adapter;
	EXPECT_FALSE(adapter.init("p1", nullptr, &env._stub, &env._bd));
	//没有 api, run 必须失败
	EXPECT_FALSE(adapter.run());
	// 空配置在 ParserAdapter::init 的首个守卫返回，失败前不会设置 id。
	EXPECT_STREQ("", adapter.id());
}

TEST(R11ParserAdapter, initRejectsEmptyModule)
{
	Env env;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "");
	ParserAdapter adapter;
	EXPECT_FALSE(adapter.init("p2", cfg, &env._stub, &env._bd));
	//module 检查发生在 _cfg->retain() 之后, 所以二次 init 直接被 _cfg!=NULL 拒绝
	EXPECT_FALSE(adapter.init("p2", cfg, &env._stub, &env._bd));
	EXPECT_FALSE(adapter.run());
	cfg->release();
}

TEST(R11ParserAdapter, initRejectsUnloadableModule)
{
	Env env;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "R11NoSuchParserModule");
	cfg->append("filter", "");
	ParserAdapter adapter;
	//只拼路径 + load_library 返回 NULL, 不会真的打开任何库
	EXPECT_FALSE(adapter.init("p3", cfg, &env._stub, &env._bd));
	EXPECT_FALSE(adapter.init("p3", cfg, &env._stub, &env._bd));
	EXPECT_FALSE(adapter.run());
	cfg->release();
}

TEST(R11ParserAdapter, initExtRejectsNullApi)
{
	Env env;
	ParserAdapter adapter;
	EXPECT_FALSE(adapter.initExt("p4", nullptr, &env._stub, &env._bd));
	EXPECT_FALSE(adapter.run());
}

//正常路径: registerSpi + init + 全量合约订阅, initExt 返回 true
TEST(R11ParserAdapter, initExtSubscribesAllContracts)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p5", api, &env._stub, &env._bd));
	EXPECT_EQ((uint32_t)1, st._regSpiCnt);
	EXPECT_TRUE(st._lastSpiIsAdapter);
	EXPECT_EQ((uint32_t)1, st._initCnt);
	EXPECT_EQ((uint32_t)1, st._subCnt);
	//_filters 为空 -> 走 else 全量分支, 只注册了 SHFE.ag2406 一个合约
	EXPECT_EQ((size_t)1, st._subbedSize);
	EXPECT_EQ("SHFE.ag2406", st._firstSubbed);
	EXPECT_STREQ("SHFE.ag2406", env._bd.getContract("ag2406", "SHFE")->getFullAltCode());

	EXPECT_TRUE(adapter.run());
	EXPECT_EQ((uint32_t)1, st._connectCnt);
	EXPECT_EQ(&env._bd, adapter.getBaseDataMgr());

	//release 内部会 delete _parser_api, 用例不再手工 delete
	adapter.release();
	EXPECT_EQ((uint32_t)1, st._releaseCnt);
}

//api->init 失败: 仍然返回 true, 但不会订阅
TEST(R11ParserAdapter, initExtStillReturnsTrueWhenApiInitFails)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = new FakeParserApi(&st);
	api->_initRet = false;
	EXPECT_TRUE(adapter.initExt("p6", api, &env._stub, &env._bd));
	EXPECT_EQ((uint32_t)1, st._initCnt);
	EXPECT_EQ((uint32_t)0, st._subCnt);
	adapter.release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

//release() 之后再 run(): api 已经不在, 但 _parser_api 仍是野指针 -> 本用例只覆盖 release 的空 api 分支
TEST(R11ParserAdapter, releaseToleratesNullApi)
{
	Env env;
	ParserAdapter adapter;
	adapter.release();
	EXPECT_FALSE(adapter.run());
	EXPECT_STREQ("", adapter.id());
}

//////////////////////////////////////////////////////////////////////////
TEST(R11ParserAdapter, handleQuoteRejectAndPassThrough)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p7", api, &env._stub, &env._bd));

	//1) 空指针
	adapter.handleQuote(nullptr, 0);
	EXPECT_EQ((uint32_t)0, env._stub._quoteCnt);

	//2) actiondate 为 0
	WTSTickData* t2 = makeTick("SHFE", "ag2406", 0, 93000000);
	adapter.handleQuote(t2, 0);
	EXPECT_EQ((uint32_t)0, env._stub._quoteCnt);
	t2->release();

	//3) 合约不存在
	WTSTickData* t3 = makeTick("SHFE", "nothere", 20240315, 93000000);
	adapter.handleQuote(t3, 0);
	EXPECT_EQ((uint32_t)0, env._stub._quoteCnt);
	t3->release();

	//4) 成功: 代码被改写成标准全码
	WTSTickData* t4 = makeTick("SHFE", "ag2406", 20240315, 93000000);
	adapter.handleQuote(t4, 0);
	EXPECT_EQ((uint32_t)1, env._stub._quoteCnt);
	EXPECT_EQ("SHFE.ag2406", env._stub._lastCode);
	t4->release();

	//5) release 之后被 _stopped 吞掉
	adapter.release();
	WTSTickData* t5 = makeTick("SHFE", "ag2406", 20240315, 93200000);
	adapter.handleQuote(t5, 0);
	EXPECT_EQ((uint32_t)1, env._stub._quoteCnt);
	t5->release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

//tick 自带合约信息时不再查 bd_mgr
TEST(R11ParserAdapter, handleQuoteUsesEmbeddedContractInfo)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p8", api, &env._stub, &env._bd));

	WTSContractInfo* cInfo = env._bd.getContract("ag2406", "SHFE");
	ASSERT_NE(nullptr, cInfo);
	WTSTickData* t1 = makeTick("DCE", "notthere", 20240315, 145959000);
	t1->setContractInfo(cInfo);
	adapter.handleQuote(t1, 0);
	//走的是 quote->getContractInfo() 分支, 输出全码来自 contract
	EXPECT_EQ((uint32_t)1, env._stub._quoteCnt);
	EXPECT_EQ("SHFE.ag2406", env._stub._lastCode);
	t1->release();

	adapter.release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

//////////////////////////////////////////////////////////////////////////
TEST(R11ParserAdapter, orderQueueRejectAndPassThrough)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p9", api, &env._stub, &env._bd));

	WTSOrdQueData* oq = WTSOrdQueData::create("ag2406");
	WTSOrdQueStruct& os = oq->getOrdQueStruct();
	wt_strcpy(os.exchg, "SHFE");
	os.action_date = 0;
	os.trading_date = 20240315;
	adapter.handleOrderQueue(oq);
	EXPECT_EQ((uint32_t)0, env._stub._oqCnt);

	os.action_date = 20240315;
	os.trading_date = 0;
	adapter.handleOrderQueue(oq);
	EXPECT_EQ((uint32_t)0, env._stub._oqCnt);

	os.trading_date = 20240315;
	wt_strcpy(os.code, "unknown");
	adapter.handleOrderQueue(oq);
	EXPECT_EQ((uint32_t)0, env._stub._oqCnt);

	oq->setCode("ag2406");
	adapter.handleOrderQueue(oq);
	EXPECT_EQ((uint32_t)1, env._stub._oqCnt);
	EXPECT_EQ("SHFE.ag2406", env._stub._lastOqCode);

	adapter.release();
	oq->setCode("ag2406");
	adapter.handleOrderQueue(oq);
	EXPECT_EQ((uint32_t)1, env._stub._oqCnt);
	oq->release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

TEST(R11ParserAdapter, orderDetailRejectAndPassThrough)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p10", api, &env._stub, &env._bd));

	WTSOrdDtlData* od = WTSOrdDtlData::create("ag2406");
	WTSOrdDtlStruct& ds = od->getOrdDtlStruct();
	wt_strcpy(ds.exchg, "SHFE");
	ds.action_date = 20240315;
	ds.trading_date = 0;
	adapter.handleOrderDetail(od);
	EXPECT_EQ((uint32_t)0, env._stub._odCnt);

	ds.trading_date = 20240315;
	wt_strcpy(ds.code, "zzzz");
	adapter.handleOrderDetail(od);
	EXPECT_EQ((uint32_t)0, env._stub._odCnt);

	od->setCode("ag2406");
	adapter.handleOrderDetail(od);
	EXPECT_EQ((uint32_t)1, env._stub._odCnt);
	EXPECT_EQ("SHFE.ag2406", env._stub._lastOdCode);

	adapter.release();
	od->setCode("ag2406");
	adapter.handleOrderDetail(od);
	EXPECT_EQ((uint32_t)1, env._stub._odCnt);
	od->release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

TEST(R11ParserAdapter, transactionRejectAndPassThrough)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p11", api, &env._stub, &env._bd));

	WTSTransData* tr = WTSTransData::create("ag2406");
	WTSTransStruct& ts = tr->getTransStruct();
	wt_strcpy(ts.exchg, "SHFE");
	ts.action_date = 0;
	ts.trading_date = 20240315;
	adapter.handleTransaction(tr);
	EXPECT_EQ((uint32_t)0, env._stub._trCnt);

	ts.action_date = 20240315;
	wt_strcpy(ts.code, "nope");
	adapter.handleTransaction(tr);
	EXPECT_EQ((uint32_t)0, env._stub._trCnt);

	tr->setCode("ag2406");
	adapter.handleTransaction(tr);
	EXPECT_EQ((uint32_t)1, env._stub._trCnt);
	EXPECT_EQ("SHFE.ag2406", env._stub._lastTrCode);

	adapter.release();
	tr->setCode("ag2406");
	adapter.handleTransaction(tr);
	EXPECT_EQ((uint32_t)1, env._stub._trCnt);
	tr->release();
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

//////////////////////////////////////////////////////////////////////////
TEST(R11ParserAdapter, handleParserLogAndSymbolList)
{
	Env env;
	ParserAdapter adapter;
	ApiStats st;
	FakeParserApi* api = newApi(&st, &adapter, &env._stub);
	ASSERT_TRUE(adapter.initExt("p12", api, &env._stub, &env._bd));

	adapter.handleParserLog(LL_INFO, "r11 parser log before stop");
	adapter.handleSymbolList(nullptr);
	adapter.release();
	//_stopped 之后的日志分支只走一次提前返回
	adapter.handleParserLog(LL_ERROR, "r11 parser log after stop");
	//api 已由 adapter->release() 里的 `delete _parser_api` 释放, 此处不得再 delete
}

//////////////////////////////////////////////////////////////////////////
TEST(R11ParserAdapter, adapterMgrRegisterAndLookup)
{
	Env env;
	ParserAdapterMgr mgr;

	ParserAdapterPtr empty;
	EXPECT_FALSE(mgr.addAdapter("m1", empty));

	ParserAdapterPtr first(new ParserAdapter());
	ApiStats st;
	FakeParserApi* api1 = newApi(&st, first.get(), &env._stub);
	ASSERT_TRUE(first->initExt("m2", api1, &env._stub, &env._bd));
	//空 id 会被 strlen(id)==0 拒绝
	EXPECT_FALSE(mgr.addAdapter("", first));
	EXPECT_TRUE(mgr.addAdapter("m2", first));

	//同名必须被拒绝
	ParserAdapterPtr dup(new ParserAdapter());
	EXPECT_FALSE(mgr.addAdapter("m2", dup));

	EXPECT_EQ(first.get(), mgr.getAdapter("m2").get());
	EXPECT_EQ(nullptr, mgr.getAdapter("not-exist").get());

	mgr.run();
	EXPECT_EQ((uint32_t)1, st._connectCnt);

	mgr.release();
	EXPECT_EQ((size_t)0, mgr._adapters.size());
	//mgr.release() 里的 adapter->release() 会 delete api1 并调用其 release()
	EXPECT_EQ((uint32_t)1, st._releaseCnt);
	//api1 同样由 mgr.release() -> adapter->release() 释放
}

} // namespace
