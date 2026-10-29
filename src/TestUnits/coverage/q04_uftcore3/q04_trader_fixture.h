/*!
 * \file q04_trader_fixture.h
 * \brief 覆盖率迭代01 第五波 q04(WtUftCore 交易链路) 共用替身
 *
 * 只提供纯内存替身: 假的 ITraderApi / IBaseDataMgr / ITrdNotifySink + 临时目录根。
 * 不接真实 UFT 柜台, 不装载任何动态库, 不起线程, 不开共享内存与 socket。
 *
 * 说明(硬约束规避):
 * - 白盒访问不使用 `#define private public`, 一律通过公开接口 + 替身回读。
 * - TraderAdapter 的头必须用 `WtUftCore/TraderAdapter.h`, 不是 WtCore 的同名头。
 * - 生产头未自行 include boost/noncopyable(BUG-L00-004), 这里显式给出。
 */
#pragma once

#include <boost/noncopyable.hpp>

#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/ActionPolicyMgr.h"
#include "WtUftCore/ITrdNotifySink.h"
#include "WtUftCore/WtHelper.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/ITraderApi.h"
#include "Includes/WTSCollection.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace q04 {

using namespace wtp;

//////////////////////////////////////////////////////////////////////////
// 临时目录根: temp_directory_path()/q04_<tag>_<pid>_<seq>, 析构清理
class TempRoot
{
public:
	explicit TempRoot(const char* tag)
	{
		static std::atomic<uint32_t> seq(0);
		std::filesystem::path base = std::filesystem::temp_directory_path();
		_dir = base / ("q04_" + std::string(tag) + "_" + std::to_string((long)::getpid())
			+ "_" + std::to_string(++seq));
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
		std::filesystem::create_directories(_dir, ec);
	}

	~TempRoot()
	{
		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	TempRoot(const TempRoot&) = delete;
	TempRoot& operator=(const TempRoot&) = delete;

	std::string file(const char* name) const { return (_dir / name).string(); }

	void write(const std::string& path, const std::string& content) const
	{
		std::ofstream ofs(path.c_str(), std::ios::out | std::ios::trunc);
		ofs << content;
		ofs.close();
	}

	const std::filesystem::path& dir() const { return _dir; }

private:
	std::filesystem::path _dir;
};

//////////////////////////////////////////////////////////////////////////
// 假的交易 API: 只记录调用, 不做任何 IO
class FakeTraderApi : public ITraderApi
{
public:
	struct EntrustSnap
	{
		std::string _code;
		std::string _exchg;
		std::string _usertag;
		std::string _entrustid;
		double		_vol = 0;
		double		_price = 0;
		int			_dir = 0;
		int			_offset = 0;
		int			_oflag = 0;
		int			_ptype = 0;
	};

	int	_insertRet = 0;
	int	_actionRet = 0;
	bool	_initRet = true;

	uint32_t _initCnt = 0;
	uint32_t _regSpiCnt = 0;
	uint32_t _connectCnt = 0;
	uint32_t _disconnectCnt = 0;
	uint32_t _loginCnt = 0;
	uint32_t _logoutCnt = 0;
	uint32_t _releaseCnt = 0;
	uint32_t _qPosCnt = 0;
	uint32_t _qOrdCnt = 0;
	uint32_t _qTrdCnt = 0;
	uint32_t _qAccCnt = 0;
	uint32_t _mkEidCnt = 0;

	std::string _lastUser;
	std::string _lastPass;
	std::string _lastProduct;
	ITraderSpi* _spi = nullptr;
	std::vector<EntrustSnap> _entrusts;
	std::vector<EntrustSnap> _actions;	// code 位存 orderid, exchg 位存 entrustid

	bool init(WTSVariant* /*params*/) override { _initCnt++; return _initRet; }

	void registerSpi(ITraderSpi* listener) override { _spi = listener; _regSpiCnt++; }

	void connect() override { _connectCnt++; }

	void disconnect() override { _disconnectCnt++; }

	bool isConnected() override { return true; }

	bool makeEntrustID(char* buffer, int /*length*/) override
	{
		_mkEidCnt++;
		snprintf(buffer, 64, "EU%06lu", (unsigned long)(_mkEidCnt));
		return true;
	}

	int login(const char* user, const char* pass, const char* productInfo) override
	{
		_loginCnt++;
		_lastUser = (user == nullptr) ? "" : user;
		_lastPass = (pass == nullptr) ? "" : pass;
		_lastProduct = (productInfo == nullptr) ? "" : productInfo;
		return 0;
	}

	int logout() override { _logoutCnt++; return 0; }

	int orderInsert(WTSEntrust* entrust) override
	{
		EntrustSnap snap;
		snap._code = entrust->getCode();
		snap._exchg = entrust->getExchg();
		snap._usertag = entrust->getUserTag();
		snap._entrustid = entrust->getEntrustID();
		snap._vol = entrust->getVolume();
		snap._price = entrust->getPrice();
		snap._dir = (int)entrust->getDirection();
		snap._offset = (int)entrust->getOffsetType();
		snap._oflag = (int)entrust->getOrderFlag();
		snap._ptype = (int)entrust->getPriceType();
		_entrusts.emplace_back(snap);
		return _insertRet;
	}

	int orderAction(WTSEntrustAction* action) override
	{
		EntrustSnap snap;
		snap._code = action->getOrderID();
		snap._exchg = action->getEntrustID();
		snap._usertag = action->getUserTag();
		_actions.emplace_back(snap);
		return _actionRet;
	}

	int queryAccount() override { _qAccCnt++; return 0; }
	int queryPositions() override { _qPosCnt++; return 0; }
	int queryOrders() override { _qOrdCnt++; return 0; }
	int queryTrades() override { _qTrdCnt++; return 0; }

	void release() override { _releaseCnt++; _spi = nullptr; }
};

//////////////////////////////////////////////////////////////////////////
// 假的基础数据管理器: 只维护自己 new 出来的合约/品种对象
class FakeBaseDataMgr : public IBaseDataMgr
{
public:
	FakeBaseDataMgr() = default;

	~FakeBaseDataMgr()
	{
		for (auto& v : _contracts)		v.second->release();
		for (auto& v : _commodities)	v.second->release();
	}

	FakeBaseDataMgr(const FakeBaseDataMgr&) = delete;
	FakeBaseDataMgr& operator=(const FakeBaseDataMgr&) = delete;

	//注册一个品种+合约, 返回合约对象便于后续改写
	WTSContractInfo* add(const char* exchg, const char* pid, const char* code,
		uint32_t maxMktVol = 0, uint32_t maxLmtVol = 0,
		CoverMode cm = CM_OpenCover, TradingMode tm = TM_Both, uint32_t volScale = 1)
	{
		std::string fullPid = std::string(exchg) + "." + pid;
		WTSCommodityInfo* comm = nullptr;
		auto cit = _commodities.find(fullPid);
		if (cit == _commodities.end())
		{
			comm = WTSCommodityInfo::create(pid, pid, exchg, "NONE", "NONE");
			comm->setVolScale(volScale);
			comm->setCoverMode(cm);
			comm->setTradingMode(tm);
			_commodities[fullPid] = comm;
		}
		else
		{
			comm = cit->second;
		}

		std::string key = std::string(exchg) + "." + code;
		WTSContractInfo* cInfo = WTSContractInfo::create(code, code, exchg, pid);
		cInfo->setAltCode(code);
		cInfo->setCommInfo(comm);
		cInfo->setVolumeLimits(maxMktVol, maxLmtVol);
		_contracts[key] = cInfo;
		return cInfo;
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		auto it = _commodities.find(exchgpid);
		return it == _commodities.end() ? nullptr : it->second;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		return getCommodity((std::string(exchg) + "." + pid).c_str());
	}

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t /*uDate*/ = 0) override
	{
		auto it = _contracts.find(std::string(exchg) + "." + code);
		return it == _contracts.end() ? nullptr : it->second;
	}

	WTSArray* getContracts(const char* /*exchg*/ = "", uint32_t /*uDate*/ = 0) override
	{
		WTSArray* ret = WTSArray::create();
		for (auto& v : _contracts)
			ret->append(v.second, true);
		return ret;
	}

	WTSSessionInfo* getSession(const char* /*sid*/) override { return nullptr; }
	WTSSessionInfo* getSessionByCode(const char* /*code*/, const char* /*exchg*/ = "") override { return nullptr; }
	WTSArray* getAllSessions() override { return nullptr; }

	bool isHoliday(const char* /*pid*/, uint32_t /*uDate*/, bool /*isTpl*/ = false) override { return false; }

	uint32_t calcTradingDate(const char* /*stdPID*/, uint32_t uDate, uint32_t /*uTime*/, bool /*isSession*/ = false) override
	{
		return uDate;
	}

	uint64_t getBoundaryTime(const char* /*stdPID*/, uint32_t /*tDate*/, bool /*isSession*/ = false, bool /*isStart*/ = true) override
	{
		return 0;
	}

private:
	wt_hashmap<std::string, WTSContractInfo*>	_contracts;
	wt_hashmap<std::string, WTSCommodityInfo*>	_commodities;
};

//////////////////////////////////////////////////////////////////////////
// 回报监听替身
class FakeSink : public ITrdNotifySink
{
public:
	struct TradeRec
	{
		uint32_t	_localid;
		std::string	_code;
		bool		_long;
		uint32_t	_offset;
		double		_vol;
		double		_price;
	};

	struct OrderRec
	{
		uint32_t	_localid;
		std::string	_code;
		bool		_long;
		uint32_t	_offset;
		double		_total;
		double		_left;
		double		_price;
		bool		_canceled;
	};

	struct PosRec
	{
		std::string	_code;
		bool		_long;
		double		_prevol;
		double		_preavail;
		double		_newvol;
		double		_newavail;
		uint32_t	_tdate;
	};

	struct EntrustRec
	{
		uint32_t	_localid;
		std::string	_code;
		bool		_succ;
		std::string	_msg;
	};

	std::vector<TradeRec>		_trades;
	std::vector<OrderRec>		_orders;
	std::vector<PosRec>			_positions;
	std::vector<EntrustRec>		_entrusts;
	uint32_t					_readyCnt = 0;
	uint32_t					_lostCnt = 0;
	uint32_t					_readyTdate = 0;

	void on_trade(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset,
		double vol, double price) override
	{
		_trades.push_back({ localid, stdCode, isLong, offset, vol, price });
	}

	void on_order(uint32_t localid, const char* stdCode, bool isLong, uint32_t offset,
		double totalQty, double leftQty, double price, bool isCanceled = false) override
	{
		_orders.push_back({ localid, stdCode, isLong, offset, totalQty, leftQty, price, isCanceled });
	}

	void on_position(const char* stdCode, bool isLong, double prevol, double preavail,
		double newvol, double newavail, uint32_t tradingday) override
	{
		_positions.push_back({ stdCode, isLong, prevol, preavail, newvol, newavail, tradingday });
	}

	void on_entrust(uint32_t localid, const char* stdCode, bool bSuccess, const char* message) override
	{
		_entrusts.push_back({ localid, stdCode, bSuccess, message == nullptr ? "" : message });
	}

	void on_channel_ready(uint32_t tradingday) override { _readyCnt++; _readyTdate = tradingday; }

	void on_channel_lost() override { _lostCnt++; }
};

//////////////////////////////////////////////////////////////////////////
// 动作策略文件内容: 两个规则组 + default 回退
inline const char* policyJsonOpenCloseLimited()
{
	return R"JSON({
	"default": {
		"order": [
			{"action":"open", "limit":0, "limit_s":0, "limit_l":0, "pure":false}
		],
		"filters": []
	},
	"limited": {
		"order": [
			{"action":"open",         "limit":0,   "limit_s":0, "limit_l":10, "pure":false},
			{"action":"closeyestoday","limit":0,   "limit_s":0, "limit_l":0,  "pure":true},
			{"action":"closetoday",   "limit":0,   "limit_s":0, "limit_l":0,  "pure":true},
			{"action":"close",        "limit":0,   "limit_s":0, "limit_l":0,  "pure":false}
		],
		"filters": ["SHFE.cu"]
	},
	"openonly": {
		"order": [
			{"action":"open", "limit":0, "limit_s":0, "limit_l":0, "pure":false}
		],
		"filters": ["SHFE.ni"]
	},
	"noclose": {
		"order": [
			{"action":"close", "limit":0, "limit_s":0, "limit_l":0, "pure":false}
		],
		"filters": ["SHFE.zn"]
	},
	"cutoff": {
		"order": [
			{"action":"open", "limit":12, "limit_s":0, "limit_l":0, "pure":false}
		],
		"filters": ["SHFE.au"]
	}
})JSON";
}

// 把 FakeTraderApi 抓到的委托还原成一个 WTSOrderInfo(便于回放回报)
inline WTSOrderInfo* orderFromSnap(const FakeTraderApi::EntrustSnap& snap, const char* orderid)
{
	WTSEntrust* e = WTSEntrust::create(snap._code.c_str(), snap._vol, snap._price, snap._exchg.c_str());
	e->setUserTag(snap._usertag.c_str());
	e->setEntrustID(snap._entrustid.c_str());
	e->setDirection((WTSDirectionType)snap._dir);
	e->setOffsetType((WTSOffsetType)snap._offset);
	e->setOrderFlag((WTSOrderFlag)snap._oflag);
	e->setPriceType((WTSPriceType)snap._ptype);

	WTSOrderInfo* ret = WTSOrderInfo::create(e);
	ret->setOrderID(orderid);
	ret->setVolLeft(snap._vol);
	ret->setVolTraded(0);
	ret->setOrderState(WOS_NotTraded_Queuing);
	e->release();
	return ret;
}

// 便捷: 用适配器实际使用的标准代码构造持仓条目
inline WTSPositionItem* posItem(const char* exchg, const char* code, WTSDirectionType dir,
	double preVol, double preAvail, double newVol, double newAvail)
{
	WTSPositionItem* ret = WTSPositionItem::create(code, "CNY", exchg);
	ret->setDirection(dir);
	ret->setPrePosition(preVol);
	ret->setAvailPrePos(preAvail);
	ret->setNewPosition(newVol);
	ret->setAvailNewPos(newAvail);
	return ret;
}

}	// namespace q04
