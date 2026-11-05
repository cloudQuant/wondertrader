/*!
 * \file ParserXTP.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "ParserXTP.h"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/ModuleHelper.hpp"
#include "../Share/Converter.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <vector>
namespace fs = std::filesystem;

 //By Wesley @ 2022.01.05
#include "../Share/fmtlib.h"
template<typename... Args>
inline void write_log(IParserSpi* sink, WTSLogLevel ll, const char* format, const Args&... args)
{
	if (sink == NULL)
		return;

	static thread_local char buffer[512] = { 0 };
	memset(buffer, 0, 512);
	fmt::format_to(buffer, format, args...);

	sink->handleParserLog(ll, buffer);
}

extern "C"
{
	EXPORT_FLAG IParserApi* createParser()
	{
		try { return new ParserXTP(); } catch (...) { return NULL; }
	}

	/*
	 *	卡9 9b：仅当没有回调/接口重入、且本实例从未创建过 SDK 对象时才删除；
	 *	否则保留指针（不置空），由宿主隔离到进程退出。
	 */
	EXPORT_FLAG void deleteParser(IParserApi* &parser)
	{
		if (NULL != parser)
		{
			ParserXTP* typed = dynamic_cast<ParserXTP*>(parser);
			if (typed == NULL || !typed->removableLegacy())
				return;
			delete parser;
			parser = NULL;
		}
	}

	WT_PARSER_LIFECYCLE_V2_EXPORT const WtParserLifecycleApiV2* WT_PARSER_LIFECYCLE_V2_CALL getParserLifecycleApiV2(uint32_t major)
	{
		return wt_vendor_md_v2::Table<ParserXTP>::get(major);
	}
};

namespace
{
	// 当前线程是哪个 ParserXTP 的 worker（connect 创建的 io_context 线程）
	thread_local const void* t_xtp_worker = nullptr;
}

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

inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}

ParserXTP::ParserXTP()
	:m_uTradingDate(0)
,m_pUserAPI(NULL)
	,m_iRequestID(0)
	,m_sink(NULL)
	,m_pBaseDataMgr(NULL)
	,m_hInst(NULL)
	,m_funcCreator(NULL)
	,_worker_stopping(false)
	{
}


ParserXTP::~ParserXTP()
{
	// 在自身回调或接口调用内直接析构无法做到安全（栈上仍有本对象帧）：fail-stop。
	if (wt_vendor_md_v2::Lifecycle::reentered(this))
		std::terminate();
	(void)shutdownLegacy();
}

/*
 *	Parser lifecycle V2（卡9 9a=A+C）
 *	XTP 头文件 API/XTP2.2.32.2/xtp_quote_api.h:234-236 对 Release() 只写「删除接口对象本身」，
 *	RegisterSpi（:258-260）只要求登录前设定；Logout（:390-393）为同步阻塞但未声明之后不再回调；
 *	头文件未声明回调线程 join 或 Release 返回后不再回调。provider 自有 worker 线程可 join，
 *	SDK 回调线程不可证明。
 *	因此一旦创建过 SDK 对象，排空状态不可证明：wait_drained 释放一次 SDK 后返回 ERROR，宿主隔离到进程退出。
 */
WtParserLifecycleV2Status ParserXTP::requestStopV2() noexcept
{
	return _lc.requestStop();
}

WtParserLifecycleV2Status ParserXTP::canWaitV2() const noexcept
{
	return (wt_vendor_md_v2::Lifecycle::reentered(this) || t_xtp_worker == this) ? WT_PARSER_LIFECYCLE_V2_WOULD_DEADLOCK : WT_PARSER_LIFECYCLE_V2_OK;
}

WtParserLifecycleV2Status ParserXTP::waitDrainedV2(uint32_t timeoutMs) noexcept
{
	return _lc.waitDrained(canWaitV2(), timeoutMs,
		[this]() { return sdkSeen(); },
		[this]() { releaseSdk(); },
		[this]() {
			std::lock_guard<std::mutex> lock(_sdk_mutex);
			m_sink = NULL;
			m_pBaseDataMgr = NULL;
		});
}

bool ParserXTP::destroyableV2() const noexcept
{
	return _lc.destroyable(canWaitV2()) && !sdkSeen();
}

bool ParserXTP::removableLegacy() noexcept
{
	return shutdownLegacy() && !sdkSeen();
}

bool ParserXTP::sdkSeen() const noexcept
{
	return _lc.sdk_created.load(std::memory_order_acquire) || m_pUserAPI != NULL;
}

bool ParserXTP::shutdownLegacy() noexcept
{
	return _lc.shutdownLegacy(this, t_xtp_worker == this, [this]() { releaseSdk(); });
}

void ParserXTP::releaseSdk() noexcept
{
	// 调用方保证：两道闸门已关闭且已排空，当前线程不在本实例回调/接口帧内
	std::lock_guard<std::mutex> lock(_sdk_mutex);
	(void)stopWorker();	// provider 自有 worker：调用线程已确认不是 worker，可 join
	XTP::API::QuoteApi* api = m_pUserAPI;
	m_pUserAPI = NULL;
	if (api == NULL)
		return;
	// 曾存在 SDK 对象：不卸载厂商库（无回调线程退出确认，卡9 9a）
	_lc.sdk_created.store(true, std::memory_order_release);
	try
	{
		api->RegisterSpi(NULL);
		api->Release();
	}
	catch (...)
	{
	}
}

bool ParserXTP::init(WTSVariant* config)
{
	auto call = _lc.call(this);
	// 卡9 9c：重复初始化拒绝并保留原实例；停止后、回调/worker 内或无配置均拒绝
	if (!call || config == NULL || _init_done || sdkSeen() || _lc.shutDown() || _lc.insideCallback(this) || t_xtp_worker == this)
		return false;

	if (m_hInst != NULL || m_pUserAPI != NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Init rejected while an XTP quote API is already loaded");
		return false;
	}

	m_strHost	= config->getCString("host");
	m_iPort		= config->getInt32("port");
	m_strUser = config->getCString("user");
	m_strPass = config->getCString("pass");
	m_iProtocol = (XTP_PROTOCOL_TYPE)config->getUInt32("protocol");
	m_uClientID = config->getUInt32("clientid");
	m_uHBInterval = config->getUInt32("hbinterval");
	m_uBuffSize = config->getUInt32("buffsize");
	m_strFlowDir = config->getCString("flowdir");
	m_strLocalIP = config->getCString("local_ip");

	if (m_strFlowDir.empty())
		m_strFlowDir = "XTPMDFlow";

	m_strFlowDir = StrUtil::standardisePath(m_strFlowDir);

	std::string module = config->getCString("xtpmodule");
	if (module.empty())
		module = "xtpquoteapi";

	const std::string moduleFile = DLLHelper::wrap_module(module.c_str(), "lib");
	const char* moduleDir = getBinDir();
	if ((moduleDir == nullptr || moduleDir[0] == '\0') && !fs::path(moduleFile).is_absolute())
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Cannot locate the parser module directory");
		return false;
	}

	std::string path = fmtutil::format("{}/{}/", m_strFlowDir.c_str(), m_strUser.c_str());
	fs::create_directories(path.c_str());

	std::string dllpath = (moduleDir ? std::string(moduleDir) : std::string()) + moduleFile;
	m_hInst = DLLHelper::load_library(dllpath.c_str());
	if (m_hInst == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Failed to load XTP quote API module: {}", dllpath.c_str());
		m_funcCreator = NULL;
		m_pUserAPI = NULL;
		return false;
	}
#ifdef _WIN32
#	ifdef _WIN64
	const char* creatorName = "?CreateQuoteApi@QuoteApi@API@XTP@@SAPEAV123@EPEBDW4XTP_LOG_LEVEL@@@Z";
#	else
	const char* creatorName = "?CreateQuoteApi@QuoteApi@API@XTP@@SAPAV123@EPBDW4XTP_LOG_LEVEL@@@Z";
#	endif
#else
	const char* creatorName = "_ZN3XTP3API8QuoteApi14CreateQuoteApiEhPKc13XTP_LOG_LEVEL";
#endif
	m_funcCreator = (XTPCreater)DLLHelper::get_symbol(m_hInst, creatorName);
	if (m_funcCreator == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] XTP quote API creator symbol was not found: {}", creatorName);
		DLLHelper::free_library(m_hInst);
		m_hInst = NULL;
		m_pUserAPI = NULL;
		return false;
	}

	m_pUserAPI = m_funcCreator(m_uClientID, path.c_str(), XTP_LOG_LEVEL_DEBUG);
	if (m_pUserAPI != NULL)
		_lc.sdk_created.store(true, std::memory_order_release);	// 本实例的排空从此不可证明
	if (m_pUserAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] XTP quote API creator returned null: {}", module.c_str());
		m_funcCreator = NULL;
		DLLHelper::free_library(m_hInst);
		m_hInst = NULL;
		return false;
	}

	m_pUserAPI->RegisterSpi(this);
	_init_done = true;

	return true;
}

void ParserXTP::release()
{
	(void)shutdownLegacy();
}

bool ParserXTP::connect()
{
	auto call = _lc.call(this);
	// 卡9 9d：SDK 回调或 worker 内同步重入 connect 返回 busy(false)
	if (!call || _lc.shutDown() || _lc.insideCallback(this) || t_xtp_worker == this)
		return false;

	if (m_pUserAPI == NULL)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Connect rejected because the XTP quote API is not initialized");
		return false;
	}

	if (_worker_stopping.load())
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Connect rejected after worker shutdown");
		return false;
	}

	DoLogin();

	if (_thrd_worker == NULL)
	{
		_worker.reset(new boost::asio::executor_work_guard<boost::asio::io_context::executor_type>(boost::asio::make_work_guard(_asyncio)));
		_thrd_worker.reset(new StdThread([this]() {
			t_xtp_worker = this;
			while (!_worker_stopping.load())
			{
				_asyncio.run_one();
			}
			t_xtp_worker = nullptr;
		}));
	}

	return true;
}

bool ParserXTP::disconnect()
{
	// 终止性关闭：SDK 回调、worker 或接口重入时只关闭准入并延后（返回 false，卡9 9d）；
	// 否则等待已准入回调/接口调用离开，join worker，再注销并释放 SDK
	return shutdownLegacy();
}

bool ParserXTP::stopWorker()
{
	_worker_stopping.store(true);
	_worker.reset();
	_asyncio.stop();

	if (_thrd_worker != NULL)
	{
		if (_thrd_worker->joinable())
		{
			if (_thrd_worker->get_id() == std::this_thread::get_id())
			{
				write_log(m_sink, LL_ERROR, "[ParserXTP] Cannot join the parser worker from itself; shutdown must be completed by its owner thread");
				return false;
			}
			_thrd_worker->join();
		}
		_thrd_worker.reset();
	}

	return true;
}

void ParserXTP::OnError(XTPRI *error_info)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(IsErrorRspInfo(error_info))
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Error occured: ({}){}", error_info->error_id, error_info->error_msg);
	}
}

void ParserXTP::OnDisconnected(int nReason)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_sink)
	{
		write_log(m_sink, LL_ERROR, "[ParserXTP] Market data server disconnected: {}...", nReason);
		m_sink->handleEvent(WPE_Close, 0);
	}

	boost::asio::post(_asyncio, [this]() {
		{
			auto task = _lc.callback(this);
			if (!task)
				return;
			write_log(m_sink, LL_WARN, "[ParserXTP] Connection lost, relogin in 2 seconds...");
		}
		// 可中断等待：关闭后 10ms 内放弃重登，不在等待期间占用回调准入（卡9）
		for (int i = 0; i < 200 && !_lc.shutDown(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		auto task = _lc.callback(this);
		if (!task)
			return;
		DoLogin();
	});
}

void ParserXTP::OnUnSubMarketData(XTPST */*ticker*/, XTPRI */*error_info*/, bool /*is_last*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;

}

namespace
{
	WTSBSDirectType mapEntrustSide(XTP_EXCHANGE_TYPE exchange, char side)
	{
		if (exchange == XTP_EXCHANGE_SH)
		{
			switch (side)
			{
			case 'B': return BDT_Buy;
			case 'S': return BDT_Sell;
			default: return BDT_Unknown;
			}
		}

		if (exchange == XTP_EXCHANGE_SZ)
		{
			switch (side)
			{
			case '1': return BDT_Buy;
			case '2': return BDT_Sell;
			case 'G': return BDT_Borrow;
			case 'F': return BDT_Lend;
			default: return BDT_Unknown;
			}
		}

		return BDT_Unknown;
	}

	WTSOrdDetailType mapEntrustType(XTP_EXCHANGE_TYPE exchange, char orderType)
	{
		if (exchange == XTP_EXCHANGE_SH)
		{
			switch (orderType)
			{
			case 'A': return ODT_Append;
			case 'D': return ODT_Delete;
			default: return ODT_Unknown;
			}
		}

		if (exchange == XTP_EXCHANGE_SZ)
		{
			switch (orderType)
			{
			case '1': return ODT_AnyPrice;
			case '2': return ODT_LimitPrice;
			case 'U': return ODT_BestPrice;
			default: return ODT_Unknown;
			}
		}

		return ODT_Unknown;
	}

	WTSBSDirectType mapTradeSide(XTP_EXCHANGE_TYPE exchange, char tradeFlag)
	{
		if (exchange == XTP_EXCHANGE_SH)
		{
			switch (tradeFlag)
			{
			case 'B': return BDT_Buy;
			case 'S': return BDT_Sell;
			case 'N': return BDT_Unknown;
			default: return BDT_Unknown;
			}
		}

		/* SZ trade_flag describes match/cancel only; it carries no direction. */
		return BDT_Unknown;
	}

	WTSTransType mapTradeType(XTP_EXCHANGE_TYPE exchange, char tradeFlag)
	{
		if (exchange == XTP_EXCHANGE_SH)
		{
			switch (tradeFlag)
			{
			case 'B':
			case 'S':
			case 'N': return TT_Match;
			default: return TT_Unknown;
			}
		}

		if (exchange == XTP_EXCHANGE_SZ)
		{
			switch (tradeFlag)
			{
			case 'F': return TT_Match;
			case '4': return TT_Cancel;
			default: return TT_Unknown;
			}
		}

		return TT_Unknown;
	}
}

void ParserXTP::OnTickByTick(XTPTBT *tbt_data)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if (m_pBaseDataMgr == NULL)
	{
		return;
	}

	std::string exchg;
	if (tbt_data->exchange_id == XTP_EXCHANGE_SH)
	{
		exchg = "SSE";
	}
	else
	{
		exchg = "SZSE";
	}

	WTSContractInfo* ct = m_pBaseDataMgr->getContract(tbt_data->ticker, exchg.c_str());
	if (ct == NULL)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Instrument {}.{} not exists...", exchg.c_str(), tbt_data->ticker);
		return;
	}
	WTSCommodityInfo* commInfo = ct->getCommInfo();

	uint32_t actDate = (uint32_t)(tbt_data->data_time / 1000000000);
	uint32_t actTime = tbt_data->data_time % 1000000000;
	[[maybe_unused]] uint32_t actHour = actTime / 10000000;

	if(tbt_data->type == XTP_TBT_ENTRUST)
	{
		WTSOrdDtlData *ordDtl = WTSOrdDtlData::create(tbt_data->ticker);
		ordDtl->setContractInfo(ct);
		WTSOrdDtlStruct& ts = ordDtl->getOrdDtlStruct();
		strcpy(ts.exchg, commInfo->getExchg());

		const XTPTickByTickEntrust& eInfo = tbt_data->entrust;

		ts.trading_date = actDate;
		ts.action_date = actDate;
		ts.action_time = actTime;

		ts.index = eInfo.seq;
		ts.side = mapEntrustSide(tbt_data->exchange_id, eInfo.side);
		ts.otype = mapEntrustType(tbt_data->exchange_id, eInfo.ord_type);

		ts.price = eInfo.price;
		ts.volume = (uint32_t)eInfo.qty;

		if (m_sink)
			m_sink->handleOrderDetail(ordDtl);
		ordDtl->release();
	}
	else if (tbt_data->type == XTP_TBT_TRADE)
	{
		WTSTransData *trans = WTSTransData::create(tbt_data->ticker);
		trans->setContractInfo(ct);
		WTSTransStruct& ts = trans->getTransStruct();
		strcpy(ts.exchg, commInfo->getExchg());

		const XTPTickByTickTrade& tInfo = tbt_data->trade;

		ts.trading_date = actDate;
		ts.action_date = actDate;
		ts.action_time = actTime;

		ts.index = tInfo.seq;
		ts.side = mapTradeSide(tbt_data->exchange_id, tInfo.trade_flag);
		ts.ttype = mapTradeType(tbt_data->exchange_id, tInfo.trade_flag);

		ts.price = tInfo.price;
		ts.volume = (uint32_t)tInfo.qty;
		ts.bidorder = tInfo.bid_no;
		ts.askorder = tInfo.ask_no;

		if (m_sink)
			m_sink->handleTransaction(trans);
		trans->release();
	}
}

void ParserXTP::OnDepthMarketData(XTPMD *market_data, int64_t bid1_qty[], int32_t bid1_count, int32_t max_bid1_count, int64_t ask1_qty[], int32_t ask1_count, int32_t max_ask1_count)
{	
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if(m_pBaseDataMgr == NULL)
	{
		return;
	}

	uint32_t actDate = (uint32_t)(market_data->data_time / 1000000000);
	uint32_t actTime = market_data->data_time % 1000000000;
	[[maybe_unused]] uint32_t actHour = actTime / 10000000;

	std::string code, exchg;
	if (market_data->exchange_id == XTP_EXCHANGE_SH)
	{
		exchg = "SSE";
	}
	else
	{
		exchg = "SZSE";
	}
	code = market_data->ticker;

	WTSContractInfo* ct = m_pBaseDataMgr->getContract(code.c_str(), exchg.c_str());
	if(ct == NULL)
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Instrument {}.{} not exists...", exchg.c_str(), market_data->ticker);
		return;
	}
	WTSCommodityInfo* commInfo = ct->getCommInfo();

	WTSTickData* tick = WTSTickData::create(code.c_str());
	if (tick == NULL)
		return;
	tick->setContractInfo(ct);
	WTSTickStruct& quote = tick->getTickStruct();
	strcpy(quote.exchg, commInfo->getExchg());
	
	quote.action_date = actDate;
	quote.action_time = actTime;
	
	quote.price = checkValid(market_data->last_price);
	quote.open = checkValid(market_data->open_price);
	quote.high = checkValid(market_data->high_price);
	quote.low = checkValid(market_data->low_price);
	quote.total_volume = (uint32_t)market_data->qty;
	quote.trading_date = m_uTradingDate;
	quote.total_turnover = market_data->turnover;

	if (commInfo->getCategoty() == CC_Future)
	{
		quote.settle_price = market_data->settl_price;
		quote.open_interest = (uint32_t)market_data->total_long_positon;

		quote.pre_settle = checkValid(market_data->pre_settl_price);
		quote.pre_interest = (uint32_t)market_data->pre_total_long_positon;
	}

	quote.upper_limit = checkValid(market_data->upper_limit_price);
	quote.lower_limit = checkValid(market_data->lower_limit_price);

	quote.pre_close = checkValid(market_data->pre_close_price);	

	//委卖价格
	for (int i = 0; i < 10; i++)
	{
		quote.ask_prices[i] = checkValid(market_data->ask[i]);
		quote.ask_qty[i] = (uint32_t)market_data->ask_qty[i];

		quote.bid_prices[i] = checkValid(market_data->bid[i]);
		quote.bid_qty[i] = (uint32_t)market_data->bid_qty[i];
	}

	if(m_sink)
		m_sink->handleQuote(tick, 1);

	//处理逐笔
	if(bid1_count > 0)
	{
		WTSOrdQueData* buyQue = WTSOrdQueData::create(code.c_str());
		buyQue->setContractInfo(ct);

		WTSOrdQueStruct& buyOS = buyQue->getOrdQueStruct();
		strcpy(buyOS.exchg, commInfo->getExchg());

		buyOS.trading_date = m_uTradingDate;
		buyOS.action_date = actDate;
		buyOS.action_time = actTime;

		buyOS.side = BDT_Buy;
		buyOS.price = quote.bid_prices[0];
		buyOS.order_items = max_bid1_count;
		buyOS.qsize = std::min(bid1_count,50);

		for (uint32_t i = 0; i < buyOS.qsize; i++)
		{
			buyOS.volumes[i] = (uint32_t)bid1_qty[i];
		}

		if (m_sink)
			m_sink->handleOrderQueue(buyQue);

		buyQue->release();
	}
	
	if(ask1_count > 0)
	{
		WTSOrdQueData* sellQue = WTSOrdQueData::create(code.c_str());
		sellQue->setContractInfo(ct);

		WTSOrdQueStruct& sellOS = sellQue->getOrdQueStruct();
		strcpy(sellOS.exchg, commInfo->getExchg());

		sellOS.trading_date = m_uTradingDate;
		sellOS.action_date = actDate;
		sellOS.action_time = actTime;

		sellOS.side = BDT_Sell;
		sellOS.price = quote.ask_prices[0];
		sellOS.order_items = max_ask1_count;
		sellOS.qsize = std::min(ask1_count, 50);

		for (uint32_t i = 0; i < sellOS.qsize; i++)
		{
			sellOS.volumes[i] = (uint32_t)ask1_qty[i];
		}

		if (m_sink)	
			m_sink->handleOrderQueue(sellQue);

		sellQue->release();
	}

	tick->release();
}

void ParserXTP::OnSubMarketData(XTPST *ticker, XTPRI *error_info, bool /*is_last*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if (!IsErrorRspInfo(error_info))
	{

	}
	else
	{
		if(m_sink)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Market data subscribe failed, code: {}.{}, err code: {}, err msg: {}", ticker->exchange_id == XTP_EXCHANGE_SH ? "SSE" : "SZSE", ticker->ticker, error_info->error_id, error_info->error_msg);
	}
}

void ParserXTP::OnSubTickByTick(XTPST *ticker, XTPRI *error_info, bool /*is_last*/)
{
	auto cb = _lc.callback(this);
	if (!cb)
		return;
	if (!IsErrorRspInfo(error_info))
	{

	}
	else
	{
		if (m_sink)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Tick-by-tick data subscribe failed, code: {}.{}, err code: {}, err msg: {}", ticker->exchange_id == XTP_EXCHANGE_SH ? "SSE" : "SZSE", ticker->ticker, error_info->error_id, error_info->error_msg);
	}
}

void ParserXTP::DoLogin()
{
	if(m_pUserAPI == NULL)
	{
		return;
	}

	m_pUserAPI->SetHeartBeatInterval(m_uHBInterval);
	m_pUserAPI->SetUDPBufferSize(m_uBuffSize);
	int iResult = m_pUserAPI->Login(m_strHost.c_str(), m_iPort, m_strUser.c_str(), m_strPass.c_str(), m_iProtocol, m_strLocalIP.c_str());
	if(iResult != 0)
	{
		if (m_sink)
		{
			auto error_info = m_pUserAPI->GetApiLastError();
			if(iResult == -1)
			{
				boost::asio::post(_asyncio, [this, iResult] {
					auto task = _lc.callback(this);
					if (task && m_sink)
						m_sink->handleEvent(WPE_Connect, iResult);
				});

				write_log(m_sink, LL_ERROR, "[ParserXTP] Connecting server failed: {}", error_info->error_msg);
			}
			else
			{
				m_sink->handleEvent(WPE_Connect, 0);
				boost::asio::post(_asyncio, [this] {
					auto task = _lc.callback(this);
					if (task && m_sink)
						m_sink->handleEvent(WPE_Connect, 0);
				});

				write_log(m_sink, LL_ERROR, "[ParserXTP] Sending login request failed: {}", error_info->error_msg);
			}			
		}
	}
	else
	{
		{
			std::lock_guard<std::recursive_mutex> lock(_data_mutex);
			m_uTradingDate = strToTime(m_pUserAPI->GetTradingDay());
		}
		boost::asio::post(_asyncio, [this] {
			auto task = _lc.callback(this);
			if (task && m_sink)
			{
				m_sink->handleEvent(WPE_Connect, 0);
				m_sink->handleEvent(WPE_Login, 0);
			}
		});
		

		write_log(m_sink, LL_INFO, "[ParserXTP] Connecting server successed: {}, begin to subscibe data ...", iResult);

		DoSubscribeMD();
	}
}

void ParserXTP::DoSubscribeMD()
{
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
	CodeSet codeFilter = m_fitSHSubs;
	if(!codeFilter.empty())
	{
		char ** subscribe = new char*[codeFilter.size()];
		int nCount = 0;
		CodeSet::iterator it = codeFilter.begin();
		for (; it != codeFilter.end(); it++)
		{
			subscribe[nCount++] = (char*)(*it).c_str();
		}

		if (m_pUserAPI && nCount > 0)
		{
			int iResult = m_pUserAPI->SubscribeMarketData(subscribe, nCount, XTP_EXCHANGE_SH);
			if (iResult != 0)
			{
				if (m_sink)
					write_log(m_sink, LL_ERROR, "[ParserXTP] Sending md subscribe request of SSE failed: {}", iResult);
			}
			else
			{
				if (m_sink)
					write_log(m_sink, LL_INFO, "[ParserXTP] Market data of {} instruments of SSE subscribed", nCount);
			}

			iResult = m_pUserAPI->SubscribeTickByTick(subscribe, nCount, XTP_EXCHANGE_SH);
			if (iResult != 0)
			{
				if (m_sink)
					write_log(m_sink, LL_ERROR, "[ParserXTP] Sending tick_by_tick subscribe request of SSE failed: {}", iResult);
			}
			else
			{
				if (m_sink)
					write_log(m_sink, LL_INFO, "[ParserXTP] Tick_by_tick data of {} instruments of SSE subscribed", nCount);
			}
		}
		codeFilter.clear();
		delete[] subscribe;
		//int iResult = m_pUserAPI->SubscribeAllMarketData(XTP_EXCHANGE_SH);
	}

	codeFilter = m_fitSZSubs;
	if (!codeFilter.empty())
	{
		char ** subscribe = new char*[codeFilter.size()];
		int nCount = 0;
		CodeSet::iterator it = codeFilter.begin();
		for (; it != codeFilter.end(); it++)
		{
			subscribe[nCount++] = (char*)(*it).c_str();
		}

		if (m_pUserAPI && nCount > 0)
		{
			int iResult = m_pUserAPI->SubscribeMarketData(subscribe, nCount, XTP_EXCHANGE_SZ);
			if (iResult != 0)
			{
				if (m_sink)
					write_log(m_sink, LL_ERROR, "[ParserXTP] Sending md subscribe request of SZSE failed: {}", iResult);
			}
			else
			{
				if (m_sink)
					write_log(m_sink, LL_INFO, "[ParserXTP] Market data of {} instruments of SZSE subscribed", nCount);
			}

			iResult = m_pUserAPI->SubscribeTickByTick(subscribe, nCount, XTP_EXCHANGE_SZ);
			if (iResult != 0)
			{
				if (m_sink)
					write_log(m_sink, LL_ERROR, "[ParserXTP] Sending tick_by_tick subscribe request of SZSE failed: {}", iResult);
			}
			else
			{
				if (m_sink)
					write_log(m_sink, LL_INFO, "[ParserXTP] Tick_by_tick data of {} instruments of SZSE subscribed", nCount);
			}
		}
		codeFilter.clear();
		delete[] subscribe;
		//int iResult = m_pUserAPI->SubscribeAllMarketData(XTP_EXCHANGE_SZ);
	}
}

bool ParserXTP::IsErrorRspInfo(XTPRI *error_info)
{
	if (error_info == NULL || error_info->error_id ==0)
		return false;

	return true;
}

void ParserXTP::subscribe(const CodeSet &vecSymbols)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
	if(m_uTradingDate == 0)
	{
		for(auto& code : vecSymbols)
		{
			if (strncmp(code.c_str(), "SSE.", 4) == 0)
			{
				m_fitSHSubs.insert(code.c_str() + 4);
			}
			else if (strncmp(code.c_str(), "SZSE.", 5) == 0)
			{
				m_fitSZSubs.insert(code.c_str() + 5);
			}
		}
	}
	else
	{
		CodeSet setSH, setSZ;
		for (auto& code : vecSymbols)
		{
			if (strncmp(code.c_str(), "SSE.", 4) == 0)
			{
				m_fitSHSubs.insert(code.c_str() + 4);
				setSH.insert(code.c_str() + 4);
			}
			else if (strncmp(code.c_str(), "SZSE.", 5) == 0)
			{
				m_fitSZSubs.insert(code.c_str() + 5);
				setSZ.insert(code.c_str() + 5);
			}
		}

		if (!setSH.empty())
		{
			char ** subscribe = new char*[setSH.size()];
			int nCount = 0;
			CodeSet::iterator it = setSH.begin();
			for (; it != setSH.end(); it++)
			{
				subscribe[nCount++] = (char*)(*it).c_str();
			}

			if (m_pUserAPI && nCount > 0)
			{
				int iResult = m_pUserAPI->SubscribeMarketData(subscribe, nCount, XTP_EXCHANGE_SH);
				if (iResult != 0)
				{
					if (m_sink)
						write_log(m_sink, LL_ERROR, "[ParserXTP] Sending md subscribe request of SSE failed: {}", iResult);
				}
				else
				{
					if (m_sink)
						write_log(m_sink, LL_INFO, "[ParserXTP] Market data of {} instruments of SSE subscribed", nCount);
				}

				iResult = m_pUserAPI->SubscribeTickByTick(subscribe, nCount, XTP_EXCHANGE_SH);
				if (iResult != 0)
				{
					if (m_sink)
						write_log(m_sink, LL_ERROR, "[ParserXTP] Sending tick_by_tick subscribe request of SSE failed: {}", iResult);
				}
				else
				{
					if (m_sink)
						write_log(m_sink, LL_INFO, "[ParserXTP] Tick_by_tick data of {} instruments of SSE subscribed", nCount);
				}
			}
			delete[] subscribe;
		}

		if (!setSZ.empty())
		{
			char ** subscribe = new char*[setSZ.size()];
			int nCount = 0;
			CodeSet::iterator it = setSZ.begin();
			for (; it != setSZ.end(); it++)
			{
				subscribe[nCount++] = (char*)(*it).c_str();
			}

			if (m_pUserAPI && nCount > 0)
			{
				int iResult = m_pUserAPI->SubscribeMarketData(subscribe, nCount, XTP_EXCHANGE_SZ);
				if (iResult != 0)
				{
					if (m_sink)
						write_log(m_sink, LL_ERROR, "[ParserXTP] Sending md subscribe request of SZSE failed: {}", iResult);
				}
				else
				{
					if (m_sink)
						write_log(m_sink, LL_INFO, "[ParserXTP] Market data of {} instruments of SZSE subscribed", nCount);
				}

				iResult = m_pUserAPI->SubscribeTickByTick(subscribe, nCount, XTP_EXCHANGE_SZ);
				if (iResult != 0)
				{
					if (m_sink)
						write_log(m_sink, LL_ERROR, "[ParserXTP] Sending tick_by_tick subscribe request of SZSE failed: {}", iResult);
				}
				else
				{
					if (m_sink)
						write_log(m_sink, LL_INFO, "[ParserXTP] Tick_by_tick data of {} instruments of SZSE subscribed", nCount);
				}
			}
			delete[] subscribe;
		}
	}
}

void ParserXTP::unsubscribe(const CodeSet &vecSymbols)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	std::lock_guard<std::recursive_mutex> lock(_data_mutex);
	CodeSet setSH, setSZ;
	for (const auto& code : vecSymbols)
	{
		if (strncmp(code.c_str(), "SSE.", 4) == 0)
		{
			const std::string ticker = code.c_str() + 4;
			if (m_fitSHSubs.find(ticker) != m_fitSHSubs.end())
				setSH.insert(ticker);
		}
		else if (strncmp(code.c_str(), "SZSE.", 5) == 0)
		{
			const std::string ticker = code.c_str() + 5;
			if (m_fitSZSubs.find(ticker) != m_fitSZSubs.end())
				setSZ.insert(ticker);
		}
	}

	auto unsubscribeExchange = [this](const CodeSet& tickers, CodeSet& subscriptions,
		XTP_EXCHANGE_TYPE exchange, const char* exchangeName)
	{
		if (tickers.empty())	return;

		if (m_uTradingDate == 0 || m_pUserAPI == NULL)
		{
			for (const auto& ticker : tickers)
				subscriptions.erase(ticker);
			return;
		}

		std::vector<char*> symbols;
		symbols.reserve(tickers.size());
		for (const auto& ticker : tickers)
			symbols.push_back(const_cast<char*>(ticker.c_str()));

		const int mdResult = m_pUserAPI->UnSubscribeMarketData(symbols.data(), static_cast<int>(symbols.size()), exchange);
		const int tbtResult = m_pUserAPI->UnSubscribeTickByTick(symbols.data(), static_cast<int>(symbols.size()), exchange);
		if (mdResult != 0)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Sending md unsubscribe request of {} failed: {}", exchangeName, mdResult);
		if (tbtResult != 0)
			write_log(m_sink, LL_ERROR, "[ParserXTP] Sending tick_by_tick unsubscribe request of {} failed: {}", exchangeName, tbtResult);

		if (mdResult == 0 && tbtResult == 0)
		{
			for (const auto& ticker : tickers)
				subscriptions.erase(ticker);
		}
	};

	unsubscribeExchange(setSH, m_fitSHSubs, XTP_EXCHANGE_SH, "SSE");
	unsubscribeExchange(setSZ, m_fitSZSubs, XTP_EXCHANGE_SZ, "SZSE");
}

bool ParserXTP::isConnected()
{
	auto call = _lc.call(this);
	return call && m_pUserAPI!=NULL;
}

void ParserXTP::registerSpi(IParserSpi* listener)
{
	auto call = _lc.call(this);
	if (!call)
		return;
	m_sink = listener;

	if(m_sink)
		m_pBaseDataMgr = m_sink->getBaseDataMgr();
}
