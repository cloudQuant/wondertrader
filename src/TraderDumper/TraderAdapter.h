/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#pragma once
#include <atomic>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <boost/noncopyable.hpp>

#include "../Includes/ExecuteDefs.h"

#include "../Includes/ITraderApi.h"

NS_WTP_BEGIN
class WTSVariant;
class ActionPolicyMgr;
class WTSContractInfo;
class WTSCommodityInfo;
class WtExecuter;
class EventNotifier;

class ITrdNotifySink;

class TraderAdapterMgr;

class TraderAdapter : public ITraderSpi
{
public:
	TraderAdapter(TraderAdapterMgr* mgr);
	virtual ~TraderAdapter();

public:
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr);

	void release();

	bool run();

	inline const char* id() const{ return _id.c_str(); }

	bool isDone() const { return _done; }

	void queryFund();
	void queryPosition();

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi½Ó¿Ú
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	virtual void onLogout() override;

	virtual void onRspAccount(WTSArray* ayAccounts) override;

	virtual void onRspPosition(const WTSArray* ayPositions) override;

	virtual void onRspTrades(const WTSArray* ayTrades) override;

	virtual void onRspOrders(const WTSArray* ayOrders) override;

	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	virtual IBaseDataMgr* getBaseDataMgr() override;

	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	TraderAdapterMgr*	_mgr;
	WTSVariant*			_cfg;
	std::string			_id;

	ITraderApi*			_trader_api;
	FuncDeleteTrader	_remover;

	IBaseDataMgr*		_bd_mgr;
	uint32_t			_date;

	bool				_done;
};

typedef std::shared_ptr<TraderAdapter>				TraderAdapterPtr;
typedef std::unordered_map<std::string, TraderAdapterPtr>	TraderAdapterMap;

//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
// N-3 / 卡9a：管理器状态（适配器表、存活计数、互斥）放在进程内常驻、永不释放的堆对象中，
// 管理器本身平凡可析构。全局 g_adapterMgr 因此没有进程退出期析构：供应商线程在静态清理期
// 回调 decAlive()、Dumper worker 调 refresh() 时状态仍有效，不依赖 SDK 回调排空。
// 所有状态登记在永不释放的表中（LSan 视为可达）；生产只有一个全局管理器。
class TraderAdapterMgr
{
public:
	TraderAdapterMgr();
	TraderAdapterMgr(const TraderAdapterMgr&) = delete;
	TraderAdapterMgr& operator=(const TraderAdapterMgr&) = delete;

	void	release();

	void	run();

	const TraderAdapterMap& getAdapters() const { return _st->adapters; }

	TraderAdapterPtr getAdapter(const char* tname);

	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

	bool	isAnyAlive() const {
		return _st->live_cnt != 0;
	}

	std::size_t size() const { return _st->adapters.size(); }

	void decAlive();

	void refresh();

private:
	struct State
	{
		TraderAdapterMap		adapters;
		std::mutex				mutex;
		std::atomic<uint32_t>	live_cnt{0};
	};
	static State*	retainState();

	State* const	_st;
};

static_assert(std::is_trivially_destructible<TraderAdapterMgr>::value,
	"TraderAdapterMgr must not run an exit-time destructor while providers may call back (card 9a)");

NS_WTP_END
