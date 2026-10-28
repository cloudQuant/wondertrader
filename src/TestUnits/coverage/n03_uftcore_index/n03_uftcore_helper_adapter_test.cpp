/**
 * n03_uftcore_helper_adapter_test.cpp
 *
 * 覆盖 src/WtUftCore 中不依赖 UFT 柜台/共享内存/网络的纯逻辑:
 *   WtHelper.cpp/.h        getCWD/getModulePath/getBaseDir/getOutputDir/getStraDataDir/
 *                          getStraUsrDatDir/getPortifolioDir/setGenerateDir/setInstDir/
 *                          setTime/setTDate/getDate/getTime/getSecs/getTradingDate
 *   UftDataDefs.h          uft::BlockHeader/DetailStruct/OrderStruct/TradeStruct/RoundStruct
 *   TraderAdapter.h/.cpp   PosItem::total_pos/avail_pos, RiskParams, 构造/析构,
 *                          id/state/init(NULL)/run(NULL api)/release/getOrders/cancel/
 *                          cancelAll/getUndoneQty/enumPosition/getPosition/isTradeEnabled/
 *                          addSink + TraderAdapterMgr::addAdapter/getAdapter/getAdapters/run/release
 *   ParserAdapter.h/.cpp   init/initExt/run/release/handleQuote/handleParserLog/getBaseDataMgr
 *                          + ParserAdapterMgr::addAdapter/getAdapter/release
 *
 * 落盘一律在 temp_directory_path() 下的 n03_ 前缀唯一目录, 结束即清理.
 * 不断言墙钟时间与本地时区.
 */
#include <gtest/gtest.h>

#include "WtUftCore/WtHelper.h"
#include "WtUftCore/UftDataDefs.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/ITrdNotifySink.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/IParserApi.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

USING_NS_WTP;

namespace {

//////////////////////////////////////////////////////////////////////////
// 假基础数据管理器
class U3BaseDataMgr : public IBaseDataMgr
{
public:
	WTSCommodityInfo* getCommodity(const char*) override { return nullptr; }
	WTSCommodityInfo* getCommodity(const char*, const char*) override { return nullptr; }

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		++_calls;
		auto it = _maps.find(std::string(exchg) + "." + code);
		return it == _maps.end() ? nullptr : it->second;
	}

	// 返回一个真实但为空的合约数组, 让 ParserAdapter 的订阅流程能走通
	WTSArray* getContracts(const char* = "", uint32_t = 0) override
	{
		++_contractArrayCalls;
		return WTSArray::create();
	}

	WTSSessionInfo* getSession(const char*) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }
	bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
	uint32_t calcTradingDate(const char*, uint32_t, uint32_t, bool = false) override { return 0; }
	uint64_t getBoundaryTime(const char*, uint32_t, bool = false, bool = true) override { return 0; }

	void reg(WTSContractInfo* cInfo) { _maps[cInfo->getFullCode()] = cInfo; }

	int _calls = 0;
	int _contractArrayCalls = 0;

private:
	wt_hashmap<std::string, WTSContractInfo*> _maps;
};

// ParserAdapter::release() 会 delete 掉外部传入的 api, 所以计数必须存放在
// 比 api 对象寿命更长的全局变量里, 否则就是 use-after-free。
static int g_apiReleases = 0;
static int g_apiConnects = 0;

//////////////////////////////////////////////////////////////////////////
// 假行情接口: 记录注册/订阅/连接调用
class U3ParserApi : public IParserApi
{
public:
	bool init(WTSVariant* cfg) override { _cfgSeen = (cfg != nullptr); return _initRet; }
	void release() override { ++g_apiReleases; }
	bool connect() override { ++g_apiConnects; return _connectRet; }
	bool disconnect() override { ++_disconnects; return true; }
	bool isConnected() override { return _connected; }
	void subscribe(const CodeSet& setCodes) override { _subscribed = (uint32_t)setCodes.size(); ++_subCalls; }
	void unsubscribe(const CodeSet&) override { ++_unsubCalls; }
	void registerSpi(IParserSpi* spi) override { _spi = spi; }

	bool _initRet = true;
	bool _connectRet = true;
	bool _connected = false;
	bool _cfgSeen = false;
	int _disconnects = 0;
	int _subCalls = 0;
	int _unsubCalls = 0;
	uint32_t _subscribed = 999;
	IParserSpi* _spi = nullptr;
};

//////////////////////////////////////////////////////////////////////////
// 假回调桩: 记录转发给策略的推送
class U3ParserStub : public IParserStub
{
public:
	void handle_push_quote(WTSTickData* curTick) override
	{
		_ticks.push_back(curTick->getTickStruct());
	}

	std::vector<WTSTickStruct> _ticks;
};

//////////////////////////////////////////////////////////////////////////
// 假交易回调接收者
class U3Sink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t, const char*, bool, uint32_t, double, double) override { ++_trades; }
	void on_order(uint32_t, const char*, bool, uint32_t, double, double, double, bool) override { ++_orders; }
	void on_channel_ready(uint32_t) override { ++_ready; }
	void on_channel_lost() override { ++_lost; }

	int _trades = 0;
	int _orders = 0;
	int _ready = 0;
	int _lost = 0;
};

//////////////////////////////////////////////////////////////////////////
// 临时目录: 进程唯一, 析构清理
class U3TempDir
{
public:
	explicit U3TempDir(const char* tag)
	{
		_dir = std::filesystem::temp_directory_path()
			/ ("n03_u3_" + std::string(tag) + "_" + std::to_string(::getpid()));
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
		std::filesystem::create_directories(_dir, ec);
	}

	~U3TempDir()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	std::string str() const { return _dir.string(); }

private:
	std::filesystem::path _dir;
};

WTSTickStruct u3Tick(const char* exchg, const char* code, uint32_t actDate = 20240301)
{
	WTSTickStruct t;
	memset(&t, 0, sizeof(WTSTickStruct));
	wt_strcpy(t.exchg, exchg);
	wt_strcpy(t.code, code);
	t.trading_date = actDate;
	t.action_date = actDate;
	t.action_time = 93000000;
	t.price = 10.5;
	return t;
}

}

//////////////////////////////////////////////////////////////////////////
// WtHelper
TEST(n03_wthelper, date_time_and_inst_dir)
{
	WtHelper::setTime(20240301, 93059000, 59);
	EXPECT_EQ(20240301u, WtHelper::getDate());
	EXPECT_EQ(93059000u, WtHelper::getTime());
	EXPECT_EQ(59u, WtHelper::getSecs());

	// 默认参数 secs/raw 省略
	WtHelper::setTime(20240302, 145900000);
	EXPECT_EQ(20240302u, WtHelper::getDate());
	EXPECT_EQ(145900000u, WtHelper::getTime());

	WtHelper::setTDate(20240304);
	EXPECT_EQ(20240304u, WtHelper::getTradingDate());

	WtHelper::setInstDir("/opt/wondertrader/");
	EXPECT_EQ("/opt/wondertrader/", WtHelper::getInstDir());
	WtHelper::setInstDir("");
	EXPECT_TRUE(WtHelper::getInstDir().empty());
}

TEST(n03_wthelper, cwd_and_module_path)
{
	const std::string cwd = WtHelper::getCWD();
	EXPECT_FALSE(cwd.empty());
	// standardisePath 保证末尾带分隔符
	EXPECT_EQ('/', cwd.back());

	const std::string mod = WtHelper::getModulePath("libutfparsers.so", "parsers");
	EXPECT_EQ(cwd + "parsers/libutfparsers.so", mod);

	WtHelper::setInstDir("/tmp/n03_inst/");
	const std::string fromInst = WtHelper::getModulePath("x.so", "facts", false);
	EXPECT_EQ("/tmp/n03_inst/facts/x.so", fromInst);
	WtHelper::setInstDir("");
}

TEST(n03_wthelper, generated_dirs_are_created_under_configured_root)
{
	// 把生成目录指到临时目录, 避免污染仓库
	U3TempDir tmp("gendirs");
	const std::string root = tmp.str();
	WtHelper::setGenerateDir(root.c_str());

	// 先清掉, 验证 getter 会自建目录
	std::error_code ec;
	std::filesystem::remove_all(root, ec);

	const char* base = WtHelper::getBaseDir();
	ASSERT_NE(nullptr, base);
	EXPECT_TRUE(std::filesystem::is_directory(base, ec));

	const char* output = WtHelper::getOutputDir();
	EXPECT_STREQ("outputs/", std::strrchr(output, '/') + 1);
	EXPECT_TRUE(std::filesystem::is_directory(output, ec));

	const char* stra = WtHelper::getStraDataDir();
	EXPECT_NE(nullptr, std::strstr(stra, "stradata/"));
	EXPECT_TRUE(std::filesystem::is_directory(stra, ec));

	const char* usr = WtHelper::getStraUsrDatDir();
	EXPECT_NE(nullptr, std::strstr(usr, "userdata/"));
	EXPECT_TRUE(std::filesystem::is_directory(usr, ec));

	const char* port = WtHelper::getPortifolioDir();
	EXPECT_NE(nullptr, std::strstr(port, "portfolio/"));
	EXPECT_TRUE(std::filesystem::is_directory(port, ec));
}

//////////////////////////////////////////////////////////////////////////
// UftDataDefs
TEST(n03_uftdefs, packed_struct_layouts)
{
	EXPECT_EQ(8, uft::FLAG_SIZE);
	EXPECT_STREQ("&^%$#@!", uft::BLK_FLAG);

	// 头部: 8 标识 + 4 类型 + 4 日期 + 4 容量 + 4 大小 = 24, 且 1 字节对齐
	EXPECT_EQ(24u, sizeof(uft::BlockHeader));
	EXPECT_EQ(24u, sizeof(uft::PositionBlock));
	EXPECT_EQ(24u, sizeof(uft::OrderBlock));
	EXPECT_EQ(24u, sizeof(uft::TradeBlock));
	EXPECT_EQ(24u, sizeof(uft::RoundBlock));

	uft::DetailStruct d;
	EXPECT_EQ(0u, d._direct);
	EXPECT_DOUBLE_EQ(0.0, d._volume);
	EXPECT_DOUBLE_EQ(0.0, d._open_price);
	EXPECT_EQ(0u, d._open_tdate);
	EXPECT_DOUBLE_EQ(0.0, d._position_profit);
	EXPECT_EQ('\0', d._code[0]);

	uft::OrderStruct o;
	memset(&o, 0, sizeof(o));
	o._direct = 1;
	o._offset = 2;
	o._state = 1;
	EXPECT_EQ(1u, o._direct);
	EXPECT_EQ(2u, o._offset);
	EXPECT_EQ(1u, o._state);

	uft::TradeStruct t;
	memset(&t, 0, sizeof(t));
	t._trading_date = 20240301;
	EXPECT_EQ(20240301u, t._trading_date);

	uft::RoundStruct r;
	memset(&r, 0, sizeof(r));
	r._profit = -1.5;
	EXPECT_DOUBLE_EQ(-1.5, r._profit);
}

//////////////////////////////////////////////////////////////////////////
// TraderAdapter
TEST(n03_trader_adapter, pos_item_and_risk_params)
{
	TraderAdapter::PosItem pos;
	EXPECT_DOUBLE_EQ(0.0, pos.total_pos(true));
	EXPECT_DOUBLE_EQ(0.0, pos.total_pos(false));
	EXPECT_DOUBLE_EQ(0.0, pos.avail_pos(true));
	EXPECT_DOUBLE_EQ(0.0, pos.avail_pos(false));

	pos.l_newvol = 3; pos.l_prevol = 5;
	pos.l_newavail = 2; pos.l_preavail = 4;
	pos.s_newvol = 7; pos.s_prevol = 8;
	pos.s_newavail = 1; pos.s_preavail = 6;
	EXPECT_DOUBLE_EQ(8.0, pos.total_pos(true));
	EXPECT_DOUBLE_EQ(15.0, pos.total_pos(false));
	EXPECT_DOUBLE_EQ(6.0, pos.avail_pos(true));
	EXPECT_DOUBLE_EQ(7.0, pos.avail_pos(false));

	TraderAdapter::RiskParams rp;
	EXPECT_EQ(0u, rp._order_times_boundary);
	EXPECT_EQ(0u, rp._order_stat_timespan);
	EXPECT_EQ(0u, rp._order_total_limits);
	EXPECT_EQ(0u, rp._cancel_times_boundary);
	EXPECT_EQ(0u, rp._cancel_stat_timespan);
	EXPECT_EQ(0u, rp._cancel_total_limits);
}

TEST(n03_trader_adapter, fresh_adapter_public_accessors)
{
	TraderAdapter adapter;
	U3Sink sink;
	adapter.addSink(&sink);

	EXPECT_STREQ("", adapter.id());
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());

	// 未 init 的只读查询走安全分支
	EXPECT_FALSE(adapter.run());
	adapter.release();
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_EQ(nullptr, adapter.getOrders("SSE.600000"));
	EXPECT_FALSE(adapter.cancel(1));
	EXPECT_TRUE(adapter.cancelAll("SSE.600000").empty());
	EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty("SSE.600000"));
	EXPECT_DOUBLE_EQ(0.0, adapter.enumPosition("SSE.600000"));
	EXPECT_DOUBLE_EQ(0.0, adapter.enumPosition());
	EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("SSE.600000", false, 3));
	EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("SSE.600000", true, 1));
	EXPECT_DOUBLE_EQ(0.0, adapter.getPosition("SSE.600000", true, 2));
	// isTradeEnabled 在头文件里声明为 inline 却只在 TraderAdapter.cpp 里定义,
	// 其它翻译单元引用会触发 -Wundefined-inline(-Werror), 见 BUG-N03-006, 故此处不调用。

	// init 传 NULL 参数直接失败
	U3BaseDataMgr bdMgr;
	EXPECT_FALSE(adapter.init("sqt", nullptr, &bdMgr, nullptr));
	EXPECT_FALSE(adapter.initExt("sqt", nullptr, &bdMgr, nullptr));
}

TEST(n03_trader_adapter_mgr, add_get_run_release)
{
	TraderAdapterMgr mgr;
	TraderAdapterPtr empty;
	TraderAdapterPtr unnamed = std::make_shared<TraderAdapter>();
	TraderAdapterPtr dup = std::make_shared<TraderAdapter>();
	// 空指针或空名字都不接受
	EXPECT_FALSE(mgr.addAdapter("a", empty));
	EXPECT_FALSE(mgr.addAdapter("", unnamed));

	TraderAdapterPtr first = std::make_shared<TraderAdapter>();
	ASSERT_TRUE(mgr.addAdapter("sqt", first));
	EXPECT_EQ(first, mgr.getAdapter("sqt"));
	// 重名拒绝
	EXPECT_FALSE(mgr.addAdapter("sqt", dup));
	EXPECT_EQ(1u, mgr.getAdapters().size());
	// 未注册返回空指针
	EXPECT_EQ(nullptr, mgr.getAdapter("nope"));

	// 没有 trader api 时 run/release 都是安全空操作
	mgr.run();
	mgr.release();
	EXPECT_EQ(0u, mgr.getAdapters().size());
	EXPECT_EQ(nullptr, mgr.getAdapter("sqt"));
}

//////////////////////////////////////////////////////////////////////////
// ParserAdapter
TEST(n03_parser_adapter, init_rejects_bad_config)
{
	U3BaseDataMgr bdMgr;
	U3ParserStub stub;
	ParserAdapter adapter;

	EXPECT_STREQ("", adapter.id());
	// NULL 配置
	EXPECT_FALSE(adapter.init("sse", nullptr, &stub, &bdMgr));
	// 没有 module 字段: 在加载动态库之前就返回
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("filter", "SSE");
	EXPECT_FALSE(adapter.init("sse", cfg, &stub, &bdMgr));
	cfg->release();

	// NULL api
	EXPECT_FALSE(adapter.initExt("sse", nullptr, &stub, &bdMgr));
	EXPECT_FALSE(adapter.run());
	// 模块缺失的 init 已经把 _bd_mgr 存下来了
	EXPECT_EQ(&bdMgr, adapter.getBaseDataMgr());
}

TEST(n03_parser_adapter, init_ext_subscribes_and_pushes_quote)
{
	U3BaseDataMgr bdMgr;
	U3ParserStub stub;
	WTSContractInfo* cInfo = WTSContractInfo::create("600000", "浦发银行", "SSE", "SSE.600000");
	ASSERT_NE(nullptr, cInfo);
	bdMgr.reg(cInfo);

	U3ParserApi* api = new U3ParserApi();
	api->_initRet = true;
	ParserAdapter adapter;
	ASSERT_TRUE(adapter.initExt("sse", api, &stub, &bdMgr));
	EXPECT_STREQ("sse", adapter.id());
	EXPECT_EQ(&bdMgr, adapter.getBaseDataMgr());
	// api 侧确实被注册与订阅过
	EXPECT_NE(nullptr, api->_spi);
	EXPECT_EQ(0u, api->_subscribed);
	EXPECT_GT(bdMgr._contractArrayCalls, 0);

	// 连接走通
	EXPECT_TRUE(adapter.run());
	EXPECT_EQ(1, g_apiConnects);

	WTSTickStruct t = u3Tick("SSE", "600000");
	WTSTickData* tick = WTSTickData::create(t);
	// 未停止 + 有交易日: 应转发给 stub, 并把 code 改写成 fullcode
	adapter.handleQuote(tick, 0);
	ASSERT_EQ(1u, stub._ticks.size());
	EXPECT_STREQ("SSE.600000", stub._ticks[0].code);
	tick->release();

	// actiondate 为 0 的快照被丢弃
	WTSTickStruct t2 = u3Tick("SSE", "600000", 0);
	t2.trading_date = 0;
	WTSTickData* tick2 = WTSTickData::create(t2);
	adapter.handleQuote(tick2, 0);
	EXPECT_EQ(1u, stub._ticks.size());
	tick2->release();

	// 基础数据里查不到的合约被丢弃
	WTSTickStruct t3 = u3Tick("SSE", "999999");
	WTSTickData* tick3 = WTSTickData::create(t3);
	adapter.handleQuote(tick3, 0);
	EXPECT_EQ(1u, stub._ticks.size());
	tick3->release();

	// NULL 快照安全返回
	adapter.handleQuote(nullptr, 0);
	EXPECT_EQ(1u, stub._ticks.size());

	// 日志回调不崩溃
	adapter.handleParserLog(LL_INFO, "hello parser");

	adapter.release();
	EXPECT_EQ(1, g_apiReleases);
	// release 之后进入停止态, 任何推送都不再转发
	adapter.handleParserLog(LL_INFO, "should be dropped");
	WTSTickStruct t5 = u3Tick("SSE", "600000");
	WTSTickData* tick4 = WTSTickData::create(t5);
	adapter.handleQuote(tick4, 0);
	EXPECT_EQ(1u, stub._ticks.size());
	tick4->release();

	cInfo->release();
}

TEST(n03_parser_adapter_mgr, add_get_release)
{
	ParserAdapterMgr mgr;
	ParserAdapterPtr empty;
	EXPECT_FALSE(mgr.addAdapter("a", empty));

	ParserAdapterPtr first = std::make_shared<ParserAdapter>();
	U3BaseDataMgr bdMgr;
	U3ParserStub stub;
	U3ParserApi* api = new U3ParserApi();
	api->_initRet = false;	// api 初始化失败, 走错误日志分支, 不碰合约列表
	ASSERT_TRUE(first->initExt("lost", api, &stub, &bdMgr));

	ASSERT_TRUE(mgr.addAdapter("lost", first));
	EXPECT_EQ(first, mgr.getAdapter("lost"));
	EXPECT_FALSE(mgr.addAdapter("lost", first));
	EXPECT_EQ(nullptr, mgr.getAdapter("nope"));

	mgr.run();
	mgr.release();
	EXPECT_EQ(nullptr, mgr.getAdapter("lost"));
}

//////////////////////////////////////////////////////////////////////////
// 缺陷类整体隔离: 未经 init/run 建好 _stat_map 与 _trader_api 时, 下面这些公开接口
// 会直接解引用空指针或 end() 迭代器, 属于同一崩溃类, 整组放进本 DISABLED_ 用例:
//   getInfos            -> _stat_map 为 NULL
//   checkOrderLimits    -> _risk_params_map 为空时 getRiskParams 返回 find("default") 的 end()
//   checkCancelLimits   -> 同上
//   openLong/buy        -> getContract() 对不含 '.' 的 stdCode 用 findFirst 的 npos 作为下标,
//                          buf[npos] = '\0' 越界写 (BUG-N03-005/007)
TEST(DISABLED_n03_trader_adapter, uninitialized_deref_class)
{
	TraderAdapter adapter;
	adapter.getInfos("SSE.600000");
	adapter.checkOrderLimits("SSE.600000");
	adapter.checkCancelLimits("SSE.600000");
	adapter.openLong("600000", 10.0, 1, 0);
	adapter.openShort("600000", 10.0, 1, 0);
}
