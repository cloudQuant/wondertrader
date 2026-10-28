/*
 * lane R93（第十七波）— TraderYD 有 API 指针半边
 *
 * 目标：src/TraderYD/TraderYD.cpp。用 r93_yd_api_stub.h 的机械空实现替代真实
 * YDApi，经派生探针把 protected m_pUserAPI 指向 stub，覆盖 connect/disconnect/
 * release、login→notifyLogin→notifyFinishInit→notifyCaughtUp、orderInsert/
 * orderAction、四个查询入口以及回调写入本地缓存的正向分支。不连真实柜台。
 *
 * 既有 T5/R60 已覆盖：init、无 API 时的拒绝、登录失败错误表、无持仓的开平仓
 * 回调缓存、错误/状态映射、release 无 API 分支。本文件不重复这些断言。
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "TraderYD/TraderYD.h"
#include "r93_yd_api_stub.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

namespace
{
	namespace fs = std::filesystem;

	int g_r93_yd_tmp_seq = 0;

	/// 产品把委托/订单缓存相对写到 ydlocal/；用例统一在系统临时目录里跑，
	/// 析构时恢复 cwd 并整目录删除。
	class R93YdTempCwd
	{
	public:
		R93YdTempCwd()
		{
			_old = fs::current_path();
			_dir = fs::temp_directory_path() /
				("wt_r93_yd_" + std::to_string((long)::getpid()) + "_" + std::to_string(++g_r93_yd_tmp_seq));
			std::error_code ec;
			fs::create_directories(_dir, ec);
			fs::current_path(_dir, ec);
		}
		~R93YdTempCwd()
		{
			std::error_code ec;
			fs::current_path(_old, ec);
			fs::remove_all(_dir, ec);
		}
		const fs::path& dir() const { return _dir; }

	private:
		fs::path _old;
		fs::path _dir;
	};

	class R93YdBdMgr : public IBaseDataMgr
	{
	public:
		virtual ~R93YdBdMgr()
		{
			for (WTSContractInfo* ct : _cts)
				ct->release();
			for (WTSCommodityInfo* comm : _comms)
				comm->release();
		}

		void regis(WTSCommodityInfo* comm, WTSContractInfo* ct)
		{
			ct->setCommInfo(comm);
			_comms.push_back(comm);
			_cts.push_back(ct);
			_key[std::string(ct->getCode()) + "|" + ct->getExchg()] = ct;
		}

		WTSCommodityInfo* getCommodity(const char* exchgpid) override
		{
			for (WTSCommodityInfo* comm : _comms)
			{
				if (strcmp(comm->getFullPid(), exchgpid) == 0)
					return comm;
			}
			return NULL;
		}
		WTSCommodityInfo* getCommodity(const char*, const char*) override { return NULL; }
		WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
		{
			auto it = _key.find(std::string(code ? code : "") + "|" + (exchg ? exchg : ""));
			return it == _key.end() ? NULL : it->second;
		}
		WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }
		WTSSessionInfo* getSession(const char*) override { return NULL; }
		WTSSessionInfo* getSessionByCode(const char*, const char* = "") override { return NULL; }
		WTSArray* getAllSessions() override { return NULL; }
		bool isHoliday(const char*, uint32_t, bool = false) override { return false; }
		uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool = false) override { return uDate; }
		uint64_t getBoundaryTime(const char*, uint32_t tDate, bool = false, bool = true) override { return tDate; }
		uint32_t getGlobalSize() override { return 8; }

	private:
		std::vector<WTSCommodityInfo*> _comms;
		std::vector<WTSContractInfo*> _cts;
		std::map<std::string, WTSContractInfo*> _key;
	};

	class R93YdSink : public ITraderSpi
	{
	public:
		struct AccountRec
		{
			bool valid = false;
			double pre = 0, deposit = 0, withdraw = 0, balance = 0;
			std::string currency;
		};
		struct PositionRec
		{
			bool valid = false;
			std::string code, exchg;
			int dir = -1;
			double pre = 0, newPos = 0, availPre = 0, availNew = 0;
			double margin = 0, avgPrice = 0, cost = 0, dynProfit = 0;
		};
		struct OrderRec
		{
			bool valid = false;
			std::string code, orderID, entrustID, userTag;
			double price = 0, volume = 0;
			int state = -1;
		};
		struct TradeRec
		{
			bool valid = false;
			std::string code, tradeID, refOrder, userTag;
			double price = 0, volume = 0, amount = 0;
		};

		IBaseDataMgr* getBaseDataMgr() override { return &bd_; }
		void handleEvent(WTSTraderEvent e, int32_t ec) override { events.push_back({ (int)e, ec }); }
		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			logs.push_back(message ? message : "");
		}
		void onLoginResult(bool succ, const char* msg, uint32_t date) override
		{
			logins.push_back({ succ, msg ? msg : "", date });
		}
		void onRspAccount(WTSArray* ay) override
		{
			AccountRec r;
			if (ay && ay->size() > 0)
			{
				WTSAccountInfo* acc = static_cast<WTSAccountInfo*>(ay->at(0));
				r.valid = true;
				r.pre = acc->getPreBalance();
				r.deposit = acc->getDeposit();
				r.withdraw = acc->getWithdraw();
				r.balance = acc->getBalance();
				r.currency = acc->getCurrency();
			}
			accounts.push_back(r);
		}
		void onRspPosition(const WTSArray* ay) override
		{
			PositionRec r;
			if (ay && ay->size() > 0)
			{
				WTSPositionItem* pos = static_cast<WTSPositionItem*>(const_cast<WTSArray*>(ay)->at(0));
				r.valid = true;
				r.code = pos->getCode();
				r.exchg = pos->getExchg();
				r.dir = (int)pos->getDirection();
				r.pre = pos->getPrePosition();
				r.newPos = pos->getNewPosition();
				r.availPre = pos->getAvailPrePos();
				r.availNew = pos->getAvailNewPos();
				r.margin = pos->getMargin();
				r.avgPrice = pos->getAvgPrice();
				r.cost = pos->getPositionCost();
				r.dynProfit = pos->getDynProfit();
			}
			positions.push_back(r);
		}
		void onRspOrders(const WTSArray* ay) override
		{
			OrderRec r;
			if (ay && ay->size() > 0)
			{
				WTSOrderInfo* o = static_cast<WTSOrderInfo*>(const_cast<WTSArray*>(ay)->at(0));
				r.valid = true;
				r.code = o->getCode();
				r.orderID = o->getOrderID();
				r.entrustID = o->getEntrustID();
				r.userTag = o->getUserTag();
				r.price = o->getPrice();
				r.volume = o->getVolume();
				r.state = (int)o->getOrderState();
			}
			orderArrays.push_back(r);
		}
		void onRspTrades(const WTSArray* ay) override
		{
			TradeRec r;
			if (ay && ay->size() > 0)
			{
				WTSTradeInfo* t = static_cast<WTSTradeInfo*>(const_cast<WTSArray*>(ay)->at(0));
				r.valid = true;
				r.code = t->getCode();
				r.tradeID = t->getTradeID();
				r.refOrder = t->getRefOrder();
				r.userTag = t->getUserTag();
				r.price = t->getPrice();
				r.volume = t->getVolume();
				r.amount = t->getAmount();
			}
			tradeArrays.push_back(r);
		}
		void onPushOrder(WTSOrderInfo* o) override
		{
			OrderRec r;
			if (o)
			{
				r.valid = true;
				r.code = o->getCode();
				r.orderID = o->getOrderID();
				r.entrustID = o->getEntrustID();
				r.userTag = o->getUserTag();
				r.price = o->getPrice();
				r.volume = o->getVolume();
				r.state = (int)o->getOrderState();
			}
			pushedOrders.push_back(r);
		}
		void onPushTrade(WTSTradeInfo* t) override
		{
			TradeRec r;
			if (t)
			{
				r.valid = true;
				r.code = t->getCode();
				r.tradeID = t->getTradeID();
				r.refOrder = t->getRefOrder();
				r.userTag = t->getUserTag();
				r.price = t->getPrice();
				r.volume = t->getVolume();
				r.amount = t->getAmount();
			}
			pushedTrades.push_back(r);
		}

		R93YdBdMgr bd_;
		std::vector<std::pair<int, int32_t>> events;
		std::vector<std::string> logs;
		struct LoginRec { bool succ; std::string msg; uint32_t date; };
		std::vector<LoginRec> logins;
		std::vector<AccountRec> accounts;
		std::vector<PositionRec> positions;
		std::vector<OrderRec> orderArrays;
		std::vector<TradeRec> tradeArrays;
		std::vector<OrderRec> pushedOrders;
		std::vector<TradeRec> pushedTrades;
	};

	class R93YdProbe : public TraderYD
	{
	public:
		void setApi(YDApi* api) { m_pUserAPI = api; }
		void setFactory(YDApi* (*factory)(const char*)) { m_funcCreator = factory; }
		void setState(WrapperState state) { m_wrapperState = state; }
		void presetWorker()
		{
			// 产品 connect() 只在 m_thrdWorker==NULL 时起查询线程；预置一个未启动的
			// std::thread 占位（非 joinable），让连接用例完全不创建线程。
			m_thrdWorker.reset(new StdThread());
		}
		bool runNextQuery()
		{
			StdUniqueLock lock(m_mtxQuery);
			if (m_queQuery.empty())
				return false;
			CommonExecuter handler = m_queQuery.front();
			handler();
			m_queQuery.pop();
			return true;
		}
		size_t queued()
		{
			StdUniqueLock lock(m_mtxQuery);
			return m_queQuery.size();
		}
	};

	R93YdApiStub* g_r93_yd_stub = NULL;
	YDApi* r93_yd_create(const char*) { return g_r93_yd_stub; }

	class R93YdTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("config", "yd_r93.conf");
			cfg->append("user", "yduser");
			cfg->append("pass", "ydpass");
			cfg->append("update_cache", true);
			ASSERT_TRUE(api_.init(cfg));
			cfg->release();

			comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
			comm_->setCategory(CC_Future);
			comm_->setCoverMode(CM_CoverToday);
			comm_->setTradingMode(TM_Both);
			comm_->setVolScale(5);
			ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
			ct_->setTotalIndex(1);
			sink_.bd_.regis(comm_, ct_);
			api_.registerSpi(&sink_);

			strncpy(stub_.instrument_.InstrumentID, "ag2512", sizeof(stub_.instrument_.InstrumentID) - 1);
			strncpy(stub_.exchange_.ExchangeID, "SHFE", sizeof(stub_.exchange_.ExchangeID) - 1);
			stub_.account_.PreBalance = 100000.0;
			stub_.account_.Deposit = 5000.0;
			stub_.account_.Withdraw = 1000.0;

			api_.setApi(&stub_);
		}

		void TearDown() override
		{
			// 合约/品种由 R93YdBdMgr 析构统一释放，避免二次 release
			api_.release();
		}

		void add_pre_position(int dir, int qty, double settlement)
		{
			YDPrePosition p;
			memset(&p, 0, sizeof(p));
			p.PositionDirection = dir;
			p.PrePosition = qty;
			p.PreSettlementPrice = settlement;
			p.m_pInstrument = &stub_.instrument_;
			p.m_pAccount = &stub_.account_;
			stub_.pre_positions_.push_back(p);
		}

		void login_and_finish()
		{
			ASSERT_EQ(0, api_.login("yduser", "ydpass", ""));
			ASSERT_EQ(1, stub_.n_login_);
			api_.notifyLogin(0, 100, false);
			ASSERT_FALSE(api_.isConnected());
			api_.notifyFinishInit();
			api_.notifyCaughtUp();
			ASSERT_TRUE(api_.isConnected());
			ASSERT_EQ(1u, sink_.logins.size());
			EXPECT_TRUE(sink_.logins.back().succ);
			EXPECT_EQ(20260105u, sink_.logins.back().date);
		}

		YDOrder make_order(int dir, int offset, int type, double price, int volume,
			int status, int sysID, int orderRef)
		{
			YDOrder o;
			memset(&o, 0, sizeof(o));
			o.Direction = (char)dir;
			o.OffsetFlag = (char)offset;
			o.OrderType = (char)type;
			o.Price = price;
			o.OrderVolume = volume;
			o.OrderStatus = status;
			o.OrderSysID = sysID;
			o.OrderRef = orderRef;
			return o;
		}

		WTSEntrust* make_entrust(const char* eid, const char* tag,
			double price, double volume, WTSDirectionType dir, WTSOffsetType offset,
			WTSPriceType ptype, WTSOrderFlag flag)
		{
			WTSEntrust* ent = WTSEntrust::create("ag2512", volume, price, "SHFE");
			ent->setContractInfo(ct_);
			ent->setDirection(dir);
			ent->setOffsetType(offset);
			ent->setPriceType(ptype);
			ent->setOrderFlag(flag);
			ent->setEntrustID(eid);
			if (tag)
				ent->setUserTag(tag);
			return ent;
		}

		R93YdTempCwd temp_;
		WTSCommodityInfo* comm_ = NULL;
		WTSContractInfo* ct_ = NULL;
		R93YdSink sink_;
		R93YdApiStub stub_;
		R93YdProbe api_;
	};

	/* ------------------------------------------------------------------ */
	/* 连接/销毁                                                            */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L661-719：release 调 startDestroy；connect 调 creator+start；
	/// disconnect 调厂商 disconnect；m_thrdWorker 非空时不重复起线程。
	TEST_F(R93YdTest, connect_disconnect_release_drive_stub_lifecycle)
	{
		g_r93_yd_stub = &stub_;
		api_.setFactory(&r93_yd_create);
		api_.presetWorker();

		api_.connect();
		EXPECT_EQ(1, stub_.n_start_);
		EXPECT_EQ(static_cast<YDListener*>(&api_), stub_.listener_);
		EXPECT_FALSE(api_.isConnected());
		EXPECT_EQ(0, api_.logout());	// m_pUserAPI 非空即返回 0

		api_.disconnect();
		EXPECT_EQ(1, stub_.n_disconnect_);

		api_.release();
		EXPECT_EQ(1, stub_.n_destroy_);
		EXPECT_FALSE(api_.isConnected());

		// release 幂等：TearDown 再调一次不会重复碰厂商对象
		api_.release();
		EXPECT_EQ(1, stub_.n_destroy_);
	}

	/* ------------------------------------------------------------------ */
	/* 登录 → 初始化 → 查询                                                 */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L258-302（登录成功：交易日、eid/oid 缓存落盘）、L346-423
	/// （notifyFinishInit 资金/昨仓）、L610-624（catchup）、L868-913（账户/持仓查询）。
	TEST_F(R93YdTest, login_finish_init_and_queries_emit_funds_and_positions)
	{
		add_pre_position(YD_PD_Long, 10, 7000.0);
		add_pre_position(YD_PD_Short, 2, 7100.0);

		login_and_finish();

		ASSERT_EQ(0, api_.queryAccount());
		ASSERT_EQ(1u, api_.queued());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.accounts.size());
		const R93YdSink::AccountRec& acc = sink_.accounts[0];
		EXPECT_TRUE(acc.valid);
		EXPECT_DOUBLE_EQ(100000.0, acc.pre);
		EXPECT_DOUBLE_EQ(5000.0, acc.deposit);
		EXPECT_DOUBLE_EQ(1000.0, acc.withdraw);
		EXPECT_DOUBLE_EQ(104000.0, acc.balance);
		EXPECT_EQ("CNY", acc.currency);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(2u, sink_.positions.size());
		const R93YdSink::PositionRec* posPtr = NULL;
		for (const R93YdSink::PositionRec& p : sink_.positions)
		{
			if (p.dir == (int)WDT_LONG)
			{
				posPtr = &p;
				break;
			}
		}
		ASSERT_NE(nullptr, posPtr);
		const R93YdSink::PositionRec& pos = *posPtr;
		EXPECT_TRUE(pos.valid);
		EXPECT_EQ("ag2512", pos.code);
		EXPECT_EQ((int)WDT_LONG, pos.dir);
		EXPECT_DOUBLE_EQ(10.0, pos.pre);
		EXPECT_DOUBLE_EQ(0.0, pos.newPos);
		EXPECT_DOUBLE_EQ(10.0, pos.availPre);
		EXPECT_DOUBLE_EQ(0.0, pos.availNew);
		EXPECT_DOUBLE_EQ(7000.0, pos.avgPrice);
		EXPECT_DOUBLE_EQ(350000.0, pos.cost);
		EXPECT_DOUBLE_EQ(0.0, pos.margin);
		EXPECT_DOUBLE_EQ(0.0, pos.dynProfit);

		// notifyAccount 的两条守卫与字段覆盖（不重算 balance，是产品现状）
		api_.notifyAccount(NULL);
		YDAccount updated = stub_.account_;
		updated.PreBalance = 200000.0;
		updated.Deposit = 2000.0;
		updated.Withdraw = 500.0;
		api_.notifyAccount(&updated);

		ASSERT_EQ(0, api_.queryAccount());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(2u, sink_.accounts.size());
		EXPECT_DOUBLE_EQ(200000.0, sink_.accounts[1].pre);
		EXPECT_DOUBLE_EQ(2000.0, sink_.accounts[1].deposit);
		EXPECT_DOUBLE_EQ(500.0, sink_.accounts[1].withdraw);
	}

	/* ------------------------------------------------------------------ */
	/* 下单 / 撤单                                                          */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L782-866：userTag 为空时用 m_orderRef；userTag 非空时解析
	/// eid 并写 m_eidCache；撤单的 OrderSysID/连接选择；失败日志。
	TEST_F(R93YdTest, order_insert_and_cancel_fill_yd_requests)
	{
		login_and_finish();

		WTSEntrust* first = make_entrust("", NULL, 7000.0, 2,
			WDT_LONG, WOT_OPEN, WPT_LIMITPRICE, WOF_NOR);
		ASSERT_EQ(0, api_.orderInsert(first));
		EXPECT_EQ(1, stub_.n_insert_);
		EXPECT_EQ(100, stub_.last_order_.OrderRef);
		EXPECT_DOUBLE_EQ(7000.0, stub_.last_order_.Price);
		EXPECT_EQ(2, stub_.last_order_.OrderVolume);
		EXPECT_EQ(YD_D_Buy, stub_.last_order_.Direction);
		EXPECT_EQ(YD_OF_Open, stub_.last_order_.OffsetFlag);
		EXPECT_EQ(YD_ODT_Limit, stub_.last_order_.OrderType);
		EXPECT_EQ(YD_YOF_Normal, stub_.last_order_.YDOrderFlag);
		EXPECT_EQ(YD_CS_Any, stub_.last_order_.ConnectionSelectionType);
		EXPECT_EQ(&stub_.instrument_, stub_.last_order_inst_);
		first->release();

		char eid[64] = { 0 };
		ASSERT_TRUE(api_.makeEntrustID(eid, sizeof(eid)));
		const std::string eidStr(eid);
		WTSEntrust* tagged = make_entrust(eidStr.c_str(), "tag-101", 7010.0, 3,
			WDT_SHORT, WOT_CLOSETODAY, WPT_ANYPRICE, WOF_FOK);
		ASSERT_EQ(0, api_.orderInsert(tagged));
		EXPECT_EQ(2, stub_.n_insert_);
		EXPECT_EQ(101, stub_.last_order_.OrderRef);
		EXPECT_EQ(YD_D_Buy, stub_.last_order_.Direction);	// 空平 = 买
		EXPECT_EQ(YD_OF_CloseToday, stub_.last_order_.OffsetFlag);
		EXPECT_EQ(YD_ODT_FOK, stub_.last_order_.OrderType);
		tagged->release();

		stub_.stub_insert_ret_ = false;
		WTSEntrust* failing = make_entrust("", NULL, 7020.0, 1,
			WDT_LONG, WOT_OPEN, WPT_LIMITPRICE, WOF_NOR);
		ASSERT_EQ(0, api_.orderInsert(failing));
		ASSERT_FALSE(sink_.logs.empty());
		EXPECT_NE(std::string::npos, sink_.logs.back().find("Order inserting failed"));
		failing->release();

		WTSEntrustAction* action = WTSEntrustAction::create("ag2512", "SHFE");
		action->setEntrustID("yduser#0000000101");
		action->setOrderID("8888");
		ASSERT_EQ(0, api_.orderAction(action));
		EXPECT_EQ(1, stub_.n_cancel_);
		EXPECT_EQ(8888, stub_.last_cancel_.OrderSysID);
		EXPECT_EQ(YD_YOF_Normal, stub_.last_cancel_.YDOrderFlag);
		EXPECT_EQ(YD_CS_Any, stub_.last_cancel_.ConnectionSelectionType);
		EXPECT_EQ(&stub_.exchange_, stub_.last_cancel_exchg_);
		EXPECT_EQ(&stub_.account_, stub_.last_cancel_account_);
		action->release();

		WTSEntrustAction* bad = WTSEntrustAction::create("ag2512", "SHFE");
		bad->setEntrustID("yduser#1#2");	// 两个 '#' → extractEntrustID 返回 false
		bad->setOrderID("9999");
		EXPECT_EQ(-1, api_.orderAction(bad));
		EXPECT_EQ(1, stub_.n_cancel_);
		bad->release();
	}

	/* ------------------------------------------------------------------ */
	/* 委托/订单缓存回填 + 查询数组                                          */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L1008-1021（makeOrderInfo 的 eidCache 命中 → userTag + oidCache.put）、
	/// L1090（makeTradeRecord 的 oidCache 命中）、L915-963（订单/成交查询数组）。
	TEST_F(R93YdTest, cache_roundtrip_marks_orders_and_trades_and_queries_arrays)
	{
		login_and_finish();

		WTSEntrust* ent = make_entrust("yduser#0000000107", "ut-42", 7000.0, 2,
			WDT_LONG, WOT_OPEN, WPT_LIMITPRICE, WOF_NOR);
		ASSERT_EQ(0, api_.orderInsert(ent));
		ent->release();

		YDOrder order = make_order(YD_D_Buy, YD_OF_Open, YD_ODT_Limit, 7000.0, 2,
			YD_OS_Queuing, 9001, 107);
		api_.notifyOrder(&order, &stub_.instrument_, NULL);
		ASSERT_EQ(1u, sink_.pushedOrders.size());
		EXPECT_EQ("ut-42", sink_.pushedOrders[0].userTag);
		EXPECT_EQ("9001", sink_.pushedOrders[0].orderID);
		EXPECT_EQ("yduser#0000000107", sink_.pushedOrders[0].entrustID);

		ASSERT_EQ(0, api_.queryOrders());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.orderArrays.size());
		EXPECT_TRUE(sink_.orderArrays[0].valid);
		EXPECT_EQ("ut-42", sink_.orderArrays[0].userTag);
		EXPECT_EQ("9001", sink_.orderArrays[0].orderID);

		YDTrade trd;
		memset(&trd, 0, sizeof(trd));
		trd.Direction = YD_D_Buy;
		trd.OffsetFlag = YD_OF_Open;
		trd.Price = 7000.0;
		trd.Volume = 2;
		trd.TradeID = 5001;
		trd.OrderSysID = 9001;
		api_.notifyTrade(&trd, &stub_.instrument_, NULL);
		ASSERT_EQ(1u, sink_.pushedTrades.size());
		EXPECT_EQ("ut-42", sink_.pushedTrades[0].userTag);
		EXPECT_EQ("9001", sink_.pushedTrades[0].refOrder);
		EXPECT_DOUBLE_EQ(70000.0, sink_.pushedTrades[0].amount);	// 5 * 2 * 7000

		ASSERT_EQ(0, api_.queryTrades());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.tradeArrays.size());
		EXPECT_TRUE(sink_.tradeArrays[0].valid);
		EXPECT_EQ("ut-42", sink_.tradeArrays[0].userTag);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.positions.size());
		EXPECT_DOUBLE_EQ(2.0, sink_.positions[0].newPos);
		EXPECT_DOUBLE_EQ(2.0, sink_.positions[0].availNew);
	}

	/* ------------------------------------------------------------------ */
	/* 平仓冻结/释放：CM_CoverToday                                         */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L444-474（首推平仓冻结：平今扣今仓）与 L481-513
	/// （撤单释放：平今加回今仓）。位置项来自 notifyFinishInit 的昨仓。
	TEST_F(R93YdTest, close_today_order_freezes_and_releases_today_available)
	{
		add_pre_position(YD_PD_Long, 10, 7000.0);
		login_and_finish();

		YDOrder freeze = make_order(YD_D_Sell, YD_OF_CloseToday, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Queuing, 6001, 201);
		api_.notifyOrder(&freeze, &stub_.instrument_, NULL);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.positions.size());
		EXPECT_DOUBLE_EQ(10.0, sink_.positions[0].availPre);
		EXPECT_DOUBLE_EQ(-4.0, sink_.positions[0].availNew);

		YDOrder canceled = make_order(YD_D_Sell, YD_OF_CloseToday, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Canceled, 6001, 201);
		canceled.TradeVolume = 1;
		api_.notifyOrder(&canceled, &stub_.instrument_, NULL);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(2u, sink_.positions.size());
		EXPECT_DOUBLE_EQ(10.0, sink_.positions[1].availPre);
		EXPECT_DOUBLE_EQ(-1.0, sink_.positions[1].availNew);
	}

	/* ------------------------------------------------------------------ */
	/* 平仓冻结/释放：CM_OpenCover                                          */
	/* ------------------------------------------------------------------ */

	/// TraderYD.cpp L461-470（不区分平今：先冻结昨仓）与 L500-509（取消释放分支）。
	/// 释放分支因 maxQty = min(newQty-availNew, untrade) 恒 <= untrade，内层
	/// decimal::lt 不可达；本用例只断言不崩且回调正常（缺陷见 bugs/R93.md）。
	TEST_F(R93YdTest, open_cover_order_freezes_yesterday_available)
	{
		comm_->setCoverMode(CM_OpenCover);
		add_pre_position(YD_PD_Long, 5, 7000.0);
		login_and_finish();

		YDOrder freeze = make_order(YD_D_Sell, YD_OF_Close, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Queuing, 6101, 301);
		api_.notifyOrder(&freeze, &stub_.instrument_, NULL);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(1u, sink_.positions.size());
		EXPECT_DOUBLE_EQ(1.0, sink_.positions[0].availPre);
		EXPECT_DOUBLE_EQ(0.0, sink_.positions[0].availNew);

		YDOrder canceled = make_order(YD_D_Sell, YD_OF_Close, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Canceled, 6101, 301);
		api_.notifyOrder(&canceled, &stub_.instrument_, NULL);
		ASSERT_EQ(0, api_.queryPositions());
		EXPECT_TRUE(api_.runNextQuery());
		EXPECT_EQ(2u, sink_.positions.size());
	}

	/// 现状缺陷：CM_OpenCover 下撤单释放只从今仓可用量里扣（newQty-availNew=0），
	/// 昨仓冻结的 4 手不会回补。修复后 availPre 应为 5；当前为 1，故整条禁用。
	TEST_F(R93YdTest, DISABLED_open_cover_cancel_should_release_yesterday_freeze)
	{
		comm_->setCoverMode(CM_OpenCover);
		add_pre_position(YD_PD_Long, 5, 7000.0);
		login_and_finish();

		YDOrder freeze = make_order(YD_D_Sell, YD_OF_Close, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Queuing, 6201, 401);
		api_.notifyOrder(&freeze, &stub_.instrument_, NULL);
		YDOrder canceled = make_order(YD_D_Sell, YD_OF_Close, YD_ODT_Limit, 7050.0, 4,
			YD_OS_Canceled, 6201, 401);
		api_.notifyOrder(&canceled, &stub_.instrument_, NULL);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(api_.runNextQuery());
		ASSERT_EQ(2u, sink_.positions.size());
		EXPECT_DOUBLE_EQ(5.0, sink_.positions[1].availPre);
	}
} // namespace
