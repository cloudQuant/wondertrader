/**
 * r103_mocker_common.h  (lane R103 · wave18)
 *
 * TraderMocker 公开面夹具：临时 cwd、最小 IBaseDataMgr、ITraderSpi 快照、
 * UDP 临时端口/发包、init 配置与 WTSEntrust 构造。
 * 不触 match_once，不镜像 TraderMocker 私有布局；UDP tick 包只用公开的 WTSTickStruct。
 */
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "TraderMocker/TraderMocker.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

USING_NS_WTP;

namespace r103m
{
	namespace fs = std::filesystem;

	template <typename Pred>
	inline bool wait_until(Pred pred, int timeout_ms = 3000)
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

	class TempCwd
	{
	public:
		TempCwd()
		{
			static std::atomic<uint32_t> seq{ 0 };
			_old = fs::current_path();
			_dir = fs::temp_directory_path() /
				("wondertrader_r103_mocker_" + std::to_string((long)::getpid()) + "_" + std::to_string(seq.fetch_add(1)));
			fs::create_directories(_dir);
			fs::current_path(_dir);
		}

		~TempCwd()
		{
			std::error_code ec;
			fs::current_path(_old, ec);
			fs::remove_all(_dir, ec);
		}

		TempCwd(const TempCwd&) = delete;
		TempCwd& operator=(const TempCwd&) = delete;

	private:
		fs::path _old;
		fs::path _dir;
	};

	class BdMgr : public IBaseDataMgr
	{
	public:
		virtual ~BdMgr()
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
				if (exchgpid != NULL && strcmp(comm->getFullPid(), exchgpid) == 0)
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

	struct PosRec
	{
		std::string code;
		int direction { -1 };
		double new_pos { 0.0 };
		double avail { 0.0 };
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
			{
				entrust_code = entrust->getCode() ? entrust->getCode() : "";
				entrust_volume = entrust->getVolume();
			}
			_entrusts.fetch_add(1);
		}

		void onPushOrder(WTSOrderInfo* info) override
		{
			std::lock_guard<std::mutex> lk(_mtx);
			if (info != NULL)
			{
				order_id = info->getOrderID() ? info->getOrderID() : "";
				state = (int)info->getOrderState();
				state_msg = info->getStateMsg() ? info->getStateMsg() : "";
				vol_left = info->getVolLeft();
			}
			_push_order.fetch_add(1);
		}

		void onRspPosition(const WTSArray* ay) override
		{
			std::vector<PosRec> recs;
			if (ay != NULL)
			{
				for (uint32_t i = 0; i < ay->size(); i++)
				{
					WTSPositionItem* item = (WTSPositionItem*)const_cast<WTSArray*>(ay)->at(i);
					if (item == NULL)
						continue;
					PosRec rec;
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
		void onRspOrders(const WTSArray*) override { _orders.fetch_add(1); }
		void onRspTrades(const WTSArray*) override { _trades.fetch_add(1); }

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
		uint32_t n_entrust() const { return _entrusts.load(); }
		uint32_t n_push_order() const { return _push_order.load(); }
		uint32_t n_position() const { return _position.load(); }
		uint32_t n_account() const { return _account.load(); }
		uint32_t n_error() const { return _error.load(); }
		uint32_t n_log() const { return _log.load(); }

		std::string last_order_id() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return order_id;
		}

		std::string last_err_msg() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return err_msg;
		}

		std::vector<PosRec> last_positions() const
		{
			std::lock_guard<std::mutex> lk(_mtx);
			return positions;
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

		BdMgr bd_;

		// 简单快照（读侧加锁）
		bool entrust_has_err { false };
		int err_code { 0 };
		std::string err_msg;
		std::string entrust_code;
		double entrust_volume { 0.0 };
		std::string order_id;
		int state { -1 };
		std::string state_msg;
		double vol_left { 0.0 };

	private:
		mutable std::mutex _mtx;
		std::atomic<uint32_t> _events{ 0 };
		std::atomic<uint32_t> _login{ 0 };
		std::atomic<uint32_t> _entrusts{ 0 };
		std::atomic<uint32_t> _push_order{ 0 };
		std::atomic<uint32_t> _position{ 0 };
		std::atomic<uint32_t> _account{ 0 };
		std::atomic<uint32_t> _orders{ 0 };
		std::atomic<uint32_t> _trades{ 0 };
		std::atomic<uint32_t> _error{ 0 };
		std::atomic<uint32_t> _log{ 0 };

		WTSTraderEvent event_ = WTE_Connect;
		int32_t ec_ = 0;
		bool login_succ_ = false;
		std::vector<std::string> _logs;
		std::vector<PosRec> positions;
	};

	inline WTSVariant* make_cfg(uint32_t id, int udpPort = 0, double maxQty = 10.0, double minQty = 1.0)
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("span", 5);
		cfg->append("newpx", true);
		cfg->append("mockerid", id);
		cfg->append("maxqty", maxQty);
		cfg->append("minqty", minQty);
		cfg->append("udp_port", udpPort);
		return cfg;
	}

	inline WTSEntrust* make_entrust(const char* code, const char* exchg, WTSContractInfo* ct,
		double price, double volume, WTSDirectionType dir, WTSOffsetType offset)
	{
		WTSEntrust* entrust = WTSEntrust::create(code, volume, price, exchg);
		if (ct != NULL)
			entrust->setContractInfo(ct);
		entrust->setDirection(dir);
		entrust->setOffsetType(offset);
		entrust->setPriceType(WPT_LIMITPRICE);
		entrust->setOrderFlag(WOF_NOR);
		entrust->setEntrustID("r103-entrust");
		return entrust;
	}

	inline uint16_t pick_free_udp_port()
	{
		int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return 0;
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = 0;
		uint16_t port = 0;
		if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0)
		{
			socklen_t len = sizeof(addr);
			if (::getsockname(fd, (sockaddr*)&addr, &len) == 0)
				port = ntohs(addr.sin_port);
		}
		::close(fd);
		return port;
	}

	inline bool send_udp(uint16_t port, const void* data, std::size_t len)
	{
		int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0)
			return false;
		sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		addr.sin_port = htons(port);
		const ssize_t n = ::sendto(fd, data, len, 0, (sockaddr*)&addr, sizeof(addr));
		::close(fd);
		return n == (ssize_t)len;
	}

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
			WTSVariant* cfg = make_cfg(id, udpPort);
			ASSERT_TRUE(api_.init(cfg));
			cfg->release();
		}

		void connect_api()
		{
			api_.connect();
			ASSERT_TRUE(wait_until([this]() { return sink_.n_event() >= 1u; }));
		}

		void write_positions(uint32_t id, const std::string& body)
		{
			const fs::path dir = "mocker_" + std::to_string(id);
			fs::create_directories(dir);
			std::ofstream out(dir / "positions.json", std::ios::binary | std::ios::trunc);
			out << body;
		}

	protected:
		TempCwd cwd_;
		Sink sink_;
		TraderMocker api_;

		WTSCommodityInfo* ag_comm_ { NULL };
		WTSContractInfo* ag_ct_ { NULL };
	};

}	// namespace r103m
