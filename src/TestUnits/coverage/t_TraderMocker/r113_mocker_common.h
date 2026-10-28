/**
 * r113_mocker_common.h  (lane R113 · wave19)
 *
 * TraderMocker 公开面夹具：临时 cwd、最小 IBaseDataMgr、ITraderSpi 快照
 * （订单推送/成交推送/查询快照）、本机回环 UDP tick 发包。
 *
 * 只走 ITraderApi 公开接口：init/connect/login/orderInsert/orderAction/query*。
 * 不镜像 TraderMocker 私有布局，不直接调用 match_once；撮合路径由公开的
 * login() 线程 + 本机 UDP tick 驱动，用例结束 release() 会 join 该线程。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "r103_mocker_common.h"

namespace r113m
{
	using r103m::wait_until;

	struct OrderSnap
	{
		std::string	id;
		int			state { -1 };
		std::string	state_msg;
		double		vol_left { 0.0 };
		double		vol_traded { 0.0 };
		double		volume { 0.0 };
	};

	struct TradeSnap
	{
		std::string	code;
		int			direction { -1 };
		int			offset { -1 };
		double		volume { 0.0 };
		double		price { 0.0 };
		std::string	ref_order;
	};

	class Sink : public ITraderSpi
	{
	public:
		IBaseDataMgr* getBaseDataMgr() override { return &bd_; }

		void handleEvent(WTSTraderEvent e, int32_t ec) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			event_ = e;
			ec_ = ec;
			_events.fetch_add(1);
		}

		void onLoginResult(bool succ, const char*, uint32_t) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			login_succ_ = succ;
			_login.fetch_add(1);
		}

		void handleTraderLog(WTSLogLevel, const char* message) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			_logs.push_back(message ? message : "");
			_log.fetch_add(1);
		}

		void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			entrust_has_err = (err != NULL);
			if (err != NULL)
			{
				err_code = (int)err->getErrorCode();
				err_msg = err->getMessage() ? err->getMessage() : "";
			}
			if (entrust != NULL)
				entrust_code = entrust->getCode() ? entrust->getCode() : "";
			_entrusts.fetch_add(1);
		}

		void onPushOrder(WTSOrderInfo* info) override
		{
			OrderSnap rec;
			if (info != NULL)
			{
				rec.id = info->getOrderID() ? info->getOrderID() : "";
				rec.state = (int)info->getOrderState();
				rec.state_msg = info->getStateMsg() ? info->getStateMsg() : "";
				rec.vol_left = info->getVolLeft();
				rec.vol_traded = info->getVolTraded();
				rec.volume = info->getVolume();
			}
			std::lock_guard<std::mutex> lk(_mtx);
			order_pushes.push_back(rec);
			_push_order.fetch_add(1);
		}

		void onPushTrade(WTSTradeInfo* info) override
		{
			TradeSnap rec;
			if (info != NULL)
			{
				rec.code = info->getCode() ? info->getCode() : "";
				rec.direction = (int)info->getDirection();
				rec.offset = (int)info->getOffsetType();
				rec.volume = info->getVolume();
				rec.price = info->getPrice();
				rec.ref_order = info->getRefOrder() ? info->getRefOrder() : "";
			}
			std::lock_guard<std::mutex> lk(_mtx);
			trade_pushes.push_back(rec);
			_trade.fetch_add(1);
		}

		void onRspPosition(const WTSArray* ay) override
		{
			std::vector<r103m::PosRec> recs;
			if (ay != NULL)
			{
				for (uint32_t i = 0; i < ay->size(); i++)
				{
					WTSPositionItem* item = (WTSPositionItem*)const_cast<WTSArray*>(ay)->at(i);
					if (item == NULL)
						continue;
					r103m::PosRec rec;
					rec.code = item->getCode() ? item->getCode() : "";
					rec.direction = (int)item->getDirection();
					rec.new_pos = item->getNewPosition();
					rec.avail = item->getAvailNewPos();
					recs.push_back(rec);
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			positions = recs;
			_position.fetch_add(1);
		}

		void onRspAccount(WTSArray*) override { _account.fetch_add(1); }

		void onRspOrders(const WTSArray* ay) override
		{
			std::vector<OrderSnap> recs;
			if (ay != NULL)
			{
				for (uint32_t i = 0; i < ay->size(); i++)
				{
					WTSOrderInfo* info = (WTSOrderInfo*)const_cast<WTSArray*>(ay)->at(i);
					if (info == NULL)
						continue;
					OrderSnap rec;
					rec.id = info->getOrderID() ? info->getOrderID() : "";
					rec.state = (int)info->getOrderState();
					rec.state_msg = info->getStateMsg() ? info->getStateMsg() : "";
					rec.vol_left = info->getVolLeft();
					rec.vol_traded = info->getVolTraded();
					rec.volume = info->getVolume();
					recs.push_back(rec);
				}
			}
			std::lock_guard<std::mutex> lk(_mtx);
			_queried_orders = recs;
			_orders_query.fetch_add(1);
		}

		void onRspTrades(const WTSArray* ay) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			_queried_trades = ay == NULL ? 0 : (uint32_t)ay->size();
			_trades_query.fetch_add(1);
		}

		void onTraderError(WTSError* err, void* = NULL) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			if (err != NULL)
			{
				err_code = (int)err->getErrorCode();
				err_msg = err->getMessage() ? err->getMessage() : "";
			}
			_error.fetch_add(1);
		}

		uint32_t n_event() const { return _events.load(); }
		uint32_t n_login() const { return _login.load(); }
		uint32_t n_entrust() const { return _entrusts.load(); }
		uint32_t n_push_order() const { return _push_order.load(); }
		uint32_t n_trade() const { return _trade.load(); }
		uint32_t n_position() const { return _position.load(); }
		uint32_t n_account() const { return _account.load(); }
		uint32_t n_orders_query() const { return _orders_query.load(); }
		uint32_t n_trades_query() const { return _trades_query.load(); }
		uint32_t n_error() const { return _error.load(); }
		uint32_t queried_trades() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _queried_trades;
		}

		bool login_succ() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return login_succ_;
		}

		std::vector<OrderSnap> orders() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return order_pushes;
		}

		std::vector<OrderSnap> queried_orders() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return _queried_orders;
		}

		std::vector<TradeSnap> trades() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return trade_pushes;
		}

		std::vector<r103m::PosRec> last_positions() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return positions;
		}

		double total_trade_volume() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			double total = 0.0;
			for (const TradeSnap& t : trade_pushes)
				total += t.volume;
			return total;
		}

		bool has_state(int state) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			for (const OrderSnap& o : order_pushes)
			{
				if (o.state == state)
					return true;
			}
			return false;
		}

		bool has_canceled_message(const std::string& msg) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			for (const OrderSnap& o : order_pushes)
			{
				if (o.state == (int)WOS_Canceled && o.state_msg == msg)
					return true;
			}
			return false;
		}

		bool has_part_traded_left(double left) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			for (const OrderSnap& o : order_pushes)
			{
				if (o.state == (int)WOS_PartTraded_Queuing && o.vol_left == left)
					return true;
			}
			return false;
		}

		std::string last_order_id() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return order_pushes.empty() ? std::string() : order_pushes.back().id;
		}

		bool log_contains(const std::string& needle) const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			for (const std::string& line : _logs)
			{
				if (line.find(needle) != std::string::npos)
					return true;
			}
			return false;
		}

		r103m::BdMgr bd_;

	private:
		mutable std::mutex _mtx;
		std::atomic<uint32_t> _events{ 0 };
		std::atomic<uint32_t> _login{ 0 };
		std::atomic<uint32_t> _entrusts{ 0 };
		std::atomic<uint32_t> _push_order{ 0 };
		std::atomic<uint32_t> _trade{ 0 };
		std::atomic<uint32_t> _position{ 0 };
		std::atomic<uint32_t> _account{ 0 };
		std::atomic<uint32_t> _orders_query{ 0 };
		std::atomic<uint32_t> _trades_query{ 0 };
		std::atomic<uint32_t> _error{ 0 };
		std::atomic<uint32_t> _log{ 0 };

		WTSTraderEvent event_ = WTE_Connect;
		int32_t ec_ = 0;
		bool login_succ_ = false;
		bool entrust_has_err { false };
		int err_code { 0 };
		std::string err_msg;
		std::string entrust_code;
		std::vector<std::string> _logs;
		std::vector<OrderSnap> order_pushes;
		std::vector<TradeSnap> trade_pushes;
		std::vector<OrderSnap> _queried_orders;
		std::vector<r103m::PosRec> positions;
		uint32_t _queried_trades { 0 };
	};

	class MockerTest : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			ag_comm_ = WTSCommodityInfo::create("ag", "沪银", "SHFE", "TRADING", "CHINA");
			ag_comm_->setCategory(CC_Future);
			ag_comm_->setCoverMode(CM_OpenCover);
			ag_comm_->setPriceMode(PM_Both);
			ag_comm_->setTradingMode(TM_Both);
			ag_comm_->setPriceTick(1.0);
			ag_comm_->setVolScale(15);
			ag_ct_ = WTSContractInfo::create("ag2512", "沪银2512", "SHFE", "ag");
			ag_ct_->setTotalIndex(1);
			sink_.bd_.regis(ag_comm_, ag_ct_);
			api_.registerSpi(&sink_);
		}

		void TearDown() override
		{
			api_.release();
		}

		void init_api(uint32_t id, int udpPort = 0)
		{
			WTSVariant* cfg = r103m::make_cfg(id, udpPort);
			ASSERT_TRUE(api_.init(cfg));
			cfg->release();
		}

		void connect_api()
		{
			api_.connect();
			ASSERT_TRUE(wait_until([this]() { return sink_.n_event() >= 1u; }));
		}

		void login_api()
		{
			ASSERT_EQ(0, api_.login("", "", ""));
			ASSERT_TRUE(wait_until([this]() { return sink_.n_login() >= 1u; }));
			ASSERT_TRUE(sink_.login_succ());
		}

		void insert_order(double volume, double price, WTSDirectionType dir, WTSOffsetType offset)
		{
			const uint32_t before = sink_.n_push_order();
			WTSEntrust* entrust = r103m::make_entrust("ag2512", "SHFE", ag_ct_, price, volume, dir, offset);
			ASSERT_EQ(0, api_.orderInsert(entrust));
			entrust->release();
			ASSERT_TRUE(wait_until([this, before]() { return sink_.n_push_order() >= before + 1u; }))
				<< "orderInsert 未推送订单回报";

			const std::string new_id = sink_.last_order_id();
			ASSERT_FALSE(new_id.empty());

			/* queryOrders 与 orderInsert 在同一 io_context 串行：只有 query 真的看到
			 * 这张新委托，才能证明 orderInsert lambda 里 _codes/_awaits 已写好；
			 * 之后发 tick 不会丢在 _codes 之前，也不会在 _awaits 里找不到它。 */
			for (int attempt = 0; attempt < 50; attempt++)
			{
				const uint32_t q = sink_.n_orders_query();
				ASSERT_EQ(0, api_.queryOrders());
				ASSERT_TRUE(wait_until([this, q]() { return sink_.n_orders_query() >= q + 1u; }));
				for (const OrderSnap& o : sink_.queried_orders())
				{
					if (o.id == new_id)
						return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			FAIL() << "queryOrders 始终看不到刚插入的委托 " << new_id;
		}

		void send_tick(double price, double askPrice, double askQty, double bidPrice, double bidQty,
			const char* code = "ag2512", const char* exchg = "SHFE")
		{
#pragma pack(push, 1)
			struct TickPacket
			{
				uint32_t		type;
				WTSTickStruct	data;
			};
#pragma pack(pop)

			TickPacket packet;
			memset(&packet, 0, sizeof(packet));
			packet.type = 0x200u;
			strncpy(packet.data.exchg, exchg, sizeof(packet.data.exchg) - 1);
			strncpy(packet.data.code, code, sizeof(packet.data.code) - 1);
			packet.data.price = price;
			packet.data.action_date = 20200102u;
			packet.data.trading_date = 20200102u;
			packet.data.action_time = 100000000u;
			packet.data.ask_prices[0] = askPrice;
			packet.data.ask_qty[0] = askQty;
			packet.data.bid_prices[0] = bidPrice;
			packet.data.bid_qty[0] = bidQty;

			ASSERT_TRUE(r103m::send_udp((uint16_t)udp_port_, &packet, sizeof(packet)));
		}

		void set_udp_port(int port) { udp_port_ = port; }

	protected:
		r103m::TempCwd cwd_;
		Sink sink_;
		TraderMocker api_;

		WTSCommodityInfo* ag_comm_ { NULL };
		WTSContractInfo* ag_ct_ { NULL };
		int udp_port_ { 0 };
	};

}	// namespace r113m
