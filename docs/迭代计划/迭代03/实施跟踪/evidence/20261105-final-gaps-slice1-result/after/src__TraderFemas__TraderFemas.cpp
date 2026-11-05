/*!
 * \file TraderFemas.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include <cmath>
#include <climits>
#include "TraderFemas.h"
#include "../Share/ProviderRawReport.hpp"
#include <boost/asio/post.hpp>

#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/ModuleHelper.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/Converter.hpp"

#include <filesystem>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(ITraderSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	const char* buffer = fmtutil::format(format, args...);
	sink->handleTraderLog(ll, buffer);
}

namespace
{
	// BUG-T4-003 (card 9d): identifies the TraderFemas whose worker runs on the
	// current thread, and the innermost TraderFemas SPI callback on this thread.
	// release()/destruction issued from either context must not block or join.
	thread_local const TraderFemas* t_femas_worker_owner = NULL;
	thread_local const TraderFemas* t_femas_callback_owner = NULL;

	// Card 9a/9b: an instance whose quiescence cannot be confirmed is kept alive
	// until process exit together with this module. The registry is never
	// destroyed so retained objects stay reachable.
	void pin_own_module()
	{
#ifdef _WIN32
		HMODULE self = NULL;
		::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCSTR>(&pin_own_module), &self);
#else
		Dl_info info;
		if (::dladdr(reinterpret_cast<void*>(&pin_own_module), &info) != 0 && info.dli_fname != NULL)
		{
			// The handle is intentionally never closed: it pins the module.
			(void)::dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
		}
#endif
	}

	void quarantine_femas(TraderFemas* trader, const char* reason)
	{
		static std::mutex* mtx = new std::mutex();
		static std::vector<TraderFemas*>* retained = new std::vector<TraderFemas*>();
		{
			std::lock_guard<std::mutex> lock(*mtx);
			retained->push_back(trader);
		}
		pin_own_module();
		std::fprintf(stderr, "[TraderFemas] deleteTrader: instance quarantined until process exit (%s)\n", reason);
	}
}

extern "C"
{
	// BUG-L01-014 / K42: optional host feature declaration (WT_TRADER_SPI_FEATURES_SYMBOL).
	EXPORT_FLAG void setTraderSpiFeatures(ITraderApi* trader, uint64_t features)
	{
		TraderFemas* self = dynamic_cast<TraderFemas*>(trader);
		if (self != NULL)
			self->setSpiFeatures(features);
	}

	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderFemas *instance = new TraderFemas();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			TraderFemas* femas = dynamic_cast<TraderFemas*>(trader);
			if (femas != NULL)
			{
				if (femas->isCallbackContext())
				{
					// Card 9d/9b: the worker or an SPI callback cannot confirm its own
					// exit. Close admission, request stop and keep the instance.
					femas->release();
					femas->detachForQuarantine();
					quarantine_femas(femas, "called from a worker or SPI callback thread");
					trader = NULL;
					return;
				}
				femas->release();
				if (!femas->isQuiesced())
				{
					// Card 9a: requests or callbacks did not drain within the bound.
					femas->detachForQuarantine();
					quarantine_femas(femas, "worker, API calls or callbacks did not drain");
					trader = NULL;
					return;
				}
			}
			delete trader;
			trader = NULL;
		}
	}
};

class TraderFemas::ApiLease
{
public:
	explicit ApiLease(TraderFemas* owner) : _owner(owner), _held(owner->acquireApiLease()) {}
	~ApiLease() { if (_held) _owner->releaseApiLease(); }
	ApiLease(const ApiLease&) = delete;
	ApiLease& operator=(const ApiLease&) = delete;
	explicit operator bool() const { return _held; }

private:
	TraderFemas*	_owner;
	bool			_held;
};

class TraderFemas::CallbackScope
{
public:
	explicit CallbackScope(TraderFemas* owner)
		: _owner(owner)
		, _previous(t_femas_callback_owner)
	{
		t_femas_callback_owner = owner;
		_admitted = owner->enterCallback();
	}
	~CallbackScope()
	{
		_owner->leaveCallback();
		t_femas_callback_owner = _previous;
	}
	CallbackScope(const CallbackScope&) = delete;
	CallbackScope& operator=(const CallbackScope&) = delete;
	bool admitted() const { return _admitted; }

private:
	TraderFemas*		_owner;
	const TraderFemas*	_previous;
	bool				_admitted = false;
};

// Every SPI callback counts itself while inside this wrapper and delivers
// nothing once release() has been requested (card 9 isolation).
#define FEMAS_CALLBACK_SCOPE() \
	CallbackScope femasCallbackScope(this); \
	if (!femasCallbackScope.admitted()) \
		return

inline uint32_t strToTime(const char* strTime)
{
	std::string str;
	const char *pos = strTime;
	while(strlen(pos) > 0)
	{
		if(pos[0] != ':')
		{
			str.append(pos, 1);
		}
		pos++;
	}

	return convert::to_uint32(str.c_str());
}

namespace
{
	template <std::size_t N>
	bool copyCStringToSdkField(char (&destination)[N], const char* source)
	{
		if (source == NULL)
			return false;

		const char* terminator = static_cast<const char*>(std::memchr(source, '\0', N));
		if (terminator == NULL)
			return false;

		const std::size_t length = static_cast<std::size_t>(terminator - source);
		std::memcpy(destination, source, length + 1);
		return true;
	}

	template <std::size_t N>
	bool formatSessionLocalOrderID(char (&destination)[N], const std::string& sessionID, uint32_t localOrderID)
	{
		constexpr std::size_t localOrderIDDigits = 12;
		if (N <= localOrderIDDigits || sessionID.size() > N - localOrderIDDigits - 1)
			return false;

		const std::string value = fmt::format("{}{:012d}", sessionID, localOrderID);
		if (value.size() + 1 > N)
			return false;

		std::memcpy(destination, value.c_str(), value.size() + 1);
		return true;
	}
}

TraderFemas::TraderFemas()
	: m_bQuickStart(false)
	, m_sink(NULL)
	, m_uLastQryTime(0)
	, m_lDate(0)
	, m_uLocalOrdID(1)
	, m_wrapperState(WS_NOTLOGIN)
	, m_pUserAPI(NULL)
	, m_iRequestID(0)
	, m_ayPosition(NULL)
	, m_ayTrades(NULL)
	, m_ayOrders(NULL)
	, m_ayPosDetail(NULL)
	, m_bdMgr(NULL)
	, m_bQryOnline(false)
	, m_bStopped(false)
	, m_strandIO(NULL)
	, m_apiLeases(0)
	, m_activeCallbacks(0)
	, m_workerExited(true)
	, m_lifecycle(LS_IDLE)
	, m_drainTimeoutMs(5000)
	, m_bInQuery(false)
	, m_lastQryTime(0)
	, m_hInstCTP(NULL)
	, m_funcCreator(NULL)
{
}


TraderFemas::~TraderFemas()
{
	if (isCallbackContext())
	{
		// Card 9b/9d: destroying the object from its own worker or from one of
		// its SPI callbacks would free memory the running thread still uses and
		// cannot join it. deleteTrader quarantines such a request instead.
		std::fprintf(stderr, "[TraderFemas] Direct delete on a worker/callback thread is unsupported; use deleteTrader\n");
		std::abort();
	}
	release();
	if (!isQuiesced())
	{
		// Card 9a: the worker, an outbound API call or an SPI callback is still
		// running inside this object; freeing it now is a use-after-free.
		std::fprintf(stderr, "[TraderFemas] Destroyed before callbacks and API calls drained; use deleteTrader\n");
		std::abort();
	}

	// I03-NEW-016: the query containers are created lazily by SDK callbacks and are
	// owned by this instance. release() may run on the worker (disconnect()) while a
	// vendor callback cannot be proven quiescent (card 9a), so it only clears them.
	// Final ownership is returned here, when the factory remover destroys the
	// instance; each container is released exactly once and the member is reset.
	WTSArray** containers[] = { &m_ayOrders, &m_ayTrades, &m_ayPosDetail, &m_ayPosition };
	for (WTSArray** slot : containers)
	{
		if (*slot != NULL)
		{
			(*slot)->release();
			*slot = NULL;
		}
	}
}

bool TraderFemas::init(WTSVariant* params)
{
	if (m_hInstCTP != NULL || m_pUserAPI != NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot initialize an API instance more than once");
		return false;
	}

	if (params == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot initialize with null configuration");
		return false;
	}

	m_strFront = params->getCString("front");
	m_strBroker = params->getCString("broker");
	m_strUser = params->getCString("user");
	m_strPass = params->getCString("pass");

	m_strAppID = params->getCString("appid");
	m_strAuthCode = params->getCString("authcode");

	m_bQuickStart = params->getBoolean("quick");

	m_strQryFront = params->getCString("query");

	if (m_strQryFront.empty())
		m_bQryOnline = true;

	m_strFlowDir = params->getCString("flowdir");

	if (m_strFlowDir.empty())
		m_strFlowDir = "FemasTDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	WTSVariant* param = params->get("ctpmodule");
	if (param != NULL)
	{
		const std::string wrappedModule = DLLHelper::wrap_module(param->asCString(), "");
		const char* moduleDir = getBinDir();
		if ((moduleDir == NULL || *moduleDir == '\0') && !fs::path(wrappedModule).is_absolute())
		{
			write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot locate trader module directory");
			return false;
		}
		m_strModule = (moduleDir == NULL ? std::string() : std::string(moduleDir)) + wrappedModule;
	}
	else
		m_strModule = DLLHelper::wrap_module("USTPtraderapiAF", "lib");

	m_hInstCTP = DLLHelper::load_library(m_strModule.c_str());
	if (m_hInstCTP == NULL)
	{
		m_funcCreator = NULL;
		write_log(m_sink, LL_ERROR, "[TraderFemas] Failed to load trader API module: {}", m_strModule);
		return false;
	}
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateFtdcTraderApi@CUstpFtdcTraderApi@@SAPEAV1@PEBD@Z";
#	else
	const char* creatorName = "?CreateFtdcTraderApi@CUstpFtdcTraderApi@@SAPAV1@PBD@Z";
#	endif	
#else
	const char* creatorName = "_ZN18CUstpFtdcTraderApi19CreateFtdcTraderApiEPKc";
#endif
	m_funcCreator = (APICreator)DLLHelper::get_symbol(m_hInstCTP, creatorName);
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Trader API creator symbol not found in {}: {}", m_strModule, creatorName);
		DLLHelper::free_library(m_hInstCTP);
		m_hInstCTP = NULL;
		return false;
	}

	return true;
}

TraderFemas::LifecycleState TraderFemas::lifecycleState() const
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	return m_lifecycle;
}

bool TraderFemas::isCallbackContext() const
{
	return t_femas_worker_owner == this || t_femas_callback_owner == this;
}

bool TraderFemas::isQuiesced() const
{
	return lifecycleState() == LS_RELEASED;
}

void TraderFemas::detachForQuarantine()
{
	// Never block: an external release() holding the lock completes the join.
	std::unique_lock<std::mutex> releaseLock(m_mtxRelease, std::try_to_lock);
	if (!releaseLock.owns_lock())
		return;
	StdThreadPtr worker;
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		worker = m_thrdWorker;
	}
	if (worker && worker->joinable())
		worker->detach();
}

bool TraderFemas::acquireApiLease()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_bStopped.load(std::memory_order_acquire))
		return false;
	++m_apiLeases;
	return true;
}

void TraderFemas::releaseApiLease()
{
	// Notify under the lock: a releaser may destroy this object as soon as it
	// observes zero leases and reacquires the mutex.
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_apiLeases > 0)
		--m_apiLeases;
	m_cvLifecycle.notify_all();
}

bool TraderFemas::enterCallback()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	++m_activeCallbacks;
	return !m_bStopped.load(std::memory_order_acquire);
}

void TraderFemas::leaveCallback()
{
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	if (m_activeCallbacks > 0)
		--m_activeCallbacks;
	m_cvLifecycle.notify_all();
}

void TraderFemas::requestStop()
{
	// Close admission first: no request, API call or callback delivery starts
	// after this point. The worker leaves run() once its current handler returns.
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		m_bStopped.store(true, std::memory_order_release);
		if (m_lifecycle == LS_IDLE || m_lifecycle == LS_RUNNING)
			m_lifecycle = LS_STOP_REQUESTED;
		m_workGuard.reset();
	}
	m_wrapperState = WS_NOTLOGIN;
	m_asyncIO.stop();
}

void TraderFemas::workerMain()
{
	{
		// Barrier: connect() publishes m_thrdWorker under this mutex.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	}
	t_femas_worker_owner = this;
	try
	{
		m_asyncIO.run();
	}
	catch (...)
	{
		std::fprintf(stderr, "[TraderFemas] Worker stopped by an exception from a query handler\n");
	}
	t_femas_worker_owner = NULL;
	// Notify under the lock; the external releaser joins this thread afterwards.
	std::lock_guard<std::mutex> lock(m_mtxLifecycle);
	m_workerExited = true;
	m_cvLifecycle.notify_all();
}

void TraderFemas::release()
{
	if (isCallbackContext())
	{
		// Card 9d: from the worker or an SPI callback only request stop. The join,
		// drain and API Release are completed by a later external release(), the
		// destructor, or never (deleteTrader quarantines the instance).
		requestStop();
		return;
	}

	// Concurrent external release()/disconnect()/destructor calls are serialized.
	std::lock_guard<std::mutex> releaseLock(m_mtxRelease);
	requestStop();
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (m_lifecycle == LS_RELEASED)
			return;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_drainTimeoutMs);
	bool drained = true;

	// 1. Outbound API calls started before admission closed (lease holders),
	// including a connect() that is still creating the API or the worker.
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		if (!m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_apiLeases == 0; }))
			drained = false;
	}

	// 2. The worker: join only from a non-worker thread after it left run().
	StdThreadPtr worker;
	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		worker = m_thrdWorker;
	}
	if (drained && worker && worker->joinable())
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		const bool exited = m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_workerExited; });
		lock.unlock();
		if (exited)
			worker->join();
		else
			drained = false;
	}

	CUstpFtdcTraderApi* api = m_pUserAPI;
	if (drained && api != NULL)
		api->RegisterSpi(NULL);

	// 3. SPI callbacks that already entered this wrapper.
	{
		std::unique_lock<std::mutex> lock(m_mtxLifecycle);
		if (!m_cvLifecycle.wait_until(lock, deadline, [this]() { return m_activeCallbacks == 0; }))
			drained = false;
	}

	if (!drained)
	{
		// Card 9a: keep the API and the query state; the instance must not be
		// destroyed (deleteTrader quarantines it). A later release() retries.
		{
			std::lock_guard<std::mutex> lock(m_mtxLifecycle);
			m_lifecycle = LS_UNQUIESCED;
		}
		std::fprintf(stderr, "[TraderFemas] release: worker, API calls or callbacks did not drain within %u ms; instance kept\n",
			m_drainTimeoutMs);
		return;
	}

	if (api != NULL)
	{
		api->Release();
		m_pUserAPI = NULL;
	}

	if(m_ayOrders)
		m_ayOrders->clear();

	if(m_ayPosDetail)
		m_ayPosDetail->clear();

	if(m_ayPosition)
		m_ayPosition->clear();

	if(m_ayTrades)
		m_ayTrades->clear();

	{
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		m_lifecycle = LS_RELEASED;
	}
}

void TraderFemas::connect()
{
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot connect: trader API creator is unavailable; init must succeed first");
		return;
	}

	{
		// Card 9c: a released instance is terminal and a connected instance keeps
		// its API; reconnecting uses a new TraderFemas instance.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (m_bStopped.load(std::memory_order_acquire))
		{
			write_log(m_sink, LL_ERROR, "[TraderFemas] Cannot connect after release");
			return;
		}
		if (m_pUserAPI != NULL || m_lifecycle != LS_IDLE)
		{
			write_log(m_sink, LL_ERROR, "[TraderFemas] Repeated connect rejected; the existing API is kept");
			return;
		}
		m_lifecycle = LS_RUNNING;
	}

	// API setup is an outbound call: release() waits for it to finish.
	ApiLease lease(this);
	if (!lease)
		return;

	std::stringstream ss;
	ss << m_strFlowDir << "flows/" << m_strBroker << "/" << m_strUser << "/";
	std::string path = ss.str();
	fs::create_directories(path.c_str());
	CUstpFtdcTraderApi* userAPI = m_funcCreator(path.c_str());
	if (userAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Trader API creator returned null; connection aborted");
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (m_lifecycle == LS_RUNNING)
			m_lifecycle = LS_IDLE;
		return;
	}
	m_pUserAPI = userAPI;
	// Init() may invoke SPI callbacks before it returns. A successful login callback
	// can synchronously reenter queryAccount(), whose triggerQuery() posts to this
	// strand, so publish the strand before registering the SPI or starting the API.
	if (m_strandIO == NULL)
	{
		m_ownedStrand.reset(new boost::asio::io_context::strand(m_asyncIO));
		m_strandIO = m_ownedStrand.get();
	}
	{
		// BUG-T4-003: a persistent work guard keeps the worker serving queries
		// while idle (run() would otherwise return and stop the context).
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		if (!m_bStopped.load(std::memory_order_acquire) && !m_workGuard)
			m_workGuard.reset(new WorkGuard(boost::asio::make_work_guard(m_asyncIO)));
	}

	m_pUserAPI->RegisterSpi(this);
	if(m_bQuickStart)
	{
		m_pUserAPI->SubscribePublicTopic(USTP_TERT_QUICK);				// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(USTP_TERT_QUICK);				// 注册私有流
	}
	else
	{
		m_pUserAPI->SubscribePublicTopic(USTP_TERT_RESUME);				// 注册公有流
		m_pUserAPI->SubscribePrivateTopic(USTP_TERT_RESUME);			// 注册私有流
	}

	m_pUserAPI->RegisterFront((char*)m_strFront.c_str());
	m_pUserAPI->RegisterQryFront((char*)m_strQryFront.c_str());
	m_pUserAPI->SetHeartbeatTimeout(60);

	std::string logfile = path + "femas_req.log";
	m_pUserAPI->OpenRequestLog(logfile.c_str());
	logfile = path + "femas_res.log";
	m_pUserAPI->OpenResponseLog(logfile.c_str());

	m_pUserAPI->Init();

	// Start dispatching only after Init() returns. A callback invoked on the Init()
	// stack may queue a query, which the worker will process after API setup ends.
	if (m_thrdWorker == NULL && !m_bStopped.load(std::memory_order_acquire))
	{
		// Publish the thread object before the worker runs any handler: a handler
		// may reach deleteTrader()/release(), which read m_thrdWorker.
		std::lock_guard<std::mutex> lock(m_mtxLifecycle);
		m_workerExited = false;
		m_thrdWorker.reset(new StdThread([this]() { workerMain(); }));
	}
}

void TraderFemas::disconnect()
{
	// BUG-T4-003: disconnect ends this instance like release(); from a callback or
	// the worker it only requests stop (card 9d).
	release();
}

bool TraderFemas::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length <= 0)
		return false;

	try
	{
		// The order reference is a uint32 and is always formatted with at least
		// twelve digits. Reserve one additional byte for the trailing NUL.
		if (length < 13 || m_strSessionID.size() > static_cast<size_t>(length - 13))
			return false;

		uint32_t orderref = genLocalOrdID();
		const std::string entrustID = fmt::format("{}{:012d}", m_strSessionID, orderref);
		if (entrustID.size() + 1 > static_cast<size_t>(length))
			return false;

		memcpy(buffer, entrustID.c_str(), entrustID.size() + 1);
		return true;
	}
	catch(...)
	{

	}

	return false;
}

void TraderFemas::registerSpi( ITraderSpi *listener )
{
	m_sink = listener;
	if(m_sink)
	{
		m_bdMgr = listener->getBaseDataMgr();
	}
}

uint32_t TraderFemas::genRequestID()
{
	return m_iRequestID.fetch_add(1) + 1;
}

uint32_t TraderFemas::genLocalOrdID()
{
	return m_uLocalOrdID.fetch_add(1) + 1;
}

int TraderFemas::authenticate()
{
	CUstpFtdcDSUserInfoField sDSUserInfo;
	strcpy(sDSUserInfo.AppID, m_strAppID.c_str());
	strcpy(sDSUserInfo.AuthCode, m_strAuthCode.c_str());
	sDSUserInfo.EncryptType = '1';
	m_pUserAPI->ReqDSUserCertification(&sDSUserInfo, genRequestID());

	return 0;
}

int TraderFemas::doLogin()
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL)
		return -1;

	CUstpFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	strcpy(req.Password, m_strPass.c_str());
	strcpy(req.UserProductInfo, m_strProduct.c_str());
	int iResult = m_pUserAPI->ReqUserLogin(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending login request failed: {}", iResult);
	}

	return 0;
}

int TraderFemas::login( const char* user, const char* pass, const char* productInfo )
{
	m_strUser = user;
	m_strPass = pass;
	m_strProduct = productInfo;

	ApiLease lease(this);
	if(!lease || m_pUserAPI == NULL)
	{
		return -1;
	}

	m_wrapperState = WS_LOGINING;
	authenticate();

	return 0;
}

int TraderFemas::logout()
{
	ApiLease lease(this);
	if (!lease || m_pUserAPI == NULL)
	{
		return -1;
	}

	CUstpFtdcReqUserLogoutField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	int iResult = m_pUserAPI->ReqUserLogout(&req, genRequestID());
	if (iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending logout request failed: {}", iResult);
	}

	return 0;
}

// Card 8 (BUG-P02-005, slice1 final gaps): this provider trades whole lots. A
// quantity that is not finite, not positive, larger than the SDK int field or
// fractional is rejected before any order identity is consumed: the strategy
// rounds, the adapter never truncates through the integer SDK field.
static bool isWholeLotVolume(double volume)
{
	return std::isfinite(volume) && volume >= 1 && volume <= (double)INT_MAX && std::floor(volume) == volume;
}

int TraderFemas::orderInsert(WTSEntrust* entrust)
{
	ApiLease lease(this);
	if (!lease || entrust == NULL || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	WTSContractInfo* ct = entrust->getContractInfo();
	if (ct == NULL)
		return -1;

	if (!isWholeLotVolume(entrust->getVolume()))
	{
		write_log(m_sink, LL_ERROR, "[TraderFemas] Order rejected: quantity {} is not a positive whole number of lots", entrust != NULL ? entrust->getVolume() : 0.0);
		return -1;
	}

	CUstpFtdcInputOrderField req = {};
	if (entrust->getUserTag()[0] == '\0')
	{
		constexpr std::size_t localOrderIDDigits = 12;
		if (sizeof(req.UserOrderLocalID) <= localOrderIDDigits + 1
			|| m_strSessionID.size() > sizeof(req.UserOrderLocalID) - localOrderIDDigits - 1
			|| !formatSessionLocalOrderID(req.UserOrderLocalID, m_strSessionID, genLocalOrdID()))
		{
			write_log(m_sink, LL_ERROR,
				"[TraderFemas] Generated user order local ID does not fit SDK field capacity ({} bytes)",
				sizeof(req.UserOrderLocalID) - 1);
			return -1;
		}
	}
	else if (!copyCStringToSdkField(req.UserOrderLocalID, entrust->getUserTag()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] User tag cannot be represented in SDK UserOrderLocalID (maximum {} bytes)",
			sizeof(req.UserOrderLocalID) - 1);
		return -1;
	}

	///经纪公司代码
	strcpy(req.BrokerID, m_strBroker.c_str());
	///投资者代码
	strcpy(req.UserID, m_strUser.c_str());
	strcpy(req.InvestorID, m_strUser.c_str());
	///合约代码
	strcpy(req.InstrumentID, entrust->getCode());

	strcpy(req.ExchangeID, wrapExchg(ct->getExchg()));
	//生成本地委托单号
	//entrust->setEntrustID(req.UserOrderLocalID);

	req.OrderPriceType = wrapPriceType(entrust->getPriceType(), strcmp(ct->getExchg(), "CFFEX") == 0);
	req.Direction = wrapDirectionType(entrust->getDirection(), entrust->getOffsetType());
	req.OffsetFlag = wrapOffsetType(entrust->getOffsetType());
	req.HedgeFlag = USTP_FTDC_CHF_Speculation;
	req.LimitPrice = entrust->getPrice();
	req.Volume = (int)entrust->getVolume();

	if (entrust->getOrderFlag() == WOF_NOR)
	{
		req.TimeCondition = USTP_FTDC_TC_GFD;
		req.VolumeCondition = USTP_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FAK)
	{
		req.TimeCondition = USTP_FTDC_TC_IOC;
		req.VolumeCondition = USTP_FTDC_VC_AV;
	}
	else if (entrust->getOrderFlag() == WOF_FOK)
	{
		req.TimeCondition = USTP_FTDC_TC_IOC;
		req.VolumeCondition = USTP_FTDC_VC_CV;
	}

	req.ForceCloseReason = USTP_FTDC_FCR_NotForceClose;

	int iResult = m_pUserAPI->ReqOrderInsert(&req, genRequestID());
	if(iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Order inserting failed: {}", iResult);
	}

	return 0;
}

int TraderFemas::orderAction( WTSEntrustAction* action )
{
	ApiLease lease(this);
	if (!lease || action == NULL || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
		return -1;

	//uint32_t frontid, sessionid, orderref;
	//if(!extractEntrustID(action->getEntrustID(), frontid, sessionid, orderref))
	//	return -1;

	CUstpFtdcOrderActionField req = {};
	if (!copyCStringToSdkField(req.UserOrderLocalID, action->getEntrustID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Entrust ID cannot be represented in SDK UserOrderLocalID (maximum {} bytes)",
			sizeof(req.UserOrderLocalID) - 1);
		return -1;
	}
	if (!copyCStringToSdkField(req.OrderSysID, action->getOrderID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Order ID cannot be represented in SDK OrderSysID (maximum {} bytes)",
		sizeof(req.OrderSysID) - 1);
		return -1;
	}
	constexpr std::size_t localOrderIDDigits = 12;
	if (sizeof(req.UserOrderActionLocalID) <= localOrderIDDigits + 1
		|| m_strSessionID.size() > sizeof(req.UserOrderActionLocalID) - localOrderIDDigits - 1
		|| !formatSessionLocalOrderID(req.UserOrderActionLocalID, m_strSessionID, genLocalOrdID()))
	{
		write_log(m_sink, LL_ERROR,
			"[TraderFemas] Generated user order action local ID does not fit SDK field capacity ({} bytes)",
		sizeof(req.UserOrderActionLocalID) - 1);
		return -1;
	}

	strcpy(req.BrokerID, m_strBroker.c_str());
	strcpy(req.InvestorID, m_strUser.c_str());
	strcpy(req.UserID, m_strUser.c_str());
	req.ActionFlag = wrapActionFlag(action->getActionFlag());

	strcpy(req.ExchangeID, wrapExchg(action->getExchg()));

	int iResult = m_pUserAPI->ReqOrderAction(&req, genRequestID());
	if(iResult != 0)
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas] Sending cancel request failed: {}", iResult);
	}

	return 0;	
}

int TraderFemas::queryAccount()
{
	if(m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	StdUniqueLock lock(m_mtxQuery);
	m_queQuery.push([this](){
		CUstpFtdcQryInvestorAccountField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, m_strBroker.c_str());
		strcpy(req.InvestorID, m_strUser.c_str());
		strcpy(req.UserID, m_strUser.c_str());
		// BUG-T4-003: the request runs only while admission is open and holds a
		// lease, so release() never calls Release() under an in-flight request.
		ApiLease lease(this);
		if (lease && m_pUserAPI != NULL)
			m_pUserAPI->ReqQryInvestorAccount(&req, genRequestID());
	});

	triggerQuery();

	return 0;
}

int TraderFemas::queryPositions()
{
	if(m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryInvestorPositionField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());
			ApiLease lease(this);
			if (lease && m_pUserAPI != NULL)
				m_pUserAPI->ReqQryInvestorPosition(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

int TraderFemas::queryOrders()
{
	if(m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryOrderField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());

			ApiLease lease(this);
			if (lease && m_pUserAPI != NULL)
				m_pUserAPI->ReqQryOrder(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

int TraderFemas::queryTrades()
{
	if(m_bStopped.load(std::memory_order_acquire) || m_pUserAPI == NULL || m_wrapperState != WS_ALLREADY)
	{
		return -1;
	}

	{
		StdUniqueLock lock(m_mtxQuery);
		m_queQuery.push([this](){
			CUstpFtdcQryTradeField req;
			memset(&req, 0, sizeof(req));
			strcpy(req.BrokerID, m_strBroker.c_str());
			strcpy(req.UserID, m_strUser.c_str());
			strcpy(req.InvestorID, m_strUser.c_str());

			ApiLease lease(this);
			if (lease && m_pUserAPI != NULL)
				m_pUserAPI->ReqQryTrade(&req, genRequestID());
		});
	}

	triggerQuery();

	return 0;
}

void TraderFemas::OnFrontConnected()
{
	FEMAS_CALLBACK_SCOPE();
	if(m_sink)
		m_sink->handleEvent(WTE_Connect, 0);
}

void TraderFemas::OnQryFrontConnected()
{
	FEMAS_CALLBACK_SCOPE();
	//m_bQryOnline = true;

	//if (m_wrapperState == WS_ALLREADY)
	//{
	//	if (m_sink)
	//		m_sink->onLoginResult(true, "", m_lDate);
	//}
}

void TraderFemas::OnFrontDisconnected( int nReason )
{
	FEMAS_CALLBACK_SCOPE();
	m_bQryOnline = false;
	m_wrapperState = WS_NOTLOGIN;
	if(m_sink)
		m_sink->handleEvent(WTE_Close, nReason);
}

void TraderFemas::OnQryFrontDisconnected(int /*nReason*/)
{
	FEMAS_CALLBACK_SCOPE();
	m_bQryOnline = false;
}

void TraderFemas::OnHeartBeatWarning( int /*nTimeLapse*/ )
{
	FEMAS_CALLBACK_SCOPE();
	write_log(m_sink,LL_DEBUG, "[TraderFemas][{}-{}] Heartbeating...", m_strBroker.c_str(), m_strUser.c_str());
}

void TraderFemas::OnRspQueryUserLogin(CUstpFtdcRspUserLoginField */*pRspUserLogin*/, CUstpFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	FEMAS_CALLBACK_SCOPE();
	m_bQryOnline = true;

	if (m_wrapperState == WS_LOGINED)
		onInitialized();
}

void TraderFemas::OnRspDSUserCertification(CUstpFtdcDSUserCertRspDataField */*pDSUserCertRspData*/, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	FEMAS_CALLBACK_SCOPE();
	if (pRspInfo->ErrorID == 0)
	{
		doLogin();
	}
	else
	{
		write_log(m_sink,LL_ERROR, "[TraderFemas][{}-{}] Authentiation failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if (m_sink)
			m_sink->onLoginResult(false, pRspInfo->ErrorMsg, 0);
	}
}

void TraderFemas::OnRspUserLogin( CUstpFtdcRspUserLoginField *pRspUserLogin, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	FEMAS_CALLBACK_SCOPE();
	if(!IsErrorRspInfo(pRspInfo))
	{
		m_wrapperState = WS_LOGINED;

		// 保存会话参数
		m_strSessionID = pRspUserLogin->MaxOrderLocalID;
		StrUtil::trim(m_strSessionID, "0", false, true);
		
		///获取当前交易日
		{
			ApiLease lease(this);
			if (!lease || m_pUserAPI == NULL)
				return;
			m_lDate = atoi(m_pUserAPI->GetTradingDay());
		}

		write_log(m_sink,LL_INFO,"[TraderFemas][{}-{}] Login succeed...", m_strBroker.c_str(), m_strUser.c_str());

		//据说飞马不支持结算,所以查不到结算单
		write_log(m_sink,LL_INFO, "[TraderFemas][{}-{}] Querying confirming state of settlement data...", m_strBroker.c_str(), m_strUser.c_str());
		if (m_bQryOnline)
			onInitialized();
	}
	else
	{
		write_log(m_sink,LL_ERROR,"[TraderFemas][{}-{}] Login failed: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg);
		m_wrapperState = WS_LOGINFAILED;

		if(m_sink)
			m_sink->onLoginResult( false, pRspInfo->ErrorMsg, 0);
	}
}

void TraderFemas::OnRspUserLogout(CUstpFtdcRspUserLogoutField */*pRspUserLogout*/, CUstpFtdcRspInfoField */*pRspInfo*/, int /*nRequestID*/, bool /*bIsLast*/)
{
	FEMAS_CALLBACK_SCOPE();
	m_wrapperState = WS_NOTLOGIN;
	if(m_sink)
		m_sink->handleEvent(WTE_Logout, 0);
}

void TraderFemas::OnRspOrderInsert( CUstpFtdcInputOrderField *pInputOrder, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/ )
{
	FEMAS_CALLBACK_SCOPE();
	WTSEntrust* entrust = makeEntrust(pInputOrder);
	if(entrust)
	{
		WTSError *err = makeError(pRspInfo);
		//g_orderMgr.onRspEntrust(entrust, err);
		if(m_sink)
			m_sink->onRspEntrust(entrust, err);
		entrust->release();

		if(err)
			err->release();
	}
}

void TraderFemas::OnRspOrderAction(CUstpFtdcOrderActionField */*pOrderAction*/, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	FEMAS_CALLBACK_SCOPE();
	if(!IsErrorRspInfo(pRspInfo))
	{

	}
	else
	{
		WTSError* error = WTSError::create(WEC_ORDERCANCEL, pRspInfo->ErrorMsg);
		if(m_sink)
			m_sink->onTraderError(error);
	}
}

void TraderFemas::OnRspQryInvestorAccount(CUstpFtdcRspInvestorAccountField *pRspInvestorAccount, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	FEMAS_CALLBACK_SCOPE();
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setPreBalance(pRspInvestorAccount->PreBalance);
		accountInfo->setCloseProfit(pRspInvestorAccount->CloseProfit);
		accountInfo->setDynProfit(pRspInvestorAccount->PositionProfit);
		accountInfo->setMargin(pRspInvestorAccount->Margin);
		accountInfo->setAvailable(pRspInvestorAccount->Available);
		accountInfo->setCommission(pRspInvestorAccount->Fee);
		accountInfo->setFrozenMargin(pRspInvestorAccount->FrozenMargin);
		accountInfo->setFrozenCommission(pRspInvestorAccount->FrozenFee);
		accountInfo->setDeposit(pRspInvestorAccount->Deposit);
		accountInfo->setWithdraw(pRspInvestorAccount->Withdraw);
		accountInfo->setBalance(accountInfo->getPreBalance() + accountInfo->getCloseProfit() - accountInfo->getCommission() + accountInfo->getDeposit() - accountInfo->getWithdraw());
		accountInfo->setCurrency("CNY");

		WTSArray * ay = WTSArray::create();
		ay->append(accountInfo, false);
		if(m_sink)
			m_sink->onRspAccount(ay);

		ay->release();
	}
}

void TraderFemas::OnRspQryInvestorPosition(CUstpFtdcRspInvestorPositionField *pRspInvestorPosition, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	FEMAS_CALLBACK_SCOPE();
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if (!IsErrorRspInfo(pRspInfo) && pRspInvestorPosition)
	{
		if(NULL == m_ayPosition)
			m_ayPosition = WTSArray::create();

		WTSContractInfo* contract = m_bdMgr->getContract(pRspInvestorPosition->InstrumentID, pRspInvestorPosition->ExchangeID);
		if (contract)
		{
			WTSCommodityInfo* commInfo = contract->getCommInfo();
			WTSPositionItem *pos = WTSPositionItem::create(pRspInvestorPosition->InstrumentID, commInfo->getCurrency(), commInfo->getExchg());
			pos->setContractInfo(contract);
			pos->setDirection(wrapPosDirection(pRspInvestorPosition->Direction));
			pos->setNewPosition(pRspInvestorPosition->Position - pRspInvestorPosition->YdPosition);
			pos->setPrePosition(pRspInvestorPosition->YdPosition);

			pos->setMargin(pRspInvestorPosition->UsedMargin);
			pos->setDynProfit(0.0);
			pos->setPositionCost(pRspInvestorPosition->PositionCost);

			if (pos->getTotalPosition() != 0)
			{
				pos->setAvgPrice(pos->getPositionCost() / pos->getTotalPosition() / commInfo->getVolScale());
			}
			else
			{
				pos->setAvgPrice(0);
			}

			if (commInfo->getCategoty() != CC_Combination)
			{
				int availNew = pRspInvestorPosition->Position - pRspInvestorPosition->YdPosition;
				availNew -= pRspInvestorPosition->FrozenPosition;

				if (availNew < 0)
					availNew = 0;
				pos->setAvailNewPos(availNew);

				double availPre = pos->getNewPosition() + pos->getPrePosition()
					- pRspInvestorPosition->FrozenPosition - pos->getAvailNewPos();
				pos->setAvailPrePos(availPre);
			}
			else
			{

			}

			//if (pos->getTotalPosition() > 0 && pos->getMargin() == 0)
			if (decimal::lt(pos->getTotalPosition(), 0.0) && decimal::eq(pos->getMargin(), 0.0))
			{
				//有仓位,但是保证金为0,则说明是套利合约,单个合约的可用持仓全部置为0
				pos->setAvailNewPos(0);
				pos->setAvailPrePos(0);
			}

			m_ayPosition->append(pos);
			pos->release();
		}
		else
		{
			// BUG-L01-014 (card 6): keep the unconverted query row (raw hex of the struct); the host pauses the account.
			wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderFemas", "position", "position query row could not be converted",
				wt_provider_raw::hexOf("CUstpFtdcRspInvestorPositionField", pRspInvestorPosition));
		}
	}
	
	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspPosition(m_ayPosition);

		if(m_ayPosition)
		{
			m_ayPosition->release();
			m_ayPosition = NULL;
		}
	}
}

void TraderFemas::OnRspQryTrade(CUstpFtdcTradeField *pTrade, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	FEMAS_CALLBACK_SCOPE();
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if(!IsErrorRspInfo(pRspInfo) && pTrade)
	{
		if(NULL == m_ayTrades)
			m_ayTrades = WTSArray::create();

		WTSTradeInfo* trade = makeTradeRecord(pTrade);
		if(trade)
		{
			m_ayTrades->append(trade, false);
		}
		else
		{
			// BUG-L01-014 (card 6): keep the unconverted query row (raw hex of the struct); the host pauses the account.
			wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderFemas", "trade", "trade query row could not be converted",
				wt_provider_raw::hexOf("CUstpFtdcTradeField", pTrade));
		}
	}

	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspTrades(m_ayTrades);

		if(NULL != m_ayTrades)
			m_ayTrades->clear();
	}
}

void TraderFemas::OnRspQryOrder(CUstpFtdcOrderField *pOrder, CUstpFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	FEMAS_CALLBACK_SCOPE();
	if (bIsLast)
	{
		m_bInQuery = false;
		triggerQuery();
	}

	if(!IsErrorRspInfo(pRspInfo) && pOrder)
	{
		if(NULL == m_ayOrders)
			m_ayOrders = WTSArray::create();

		WTSOrderInfo* orderInfo = makeOrderInfo(pOrder);
		if(orderInfo)
		{
			m_ayOrders->append(orderInfo, false);
		}
		else
		{
			// BUG-L01-014 (card 6): keep the unconverted query row (raw hex of the struct); the host pauses the account.
			wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderFemas", "order", "order query row could not be converted",
				wt_provider_raw::hexOf("CUstpFtdcOrderField", pOrder));
		}
	}

	if(bIsLast)
	{
		if(m_sink)
			m_sink->onRspOrders(m_ayOrders);

		if(m_ayOrders)
			m_ayOrders->clear();
	}
}

void TraderFemas::onInitialized()
{
	write_log(m_sink,LL_INFO, "[TraderFemas][{}-{}] Trading channel initialized...", m_strBroker.c_str(), m_strUser.c_str());
	m_wrapperState = WS_ALLREADY;
	if (m_sink)
		m_sink->onLoginResult(true, "", m_lDate);
}

void TraderFemas::OnRspError( CUstpFtdcRspInfoField *pRspInfo, int nRequestID, bool /*bIsLast*/ )
{
	FEMAS_CALLBACK_SCOPE();
	if (m_sink)
		write_log(m_sink,LL_ERROR, "[TraderFemas][{}-{}] Error occured: {}, request id: {}", m_strBroker.c_str(), m_strUser.c_str(), pRspInfo->ErrorMsg, nRequestID);
}

void TraderFemas::OnRtnOrder( CUstpFtdcOrderField *pOrder )
{
	FEMAS_CALLBACK_SCOPE();
	WTSOrderInfo *orderInfo = makeOrderInfo(pOrder);
	if(orderInfo)
	{
		if(m_sink)
			m_sink->onPushOrder(orderInfo);

		orderInfo->release();
	}
	else if (pOrder != NULL)
	{
		// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
		wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderFemas", "order", "order report could not be converted",
			wt_provider_raw::hexOf("CUstpFtdcOrderField", pOrder));
	}
}

void TraderFemas::OnRtnTrade( CUstpFtdcTradeField *pTrade )
{
	FEMAS_CALLBACK_SCOPE();
	WTSTradeInfo *tRecord = makeTradeRecord(pTrade);
	if(tRecord)
	{
		if(m_sink)
			m_sink->onPushTrade(tRecord);

		tRecord->release();
	}
	else if (pTrade != NULL)
	{
		// BUG-L01-014: keep the unconverted SDK event for reconciliation (raw hex of the struct).
		wt_provider_raw::report(m_sink, m_spiFeatures.load(), "TraderFemas", "trade", "trade report could not be converted",
			wt_provider_raw::hexOf("CUstpFtdcTradeField", pTrade));
	}
}

int TraderFemas::wrapDirectionType(WTSDirectionType dirType, WTSOffsetType offsetType)
{
	if( WDT_LONG == dirType)
		if(offsetType == WOT_OPEN)
			return USTP_FTDC_D_Buy;
		else
			return USTP_FTDC_D_Sell;
	else
		if(offsetType == WOT_OPEN)
			return USTP_FTDC_D_Sell;
		else
			return USTP_FTDC_D_Buy;
}

WTSDirectionType TraderFemas::wrapDirectionType(TUstpFtdcDirectionType dirType, TUstpFtdcOffsetFlagType offsetType)
{
	if( USTP_FTDC_D_Buy == dirType)
		if(offsetType == USTP_FTDC_OF_Open)
			return WDT_LONG;
		else
			return WDT_SHORT;
	else
		if(offsetType == USTP_FTDC_OF_Open)
			return WDT_SHORT;
		else
			return WDT_LONG;
}

WTSDirectionType TraderFemas::wrapPosDirection(TUstpFtdcDirectionType dirType)
{
	if (USTP_FTDC_D_Buy == dirType)
		return WDT_LONG;
	else
		return WDT_SHORT;
}

int TraderFemas::wrapOffsetType(WTSOffsetType offType)
{
	if( WOT_OPEN == offType)
		return USTP_FTDC_OF_Open;
	else if( WOT_CLOSE == offType)
		return USTP_FTDC_OF_Close;
	else if( WOT_CLOSETODAY == offType)
		return USTP_FTDC_OF_CloseToday;
	else if( WOT_CLOSEYESTERDAY == offType)
		return USTP_FTDC_OF_Close;
	else
		return USTP_FTDC_OF_ForceClose;
}

WTSOffsetType TraderFemas::wrapOffsetType(TUstpFtdcOffsetFlagType offType)
{
	if( USTP_FTDC_OF_Open == offType)
		return WOT_OPEN;
	else if( USTP_FTDC_OF_Close == offType)
		return WOT_CLOSE;
	else if( USTP_FTDC_OF_CloseToday == offType)
		return WOT_CLOSETODAY;
	else
		return WOT_FORCECLOSE;
}

int TraderFemas::wrapPriceType(WTSPriceType priceType, bool isCFFEX /* = false */)
{
	if(WPT_ANYPRICE == priceType)
		return isCFFEX ? USTP_FTDC_OPT_BestPrice : USTP_FTDC_OPT_AnyPrice;
	else if(WPT_LIMITPRICE == priceType)
		return USTP_FTDC_OPT_LimitPrice;
	else if(WPT_BESTPRICE == priceType)
		return USTP_FTDC_OPT_BestPrice;
	else
		return USTP_FTDC_OPT_BestPrice;
}

WTSPriceType TraderFemas::wrapPriceType(TUstpFtdcPriceType priceType)
{
	if (USTP_FTDC_OPT_AnyPrice == priceType || USTP_FTDC_OPT_BestPrice == priceType)
		return WPT_ANYPRICE;
	else if(USTP_FTDC_OPT_LimitPrice == priceType)
		return WPT_LIMITPRICE;
	else
		return WPT_LASTPRICE;
}

int TraderFemas::wrapTimeCondition(WTSTimeCondition timeCond)
{
	if(WTC_IOC == timeCond)
		return USTP_FTDC_TC_IOC;
	else if(WTC_GFD == timeCond)
		return USTP_FTDC_TC_GFD;
	else
		return USTP_FTDC_TC_GFS;
}

WTSTimeCondition TraderFemas::wrapTimeCondition(TUstpFtdcTimeConditionType timeCond)
{
	if(USTP_FTDC_TC_IOC == timeCond)
		return WTC_IOC;
	else if(USTP_FTDC_TC_GFD == timeCond)
		return WTC_GFD;
	else
		return WTC_GFS;
}

WTSOrderState TraderFemas::wrapOrderState(TUstpFtdcOrderStatusType orderState)
{
	if (orderState < USTP_FTDC_OS_AcceptedNoReply)
		return (WTSOrderState)orderState;
	else
		return WOS_Submitting;
}

int TraderFemas::wrapActionFlag(WTSActionFlag actionFlag)
{
	if(WAF_CANCEL == actionFlag)
		return USTP_FTDC_AF_Delete;
	else
		return USTP_FTDC_AF_Modify;
}


WTSOrderInfo* TraderFemas::makeOrderInfo(CUstpFtdcOrderField* orderField)
{
	WTSContractInfo* cInfo = m_bdMgr->getContract(orderField->InstrumentID, orderField->ExchangeID);
	if(cInfo == NULL)
		return NULL;

	WTSOrderInfo* pRet = WTSOrderInfo::create();
	pRet->setPrice(orderField->LimitPrice);
	pRet->setVolume(orderField->Volume);
	pRet->setDirection(wrapDirectionType(orderField->Direction, orderField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(orderField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(orderField->OffsetFlag));
	pRet->setContractInfo(cInfo);

	if (orderField->TimeCondition == USTP_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (orderField->TimeCondition == USTP_FTDC_TC_IOC)
	{
		if (orderField->VolumeCondition == USTP_FTDC_VC_AV || orderField->VolumeCondition == USTP_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	pRet->setVolTraded(orderField->VolumeTraded);
	pRet->setVolLeft(orderField->VolumeRemain);

	pRet->setCode(cInfo->getCode());
	pRet->setExchange(cInfo->getExchg());

	pRet->setOrderDate(convert::to_uint32(orderField->TradingDay));
	std::string strTime = orderField->InsertTime;
	StrUtil::replace(strTime, ":", "");
	[[maybe_unused]] uint32_t uTime = convert::to_uint32(strTime.c_str());
	pRet->setOrderTime(TimeUtils::makeTime(pRet->getOrderDate(), convert::to_uint32(strTime.c_str())*1000));
	pRet->setOrderState(wrapOrderState(orderField->OrderStatus));

	char userOrderLocalID[sizeof(orderField->UserOrderLocalID) + 1] = {};
	size_t userOrderLocalIDLength = 0;
	while (userOrderLocalIDLength < sizeof(orderField->UserOrderLocalID)
		&& orderField->UserOrderLocalID[userOrderLocalIDLength] != '\0')
		++userOrderLocalIDLength;
	memcpy(userOrderLocalID, orderField->UserOrderLocalID, userOrderLocalIDLength);

	bool userOrderLocalIDIsAllZero = true;
	for (size_t i = 0; i < userOrderLocalIDLength; ++i)
	{
		if (userOrderLocalID[i] != '0')
		{
			userOrderLocalIDIsAllZero = false;
			break;
		}
	}

	if (userOrderLocalIDLength == 0 || userOrderLocalIDIsAllZero)
	{
		char orderLocalID[sizeof(orderField->OrderLocalID) + 1] = {};
		size_t orderLocalIDLength = 0;
		while (orderLocalIDLength < sizeof(orderField->OrderLocalID)
			&& orderField->OrderLocalID[orderLocalIDLength] != '\0')
			++orderLocalIDLength;
		memcpy(orderLocalID, orderField->OrderLocalID, orderLocalIDLength);
		pRet->setEntrustID(orderLocalID);
	}
	else
	{
		pRet->setEntrustID(userOrderLocalID);
	}
	pRet->setOrderID(orderField->OrderSysID);

	//pRet->setStateMsg(orderField->StatusMsg);

	pRet->setUserTag(userOrderLocalID);

	return pRet;
}

WTSEntrust* TraderFemas::makeEntrust(CUstpFtdcInputOrderField *entrustField)
{
	WTSContractInfo* ct = m_bdMgr->getContract(entrustField->InstrumentID, entrustField->ExchangeID);
	if (ct == NULL)
		return NULL;

	WTSEntrust* pRet = WTSEntrust::create( 
		entrustField->InstrumentID, 
		entrustField->Volume, 
		entrustField->LimitPrice);

	pRet->setContractInfo(ct);

	pRet->setDirection(wrapDirectionType(entrustField->Direction, entrustField->OffsetFlag));
	pRet->setPriceType(wrapPriceType(entrustField->OrderPriceType));
	pRet->setOffsetType(wrapOffsetType(entrustField->OffsetFlag));
	
	if (entrustField->TimeCondition == USTP_FTDC_TC_GFD)
	{
		pRet->setOrderFlag(WOF_NOR);
	}
	else if (entrustField->TimeCondition == USTP_FTDC_TC_IOC)
	{
		if (entrustField->VolumeCondition == USTP_FTDC_VC_AV || entrustField->VolumeCondition == USTP_FTDC_VC_MV)
			pRet->setOrderFlag(WOF_FAK);
		else
			pRet->setOrderFlag(WOF_FOK);
	}

	pRet->setEntrustID(entrustField->UserOrderLocalID);
	pRet->setUserTag(entrustField->UserOrderLocalID);

	return pRet;
}

WTSError* TraderFemas::makeError(CUstpFtdcRspInfoField* rspInfo)
{
	if (rspInfo == NULL || rspInfo->ErrorID == 0)
		return NULL;

	WTSError* pRet = WTSError::create((WTSErroCode)rspInfo->ErrorID, rspInfo->ErrorMsg);
	return pRet;
}

WTSTradeInfo* TraderFemas::makeTradeRecord(CUstpFtdcTradeField *tradeField)
{
	WTSContractInfo* cInfo = m_bdMgr->getContract(tradeField->InstrumentID, tradeField->ExchangeID);
	if(cInfo == NULL)
		return NULL;

	WTSCommodityInfo* mInfo = cInfo->getCommInfo();

	WTSTradeInfo *pRet = WTSTradeInfo::create(cInfo->getCode(), cInfo->getExchg());
	pRet->setVolume(tradeField->TradeVolume);
	pRet->setPrice(tradeField->TradePrice);
	pRet->setTradeID(tradeField->TradeID);
	pRet->setContractInfo(cInfo);

	std::string strTime = tradeField->TradeTime;
	StrUtil::replace(strTime, ":", "");
	uint32_t uTime = convert::to_uint32(strTime.c_str());
	uint32_t uDate = convert::to_uint32(tradeField->TradingDay);
	//if(uDate == m_pContractMgr->getTradingDate())
	//{
	//	//如果当前日期和交易日一致,且时间大于21点,说明是夜盘,也就是实际日期要单独计算
	//	if (uTime / 10000 >= 21)
	//	{
	//		uDate = m_pMarketMgr->getPrevTDate(mInfo->getExchange(), uDate, 1);
	//	}
	//	else if(uTime <= 3)
	//	{
	//		//如果在3点以内,就要先获取上一个交易日,再获取下一个自然日
	//		//这样做的目的是,遇到周五晚上的情况,可以处理过来
	//		uDate = m_pMarketMgr->getPrevTDate(mInfo->getExchange(), uDate, 1);
	//		uDate = TimeUtils::getNextDate(uDate);
	//	}
	//}

	pRet->setTradeDate(uDate);
	pRet->setTradeTime(TimeUtils::makeTime(uDate, uTime*1000));

	WTSDirectionType dType = wrapDirectionType(tradeField->Direction, tradeField->OffsetFlag);

	pRet->setDirection(dType);
	pRet->setOffsetType(wrapOffsetType(tradeField->OffsetFlag));
	pRet->setRefOrder(tradeField->OrderSysID);
	pRet->setTradeType(WTT_Common);

	double amount = mInfo->getVolScale()*tradeField->TradeVolume*pRet->getPrice();
	pRet->setAmount(amount);
	
	StringMap::iterator it = m_mapOrderTag.find(pRet->getRefOrder());
	if(it != m_mapOrderTag.end())
	{
		pRet->setUserTag(it->second.c_str());
	}

	return pRet;
}

bool TraderFemas::IsErrorRspInfo(CUstpFtdcRspInfoField *pRspInfo)
{
	if(pRspInfo && pRspInfo->ErrorID!=0)
		return true;

	return false;
}

void TraderFemas::OnErrRtnOrderInsert(CUstpFtdcInputOrderField */*pInputOrder*/, CUstpFtdcRspInfoField */*pRspInfo*/)
{
	FEMAS_CALLBACK_SCOPE();

}

void TraderFemas::OnErrRtnOrderAction(CUstpFtdcOrderActionField */*pOrderAction*/, CUstpFtdcRspInfoField */*pRspInfo*/)
{
	FEMAS_CALLBACK_SCOPE();

}

void TraderFemas::OnRtnInstrumentStatus(CUstpFtdcInstrumentStatusField *pInstrumentStatus)
{
	FEMAS_CALLBACK_SCOPE();
	if (m_sink)
		m_sink->onPushInstrumentStatus(pInstrumentStatus->ExchangeID, pInstrumentStatus->InstrumentID, (WTSTradeStatus)pInstrumentStatus->InstrumentStatus);
}

bool TraderFemas::isConnected()
{
	return (m_wrapperState == WS_ALLREADY);
}

const char* TraderFemas::wrapExchg(const char* exchg)
{
	if (strcmp(exchg, "CZCE") == 0)
		return "ZCE";
	else
		return exchg;
}

void TraderFemas::triggerQuery()
{
	// BUG-T4-003: nothing is scheduled once release() was requested; a query
	// queued before connect() published the strand is picked up by a later trigger.
	if (m_strandIO == NULL || m_bStopped.load(std::memory_order_acquire))
		return;
	boost::asio::post(*m_strandIO, [this](){
		if (m_bStopped.load(std::memory_order_acquire) || m_queQuery.empty() || m_bInQuery)
			return;

		uint64_t curTime = TimeUtils::getLocalTimeNow();
		if (curTime - m_lastQryTime < 1000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			boost::asio::post(*m_strandIO, [this](){
				triggerQuery();
			});
			return;
		}


		m_bInQuery = true;
		CommonExecuter& handler = m_queQuery.front();
		handler();

		{
			StdUniqueLock lock(m_mtxQuery);
			m_queQuery.pop();
		}

		m_lastQryTime = TimeUtils::getLocalTimeNow();
	});
}
