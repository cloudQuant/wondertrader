/*
 * lane R60（第十二波）— TraderMocker 异步公开面补测
 *
 * 目标文件：src/TraderMocker/TraderMocker.cpp（missed 598/984）
 *
 * 与 t6_tradermocker_reachable_test.cpp 的分工（不重复）：
 *  - T6 已覆盖构造/析构、init、release/disconnect 空转、makeEntrustID、splitVolume、
 *    以及「未驱动 io_context 时只入队、不回报」的 deferred 契约。
 *  - 本文件只走 **公开 API connect()** 起 io_context（不调 login、不进入私有 match_once），
 *    从而真正执行 orderInsert/orderAction/query* 的 lambda 体、load_positions/save_positions
 *    与 UDP handle_read/extract_buffer 的公开可达部分。
 *  - connect() 只 bind 0.0.0.0:<span 内配置端口>（本文件除 UDP 用例传 0 取临时端口，
 *    UDP 用例用回环临时端口），不访问外网、不连真实柜台；等待全部是 3s 上限内的短轮询。
 *  - match_once()/撮合价量、私有 _awaits/_positions 仍不硬闯（见 bugs/R60.md 与 T6-011）。
 *
 * 临时落盘：init 会在 cwd 下建 ./mocker_<id>/，本文件把 cwd 切到系统临时目录，
 * 用例结束（含 ASSERT 早退）由 R60TempCwd 析构恢复并清理。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <boost/asio.hpp>

#include "TraderMocker/TraderMocker.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"

USING_NS_WTP;

namespace
{
	namespace fs = std::filesystem;

	/// 3s 上限的短轮询：所有异步回报都由 connect() 起的 io 线程产出，
	/// 不用固定 sleep 猜时序；超时即断言失败。
	template <typename Pred>
	bool wait_until(Pred pred, int timeout_ms = 3000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (pred())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		return pred();
	}

	/// 把 cwd 切到系统临时目录，保证 TraderMocker::init 的 ./mocker_<id>/ 不落在仓库里。
	class R60TempCwd
	{
	public:
		R60TempCwd()
		{
			static std::atomic<uint32_t> seq{ 0 };
			_old = fs::current_path();
			_dir = fs::temp_directory_path() /
				("wondertrader_r60_mocker_" + std::to_string((long)::getpid()) + "_" + std::to_string(seq.fetch_add(1)));
			fs::create_directories(_dir);
			fs::current_path(_dir);
		}

		~R60TempCwd()
		{
			std::error_code ec;
			fs::current_path(_old, ec);
			fs::remove_all(_dir, ec);
		}

		R60TempCwd(const R60TempCwd&) = delete;
		R60TempCwd& operator=(const R60TempCwd&) = delete;

	private:
		fs::path _old;
		fs::path _dir;
	};

	class R60MockerBdMgr : public IBaseDataMgr
	{
	public:
		virtual ~R60MockerBdMgr()
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

		virtual WTSCommodityInfo* getCommodity(const char* exchgpid) override
		{
			for (WTSCommodityInfo* comm : _comms)
			{
				if (strcmp(comm->getFullPid(), exchgpid) == 0)
					return comm;
			}
			return NULL;
		}

		virtual WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
		{
			(void)exchg;
			(void)pid;
			return NULL;
		}

		virtual WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override
		{
			(void)uDate;
			auto it = _key.find(std::string(code ? code : "") + "|" + (exchg ? exchg : ""));
			return it == _key.end() ? NULL : it->second;
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
			(void)code;
			(void)exchg;
			return NULL;
		}
		virtual WTSArray* getAllSessions() override { return NULL; }
		virtual bool isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) override
		{
			(void)pid;
			(void)uDate;
			(void)isTpl;
			return false;
		}
		virtual uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override
		{
			(void)stdPID;
			(void)uTime;
			(void)isSession;
			return uDate;
		}
		virtual uint64_t getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override
		{
			(void)stdPID;
			(void)isSession;
			(void)isStart;
			return tDate;
		}
		virtual uint32_t getGlobalSize() override { return 8; }

	private:
		std::vector<WTSCommodityInfo*> _comms;
		std::vector<WTSContractInfo*> _cts;
		std::map<std::string, WTSContractInfo*> _key;
	};

	/// ITraderSpi 替身：所有回调只做快照，回调返回后产品代码会 release 原对象。
	class R60MockerSink : public ITraderSpi
	{
	public:
		struct EntrustRec
		{
			bool has_err = false;
			int err_code = 0;
			std::string err_msg;
			std::string code;
			std::string exchg;
			double price = 0;
			double volume = 0;
		};
		struct OrderRec
		{
			bool valid = false;
			std::string code;
			std::string exchg;
			std::string order_id;
			std::string entrust_id;
			std::string state_msg;
			double price = 0;
			double volume = 0;
			double vol_left = 0;
			int state = -1;
		};
		struct PosRec
		{
			std::string code;
			int direction = -1;
			double new_pos = 0;
			double avail = 0;
		};
		struct AccountRec
		{
			std::string currency;
			double balance = -1;
			double available = -1;
			double margin = -1;
		};
		struct ArrayRec
		{
			bool is_null = true;
			uint32_t size = 0;
			std::string first_id;
			int first_state = -1;
		};

		virtual IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

		virtual void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			event_ = e;
			event_ec_ = ec;
			_event_.fetch_add(1);
		}

		virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override
		{
			(void)msg;
			(void)tradingdate;
			std::lock_guard<std::mutex> lk(_mtx);
			login_succ_ = bSucc;
			_login_.fetch_add(1);
		}

		virtual void handleTraderLog(WTSLogLevel ll, const char* message) override
		{
			(void)ll;
			std::lock_guard<std::mutex> lk(_mtx);
			_logs.push_back(message ? message : "");
			_log_.fetch_add(1);
		}

		virtual void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
		{
			EntrustRec r;
			if (entrust)
			{
				r.code = entrust->getCode() ? entrust->getCode() : "";
				r.exchg = entrust->getExchg() ? entrust->getExchg() : "";
				r.price = entrust->getPrice();
				r.volume = entrust->getVolume();
			}
			if (err)
			{
				r.has_err = true;
				r.err_code = (int)err->getErrorCode();
				r.err_msg = err->getMessage() ? err->getMessage() : "";
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_entrusts.push_back(r);
			_entrust_.fetch_add(1);
		}

		virtual void onPushOrder(WTSOrderInfo* orderInfo) override
		{
			OrderRec r;
			if (orderInfo)
			{
				r.valid = true;
				r.code = orderInfo->getCode() ? orderInfo->getCode() : "";
				r.exchg = orderInfo->getExchg() ? orderInfo->getExchg() : "";
				r.order_id = orderInfo->getOrderID() ? orderInfo->getOrderID() : "";
				r.entrust_id = orderInfo->getEntrustID() ? orderInfo->getEntrustID() : "";
				r.state_msg = orderInfo->getStateMsg() ? orderInfo->getStateMsg() : "";
				r.price = orderInfo->getPrice();
				r.volume = orderInfo->getVolume();
				r.vol_left = orderInfo->getVolLeft();
				r.state = (int)orderInfo->getOrderState();
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_orders.push_back(r);
			_push_order_.fetch_add(1);
		}

		virtual void onPushTrade(WTSTradeInfo* tradeRecord) override
		{
			(void)tradeRecord;
			_push_trade_.fetch_add(1);
		}

		virtual void onRspAccount(WTSArray* ayAccounts) override
		{
			AccountRec r;
			if (ayAccounts && ayAccounts->size() > 0)
			{
				WTSAccountInfo* info = (WTSAccountInfo*)ayAccounts->at(0);
				if (info)
				{
					r.currency = info->getCurrency() ? info->getCurrency() : "";
					r.balance = info->getBalance();
					r.available = info->getAvailable();
					r.margin = info->getMargin();
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_accounts.push_back(r);
			_account_.fetch_add(1);
		}

		virtual void onRspPosition(const WTSArray* ayPositions) override
		{
			std::vector<PosRec> recs;
			if (ayPositions)
			{
				for (uint32_t i = 0; i < ayPositions->size(); i++)
				{
					WTSPositionItem* item = (WTSPositionItem*)const_cast<WTSArray*>(ayPositions)->at(i);
					if (!item)
						continue;
					PosRec r;
					r.code = item->getCode() ? item->getCode() : "";
					r.direction = (int)item->getDirection();
					r.new_pos = item->getNewPosition();
					r.avail = item->getAvailNewPos();
					recs.push_back(r);
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_positions.push_back(recs);
			_position_.fetch_add(1);
		}

		virtual void onRspOrders(const WTSArray* ayOrders) override
		{
			ArrayRec r;
			if (ayOrders)
			{
				r.is_null = false;
				r.size = ayOrders->size();
				if (r.size > 0)
				{
					WTSOrderInfo* first = (WTSOrderInfo*)const_cast<WTSArray*>(ayOrders)->at(0);
					if (first)
					{
						r.first_id = first->getOrderID() ? first->getOrderID() : "";
						r.first_state = (int)first->getOrderState();
					}
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_orders_array = r;
			_orders_rsp_.fetch_add(1);
		}

		virtual void onRspTrades(const WTSArray* ayTrades) override
		{
			ArrayRec r;
			if (ayTrades)
			{
				r.is_null = false;
				r.size = ayTrades->size();
				if (r.size > 0)
				{
					WTSTradeInfo* first = (WTSTradeInfo*)const_cast<WTSArray*>(ayTrades)->at(0);
					if (first)
						r.first_id = first->getTradeID() ? first->getTradeID() : "";
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_trades_array = r;
			_trades_rsp_.fetch_add(1);
		}

		virtual void onTraderError(WTSError* err, void* pData = NULL) override
		{
			(void)pData;
			std::string msg;
			int code = 0;
			if (err)
			{
				code = (int)err->getErrorCode();
				msg = err->getMessage() ? err->getMessage() : "";
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_error_code = code;
			_error_msg = msg;
			_error_.fetch_add(1);
		}

		uint32_t n_event() const { return _event_.load(); }
		uint32_t n_login() const { return _login_.load(); }
		uint32_t n_entrust() const { return _entrust_.load(); }
		uint32_t n_push_order() const { return _push_order_.load(); }
		uint32_t n_push_trade() const { return _push_trade_.load(); }
		uint32_t n_account() const { return _account_.load(); }
		uint32_t n_position() const { return _position_.load(); }
		uint32_t n_orders_rsp() const { return _orders_rsp_.load(); }
		uint32_t n_trades_rsp() const { return _trades_rsp_.load(); }
		uint32_t n_error() const { return _error_.load(); }
		uint32_t n_log() const { return _log_.load(); }

		EntrustRec last_entrust() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _entrusts.empty() ? EntrustRec{} : _entrusts.back();
		}
		OrderRec last_order() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _orders.empty() ? OrderRec{} : _orders.back();
		}
		std::vector<PosRec> last_positions() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _positions.empty() ? std::vector<PosRec>{} : _positions.back();
		}
		AccountRec last_account() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _accounts.empty() ? AccountRec{} : _accounts.back();
		}
		ArrayRec last_orders_array() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _orders_array;
		}
		ArrayRec last_trades_array() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _trades_array;
		}
		int last_error_code() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _error_code;
		}
		std::string last_error_msg() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _error_msg;
		}
		bool log_contains(const std::string& needle) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			for (const std::string& s : _logs)
			{
				if (s.find(needle) != std::string::npos)
					return true;
			}
			return false;
		}
		int last_event() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return (int)event_;
		}

		R60MockerBdMgr bd_;

	private:
		mutable std::mutex _mtx;
		std::atomic<uint32_t> _event_{ 0 };
		std::atomic<uint32_t> _login_{ 0 };
		std::atomic<uint32_t> _entrust_{ 0 };
		std::atomic<uint32_t> _push_order_{ 0 };
		std::atomic<uint32_t> _push_trade_{ 0 };
		std::atomic<uint32_t> _account_{ 0 };
		std::atomic<uint32_t> _position_{ 0 };
		std::atomic<uint32_t> _orders_rsp_{ 0 };
		std::atomic<uint32_t> _trades_rsp_{ 0 };
		std::atomic<uint32_t> _error_{ 0 };
		std::atomic<uint32_t> _log_{ 0 };

		WTSTraderEvent event_ = WTE_Connect;
		int32_t event_ec_ = 0;
		bool login_succ_ = false;
		std::vector<std::string> _logs;
		std::vector<EntrustRec> _entrusts;
		std::vector<OrderRec> _orders;
		std::vector<std::vector<PosRec>> _positions;
		std::vector<AccountRec> _accounts;
		ArrayRec _orders_array;
		ArrayRec _trades_array;
		int _error_code = 0;
		std::string _error_msg;
	};

	class R60MockerTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ag_comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
			ag_comm_->setCategory(CC_Future);
			ag_comm_->setCoverMode(CM_OpenCover);
			ag_comm_->setPriceMode(PM_Both);
			ag_comm_->setTradingMode(TM_Both);
			ag_comm_->setPriceTick(0.5);
			ag_comm_->setVolScale(5);
			ag_ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
			ag_ct_->setTotalIndex(1);
			sink_.bd_.regis(ag_comm_, ag_ct_);

			stock_comm_ = WTSCommodityInfo::create("stock", "股票", "SSE", "TRADING", "CHINA");
			stock_comm_->setCategory(CC_Stock);
			stock_comm_->setCoverMode(CM_OpenCover);
			stock_comm_->setPriceMode(PM_Limit);
			stock_comm_->setTradingMode(TM_Long);
			stock_comm_->setPriceTick(0.01);
			stock_comm_->setVolScale(1);
			stock_ct_ = WTSContractInfo::create("600000", "浦发银行", "SSE", "stock");
			stock_ct_->setTotalIndex(2);
			sink_.bd_.regis(stock_comm_, stock_ct_);

			rb_comm_ = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "TRADING", "CHINA");
			rb_comm_->setCategory(CC_Future);
			rb_comm_->setCoverMode(CM_CoverToday);
			rb_comm_->setPriceMode(PM_Both);
			rb_comm_->setTradingMode(TM_Both);
			rb_comm_->setPriceTick(1.0);
			rb_comm_->setVolScale(10);
			rb_ct_ = WTSContractInfo::create("rb2510", "螺纹2510", "SHFE", "rb");
			rb_ct_->setTotalIndex(3);
			sink_.bd_.regis(rb_comm_, rb_ct_);

			if_comm_ = WTSCommodityInfo::create("IF", "沪深300", "CFFEX", "TRADING", "CHINA");
			if_comm_->setCategory(CC_Future);
			if_comm_->setCoverMode(CM_None);
			if_comm_->setPriceMode(PM_Both);
			if_comm_->setTradingMode(TM_Both);
			if_comm_->setPriceTick(0.2);
			if_comm_->setVolScale(300);
			if_ct_ = WTSContractInfo::create("IF2512", "IF2512", "CFFEX", "IF");
			if_ct_->setTotalIndex(4);
			sink_.bd_.regis(if_comm_, if_ct_);

			api_.registerSpi(&sink_);
		}

		void TearDown() override
		{
			// release 会 join connect() 起的 io 线程；必须在本用例结束前完成。
			api_.release();
		}

		void init_api(uint32_t id, int udp_port = 0)
		{
			WTSVariant* cfg = WTSVariant::createObject();
			cfg->append("span", 5);
			cfg->append("newpx", true);
			cfg->append("mockerid", id);
			cfg->append("maxqty", 10);
			cfg->append("minqty", 1);
			cfg->append("udp_port", udp_port);
			ASSERT_TRUE(api_.init(cfg));
			cfg->release();
		}

		void connect_api()
		{
			api_.connect();
			ASSERT_TRUE(wait_until([this]() { return sink_.n_event() >= 1; }));
		}

		void write_positions(uint32_t id, const std::string& body)
		{
			const fs::path dir = "mocker_" + std::to_string(id);
			fs::create_directories(dir);
			std::ofstream out(dir / "positions.json", std::ios::binary | std::ios::trunc);
			out << body;
		}

		WTSEntrust* make_entrust(const char* code, const char* exchg, WTSContractInfo* ct,
			double price, double volume, WTSDirectionType dir, WTSOffsetType offset,
			WTSPriceType price_type = WPT_LIMITPRICE)
		{
			WTSEntrust* e = WTSEntrust::create(code, volume, price, exchg);
			if (ct)
				e->setContractInfo(ct);
			e->setDirection(dir);
			e->setOffsetType(offset);
			e->setPriceType(price_type);
			e->setOrderFlag(WOF_NOR);
			e->setEntrustID("r60-entrust-1");
			return e;
		}

		WTSTraderEvent event() const { return (WTSTraderEvent)sink_.last_event(); }

	protected:
		R60TempCwd cwd_;
		R60MockerSink sink_;
		TraderMocker api_;

		WTSCommodityInfo* ag_comm_ = NULL;
		WTSContractInfo* ag_ct_ = NULL;
		WTSCommodityInfo* stock_comm_ = NULL;
		WTSContractInfo* stock_ct_ = NULL;
		WTSCommodityInfo* rb_comm_ = NULL;
		WTSContractInfo* rb_ct_ = NULL;
		WTSCommodityInfo* if_comm_ = NULL;
		WTSContractInfo* if_ct_ = NULL;
	};

	/// 取一个当前空闲的回环 UDP 端口：先 bind 0 拿系统分配值，再关闭供 connect() 使用。
	uint16_t pick_free_udp_port()
	{
		boost::asio::io_context io;
		boost::asio::ip::udp::socket probe(io);
		probe.open(boost::asio::ip::udp::v4());
		probe.bind(boost::asio::ip::udp::endpoint(boost::asio::ip::address_v4::any(), 0));
		const uint16_t port = probe.local_endpoint().port();
		probe.close();
		return port;
	}

	void send_udp_word(uint16_t port, uint32_t word)
	{
		boost::asio::io_context io;
		boost::asio::ip::udp::socket sender(io);
		sender.open(boost::asio::ip::udp::v4());
		const boost::asio::ip::udp::endpoint ep(boost::asio::ip::address_v4::loopback(), port);
		sender.send_to(boost::asio::buffer(&word, sizeof(word)), ep);
	}

	/* ------------------------------------------------------------------ */
	/* connect + load_positions + queryPositions                          */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L549-590（load_positions 解析/未登记合约跳过）、
	/// L689-703（connect 的 post: load_positions + WTE_Connect）、
	/// L862-909（queryPositions 对多空持仓的转换与回报）。
	TEST_F(R60MockerTest, connect_loads_positions_and_query_returns_long_short)
	{
		init_api(9601);
		write_positions(9601,
			"{\"positions\":["
			"{\"exchg\":\"SHFE\",\"code\":\"ag2512\",\"long\":{\"volume\":5},\"short\":{\"volume\":2}},"
			"{\"exchg\":\"DCE\",\"code\":\"unknown999\",\"long\":{\"volume\":7},\"short\":{\"volume\":0}}"
			"]}");

		connect_api();
		EXPECT_EQ(1u, sink_.n_event());
		EXPECT_EQ((int)WTE_Connect, (int)event());

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 1; }));

		const std::vector<R60MockerSink::PosRec> recs = sink_.last_positions();
		ASSERT_EQ(2u, recs.size());		// 未登记合约被 continue 跳过
		const R60MockerSink::PosRec* long_rec = NULL;
		const R60MockerSink::PosRec* short_rec = NULL;
		for (const auto& r : recs)
		{
			if (r.direction == (int)WDT_LONG)
				long_rec = &r;
			else if (r.direction == (int)WDT_SHORT)
				short_rec = &r;
		}
		ASSERT_NE(nullptr, long_rec);
		ASSERT_NE(nullptr, short_rec);
		EXPECT_EQ("ag2512", long_rec->code);
		EXPECT_DOUBLE_EQ(5.0, long_rec->new_pos);
		EXPECT_DOUBLE_EQ(5.0, long_rec->avail);
		EXPECT_DOUBLE_EQ(2.0, short_rec->new_pos);
		EXPECT_DOUBLE_EQ(2.0, short_rec->avail);
	}

	/* ------------------------------------------------------------------ */
	/* queryAccount / 空持仓 / 空订单 / 空成交                            */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L830-860（queryAccount 建账并回报）、L911-933（queryOrders/queryTrades
	/// 把为 NULL 的 _orders/_trades 原样透传）、L864-906（空持仓）。
	TEST_F(R60MockerTest, empty_queries_dispatch_default_account_and_null_arrays)
	{
		init_api(9602);
		connect_api();

		ASSERT_EQ(0, api_.queryAccount());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_account() >= 1; }));
		const R60MockerSink::AccountRec acc = sink_.last_account();
		EXPECT_EQ("CNY", acc.currency);
		EXPECT_DOUBLE_EQ(0.0, acc.balance);
		EXPECT_DOUBLE_EQ(0.0, acc.available);
		EXPECT_DOUBLE_EQ(0.0, acc.margin);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 1; }));
		EXPECT_TRUE(sink_.last_positions().empty());

		ASSERT_EQ(0, api_.queryOrders());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_orders_rsp() >= 1; }));
		EXPECT_TRUE(sink_.last_orders_array().is_null);

		ASSERT_EQ(0, api_.queryTrades());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_trades_rsp() >= 1; }));
		EXPECT_TRUE(sink_.last_trades_array().is_null);
	}

	/* ------------------------------------------------------------------ */
	/* orderInsert 成功路径 + save_positions                              */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L138-146 + L268-312（开仓校验通过、订单字段映射、
	/// onRspEntrust/onPushOrder、_orders/_awaits 入表、save_positions 落盘）。
	TEST_F(R60MockerTest, insert_open_order_dispatches_and_persists_positions_file)
	{
		init_api(9603);
		connect_api();

		WTSEntrust* e = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2, WDT_LONG, WOT_OPEN);
		ASSERT_NE(nullptr, e);
		const uint32_t before_push = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(e));
		e->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 1; }));

		const R60MockerSink::EntrustRec er = sink_.last_entrust();
		EXPECT_FALSE(er.has_err);
		EXPECT_EQ("ag2512", er.code);
		EXPECT_EQ("SHFE", er.exchg);
		EXPECT_DOUBLE_EQ(7000.0, er.price);
		EXPECT_DOUBLE_EQ(2.0, er.volume);

		const R60MockerSink::OrderRec orr = sink_.last_order();
		ASSERT_TRUE(orr.valid);
		EXPECT_EQ("ag2512", orr.code);
		EXPECT_EQ("SHFE", orr.exchg);
		EXPECT_DOUBLE_EQ(2.0, orr.volume);
		EXPECT_DOUBLE_EQ(2.0, orr.vol_left);
		EXPECT_EQ((int)WOS_NotTraded_Queuing, orr.state);
		// 开仓成功时 state_msg 为空（产品只给平仓成功路径写 "下单成功"），
		// 该语义差异保留在 DISABLED_open_order_success_state_message，
		// 见 docs/迭代计划/迭代01/bugs/R65.md BUG-R65-001（历史注释曾引用 BUG-R57-005）。
		EXPECT_EQ(0u, orr.order_id.find("mo.9603."));

		ASSERT_EQ(0, api_.queryOrders());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_orders_rsp() >= 1; }));
		const R60MockerSink::ArrayRec arr = sink_.last_orders_array();
		EXPECT_FALSE(arr.is_null);
		ASSERT_EQ(1u, arr.size);
		EXPECT_EQ(orr.order_id, arr.first_id);
		EXPECT_EQ((int)WOS_NotTraded_Queuing, arr.first_state);

		// 订单 lambda 末尾的 save_positions 已随 queryOrders 之前的 lambda 完成
		EXPECT_TRUE(fs::exists(fs::path("mocker_9603") / "positions.json"));
		EXPECT_EQ(0u, sink_.n_push_trade());
	}

	/* ------------------------------------------------------------------ */
	/* orderInsert 校验拒绝分支                                            */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L160-266：品种不存在/价格类型/股票整手/股票做空/
	/// 价格非 tick 整数倍/区分平今直接拒绝/无持仓可平 七条拒绝路径。
	TEST_F(R60MockerTest, insert_rejections_map_to_distinct_error_messages)
	{
		init_api(9604);
		connect_api();

		auto expect_error = [this](WTSEntrust* e, const char* needle) -> bool
		{
			const uint32_t before = sink_.n_entrust();
			EXPECT_EQ(0, api_.orderInsert(e));
			e->release();
			if (!wait_until([this, before]() { return sink_.n_entrust() >= before + 1; }))
				return false;
			const R60MockerSink::EntrustRec r = sink_.last_entrust();
			if (!r.has_err)
				return false;
			if (r.err_code != (int)WEC_ORDERINSERT)
				return false;
			return r.err_msg.find(needle) != std::string::npos;
		};

		// 1) 合约信息为空且基础数据里查不到
		EXPECT_TRUE(expect_error(
			make_entrust("xx9999", "DCE", NULL, 100.0, 1, WDT_LONG, WOT_OPEN),
			"品种不存在"));

		// 2) PM_Limit 品种上使用市价（ANYPRICE）
		EXPECT_TRUE(expect_error(
			make_entrust("600000", "SSE", stock_ct_, 10.0, 100, WDT_LONG, WOT_OPEN, WPT_ANYPRICE),
			"价格类型不合法"));

		// 3) 股票买入必须 100 的整数倍
		EXPECT_TRUE(expect_error(
			make_entrust("600000", "SSE", stock_ct_, 10.0, 150, WDT_LONG, WOT_OPEN),
			"股票买入数量必须为100的整数倍"));

		// 4) 只能做多（TM_Long）的股票做空
		EXPECT_TRUE(expect_error(
			make_entrust("600000", "SSE", stock_ct_, 10.0, 100, WDT_SHORT, WOT_OPEN),
			"股票不能做空"));

		// 5) 价格不是 priceTick 的整数倍（tick=0.01，10.005）
		EXPECT_TRUE(expect_error(
			make_entrust("600000", "SSE", stock_ct_, 10.005, 100, WDT_LONG, WOT_OPEN),
			"委托价格不合法"));

		// 6) CM_CoverToday 下平仓/平昨直接拒绝（不查持仓）
		EXPECT_TRUE(expect_error(
			make_entrust("rb2510", "SHFE", rb_ct_, 3500.0, 1, WDT_LONG, WOT_CLOSE),
			"没有足够的可平仓位"));

		// 7) CM_OpenCover 且 _positions 无该合约
		EXPECT_TRUE(expect_error(
			make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 1, WDT_LONG, WOT_CLOSE),
			"没有足够的可平仓位"));

		EXPECT_EQ(7u, sink_.n_entrust());
		EXPECT_EQ(0u, sink_.n_push_order());
	}

	/* ------------------------------------------------------------------ */
	/* 平仓冻结 / 可平不足 / CM_None 直接放行                              */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L233-266（持仓可平量校验与冻结）、L218-223（CM_None 直接通过）、
	/// L862-909（冻结后的 avail 反映到持仓查询）。
	TEST_F(R60MockerTest, close_order_freezes_position_and_cover_none_bypasses_check)
	{
		init_api(9605);
		write_positions(9605,
			"{\"positions\":[{\"exchg\":\"SHFE\",\"code\":\"ag2512\","
			"\"long\":{\"volume\":5},\"short\":{\"volume\":3}}]}");
		connect_api();

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 1; }));

		WTSEntrust* close2 = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2, WDT_LONG, WOT_CLOSE);
		const uint32_t before_push = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(close2));
		close2->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 1; }));

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 2; }));
		{
			const auto recs = sink_.last_positions();
			ASSERT_EQ(2u, recs.size());
			for (const auto& r : recs)
			{
				if (r.direction == (int)WDT_LONG)
				{
					EXPECT_DOUBLE_EQ(5.0, r.new_pos);
					EXPECT_DOUBLE_EQ(3.0, r.avail);	// 冻结 2 手
				}
			}
		}

		// 可平只有 3，再平 4 → 拒绝
		WTSEntrust* close4 = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 4, WDT_LONG, WOT_CLOSE);
		const uint32_t before_entrust = sink_.n_entrust();
		EXPECT_EQ(0, api_.orderInsert(close4));
		close4->release();
		ASSERT_TRUE(wait_until([this, before_entrust]() { return sink_.n_entrust() >= before_entrust + 1; }));
		EXPECT_TRUE(sink_.last_entrust().has_err);
		EXPECT_NE(std::string::npos, sink_.last_entrust().err_msg.find("没有足够的可平仓位"));

		// CM_None：无持仓也能平（L218-223）
		WTSEntrust* if_close = make_entrust("IF2512", "CFFEX", if_ct_, 4000.0, 10, WDT_LONG, WOT_CLOSE);
		const uint32_t before_push2 = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(if_close));
		if_close->release();
		ASSERT_TRUE(wait_until([this, before_push2]() { return sink_.n_push_order() >= before_push2 + 1; }));
		EXPECT_EQ((int)WOS_NotTraded_Queuing, sink_.last_order().state);

		// CM_None 的撤单也走 L788-792 的「不区分开平，直接撤销」
		const std::string if_oid = sink_.last_order().order_id;
		WTSEntrustAction* if_cancel = WTSEntrustAction::create();
		if_cancel->setOrderID(if_oid.c_str());
		if_cancel->setEntrustID("r60-entrust-1");
		EXPECT_EQ(0, api_.orderAction(if_cancel));
		if_cancel->release();
		ASSERT_TRUE(wait_until([this, before_push2]() { return sink_.n_push_order() >= before_push2 + 2; }));
		EXPECT_EQ((int)WOS_Canceled, sink_.last_order().state);
	}

	/* ------------------------------------------------------------------ */
	/* 撤单：开仓直接撤 + 不存在订单报错                                    */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L750-828：开仓撤单（L781-785）、订单不存在（L764-772）、
	/// 回报 WOS_Canceled + 从 _awaits 移除（L809-822）。
	TEST_F(R60MockerTest, cancel_open_order_and_unknown_cancel_reports_error)
	{
		init_api(9606);
		connect_api();

		WTSEntrust* e = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2, WDT_LONG, WOT_OPEN);
		const uint32_t before_push = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(e));
		e->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 1; }));
		const std::string oid = sink_.last_order().order_id;
		ASSERT_FALSE(oid.empty());

		WTSEntrustAction* act = WTSEntrustAction::create();
		act->setOrderID(oid.c_str());
		act->setEntrustID("r60-entrust-1");
		EXPECT_EQ(0, api_.orderAction(act));
		act->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 2; }));
		const R60MockerSink::OrderRec canceled = sink_.last_order();
		EXPECT_EQ((int)WOS_Canceled, canceled.state);
		EXPECT_EQ("撤单成功", canceled.state_msg);
		EXPECT_EQ(oid, canceled.order_id);
		EXPECT_EQ(0u, sink_.n_error());

		WTSEntrustAction* missing = WTSEntrustAction::create();
		missing->setOrderID("mo.9606.99999999");
		missing->setEntrustID("r60-entrust-1");
		EXPECT_EQ(0, api_.orderAction(missing));
		missing->release();
		ASSERT_TRUE(wait_until([this]() { return sink_.n_error() >= 1; }));
		EXPECT_EQ((int)WEC_ORDERCANCEL, sink_.last_error_code());
		EXPECT_NE(std::string::npos, sink_.last_error_msg().find("不可撤销"));
		EXPECT_TRUE(sink_.log_contains("不存在或者已完成"));
	}

	/* ------------------------------------------------------------------ */
	/* 撤单：平仓释放冻结                                                   */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L787-805（非开仓且区分平仓时释放冻结）、L809-824（撤单回报+落盘）；
	/// 与 L862-909 的 avail 联动。
	TEST_F(R60MockerTest, cancel_close_order_releases_frozen_volume)
	{
		init_api(9607);
		write_positions(9607,
			"{\"positions\":[{\"exchg\":\"SHFE\",\"code\":\"ag2512\","
			"\"long\":{\"volume\":5},\"short\":{\"volume\":0}}]}");
		connect_api();

		WTSEntrust* close4 = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 4, WDT_LONG, WOT_CLOSE);
		const uint32_t before_push = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(close4));
		close4->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 1; }));

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 1; }));
		{
			const auto recs = sink_.last_positions();
			ASSERT_EQ(1u, recs.size());
			EXPECT_DOUBLE_EQ(1.0, recs[0].avail);	// 5 - 冻结 4
		}

		const std::string oid = sink_.last_order().order_id;
		WTSEntrustAction* act = WTSEntrustAction::create();
		act->setOrderID(oid.c_str());
		act->setEntrustID("r60-entrust-1");
		EXPECT_EQ(0, api_.orderAction(act));
		act->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 2; }));
		EXPECT_EQ((int)WOS_Canceled, sink_.last_order().state);

		ASSERT_EQ(0, api_.queryPositions());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_position() >= 2; }));
		{
			const auto recs = sink_.last_positions();
			ASSERT_EQ(1u, recs.size());
			EXPECT_DOUBLE_EQ(5.0, recs[0].avail);	// 冻结已释放
		}
	}

	/* ------------------------------------------------------------------ */
	/* UDP 接收链：未知包头被忽略且 io 线程仍存活                            */
	/* ------------------------------------------------------------------ */

	/// TraderMocker.cpp L666-687（reconn_udp 的 open/bind/async_receive_from）、
	/// L935-962（handle_read 成功路径与重新 arm）、L985-996（extract_buffer 的
	/// _type != UDP_MSG_PUSHTICK 分支）。只发 4 字节未知头，不镜像 WTSTickStruct 布局。
	TEST_F(R60MockerTest, udp_unknown_header_is_ignored_and_io_loop_survives)
	{
		const uint16_t port = pick_free_udp_port();
		init_api(9608, (int)port);
		connect_api();

		send_udp_word(port, 0x12345678u);
		// 给内核/UDP handler 一个短窗口；随后用 queryAccount 证明 io_context 没被异常终止
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		ASSERT_EQ(0, api_.queryAccount());
		ASSERT_TRUE(wait_until([this]() { return sink_.n_account() >= 1; }));
		EXPECT_EQ(1u, sink_.n_account());
		EXPECT_EQ(0u, sink_.n_push_trade());
	}

	/// 产品缺陷占位：TraderMocker.cpp 的成功状态文案只在「平仓冻结」分支赋值
	/// （L263-264 msg = "下单成功"），开仓分支 L211-216 直接 bPass=true; break，
	/// msg 保持空串，于是 setStateMsg("")。HftMocker 对开/平仓都写
	/// "下单成功"（HftMocker.cpp:630/833），两套 mocker 语义不一致。
	/// 见 docs/迭代计划/迭代01/bugs/R65.md BUG-R65-001（历史注释曾引用 BUG-R57-005）；默认不执行。
	TEST_F(R60MockerTest, DISABLED_open_order_success_state_message)
	{
		init_api(9613);
		connect_api();

		WTSEntrust* e = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2, WDT_LONG, WOT_OPEN);
		ASSERT_NE(nullptr, e);
		const uint32_t before_push = sink_.n_push_order();
		EXPECT_EQ(0, api_.orderInsert(e));
		e->release();
		ASSERT_TRUE(wait_until([this, before_push]() { return sink_.n_push_order() >= before_push + 1; }));

		const R60MockerSink::OrderRec orr = sink_.last_order();
		ASSERT_TRUE(orr.valid);
		EXPECT_EQ("下单成功", orr.state_msg);
	}

} // namespace
