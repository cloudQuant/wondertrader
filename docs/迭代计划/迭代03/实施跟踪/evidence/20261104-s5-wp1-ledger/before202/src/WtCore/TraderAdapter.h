/*!
 * \file TraderAdapter.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 交易适配器头文件
 * \details 定义了交易适配器类，用于封装交易接口，实现交易指令的发送和管理
 */
#pragma once

#include <boost/core/noncopyable.hpp>

#include "../Includes/ExecuteDefs.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/ITraderApi.h"
#include "../Share/DLLHelper.hpp"
#include "../Share/BoostFile.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/SpinMutex.hpp"
#include "../Share/FrequencyWindow.hpp"
#include "AccountTradingGate.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

NS_WTP_BEGIN
class WTSVariant;
class ActionPolicyMgr;
class WTSContractInfo;
class WTSCommodityInfo;
class WtLocalExecuter;
class EventNotifier;
class UnresolvedReportJournal;
class DiffOrderOwnershipJournalV1;

class ITrdNotifySink;

/*!
 * \brief 枚举通道持仓回调函数类型
 * \details 用于枚举交易通道的持仓信息的回调函数类型
 * \param stdCode 标准化合约代码
 * \param isLong 是否为多头持仓
 * \param preTotalPos 昨日总持仓
 * \param preAvailPos 昨日可用持仓
 * \param newTotalPos 今日总持仓
 * \param newAvailPos 今日可用持仓
 */
typedef std::function<void(const char*, bool, double, double, double, double)> FuncEnumChnlPosCallBack;

/*!
 * \brief 交易适配器类
 * \details 实现了ITraderSpi接口，用于封装交易接口，处理交易指令的发送、撤单、查询等操作
 * 并提供了风控功能、持仓管理、订单管理等功能
 */
class TraderAdapter : public ITraderSpi
{
public:
	friend struct N01TraderAdapterRiskTestAccess;

	// Monotonic millisecond clock for order/cancel frequency windows (card 7c).
	// Wall-clock time is only used by the logger.
	typedef uint64_t (*RateClock)();

	// A versioned WTS-visible snapshot, not a provider SDK payload.
	struct UnresolvedTradeReport
	{
		std::string payload;
		std::string diagnostic;
		uint64_t journal_sequence = 0;
		bool persisted = false;
	};
	// True while the account gate this adapter is bound to is paused. The
	// gate is shared by every adapter configured with the same account_id
	// (and so by every executer trading through them).
	bool isUnresolvedReportPaused() const noexcept;
	std::vector<UnresolvedTradeReport> unresolvedTradeReports() const;
	std::string unresolvedReportDiagnostic() const;

	//////////////////////////////////////////////////////////////////////////
	// Account gate (decision card 6a/6b) and order identity (card 5a/5d).
	/**
	 * Bind this adapter to a configured account key. Must be called before
	 * run() (init() calls it for the account_id / account_audit_log config).
	 * A pause on the previous binding is carried to the new gate. An audit log
	 * conflict or open failure pauses the account (fail closed) and returns false.
	 */
	bool bindAccount(const std::string& account_id, const std::string& audit_log,
		std::string* error = nullptr);
	std::shared_ptr<AccountTradingGate> accountGate() const { return _account_gate; }
	uint64_t accountPauseGeneration() const;
	/** Explicit manual resume; audited, generation-checked, never automatic. */
	AccountTradingGate::ResumeStatus resumeAccountTrading(
		const AccountTradingGate::ResumeRequest& request, std::string* diagnostic = nullptr);
	/** Admission closed: account paused or startup identity recovery not finished. */
	bool isTradingAdmissionClosed() const noexcept;
	bool isIdentityRecoveryPending() const noexcept;

	/**
	 * Versioned, non-reusable order identity. intent_id is unique per adapter
	 * boot (and durable when an identity journal is configured); localid alone
	 * may be reused across restarts without a journal and is never sufficient.
	 */
	struct OrderIdentityToken
	{
		std::string account;
		std::string intent_id;
		uint32_t localid = 0;
		bool valid() const noexcept { return localid != 0 && localid != UINT32_MAX && !intent_id.empty(); }
	};
	bool orderIdentity(uint32_t localid, OrderIdentityToken& token) const;
	/**
	 * Cancel only if localid still names the exact live order of token.
	 * Allowed while the account is paused (user decision 2026-11-04): only
	 * cancels of live orders whose identity is verified pass the paused gate.
	 */
	bool cancelVerified(const OrderIdentityToken& token);

	//////////////////////////////////////////////////////////////////////////
	// Owner routing (cards 5b/5c, NEW-008/N05-003)
	/**
	 * RAII order-entry owner for the current thread. Orders entered on this
	 * adapter inside the scope belong to owner. shared_capable says the owner's
	 * execution unit supports host-verified identity; the account claim is
	 * shared only when it is shared_capable and owner routing is enabled,
	 * otherwise the owner must exclusively own the contract (non-shared mode).
	 */
	class OwnerScope
	{
	public:
		OwnerScope(TraderAdapter* adapter, const std::string& owner, bool shared_capable);
		~OwnerScope();
		OwnerScope(const OwnerScope&) = delete;
		OwnerScope& operator=(const OwnerScope&) = delete;
		struct Frame
		{
			const TraderAdapter* adapter = nullptr;
			std::string owner;
			bool shared_capable = false;
			Frame* previous = nullptr;
		};
	private:
		Frame _frame;
	};
	/**
	 * RAII order-entry observer for the current thread (R15-001). For every
	 * entry made on this adapter inside the scope, observer(localid) runs after
	 * the local order id is reserved and before the provider call, so per-order
	 * data can be registered before any synchronous report. It also runs for
	 * reservations that later fail; buy()/sell() return only accepted ids.
	 * Neither owner scopes nor observers apply inside sink callbacks: an entry
	 * made re-entrantly from a callback belongs to its own caller.
	 */
	class EntrustObserverScope
	{
	public:
		EntrustObserverScope(TraderAdapter* adapter, const std::function<void(uint32_t)>& observer);
		~EntrustObserverScope();
		EntrustObserverScope(const EntrustObserverScope&) = delete;
		EntrustObserverScope& operator=(const EntrustObserverScope&) = delete;
		struct Frame
		{
			const TraderAdapter* adapter = nullptr;
			std::function<void(uint32_t)> observer;
			Frame* previous = nullptr;
		};
	private:
		Frame _frame;
	};
	OrderIDs buyAsOwner(const std::string& owner, bool shared_capable, const char* stdCode,
		double price, double qty, int flag, bool bForceClose);
	OrderIDs sellAsOwner(const std::string& owner, bool shared_capable, const char* stdCode,
		double price, double qty, int flag, bool bForceClose);
	/** Owner-verified cancel; never touches another owner's or an unverifiable order. */
	bool cancelForOwner(uint32_t localid, const std::string& owner);
	OrderIDs cancelForOwner(const char* stdCode, bool isBuy, double qty, const std::string& owner);
	double getUndoneQtyForOwner(const char* stdCode, const std::string& owner) const;
	OrderMap* getOrdersForOwner(const char* stdCode, const std::string& owner);
	bool isOwnerRoutingEnabled() const;
	/**
	 * Declare that sink acts for owner. If owner routing is enabled the owner
	 * registration is made atomically inside addSink(), before READY can be
	 * delivered to that sink; if the sink is already added it registers now.
	 */
	bool declareOwnerSink(ITrdNotifySink* sink, const std::string& owner);
	void releaseOwner(const std::string& owner);

	/** Per-order ledger view (tests/diagnostics). */
	enum class OrderLedgerState { Submitting, Live, Terminal, Absent };
	struct OrderLedgerView
	{
		bool found = false;
		OrderLedgerState state = OrderLedgerState::Submitting;
		double accounted = 0;	///< quantity currently counted in undone qty
		uint64_t last_version = 0;
		std::string std_code;
		std::string intent_id;
		std::string owner_id;
		bool verified = false;
	};
	OrderLedgerView orderLedger(uint32_t localid) const;
	uint64_t ordersSnapshotVersion() const;

	/*!
	 * \brief 构造函数
	 * \param caster 事件通知器指针，默认为NULL
	 */
	TraderAdapter(EventNotifier* caster = NULL);

	/*!
	 * \brief 析构函数
	 * \details 会请求停止并等待 sink 通知排空。适配器不能在自身 sink callback 栈内析构。
	 */
	virtual ~TraderAdapter();

		/*!
	 * \brief 适配器状态枚举
	 * \details 定义了交易适配器的各种状态，从未登录到全部就绪
	 */
	typedef enum tagAdapterState
	{
		AS_NOTLOGIN,		//未登录
		AS_LOGINING,		//正在登录
		AS_LOGINED,			//已登录
		AS_LOGINFAILED,		//登录失败
		AS_POSITION_QRYED,	//仓位已查
		AS_ORDERS_QRYED,	//订单已查
		AS_TRADES_QRYED,	//成交已查
		AS_ALLREADY			//全部就绪
	} AdapterState;

		/*!
	 * \brief 持仓项结构体
	 * \details 记录单个持仓项的详细信息，包括多空两个方向的今仓和昨仓数据
	 */
	typedef struct _PosItem
	{
		//多仓数据
		double	l_newvol;    ///< 多头今仓总量
		double	l_newavail;  ///< 多头今仓可用量
		double	l_prevol;    ///< 多头昨仓总量
		double	l_preavail;  ///< 多头昨仓可用量

		//空仓数据
		double	s_newvol;    ///< 空头今仓总量
		double	s_newavail;  ///< 空头今仓可用量
		double	s_prevol;    ///< 空头昨仓总量
		double	s_preavail;  ///< 空头昨仓可用量

		/*!
		 * \brief 构造函数
		 * \details 初始化所有持仓数据为0
		 */
		_PosItem()
		{
			memset(this, 0, sizeof(_PosItem));
		}

		/*!
		 * \brief 获取总持仓量
		 * \param isLong 是否为多头持仓
		 * \return 总持仓量（今仓+昨仓）
		 */
		double total_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newvol + l_prevol;
			else
				return s_newvol + s_prevol;
		}

		/*!
		 * \brief 获取可用持仓量
		 * \param isLong 是否为多头持仓
		 * \return 可用持仓量（今仓可用+昨仓可用）
		 */
		double avail_pos(bool isLong = true) const
		{
			if (isLong)
				return l_newavail + l_preavail;
			else
				return s_newavail + s_preavail;
		}

	} PosItem;

		/*!
	 * \brief 风控参数结构体
	 * \details 定义了交易风控的各项参数，包括下单和撤单的频率限制和总量限制
	 */
	typedef struct _RiskParams
	{
		uint32_t	_order_times_boundary;    ///< 下单次数边界（单位时间内的最大下单次数）
		uint32_t	_order_stat_timespan;     ///< 下单统计时间跨度（单位：秒）
		uint32_t	_order_total_limits;      ///< 下单总量限制（最大下单总次数）

		uint32_t	_cancel_times_boundary;   ///< 撤单次数边界（单位时间内的最大撤单次数）
		uint32_t	_cancel_stat_timespan;    ///< 撤单统计时间跨度（单位：秒）
		uint32_t	_cancel_total_limits;     ///< 撤单总量限制（最大撤单总次数）

		/*!
		 * \brief 构造函数
		 * \details 初始化所有风控参数为0
		 */
		_RiskParams()
		{
			memset(this, 0, sizeof(_RiskParams));
		}
	} RiskParams;

public:
	/*!
	 * \brief 初始化交易适配器
	 * \param id 适配器ID
	 * \param params 初始化参数
	 * \param bdMgr 基础数据管理器
	 * \param policyMgr 动作策略管理器
	 * \return 是否初始化成功
	 */
	bool init(const char* id, WTSVariant* params, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/*!
	 * \brief 使用外部API初始化交易适配器
	 * \details 登录责任（决策卡 10 = A）：外部 API 自行认证并登录，适配器不持有
	 *          其凭据；WTE_Connect 只报告连接状态，不调用 login()，登录结果经
	 *          外部 API 的 onLoginResult() 回调推进状态。init() 配置路径不变，
	 *          仍在 WTE_Connect(0) 时用配置中的 user/pass 登录。
	 * \param id 适配器ID
	 * \param api 外部交易API接口
	 * \param bdMgr 基础数据管理器
	 * \param policyMgr 动作策略管理器
	 * \return 是否初始化成功
	 */
	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	/*!
	 * \brief 释放交易适配器资源
	 */
	void release();

	/*!
	 * \brief 启动交易适配器
	 * \return 是否启动成功
	 */
	bool run();

		/*!
	 * \brief 获取适配器ID
	 * \return 适配器ID字符串
	 */
	inline const char* id() const{ return _id.c_str(); }

	/*!
	 * \brief 获取适配器状态
	 * \return 适配器状态枚举值
	 */
	AdapterState state() const
	{
		std::lock_guard<std::mutex> lock(_sink_mutex);
		return _state;
	}

	/*!
	 * \brief 订阅交易通知接收器
	 * \details 注册和 READY 状态读取在同一锁内线性化；若已就绪，会将初始 READY 放入
	 *          与后续 READY/LOST 相同的顺序队列。适配器不拥有 sink；销毁 sink 前应先
	 *          调用 removeSink() 并确认注销屏障完成。调用方不应再额外读取 isReady() 补发状态。
	 */
	void addSink(ITrdNotifySink* sink);

	/*!
	 * \brief 注销交易通知接收器
	 * \details 普通线程会等待已开始的回调结束。若从本适配器的 sink 回调中重入调用，
	 *          为避免等待自身或造成交叉注销死锁，只标记注销并返回 false；此时不能销毁
	 *          另一个仍在执行回调的 sink，需在回调栈退出后再次确认注销完成。
	 * \return true 表示当前没有该 sink 的回调在执行；false 表示重入注销时仍有回调在执行
	 */
	bool removeSink(ITrdNotifySink* sink);

	/*!
	 * \brief 非阻塞地停止新的 sink 通知
	 * \details 禁止新订阅，清空排队的通道状态事件，并将现有注册标记为 inactive。
	 *          此操作不等待正在执行的 callback，也不释放注册记录。
	 */
	void requestStopSinkNotifications();

	/*!
	 * \brief 等待 sink 通知完全停止
	 * \details 只能由当前线程不在本适配器任何 sink callback 栈中的调用方等待。
	 *          必须先调用 requestStopSinkNotifications()；未 request 或 callback 栈内调用会
	 *          立即返回 false。可重复调用；成功后释放 active/retired registration 记录。
	 * \return true 表示所有 callback 与 channel drain 均已结束；回调栈内返回 false
	 */
	bool waitSinkNotificationsStopped();

		/*!
	 * \brief 检查适配器是否已就绪
	 * \return 是否已就绪
	 */
	inline bool isReady() const { return state() == AS_ALLREADY; }

		/*!
	 * \brief 查询资金信息
	 */
	void queryFund();

	/**
	 * Capabilities required before owner-specific report routing can be enabled.
	 * All fields default to false. This only enables the internal sink-routing
	 * registry; it does not enable order submission.
	 */
	struct OwnerRoutingCapabilities
	{
		bool durable_journal = false;
		bool local_order_id_floor = false;
		bool provider_user_tag_round_trip = false;
		bool stable_trade_id_across_push_and_query = false;

		bool complete() const noexcept
		{
			return durable_journal && local_order_id_floor &&
				provider_user_tag_round_trip && stable_trade_id_across_push_and_query;
		}
	};

	/** Opaque generation token. A token is valid only for its adapter instance. */
	struct OwnerSinkRegistrationToken
	{
		OwnerSinkRegistrationToken() : _adapter_instance_id(0), _generation(0) {}
		bool valid() const noexcept { return _adapter_instance_id != 0 && _generation != 0 && !_owner_id.empty(); }

	private:
		std::string _owner_id;
		uint64_t _adapter_instance_id;
		uint64_t _generation;
		friend class TraderAdapter;
	};

	/**
	 * Enable the owner registry only after all required capabilities are verified.
	 * An incomplete capability set keeps owner registration and routing disabled.
	 */
	bool configureOwnerRoutingCapabilities(const OwnerRoutingCapabilities& capabilities);

	/**
	 * Register an already-added sink under a stable owner ID. Owner IDs must be
	 * unique among active registrations on this adapter; token must be invalid.
	 * Failed registrations leave the token and registry unchanged.
	 */
	bool registerOwnerSink(const std::string& stable_owner_id, ITrdNotifySink* sink,
		OwnerSinkRegistrationToken& token);

	/**
	 * Remove exactly the owner route represented by token; stale tokens fail.
	 * The sink remains a legacy subscriber. Before destroying it, also call
	 * removeSink() and confirm its callback barrier has drained.
	 */
	bool unregisterOwnerSink(const OwnerSinkRegistrationToken& token);

private:
	friend class TraderAdapterMgr;

	struct SinkRegistration
	{
		explicit SinkRegistration(ITrdNotifySink* value)
			: sink(value), active(true), in_flight(0), removal_waiter(false),
			  owner_registered(false), owner_callbacks_in_flight(0), owner_generation(0) {}

		ITrdNotifySink* sink;
		std::mutex mutex;
		std::condition_variable idle;
		bool active;
		size_t in_flight;
		bool removal_waiter;
		bool owner_registered;
		size_t owner_callbacks_in_flight;
		std::string owner_id;
		uint64_t owner_generation;
	};
	typedef std::shared_ptr<SinkRegistration> SinkRegistrationPtr;

	struct OwnerSinkRegistration
	{
		SinkRegistrationPtr sink_registration;
		uint64_t generation = 0;
	};

	struct ChannelNotification
	{
		uint64_t version;
		bool ready;
		std::vector<SinkRegistrationPtr> recipients;
	};

	void setState(AdapterState state);
	bool transitionState(AdapterState expected, AdapterState next);
	void forEachSink(const std::function<void(ITrdNotifySink*)>& callback);
	// Returns true only when the owner callback is invoked and returns normally;
	// false means no callback was invoked. Callback exceptions propagate.
	// This does not establish downstream processing or durable event storage.
	bool forEachSinkForOwner(const std::string& owner_id,
		const std::function<void(ITrdNotifySink*)>& callback);
	void invokeSink(const SinkRegistrationPtr& registration,
		const std::function<void(ITrdNotifySink*)>& callback);
	bool invokeOwnerSink(const SinkRegistrationPtr& registration, const std::string& owner_id,
		uint64_t generation, const std::function<void(ITrdNotifySink*)>& callback);
	void forEachNonOwnerSink(const std::function<void(ITrdNotifySink*)>& callback);
	// Without owner routing every event keeps the legacy broadcast. With owner
	// routing, owned events go to the owner sink plus non-owner observers and
	// unowned events go to non-owner observers only.
	void dispatchOrderEvent(const std::string& owner, const std::function<void(ITrdNotifySink*)>& callback);
	bool registerOwnerSinkLocked(const std::string& stable_owner_id, ITrdNotifySink* sink,
		OwnerSinkRegistrationToken& token);
	void finishSinkBroadcast();
	void finishSinkCallback(const SinkRegistrationPtr& registration, bool owner_callback = false);
	bool queueChannelNotificationLocked(bool ready,
		const std::vector<SinkRegistrationPtr>& recipients);
	void drainChannelNotifications();
	bool isInSinkCallback() const;
	static bool isInAnySinkCallback();
	bool sinkCallbacksDrainedLocked() const;

	// NEW-052 (card 9a, host side): provider SPI ingress. release() closes it
	// (callbacks arriving later are dropped without touching adapter state);
	// the destructor also waits for SPI callbacks already running inside this
	// adapter before releasing adapter-owned state. ITraderApi has no versioned quiescence capability, so a
	// callback after the adapter object itself is destroyed stays the
	// provider's contract (external).
	class SpiIngress
	{
	public:
		explicit SpiIngress(TraderAdapter* adapter);
		~SpiIngress();
		explicit operator bool() const noexcept { return _entered; }
		SpiIngress(const SpiIngress&) = delete;
		SpiIngress& operator=(const SpiIngress&) = delete;
	private:
		friend class TraderAdapter;
		TraderAdapter* _adapter;
		bool _entered;
		const SpiIngress* _previous;
	};
	static thread_local const SpiIngress* s_spi_ingress;	///< SPI callbacks running on this thread
	// Closes the ingress; with wait, also waits for running callbacks. Returns
	// true only after such a wait (false when not waiting or when called from
	// an SPI callback of this adapter, which cannot wait for itself).
	bool closeSpiIngress(bool wait);
	void reopenSpiIngress();

	void configureUnresolvedJournal(const std::string& path);
	void quarantineTrade(const WTSTradeInfo* trade, const char* reason);

	uint32_t doEntrust(WTSEntrust* entrust);
	bool	doCancel(WTSOrderInfo* ordInfo);

	// Undone quantity is owned by the per-order ledger created in doEntrust
	// before the provider call; submitSplit only records the result.
	bool submitSplit(OrderIDs& ids, const char* stdCode, bool isBuy, double qty,
		const std::function<uint32_t()>& submit);
	// ORs report flags into an in-flight submission; returns the previous
	// flags, or 0x80 when localid is not being submitted.
	uint8_t noteInflightReport(uint32_t localid, uint8_t bits);
	uint64_t rateNow() const { return _rate_clock(); }
	bool frequencyAvailable(bool isOrder, const char* stdCode, const RiskParams* riskPara) const;

	inline void	printPosition(const char* stdCode, const PosItem& pItem);

	inline WTSContractInfo* getContract(const char* stdCode);
	inline WTSCommodityInfo* getCommodify(const char* stdCommID);

	const RiskParams* getRiskParams(const char* stdCode);

	void initSaveData();

	inline void	logTrade(uint32_t localid, const char* stdCode, WTSTradeInfo* trdInfo);
	inline void	logOrder(uint32_t localid, const char* stdCode, WTSOrderInfo* ordInfo);

	void	saveData(WTSArray* ayFunds = NULL);

	inline void updateUndone(const char* stdCode, double qty, bool bOuput = true);

	struct OrderLedgerEntry
	{
		std::string std_code;
		std::string intent_id;
		double sign = 1.0;
		double accounted = 0;
		uint64_t created_version = 0;
		uint64_t last_version = 0;
		OrderLedgerState state = OrderLedgerState::Submitting;
		bool journal_terminal = false;
		bool cancel_claimed = false;
		bool verified = false;			///< identity created/recovered by this adapter boot
		std::string owner_id;
		std::unordered_set<std::string> trade_ids;	///< trade ids already applied to accounted qty
		// I03-NEW-057: order reports and trade reports carry the same fills
		// through two streams; accounted = min(order_left, volume - trade_filled).
		double volume = 0;			///< order volume (provider report, else entrust qty)
		double order_left = -1;		///< smallest volLeft of order reports (<0: none yet)
		double trade_filled = 0;	///< sum of distinct trade reports
	};
	// Applies one order report to the ledger. Returns true if the entry made
	// its first transition to Terminal (for one-shot notifications).
	bool ledgerApplyOrderLocked(uint32_t localid, const std::string& stdCode, bool isBuy,
		bool alive, double volume, double volLeft, uint64_t version);
	// May prune old tombstones: erasing from the dense ledger moves another
	// entry, so callers must not use any _ledger reference or iterator after it.
	void ledgerTombstoneLocked(uint32_t localid, OrderLedgerEntry& entry, OrderLedgerState state);
	void ledgerRecomputeLocked(OrderLedgerEntry& entry);
	void ledgerSetAccountedLocked(OrderLedgerEntry& entry, double accounted);
	// Pause-aware, owner-aware cancel admission; owner == nullptr is a bare call.
	bool cancelAdmissibleLocked(uint32_t localid, const std::string* owner, bool paused) const;
	bool cancelClaimed(uint32_t localid, const std::string* owner);
	void cancelUnclaim(uint32_t localid);
	bool cancelOne(uint32_t localid, const std::string* owner);
	OrderIDs cancelByCode(const char* stdCode, bool isBuy, double qty, const std::string* owner);
	void ledgerPruneLocked();
	uint64_t nextEventVersion();
	void pauseAccount(const char* source, const std::string& reason);
	void retainUnresolvedPayload(const std::string& payload, const char* kind);
	void quarantineOrder(const WTSOrderInfo* order, const char* reason);
	void configureIdentityJournal(const std::string& path, uint32_t floor, const std::string& floorEvidence, bool hasFloor);
	void journalTerminal(uint32_t localid, const std::string& intent, uint32_t status, double filled);
	bool identityResumeParticipant(const AccountTradingGate::ResumeRequest& request, std::string& diagnostic);
	void reconcileIdentityRecovery(const std::vector<std::pair<uint32_t, const WTSOrderInfo*>>& seen);

public:
		/*!
	 * \brief 获取持仓量
	 * \param stdCode 标准化合约代码
	 * \param bValidOnly 是否只获取有效持仓
	 * \param flag 持仓标志，默认为3（多空均获取）
	 * \return 持仓量
	 */
	double getPosition(const char* stdCode, bool bValidOnly, int32_t flag = 3);

	/*!
	 * \brief 获取指定合约的订单集合
	 * \param stdCode 标准化合约代码
	 * \return 订单集合指针
	 */
	OrderMap* getOrders(const char* stdCode);
		/*!
	 * \brief 获取未完成数量
	 * \param stdCode 标准化合约代码
	 * \return 未完成数量
	 */
	double getUndoneQty(const char* stdCode)
	{
		std::lock_guard<std::mutex> lock(_ledger_mutex);
		auto it = _undone_qty.find(stdCode);
		if (it != _undone_qty.end())
			return it->second;

		return 0;
	}

		/*!
	 * \brief 枚举所有持仓
	 * \param cb 回调函数
	 * \details 遍历所有持仓信息，并通过回调函数返回每个持仓项的详细信息
	 */
	void enumPosition(FuncEnumChnlPosCallBack cb);

		/*!
	 * \brief 开多仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t openLong(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 开空仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t openShort(const char* stdCode, double price, double qty, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 平多仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param isToday 是否平今仓
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t closeLong(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 平空仓
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param isToday 是否平今仓
	 * \param flag 标志
	 * \param cInfo 合约信息，默认为NULL
	 * \return 本地订单ID
	 */
	uint32_t closeShort(const char* stdCode, double price, double qty, bool isToday, int flag, WTSContractInfo* cInfo = NULL);
	
		/*!
	 * \brief 买入操作
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param bForceClose 是否强制平仓
	 * \param cInfo 合约信息，默认为NULL
	 * \return 订单ID集合
	 */
	OrderIDs buy(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 卖出操作
	 * \param stdCode 标准化合约代码
	 * \param price 价格
	 * \param qty 数量
	 * \param flag 标志
	 * \param bForceClose 是否强制平仓
	 * \param cInfo 合约信息，默认为NULL
	 * \return 订单ID集合
	 */
	OrderIDs sell(const char* stdCode, double price, double qty, int flag, bool bForceClose, WTSContractInfo* cInfo = NULL);

	/*!
	 * \brief 撤销指定订单
	 * \param localid 本地订单ID
	 * \return 是否撤单成功
	 */
	bool	cancel(uint32_t localid);

	/*!
	 * \brief 撤销指定合约的订单
	 * \param stdCode 标准化合约代码
	 * \param isBuy 是否为买入订单
	 * \param qty 撤单数量，默认为0（全部撤销）
	 * \return 撤销的订单ID集合
	 */
	OrderIDs cancel(const char* stdCode, bool isBuy, double qty = 0);

		/*!
	 * \brief 检查是否允许交易
	 * \param stdCode 标准化合约代码
	 * \return 是否允许交易
	 */
	bool	isTradeEnabled(const char* stdCode) const;

		/*!
	 * \brief 检查撤单限制
	 * \param stdCode 标准化合约代码
	 * \return 是否允许撤单
	 */
	bool	checkCancelLimits(const char* stdCode);

	/*!
	 * \brief 检查下单限制
	 * \param stdCode 标准化合约代码
	 * \return 是否允许下单
	 */
	bool	checkOrderLimits(const char* stdCode);

		/*!
	 * \brief 检查自成交
	 * \param stdCode 标准化合约代码
	 * \param tInfo 成交信息
	 * \return 是否存在自成交
	 */
	bool	checkSelfMatch(const char* stdCode, WTSTradeInfo* tInfo);

		/*!
	 * \brief 检查是否自成交
	 * \param stdCode 标准化合约代码
	 * \return 是否自成交
	 */
	inline	bool isSelfMatched(const char* stdCode)
	{
		//如果忽略自成交，则直接返回false
		if (_ignore_sefmatch)
			return false;

		auto it = _self_matches.find(stdCode);
		return it != _self_matches.end();
	}

public:
	//////////////////////////////////////////////////////////////////////////
	//ITraderSpi接口
	/*!
	 * \brief 处理交易事件
	 * \param e 交易事件类型
	 * \param ec 事件代码
	 */
	virtual void handleEvent(WTSTraderEvent e, int32_t ec) override;

	/*!
	 * \brief 登录结果回调
	 * \param bSucc 是否登录成功
	 * \param msg 登录结果消息
	 * \param tradingdate 交易日期
	 */
	virtual void onLoginResult(bool bSucc, const char* msg, uint32_t tradingdate) override;

	/*!
	 * \brief 登出回调
	 */
	virtual void onLogout() override;

	/*!
	 * \brief 委托响应回调
	 * \param entrust 委托信息
	 * \param err 错误信息
	 */
	virtual void onRspEntrust(WTSEntrust* entrust, WTSError *err) override;

	/*!
	 * \brief 账户资金响应回调
	 * \param ayAccounts 账户资金信息数组
	 */
	virtual void onRspAccount(WTSArray* ayAccounts) override;

	/*!
	 * \brief 持仓响应回调
	 * \param ayPositions 持仓信息数组
	 */
	virtual void onRspPosition(const WTSArray* ayPositions) override;

	/*!
	 * \brief 订单响应回调
	 * \param ayOrders 订单信息数组
	 */
	virtual void onRspOrders(const WTSArray* ayOrders) override;

	/*!
	 * \brief 成交响应回调
	 * \param ayTrades 成交信息数组
	 */
	virtual void onRspTrades(const WTSArray* ayTrades) override;

	/*!
	 * \brief 订单推送回调
	 * \param orderInfo 订单信息
	 */
	virtual void onPushOrder(WTSOrderInfo* orderInfo) override;

	/*!
	 * \brief 成交推送回调
	 * \param tradeRecord 成交记录
	 */
	virtual void onPushTrade(WTSTradeInfo* tradeRecord) override;

	/*!
	 * \brief 交易错误回调
	 * \param err 错误信息
	 * \param pData 附加数据，默认为NULL
	 */
	virtual void onTraderError(WTSError* err, void* pData = NULL) override;

	/*!
	 * \brief 获取基础数据管理器
	 * \return 基础数据管理器指针
	 */
	virtual IBaseDataMgr* getBaseDataMgr() override;

	/*!
	 * \brief 处理交易日志
	 * \param ll 日志级别
	 * \param message 日志消息
	 */
	virtual void handleTraderLog(WTSLogLevel ll, const char* message) override;

private:
	WTSVariant*			_cfg;				///< 配置项
	std::string			_id;					///< 适配器ID
	std::string			_order_pattern;		///< 订单模式

		uint32_t			_trading_day;			///< 交易日

	ITraderApi*			_trader_api;			///< 交易API接口
	FuncDeleteTrader	_remover;				///< 交易实例删除器
	DllHandle			_dll_handle;			///< 创建交易API的动态库句柄
	bool				_owns_trader_api;	///< 是否由插件工厂创建
	bool				_spi_registered;		///< 是否已向API注册SPI
	bool				_api_released;			///< 是否已释放API
	AdapterState		_state;				///< 适配器状态

		EventNotifier*		_notifier;			///< 事件通知器

	// _sinks is retained for source compatibility with existing diagnostics and tests.
	// All adapter access to it and to the registration tables is protected by _sink_mutex.
	wt_hashset<ITrdNotifySink*>	_sinks;	///< 交易通知接收器集合
	mutable std::mutex	_sink_mutex;
	std::unordered_map<ITrdNotifySink*, SinkRegistrationPtr> _sink_registrations;
	std::unordered_map<ITrdNotifySink*, SinkRegistrationPtr> _retired_sink_registrations;
	std::unordered_map<std::string, OwnerSinkRegistration> _owner_sink_registrations;
	// Retain this classification after owner unregistration until removeSink(),
	// so an owner sink never silently falls back into owner-event broadcasts.
	std::unordered_set<ITrdNotifySink*> _owner_sink_classified;
	uint64_t _owner_registry_instance_id = 0;
	uint64_t _next_owner_sink_generation = 1;
	bool _owner_routing_enabled = false;
	std::deque<ChannelNotification> _channel_notifications;
	uint64_t	_channel_event_version = 0;
	size_t	_sink_broadcasts_in_flight = 0;
	bool	_channel_drain_active = false;
	bool	_sink_notifications_stopping = false;
	std::condition_variable	_sink_notifications_idle;

	std::atomic<bool> _spi_closed{false};
	std::atomic<uint32_t> _spi_inflight{0};
	std::mutex _spi_mutex;
	std::condition_variable _spi_idle;

	std::atomic<bool> _unresolved_report_paused{false};	///< legacy flag, mirrors pauses of this adapter
	std::shared_ptr<AccountTradingGate> _account_gate;
	std::string _account_id;
	std::atomic<bool> _identity_recovery_pending{false};
	std::shared_ptr<DiffOrderOwnershipJournalV1> _identity_journal;
	std::string _identity_owner;
	std::unordered_map<ITrdNotifySink*, std::string> _declared_owner_sinks;	///< guarded by _sink_mutex
	std::unordered_map<std::string, OwnerSinkRegistrationToken> _declared_owner_tokens;	///< guarded by _sink_mutex
	// owner -> stdCode -> signed accounted undone qty (guarded by _ledger_mutex)
	std::unordered_map<std::string, std::unordered_map<std::string, double>> _owner_undone;
	std::string _boot_nonce;
	std::atomic<uint64_t> _intent_sequence{0};
	// localid -> intent of nonterminal journal intents found at startup.
	mutable std::mutex _identity_mutex;	///< guards _recovery_intents/_unrecoverable_intents; never held while calling the gate
	std::map<uint32_t, std::string> _recovery_intents;
	std::vector<std::string> _unrecoverable_intents;

	mutable std::mutex _ledger_mutex;	///< guards _ledger, _undone_qty and versions; never held across provider calls
	wt_hashmap<uint32_t, OrderLedgerEntry> _ledger;
	std::deque<uint32_t> _ledger_tombstones;
	uint64_t _event_version = 0;
	uint64_t _orders_query_version = 0;
	uint64_t _orders_snapshot_version = 0;
	uint64_t _positions_query_version = 0;
	std::unordered_map<std::string, uint64_t> _position_event_version;	///< stdCode -> last realtime trade version
	std::unordered_set<std::string> _position_baseline;	///< stdCodes whose position was set by a snapshot row
	// NEW-154: trade reports identified by exchange|trade id|order ref|direction.
	// _pushed_trades: realtime effects applied once; _counted_trades: statistics
	// counted once across push and query. Cleared when the trading day changes.
	std::unordered_set<std::string> _pushed_trades;
	std::unordered_set<std::string> _counted_trades;
	uint32_t _trade_dedupe_day = 0;
	mutable std::mutex _unresolved_report_mutex;
	std::shared_ptr<UnresolvedReportJournal> _unresolved_journal;
	std::vector<UnresolvedTradeReport> _unresolved_reports;
	std::string _unresolved_diagnostic{"journal not configured"};

	IBaseDataMgr*		_bd_mgr;				///< 基础数据管理器
	ActionPolicyMgr*	_policy_mgr;			///< 动作策略管理器

		wt_hashmap<std::string, PosItem> _positions;	///< 持仓集合

	SpinMutex	_mtx_orders;					///< 订单互斥锁
	OrderMap*	_orders;						///< 订单集合
	wt_hashset<std::string> _orderids;			///< 主要用于标记有没有处理过该订单

		wt_hashmap<std::string, std::string>		_trade_refs;		///< 用于记录成交单和订单的匹配
	wt_hashset<std::string>					_self_matches;	///< 自成交的合约

	/*
	 *	By Wesley @ 2023.03.16
	 *	加一个控制，这样自成交发生以后，还可以恢复交易
	 */
		bool			_ignore_sefmatch;		///< 忽略自成交限制

		wt_hashmap<std::string, double> _undone_qty;	///< 未完成数量

		typedef WTSHashMap<std::string>	TradeStatMap;
	TradeStatMap*	_stat_map;	///< 统计数据

		//这两个缓存时间内的容器,主要是为了控制电光流量而设置的
	typedef std::vector<uint64_t> TimeCacheList;
	typedef wt_hashmap<std::string, TimeCacheList> CodeTimeCacheMap;
	// Sliding windows ending at "now": N requests (including pending ones)
	// are admitted, N+1 is rejected before the API, and quota recovers when
	// recorded requests expire. There is no permanent frequency exclusion.
	mutable FrequencyWindow<CodeTimeCacheMap> _order_frequency;
	mutable FrequencyWindow<CodeTimeCacheMap> _cancel_frequency;
	RateClock _rate_clock = &FrequencyWindow<CodeTimeCacheMap>::now;

	// Submissions currently inside orderInsert, keyed by local order id, so a
	// synchronous (re-entrant) provider report can be reconciled with the
	// return value. Bits: 1 = rejection/cancel observed, 2 = acceptance observed.
	std::mutex _inflight_mutex;
	wt_hashmap<uint32_t, uint8_t> _inflight_entrusts;
	CodeTimeCacheMap& _order_time_cache = _order_frequency.cache;
	CodeTimeCacheMap& _cancel_time_cache = _cancel_frequency.cache;
	mutable std::mutex _risk_exclude_mutex;

		//如果被风控了,就会进入到排除队列
	wt_hashset<std::string>	_exclude_codes;	///< 被风控排除的合约代码集合
	
		typedef wt_hashmap<std::string, RiskParams>	RiskParamsMap;
	RiskParamsMap	_risk_params_map;		///< 风控参数映射表
	bool			_risk_mon_enabled;			///< 是否启用风控监控

		bool			_save_data;	///< 是否保存交易日志
	BoostFilePtr	_trades_log;		///< 交易数据日志
	BoostFilePtr	_orders_log;		///< 订单数据日志
	std::string		_rt_data_file;		///< 实时数据文件
};

/*!
 * \brief 交易适配器智能指针类型
 */
typedef std::shared_ptr<TraderAdapter>				TraderAdapterPtr;

/*!
 * \brief 交易适配器映射表类型
 */
typedef wt_hashmap<std::string, TraderAdapterPtr>	TraderAdapterMap;


//////////////////////////////////////////////////////////////////////////
//TraderAdapterMgr
/*!
 * \brief 交易适配器管理器
 * \details 管理多个交易适配器实例，提供适配器的添加、获取等功能
 */
class TraderAdapterMgr : private boost::noncopyable
{
public:
	~TraderAdapterMgr();

	/*!
	 * \brief 释放所有适配器资源
	 */
	void	release();

	/*!
	 * \brief 非阻塞地请求所有适配器停止 sink 通知
	 */
	void requestStopSinkNotifications();

	/*!
	 * \brief 等待所有适配器的 sink 通知排空
	 * \details 必须先请求全部 adapter 停止。若当前线程在任一 sink callback 栈内，立即返回
	 *          false；任一等待失败时保留管理器中的全部 adapter。
	 */
	bool waitSinkNotificationsStopped();

	/*!
	 * \brief 查询当前线程是否位于本管理器任一 adapter 的 sink callback 栈内
	 * \details 只读且不等待。TraderAdapterMgr 本身与其他管理接口一样，不支持和 addAdapter/
	 *          release 并发访问；调用方须保证查询期间管理器映射不被修改。
	 */
	bool isInSinkCallback() const;

	/*!
	 * \brief 运行所有适配器
	 */
	void	run();

	/*!
	 * \brief 获取所有适配器
	 * \return 适配器映射表引用
	 */
	const TraderAdapterMap& getAdapters() const { return _adapters; }

	/*!
	 * \brief 获取指定适配器
	 * \param tname 适配器名称
	 * \return 适配器指针
	 */
	TraderAdapterPtr getAdapter(const char* tname);

	/*!
	 * \brief 添加交易适配器
	 * \param tname 适配器名称
	 * \param adapter 适配器指针
	 * \return 是否添加成功
	 */
	bool	addAdapter(const char* tname, TraderAdapterPtr& adapter);

	/*!
	 * \brief 刷新资金信息
	 */
	void	refresh_funds();

	/*!
	 * \brief Explicit audited account resume command (card 6b) for the account
	 *        gate bound to adapter tname. Returns NotPaused/InvalidRequest when
	 *        the adapter is unknown (diagnostic says which).
	 */
	AccountTradingGate::ResumeStatus resumeAccountTrading(const char* tname,
		const AccountTradingGate::ResumeRequest& request, std::string* diagnostic = nullptr);
	/*! \brief Current pause generation of the account gate of tname (0 if unknown). */
	uint64_t accountPauseGeneration(const char* tname);

private:
	TraderAdapterMap	_adapters;			///< 适配器映射表
};

NS_WTP_END
