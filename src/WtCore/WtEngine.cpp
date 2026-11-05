/*!
 * \file WtEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "WtEngine.h"
#include "WtDtMgr.h"
#include "WtHelper.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/charconv.hpp"
#include "../Share/decimal.h"
#include "../Share/CodeHelper.hpp"

#include "../Includes/IBaseDataMgr.h"
#include "../Includes/IHotMgr.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSRiskDef.hpp"

#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSUtils/yamlcpp/yaml.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/writer.h>
#include "../Share/FeeSchedule.hpp"
#include "../Includes/IRiskMonPluginV2.h"
#include <mutex>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <vector>
namespace rj = rapidjson;


USING_NS_WTP;

NS_WTP_BEGIN
/*
 * Context lent to a plugin risk monitor (card 9a, BUG-I02-NEW-034). While the
 * engine is alive every call is forwarded. When a monitor without a proven
 * synchronous stop is quarantined, the proxy is detached: it keeps a reference
 * to the fund object, answers late calls without touching the engine, and is
 * never destroyed.
 */
class WtRiskContextProxy final : public WtPortContext
{
public:
	explicit WtRiskContextProxy(WtEngine* engine) : _engine(engine), _fund(NULL) {}

	WTSPortFundInfo* getFundInfo() override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->getFundInfo() : _fund;
	}
	void setVolScale(double scale) override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		if (_engine) _engine->setVolScale(scale);
	}
	bool isInTrading() override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->isInTrading() : false;
	}
	void writeRiskLog(const char* message) override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		if (_engine) _engine->writeRiskLog(message);
	}
	uint32_t getCurDate() override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->getCurDate() : 0;
	}
	uint32_t getCurTime() override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->getCurTime() : 0;
	}
	uint32_t getTradingDate() override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->getTradingDate() : 0;
	}
	uint32_t transTimeToMin(uint32_t uTime) override
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		return _engine ? _engine->transTimeToMin(uTime) : 0;
	}

	// Waits for in-flight forwarded calls, then stops forwarding. The fund
	// reference (if any) is owned by the proxy from now on.
	void detach(WTSPortFundInfo* retainedFund)
	{
		std::lock_guard<std::recursive_mutex> lock(_mtx);
		_engine = NULL;
		_fund = retainedFund;
	}

private:
	std::recursive_mutex	_mtx;
	WtEngine*				_engine;
	WTSPortFundInfo*		_fund;
};
NS_WTP_END

namespace
{
// Card 13d: a YAML fee template in the versioned envelope is converted to the
// JSON form accepted by wt_fee::Schedule. Plain scalars become booleans or
// numbers when they parse completely; quoted scalars stay strings so that the
// strict v2 parser rejects them. Sequences and nulls are not part of the format.
bool feeYamlToJson(const YAML::Node& node, rj::Writer<rj::StringBuffer>& writer, int depth)
{
	if (depth > 8)
		return false;
	if (node.IsMap())
	{
		writer.StartObject();
		for (const auto& item : node)
		{
			if (!item.first.IsScalar())
				return false;
			const std::string key = item.first.as<std::string>();
			writer.Key(key.c_str(), (rj::SizeType)key.size());
			if (!feeYamlToJson(item.second, writer, depth + 1))
				return false;
		}
		writer.EndObject();
		return true;
	}
	if (!node.IsScalar())
		return false;
	const std::string text = node.Scalar();
	if (node.Tag() != "!")
	{
		if (text == "true" || text == "True" || text == "TRUE")
			return writer.Bool(true);
		if (text == "false" || text == "False" || text == "FALSE")
			return writer.Bool(false);
		if (!text.empty())
		{
			// Integers stay integers (the envelope's "version" must be an int).
			char* end = NULL;
			errno = 0;
			const long long integer = std::strtoll(text.c_str(), &end, 10);
			if (errno == 0 && end == text.c_str() + text.size())
				return writer.Int64(integer);
			end = NULL;
			const double value = std::strtod(text.c_str(), &end);
			if (end == text.c_str() + text.size() && std::isfinite(value))
				return writer.Double(value);
		}
	}
	return writer.String(text.c_str(), (rj::SizeType)text.size());
}

thread_local const WtEngine* g_task_loop_engine = NULL;

class TaskLoopThreadMarker
{
public:
	TaskLoopThreadMarker(const WtEngine* engine)
		: _previous(g_task_loop_engine)
	{
		g_task_loop_engine = engine;
	}

	~TaskLoopThreadMarker()
	{
		g_task_loop_engine = _previous;
	}

private:
	const WtEngine* _previous;
};
}

WtEngine::WtEngine()
	: _fund_udt_span(0)
	// BUG-I02-NEW-130: an engine whose init() never ran (for example the
	// unconfigured sibling engines of a runner) must report missing metadata
	// rather than dereference indeterminate manager pointers.
	, _base_data_mgr(NULL)
	, _hot_mgr(NULL)
	, _data_mgr(NULL)
	, _evt_listener(NULL)
	, _notifier(NULL)
	, _fee_error_count(0)
	, _port_fund(NULL)
	, _terminated(false)
	, _risk_ctx(NULL)
	, _risk_caps(NULL)
	, _risk_mon_stop_proven(true)
	, _risk_mon_stopped(false)
	, _risk_volscale(1.0)
	, _risk_date(0)
	, _adapter_mgr(NULL)
	, _ready(false)
	, _fee_config_valid(true)
{
	_risk_fact._module_inst = NULL;
	_risk_fact._fact = NULL;
	_risk_fact._creator = NULL;
	_risk_fact._remover = NULL;

	uint32_t curDate = 0;
	uint32_t curTime = 0;
	TimeUtils::getDateTime(curDate, curTime);
	const uint32_t curSecs = curTime % 100000;
	curTime /= 100000;
	_cur_date = curDate;
	_cur_secs = curSecs;
	_cur_time = curTime;
	_cur_raw_time = curTime;
	_cur_tdate = curDate;

	WtHelper::setTime(curDate, curTime, curSecs);
}

WtEngine::~WtEngine()
{
	// getFundInfo() lends _port_fund to the task worker and to the risk
	// monitor (through WtPortContext). Release the engine reference only after
	// both borrowers are stopped. If this destructor runs on the engine's own
	// task worker, the worker cannot be joined here; keep the fund alive
	// (bounded, one object) rather than risk a use-after-free.
	WtEngine::release();
	stop_risk_monitor();

	if (!_risk_mon_stop_proven && _risk_mon)
	{
		// Card 9a quarantine: the monitor may still run. Keep the monitor
		// object (one leaked owner), its module, the detached context and the
		// fund it may read alive until process exit.
		new WtRiskMonPtr(_risk_mon);
		_risk_ctx = NULL;	// owned by the quarantine from now on
		_port_fund = NULL;	// reference transferred to the detached context
		return;
	}

	if (_risk_ctx != NULL)
	{
		// The stopped plugin monitor is destroyed before the context it holds.
		_risk_mon.reset();
		delete _risk_ctx;
		_risk_ctx = NULL;

		// I03-NEW-062: with no monitor left, release the factory through the
		// verified by-value deleter (never through the legacy by-reference
		// typedef). Modules without it keep their factory with the module.
		WtRiskMonPluginV2DestroyFactoryFn destroyFactory = wt_riskmon_plugin::factoryDestroyer(_risk_caps);
		if (destroyFactory != NULL && _risk_fact._fact != NULL)
		{
			try
			{
				destroyFactory(static_cast<void*>(_risk_fact._fact));
			}
			catch (...)
			{
			}
			_risk_fact._fact = NULL;
		}
	}

	if (_port_fund != NULL && g_task_loop_engine != this)
	{
		_port_fund->release();
		_port_fund = NULL;
	}
}

void WtEngine::stop_risk_monitor()
{
	// Called by release() on the owner thread and again by the destructor;
	// the monitor is stopped exactly once.
	if (_risk_mon_stopped || !_risk_mon || _risk_mon->self() == NULL)
		return;
	_risk_mon_stopped = true;
	_risk_mon->self()->stop();
	if (!_risk_mon_stop_proven && _risk_ctx != NULL)
	{
		if (_port_fund != NULL)
			_port_fund->retain();
		_risk_ctx->detach(_port_fund);
		WTSLogger::log_by_cat("risk", LL_ERROR,
			"Risk monitor module {} does not declare a synchronous stop; monitor, module and fund object stay quarantined until exit",
			_risk_fact._module_path.c_str());
	}
}

void WtEngine::set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs /* = 0 */, uint32_t rawTime /* = 0 */)
{
	// I03-NEW-115: each clock field is a relaxed atomic (WtEngineClockField).
	_cur_date = curDate;
	_cur_time = curTime;
	_cur_secs = curSecs;

	if (rawTime == 0)
		rawTime = curTime;

	_cur_raw_time = rawTime;

	WtHelper::setTime(curDate, rawTime, curSecs);
}

void WtEngine::set_trading_date(uint32_t curTDate)
{
	_cur_tdate = curTDate;

	WtHelper::setTDate(curTDate);
}

WTSCommodityInfo* WtEngine::get_commodity_info(const char* stdCode)
{
	if (_base_data_mgr == NULL || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	return _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
}

WTSContractInfo* WtEngine::get_contract_info(const char* stdCode)
{
	if (_base_data_mgr == NULL || stdCode == NULL)
		return NULL;
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	return _base_data_mgr->getContract(cInfo._code, cInfo._exchg);
}

std::string WtEngine::get_rawcode(const char* stdCode)
{
	if (_hot_mgr == NULL || stdCode == NULL)
		return "";
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	if (cInfo.hasRule())
	{
		std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
		return CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	}

	return "";
}

WTSSessionInfo* WtEngine::get_session_info(const char* sid, bool isCode /* = false */)
{
	if (_base_data_mgr == NULL || sid == NULL)
		return NULL;

	if (!isCode)
		return _base_data_mgr->getSession(sid);

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, _hot_mgr);
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	return _base_data_mgr->getSession(cInfo->getSession());
}

void WtEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	set_cur_price(stdCode, curTick->price());

	//先检查是否要信号要触发
	{
		bool bTriggered = false;
		{
			// I03-NEW-124: consume the pending signal under _sig_mtx, the lock
			// handle_pos_change() holds for its read-modify-write of the target.
			SpinLock sigLock(_sig_mtx);
			auto it = _sig_map.find(stdCode);
			if (it != _sig_map.end())
			{
				WTSSessionInfo* sInfo = get_session_info(stdCode, true);

				if (sInfo == NULL)
				{
					WTSLogger::error("Pending position signal for {} retained: trading session metadata is unavailable", stdCode);
				}
				else if (sInfo->isInTradingTime(_cur_raw_time, true))
				{
					const SigInfo& sInfo = it->second;
					double pos = sInfo._volume;
					std::string code = stdCode;
					do_set_position(code.c_str(), pos, curTick->price());
					_sig_map.erase(it);
					bTriggered = true;
				}

			}
		}

		if(bTriggered)
			save_datas();
	}

	//如果成交量为0，价格也不会有变动
	if (curTick->volume() == 0)
		return;

	std::string code = stdCode;
	double price = curTick->price();
	push_task([this, code, price]{
		// I03-NEW-115: the engine thread may insert into _pos_map at the same
		// time (an insert can relocate the map storage). Look the position up
		// and copy its owner under _pos_mtx, then lock only the position.
		PosInfoPtr pInfo;
		{
			SpinLock mapLock(_pos_mtx);
			auto it = _pos_map.find(code);
			if (it == _pos_map.end())
				return;
			pInfo = it->second;
		}

		SpinLock lock(pInfo->_mtx);
		if (pInfo->_volume == 0)
		{
			pInfo->_dynprofit = 0;
		}
		else
		{
			WTSCommodityInfo* commInfo = get_commodity_info(code.c_str());
			double dynprofit = 0;
			for (auto pit = pInfo->_details.begin(); pit != pInfo->_details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				dynprofit += dInfo._profit;
			}

			pInfo->_dynprofit = dynprofit;
		}
	});

	push_task([this]() {
		update_fund_dynprofit();
	});
}

std::vector<std::pair<std::string, WtEngine::PosInfoPtr>> WtEngine::snapshot_positions()
{
	std::vector<std::pair<std::string, PosInfoPtr>> ret;
	SpinLock lock(_pos_mtx);
	ret.reserve(_pos_map.size());
	for (const auto& v : _pos_map)
		ret.emplace_back(v.first, v.second);
	return ret;
}

void WtEngine::set_cur_price(const char* stdCode, double price)
{
	SpinLock lock(_price_mtx);
	_price_map[stdCode] = price;
}

WtEngine::PosInfoPtr WtEngine::get_or_create_position(const char* stdCode)
{
	SpinLock lock(_pos_mtx);
	PosInfoPtr& slot = _pos_map[stdCode];
	if (slot == NULL)
		slot = std::make_shared<PosInfo>();
	return slot;
}

void WtEngine::update_fund_dynprofit()
{
	/*
	 *	I03-NEW-115: runs on the task worker (and in getFundInfo callers)
	 *	while the engine thread settles, trades and saves. Lock order:
	 *	_pos_mtx, then each PosInfo::_mtx, then _fund_mtx; none is nested
	 *	here. The settlement check is repeated under _fund_mtx.
	 */
	if (_port_fund == NULL)
		return;

	int64_t now = TimeUtils::getLocalTimeNow();
	{
		SpinLock fundLock(_fund_mtx);
		const WTSFundStruct& fundInfo = _port_fund->fundInfo();
		if (fundInfo._last_date == _cur_tdate)
		{
			//上次结算日期等于当前交易日,说明已经结算,不再更新了
			return;
		}

		if(_fund_udt_span != 0)
		{
			if (now - fundInfo._update_time < _fund_udt_span * 1000)
				return;
		}
	}

	double profit = 0.0;
	{
		const auto positions = snapshot_positions();
		for (const auto& v : positions)
		{
			const PosInfoPtr& pItem = v.second;
			SpinLock posLock(pItem->_mtx);
			profit += pItem->_dynprofit;
		}
	}

	SpinLock fundLock(_fund_mtx);
	WTSFundStruct& fundInfo = _port_fund->fundInfo();
	if (fundInfo._last_date == _cur_tdate)
		return;	//在汇总期间已经结算, 不再更新

	fundInfo._dynprofit = profit;
	double dynbal = fundInfo._balance + profit;
	if (fundInfo._max_dyn_bal == DBL_MAX || decimal::gt(dynbal, fundInfo._max_dyn_bal))
	{
		fundInfo._max_dyn_bal = dynbal;
		fundInfo._max_time = _cur_raw_time * 100000 + _cur_secs;
	}

	if (fundInfo._min_dyn_bal == DBL_MAX || decimal::lt(dynbal, fundInfo._min_dyn_bal))
	{
		fundInfo._min_dyn_bal = dynbal;
		fundInfo._min_time = _cur_raw_time * 100000 + _cur_secs;;
	}

	double dynbalance = fundInfo._balance + profit;
	if (fundInfo._max_md_dyn_bal._date == 0 || decimal::gt(dynbalance, fundInfo._max_md_dyn_bal._dyn_balance))
	{
		fundInfo._max_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._max_md_dyn_bal._date = _cur_tdate;
	}

	if (fundInfo._min_md_dyn_bal._date == 0 || decimal::lt(dynbalance, fundInfo._min_md_dyn_bal._dyn_balance))
	{
		fundInfo._min_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._min_md_dyn_bal._date = _cur_tdate;
	}

	fundInfo._update_time = now;
}

void WtEngine::writeRiskLog(const char* message)
{
	static thread_local char szBuf[2048] = { 0 };
	auto len = wt_strcpy(szBuf, "[RiskControl] ");
	wt_strcpy(szBuf + len, message);
	WTSLogger::log_raw_by_cat("risk", LL_INFO, szBuf);
}

uint32_t WtEngine::getCurDate()
{
	return _cur_date;
}

uint32_t WtEngine::getCurTime()
{
	return _cur_time;
}

uint32_t WtEngine::getTradingDate()
{
	return _cur_tdate;
}

bool WtEngine::isInTrading()
{
	return false;
}

void WtEngine::setVolScale(double scale)
{
	double oldScale = _risk_volscale;
	_risk_volscale = scale;
	_risk_date = _cur_tdate;

	WTSLogger::log_by_cat("risk", LL_INFO, "Position risk scale updated: {} - > {}", oldScale, scale);
	save_datas();
}

WTSPortFundInfo* WtEngine::getFundInfo()
{
	update_fund_dynprofit();
	save_datas();

	return _port_fund;
}

void WtEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier)
{
	_base_data_mgr = bdMgr;
	_data_mgr = dataMgr;
	_hot_mgr = hotMgr;
	_notifier = notifier;

	WTSLogger::info("Running mode: Production");

	_filter_mgr.set_notifier(notifier);

	_filter_mgr.load_filters(cfg->getCString("filters"));

	_fee_config_valid = true;
	load_fees(cfg->getCString("fees"));

	load_datas();

	init_outputs();

	WTSVariant* cfgRisk = cfg->get("riskmon");
	if(cfgRisk)
	{
		init_riskmon(cfgRisk);
	}
	else
	{
		//如果没有配置风控线程，则需要自己更新浮动盈亏
		//把更新时间间隔设置为5s
		_fund_udt_span = 5;
		WTSLogger::log_raw(LL_WARN, "RiskMon is not configured, portfilio fund will be updated every 5s");
	}
}

void WtEngine::on_session_end()
{
	//资金结算
	// I03-NEW-115: settle a copy under _fund_mtx; the record is written and
	// save_datas() runs after the lock is released.
	const uint32_t curTDate = _cur_tdate;
	bool bSettled = false;
	WTSFundStruct settledInfo;
	if (_port_fund != NULL)
	{
		SpinLock fundLock(_fund_mtx);
		WTSFundStruct& fundInfo = _port_fund->fundInfo();
		if (fundInfo._last_date < curTDate)
		{
			settledInfo = fundInfo;
			bSettled = true;

			fundInfo._last_date = curTDate;
			fundInfo._predynbal = fundInfo._balance + fundInfo._dynprofit;
			fundInfo._prebalance = fundInfo._balance;
			fundInfo._profit = 0;
			fundInfo._fees = 0;
			fundInfo._max_dyn_bal = DBL_MAX;
			fundInfo._min_dyn_bal = DBL_MAX;
			fundInfo._max_time = 0;
			fundInfo._min_time = 0;
		}
	}

	if (bSettled)
	{
		const WTSFundStruct& fundInfo = settledInfo;
		std::string filename = WtHelper::getPortifolioDir();
		filename += "funds.csv";
		BoostFilePtr fund_log(new BoostFile());
		{
			bool isNewFile = !StdFile::exists(filename.c_str());
			fund_log->create_or_open_file(filename.c_str());
			if (isNewFile)
			{
				fund_log->write_file("date,predynbalance,prebalance,balance,closeprofit,dynprofit,fee,maxdynbalance,maxtime,mindynbalance,mintime,mdmaxbalance,mdmaxdate,mdminbalance,mdmindate\n");
			}
			else
			{
				fund_log->seek_to_end();
			}
		}

		//可能这里还需要写一条资金记录
		//date,predynbalance,prebalance,balance,closeprofit,dynprofit,fee,maxdynbalance,maxtime,mindynbalance,mintime,mdmaxbalance,mdmaxdate,mdminbalance,mdmindate
		fund_log->write_file(fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", 
			curTDate, fundInfo._predynbal, fundInfo._prebalance, fundInfo._balance, 
			fundInfo._profit, fundInfo._dynprofit, fundInfo._fees, fundInfo._max_dyn_bal,
			fundInfo._max_time, fundInfo._min_dyn_bal, fundInfo._min_time,
			fundInfo._max_md_dyn_bal._dyn_balance, fundInfo._max_md_dyn_bal._date,
			fundInfo._min_md_dyn_bal._dyn_balance, fundInfo._min_md_dyn_bal._date));
	}
	save_datas();
}

void WtEngine::on_session_begin()
{

}

void WtEngine::save_datas()
{
	// I03-NEW-124: one writer at a time (the CTA pool saves concurrently).
	StdUniqueLock saveLock(_save_mtx);

	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	if (_port_fund != NULL)
	{//保存资金数据
		// I03-NEW-115: serialize a copy taken under _fund_mtx.
		WTSFundStruct fundSnapshot;
		{
			SpinLock fundLock(_fund_mtx);
			fundSnapshot = _port_fund->fundInfo();
		}
		const WTSFundStruct& fundInfo = fundSnapshot;
		rj::Value jFund(rj::kObjectType);
		jFund.AddMember("predynbal", fundInfo._predynbal, allocator);
		jFund.AddMember("balance", fundInfo._balance, allocator);
		jFund.AddMember("prebalance", fundInfo._prebalance, allocator);
		jFund.AddMember("profit", fundInfo._profit, allocator);
		jFund.AddMember("dynprofit", fundInfo._dynprofit, allocator);
		jFund.AddMember("fees", fundInfo._fees, allocator);

		jFund.AddMember("max_dyn_bal", fundInfo._max_dyn_bal, allocator);
		jFund.AddMember("max_time", fundInfo._max_time, allocator);
		jFund.AddMember("min_dyn_bal", fundInfo._min_dyn_bal, allocator);
		jFund.AddMember("min_time", fundInfo._min_time, allocator);

		jFund.AddMember("last_date", fundInfo._last_date, allocator);
		jFund.AddMember("date", _cur_tdate.load(), allocator);

		rj::Value jMaxMD(rj::kObjectType);
		jMaxMD.AddMember("date", fundInfo._max_md_dyn_bal._date, allocator);
		jMaxMD.AddMember("dyn_balance", fundInfo._max_md_dyn_bal._dyn_balance, allocator);

		rj::Value jMinMD(rj::kObjectType);
		jMinMD.AddMember("date", fundInfo._min_md_dyn_bal._date, allocator);
		jMinMD.AddMember("dyn_balance", fundInfo._min_md_dyn_bal._dyn_balance, allocator);

		jFund.AddMember("maxmd", jMaxMD, allocator);
		jFund.AddMember("minmd", jMinMD, allocator);

		jFund.AddMember("update_time", fundInfo._update_time, allocator);

		root.AddMember("fund", jFund, allocator);
	}

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		// I03-NEW-115: iterate a snapshot taken under _pos_mtx (map order is
		// kept) and read each position under its own lock. No caller holds a
		// PosInfo::_mtx or _pos_mtx when calling save_datas().
		const auto positions = snapshot_positions();
		for (auto it = positions.begin(); it != positions.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosInfoPtr& pInfo = it->second;
			SpinLock posLock(pInfo->_mtx);

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);
			pItem.AddMember("volume", pInfo->_volume, allocator);
			pItem.AddMember("closeprofit", pInfo->_closeprofit, allocator);
			pItem.AddMember("dynprofit", pInfo->_dynprofit, allocator);

			rj::Value details(rj::kArrayType);
			for (auto dit = pInfo->_details.begin(); dit != pInfo->_details.end(); dit++)
			{
				const DetailInfo& dInfo = *dit;
				if(decimal::eq(dInfo._volume, 0))
					continue;
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("long", dInfo._long, allocator);
				dItem.AddMember("price", dInfo._price, allocator);
				dItem.AddMember("volume", dInfo._volume, allocator);
				dItem.AddMember("opentime", dInfo._opentime, allocator);
				dItem.AddMember("opentdate", dInfo._opentdate, allocator);

				dItem.AddMember("profit", dInfo._profit, allocator);

				details.PushBack(dItem, allocator);
			}

			pItem.AddMember("details", details, allocator);

			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	//风控参数设置
	{
		rj::Value jRisk(rj::kObjectType);

		jRisk.AddMember("scale", _risk_volscale, allocator);
		jRisk.AddMember("date", _risk_date, allocator);

		root.AddMember("riskmon", jRisk, allocator);
	}

	{
		std::string filename = WtHelper::getPortifolioDir();
		filename += "datas.json";

		BoostFile bf;
		if (bf.create_new_file(filename.c_str()))
		{
			rj::StringBuffer sb;
			rj::PrettyWriter<rj::StringBuffer> writer(sb);
			root.Accept(writer);
			bf.write_file(sb.GetString());
			bf.close_file();
		}
	}
}

void WtEngine::load_datas()
{
	// The engine owns exactly one fund object for its lifetime. A repeated
	// load must not overwrite (and leak) a pointer that may already be lent
	// out through getFundInfo(); reset its contents in place instead.
	if (_port_fund == NULL)
		_port_fund = WTSPortFundInfo::create();
	else
		_port_fund->fundInfo() = WTSFundStruct();

	std::string filename = WtHelper::getPortifolioDir();
	filename += "datas.json";

	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	if (content.empty())
		return;

	rj::Document root;
	root.Parse(content.c_str());

	if (root.HasParseError())
		return;

	//读取资金
	{
		const rj::Value& jFund = root["fund"];
		if (!jFund.IsNull() && jFund.IsObject())
		{
			WTSFundStruct& fundInfo = _port_fund->fundInfo();
			fundInfo._predynbal = jFund["predynbal"].GetDouble();
			fundInfo._balance = jFund["balance"].GetDouble();
			fundInfo._prebalance = jFund["prebalance"].GetDouble();
			fundInfo._profit = jFund["profit"].GetDouble();
			fundInfo._dynprofit = jFund["dynprofit"].GetDouble();
			fundInfo._fees = jFund["fees"].GetDouble();
			fundInfo._last_date = jFund["last_date"].GetUint();
			fundInfo._max_dyn_bal = jFund["max_dyn_bal"].GetDouble();
			fundInfo._max_time = jFund["max_time"].GetUint();
			fundInfo._min_dyn_bal = jFund["min_dyn_bal"].GetDouble();
			fundInfo._min_time = jFund["min_time"].GetUint();

			const rj::Value& jMaxMD = jFund["maxmd"];
			fundInfo._max_md_dyn_bal._dyn_balance = jMaxMD["dyn_balance"].GetDouble();
			fundInfo._max_md_dyn_bal._date = jMaxMD["date"].GetUint();

			const rj::Value& jMinMD = jFund["minmd"];
			fundInfo._min_md_dyn_bal._dyn_balance = jMinMD["dyn_balance"].GetDouble();
			fundInfo._min_md_dyn_bal._date = jMinMD["date"].GetUint();

			if(jFund.HasMember("update_time"))
			{
				fundInfo._update_time = jFund["update_time"].GetInt64();
			}
		}
	}

	{//读取仓位
		[[maybe_unused]] double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* stdCode = pItem["code"].GetString();
				PosInfoPtr& pInfo = _pos_map[stdCode];
				if (pInfo == NULL)
					pInfo.reset(new PosInfo);
				pInfo->_closeprofit = pItem["closeprofit"].GetDouble();
				pInfo->_volume = pItem["volume"].GetDouble();
				if (pInfo->_volume == 0)
					pInfo->_dynprofit = 0;
				else
					pInfo->_dynprofit = pItem["dynprofit"].GetDouble();

				total_profit += pInfo->_closeprofit;
				total_dynprofit += pInfo->_dynprofit;

				const rj::Value& details = pItem["details"];
				if (details.IsNull() || !details.IsArray() || details.Size() == 0)
					continue;

				for (uint32_t i = 0; i < details.Size(); i++)
				{
					const rj::Value& dItem = details[i];
					DetailInfo dInfo;
					dInfo._long = dItem["long"].GetBool();
					dInfo._price = dItem["price"].GetDouble();
					dInfo._volume = dItem["volume"].GetDouble();
					dInfo._opentime = dItem["opentime"].GetUint64();
					if (dItem.HasMember("opentdate"))
						dInfo._opentdate = dItem["opentdate"].GetUint();

					dInfo._profit = dItem["profit"].GetDouble();
					pInfo->_details.emplace_back(dInfo);
				}

				WTSLogger::debug("Porfolio position confirmed,{} -> {}", stdCode, pInfo->_volume);
			}
		}

		WTSFundStruct& fundInfo = _port_fund->fundInfo();
		fundInfo._dynprofit = total_dynprofit;

		WTSLogger::debug("{} position info of portfolio loaded", _pos_map.size());
	}

	if(root.HasMember("riskmon"))
	{
		//读取风控参数
		const rj::Value& jRisk = root["riskmon"];
		if (!jRisk.IsNull() && jRisk.IsObject())
		{
			_risk_date = jRisk["date"].GetUint();
			_risk_volscale = jRisk["scale"].GetDouble();
		}
	}
}

WTSTickSlice* WtEngine::get_tick_slice(uint32_t /*sid*/, const char* code, uint32_t count)
{
	if (_data_mgr == NULL)
		return NULL;
	return _data_mgr->get_tick_slice(code, count);
}

WTSTickData* WtEngine::get_last_tick(uint32_t /*sid*/, const char* stdCode)
{
	if (_data_mgr == NULL)
		return NULL;
	return _data_mgr->grab_last_tick(stdCode);
}

WTSKlineSlice* WtEngine::get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */, uint64_t etime /* = 0 */)
{
	if (_base_data_mgr == NULL || _data_mgr == NULL || stdCode == NULL || period == NULL)
		return NULL;
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	if (cInfo == NULL)
		return NULL;

	// I03-NEW-039: caller stdCode is unbounded; no fixed 64-byte key buffer
	std::string key = fmt::format("{}-{}-{}", stdCode, period, times);

	SubList& sids = _bar_sub_map[key];
	sids[sid] = std::make_pair(sid, 0);

	WTSKlinePeriod kp;
	if (period[0] == 'm')
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			times /= 5;
		}
		else
			kp = KP_Minute1;
	}
	else if (strcmp(period, "h") == 0)	//小时线
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HOUR;
	}
	else if (strcmp(period, "f") == 0)	//半小时线
	{
		kp = KP_Minute5;
		times = PERIOD_TIMES_HALF;
	}
	else
	{
		kp = KP_DAY;
	}

	return _data_mgr->get_kline_slice(stdCode, kp, times, count, etime);
}


void WtEngine::handle_push_quote(WTSTickData* curTick)
{
	std::string stdCode = curTick->code();
	_data_mgr->handle_push_quote(stdCode.c_str(), curTick);
	on_tick(stdCode.c_str(), curTick);

	[[maybe_unused]] double price = curTick->price();
	WTSContractInfo* cInfo = curTick->getContractInfo();

	//if(hotFlag == 1)
	// I03-NEW-094: a tick without contract metadata has no hot-code fan-out.
	if(cInfo != NULL && !cInfo->isFlat())
	{
		auto hotCodes = cInfo->getHotCodes();
		for (const std::string& hotCode : hotCodes)
		{
			WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
			if (hotTick == NULL)
				continue;
			if (!hotTick->trySetCode(hotCode.c_str()))
			{
				hotTick->release();
				continue;
			}
			hotTick->setContractInfo(curTick->getContractInfo());

			_data_mgr->handle_push_quote(hotCode.c_str(), hotTick);
			on_tick(hotCode.c_str(), hotTick);

			hotTick->release();
		}		
	}
}

double WtEngine::get_cur_price(const char* stdCode)
{
	if (stdCode == NULL || stdCode[0] == '\0')
		return 0.0;

	auto len = strlen(stdCode);
	char lastChar = stdCode[len - 1];
	//前复权直接读取标准合约代码
	bool bAdjusted = (lastChar == SUFFIX_QFQ || lastChar == SUFFIX_HFQ);
	//前复权需要去掉－，后复权和未复权都直接查找
	std::string sCode = (lastChar == SUFFIX_QFQ) ? std::string(stdCode, len - 1) : stdCode;
	// I03-NEW-124: strategy threads read the cache while the engine thread
	// inserts into it. _price_mtx is a leaf lock and is not held across the
	// data manager call below.
	{
		SpinLock lock(_price_mtx);
		auto it = _price_map.find(sCode);
		if (it != _price_map.end())
			return it->second;
	}

	//找不到的时候，先读取未复权的tick数据
	if (_data_mgr == NULL)
		return 0.0;
	std::string fCode = bAdjusted ? std::string(stdCode, len - 1) : stdCode;
	WTSTickData* lastTick = _data_mgr->grab_last_tick(fCode.c_str());
	if (lastTick == NULL)
		return 0.0;

	WTSContractInfo* cInfo = lastTick->getContractInfo();

	double ret = lastTick->price();
	lastTick->release();

	//如果是后复权，则进行复权处理
	if (lastChar == SUFFIX_HFQ)
	{
		ret *= get_exright_factor(stdCode, cInfo->getCommInfo());
	}

	// A price another thread stored meanwhile (on_tick) is kept: it is newer.
	SpinLock lock(_price_mtx);
	return _price_map.emplace(sCode, ret).first->second;
}

double WtEngine::get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if (stdCode == NULL || stdCode[0] == '\0')
		return 0.0;

	auto len = strlen(stdCode);
	char lastChar = stdCode[len - 1];
	//前复权直接读取标准合约代码
	bool bAdjusted = (lastChar == SUFFIX_QFQ || lastChar == SUFFIX_HFQ);
	//前复权需要去掉－，后复权和未复权都直接查找
	std::string sCode = (lastChar == SUFFIX_QFQ) ? std::string(stdCode, len - 1) : stdCode;

	//找不到的时候，先读取未复权的tick数据
	if (_data_mgr == NULL)
		return 0.0;
	std::string fCode = bAdjusted ? std::string(stdCode, len - 1) : stdCode;
	WTSTickData* lastTick = _data_mgr->grab_last_tick(fCode.c_str());
	if (lastTick == NULL)
		return 0.0;

	WTSCommodityInfo* commInfo = get_commodity_info(fCode.c_str());

	double ret = 0.0;
	switch (flag)
	{
	case 0:
		ret = lastTick->open(); break;
	case 1:
		ret = lastTick->high(); break;
	case 2:
		ret = lastTick->low(); break;
	case 3:
		ret = lastTick->price(); break;
	default:
		break;
	}
	lastTick->release();

	//如果是后复权，则进行复权处理
	if (lastChar == SUFFIX_HFQ)
	{
		ret *= get_exright_factor(stdCode, commInfo);
	}

	return ret;
}

double WtEngine::get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo /* = NULL */)
{
	if (commInfo == NULL)
		commInfo = get_commodity_info(stdCode);

	if (commInfo == NULL)
		return 1.0;

	if (commInfo->isStock())
		return _data_mgr != NULL ? _data_mgr->get_adjusting_factor(stdCode, get_trading_date()) : 1.0;
	else if (_hot_mgr != NULL)
	{
		const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
		if(strlen(ruleTag) > 0)
			return _hot_mgr->getRuleFactor(ruleTag, commInfo->getFullPid(), get_trading_date());
	}

	return 1.0;
}

uint32_t WtEngine::get_adjusting_flag()
{
	if (_data_mgr)
		return _data_mgr->get_adjusting_flag();

	return 0;
}

void WtEngine::sub_tick(uint32_t sid, const char* stdCode)
{
	//如果是主力合约代码, 如SHFE.ag.HOT, 那么要转换成原合约代码, SHFE.ag.1912
	//因为执行器只识别原合约代码
	if (stdCode == NULL || stdCode[0] == '\0')
		return;
	const char* ruleTag = (_hot_mgr != NULL) ? _hot_mgr->getRuleTag(stdCode) : "";
	if(ruleTag != NULL && strlen(ruleTag) > 0)
	{
		//SubList& sids = _tick_sub_map[stdCode];
		//sids[sid] = std::make_pair(sid, 0);

		std::size_t length = strlen(stdCode);
		uint32_t flag = 0;
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			length--;

			flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
		}

		SubList& sids = _tick_sub_map[std::string(stdCode, length)];
		sids[sid] = std::make_pair(sid, flag);

		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
		std::string rawCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), _cur_tdate);
		std::string stdRawCode = CodeHelper::rawMonthCodeToStdCode(rawCode.c_str(), cInfo._exchg);
	}
	//if (CodeHelper::isStdFutHotCode(stdCode))
	//{
	//	SubList& sids = _tick_sub_map[stdCode];
	//	sids[sid] = std::make_pair(sid, 0);

	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string rawCode = _hot_mgr->getRawCode(cInfo._exchg, cInfo._product, _cur_tdate);
	//	std::string stdRawCode = CodeHelper::rawMonthCodeToStdCode(rawCode.c_str(), cInfo._exchg);
	//	//_ticksubed_raw_codes.insert(stdRawCode);
	//}
	//else if (CodeHelper::isStdFut2ndCode(stdCode))
	//{
	//	SubList& sids = _tick_sub_map[stdCode];
	//	sids[sid] = std::make_pair(sid, 0);

	//	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode);
	//	std::string rawCode = _hot_mgr->getSecondRawCode(cInfo._exchg, cInfo._product, _cur_tdate);
	//	std::string stdRawCode = CodeHelper::rawMonthCodeToStdCode(rawCode.c_str(), cInfo._exchg);
	//	//_ticksubed_raw_codes.insert(stdRawCode);
	//}
	else
	{
		std::size_t length = strlen(stdCode);
		uint32_t flag = 0;
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			length--;

			flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
		}

		SubList& sids = _tick_sub_map[std::string(stdCode, length)];
		sids[sid] = std::make_pair(sid, flag);

		//_ticksubed_raw_codes.insert(std::string(stdCode, length));
	}
}

void WtEngine::load_fees(const char* filename)
{
	_fee_config_valid = true;
	if (filename == NULL || filename[0] == '\0')
		return;

	// A configured template must be read and validated before the engine can
	// start. In particular, a parse/read error must not silently revive an old
	// fee snapshot or turn an unsupported template into zero-cost trading.
	_fee_config_valid = false;

	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Fee templates file {} not exists", filename);
		return;
	}

	const bool isYamlTemplate = StrUtil::endsWith(filename, ".yaml") || StrUtil::endsWith(filename, ".yml");
	const bool isJsonTemplate = StrUtil::endsWith(filename, ".json");
	if (!isYamlTemplate && !isJsonTemplate)
	{
		WTSLogger::error("Fee templates file {} has an unsupported extension", filename);
		return;
	}

	std::string content;
	try
	{
		StdFile::read_file_content(filename, content);
	}
	catch (const std::exception& e)
	{
		WTSLogger::error("Fee templates file {} reading failed: {}", filename, e.what());
		return;
	}
	if (content.empty())
	{
		WTSLogger::error("Fee templates file {} is empty", filename);
		return;
	}

	WTSVariant* cfg = NULL;
	try
	{
		cfg = WTSCfgLoader::load_from_content(content, isYamlTemplate);
	}
	catch (const YAML::Exception& e)
	{
		WTSLogger::error("Fee templates file {} loading failed: {}", filename, e.what());
		return;
	}
	catch (const std::exception& e)
	{
		WTSLogger::error("Fee templates file {} loading failed: {}", filename, e.what());
		return;
	}
	if (cfg == NULL)
	{
		WTSLogger::error("Fee templates file {} loading failed", filename);
		return;
	}

	if (cfg->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("Fee templates file {} must contain an object", filename);
		cfg->release();
		return;
	}

	// WTSCfgLoader intentionally drops null-valued entries. Keep the raw keys so
	// null entries are still recognized and skipped, matching the old
	// memberNames()-based loader for both known and unknown commodities.
	std::string rawContent = content;
	const bool isUtf8 = EncodingHelper::isUtf8((unsigned char*)content.data(), content.size());
#ifdef _WIN32
	if (isUtf8)
		rawContent = UTF8toChar(content);
#else
	if (!isUtf8)
		rawContent = ChartoUTF8(content);
#endif
	if (rawContent.empty())
		rawContent = content;

	rj::Document rawJson;
	YAML::Node rawYaml;
	std::vector<std::string> rawKeys;
	if (isJsonTemplate)
	{
		rawJson.Parse(rawContent.c_str());
		if (rawJson.HasParseError() || !rawJson.IsObject())
		{
			WTSLogger::error("Fee templates file {} must contain an object", filename);
			cfg->release();
			return;
		}
		for (const auto& item : rawJson.GetObject())
			rawKeys.emplace_back(item.name.GetString(), item.name.GetStringLength());
	}
	else
	{
		try
		{
			rawYaml = YAML::Load(rawContent);
		}
		catch (const YAML::Exception& e)
		{
			WTSLogger::error("Fee templates file {} loading failed: {}", filename, e.what());
			cfg->release();
			return;
		}
		if (!rawYaml.IsMap())
		{
			WTSLogger::error("Fee templates file {} must contain an object", filename);
			cfg->release();
			return;
		}
		try
		{
			for (const auto& item : rawYaml)
				rawKeys.emplace_back(item.first.as<std::string>());
		}
		catch (const YAML::Exception& e)
		{
			WTSLogger::error("Fee templates file {} has an invalid key: {}", filename, e.what());
			cfg->release();
			return;
		}
	}

	// Card 13d (I03-NEW-008): a versioned envelope carries contract-level and
	// product-level rates. It is parsed strictly by wt_fee::Schedule and the
	// whole file is rejected on any error; live fills then use
	// calc_fee_checked (contract first, product fallback, missing is an error).
	bool versionedEnvelope = false;
	for (const std::string& key : rawKeys)
		if (key == "version" || key == "contracts" || key == "products")
			versionedEnvelope = true;
	if (versionedEnvelope)
	{
		std::string json;
		if (isJsonTemplate)
			json = rawContent;
		else
		{
			rj::StringBuffer buffer;
			rj::Writer<rj::StringBuffer> writer(buffer);
			bool converted = false;
			try
			{
				converted = feeYamlToJson(rawYaml, writer, 0);
			}
			catch (const YAML::Exception&)
			{
				converted = false;
			}
			if (!converted)
			{
				WTSLogger::error("Fee templates file {} has an invalid versioned YAML structure", filename);
				cfg->release();
				return;
			}
			json.assign(buffer.GetString(), buffer.GetSize());
		}

		wt_fee::Schedule schedule;
		std::string scheduleError;
		if (!schedule.loadJson(json, scheduleError))
		{
			WTSLogger::error("Fee templates file {} is invalid: {}", filename, scheduleError);
			cfg->release();
			return;
		}

		auto toItem = [](const wt_fee::Rates& rates) {
			FeeItem item;
			item._open = rates.open;
			item._close = rates.close;
			item._close_today = rates.close_today;
			item._by_volume = rates.by_volume;
			item._min_fee = rates.min_fee;
			return item;
		};
		FeeMap products, contracts;
		for (const auto& entry : schedule.productRates())
		{
			products[entry.first] = toItem(entry.second);
			const StringVector& ay = StrUtil::split(entry.first, ".");
			WTSCommodityInfo* commInfo = (ay.size() == 2) ? _base_data_mgr->getCommodity(ay[0].c_str(), ay[1].c_str()) : NULL;
			if (commInfo == NULL)
			{
				WTSLogger::warn("Fee template product '{}' does not match a registered commodity", entry.first);
				continue;
			}
			// Legacy consumers outside WtCore still read the commodity rates.
			commInfo->setFeeRates(entry.second.open, entry.second.close, entry.second.close_today, entry.second.by_volume);
			commInfo->setMarginRate(entry.second.margin);
		}
		for (const auto& entry : schedule.contractRates())
			contracts[entry.first] = toItem(entry.second);

		_fee_map.swap(products);
		_contract_fee_map.swap(contracts);
		cfg->release();
		_fee_config_valid = true;
		WTSLogger::info("{} product and {} contract fee templates loaded (version 2)",
			_fee_map.size(), _contract_fee_map.size());
		return;
	}

	auto readNumber = [](WTSVariant* field, double& value)
	{
		if (field == NULL)
		{
			value = 0.0;
			return true;
		}

		switch (field->type())
		{
		case WTSVariant::VT_Null:
			value = 0.0;
			break;
		case WTSVariant::VT_Int32:
		case WTSVariant::VT_Uint32:
		case WTSVariant::VT_Int64:
		case WTSVariant::VT_Uint64:
		case WTSVariant::VT_Real:
			value = field->asDouble();
			break;
		case WTSVariant::VT_String:
		{
			// WTSVariant has historically converted numeric strings. Keep that
			// compatibility but reject partial parses and non-finite values.
			const std::string text = field->asString();
			if (text.empty())
				return false;
			char* end = NULL;
			value = std::strtod(text.c_str(), &end);
			if (end == text.c_str())
				return false;
			while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end)))
				++end;
			if (end != text.c_str() + text.size())
				return false;
			break;
		}
		default:
			return false;
		}

		return std::isfinite(value);
	};
	auto readBoolean = [](WTSVariant* field)
	{
		// Keep WTSVariant's legacy conversion: missing/null and every value
		// other than a case-insensitive "true" or "yes" map to false.
		return field != NULL && field->asBoolean();
	};
	struct StagedFee
	{
		WTSCommodityInfo* commodity;
		double open;
		double close;
		double closeToday;
		double margin;
		double minFee;
		bool byVolume;

		StagedFee() : commodity(NULL), open(0.0), close(0.0), closeToday(0.0), margin(0.0), minFee(0.0), byVolume(false) {}
	};
	wt_hashmap<std::string, StagedFee> stagedFees;
	std::string validationError;
	for (const std::string& fullPid : rawKeys)
	{
		const StringVector& ay = StrUtil::split(fullPid, ".");
		if (ay.size() != 2 || ay[0].empty() || ay[1].empty())
		{
			validationError = fmt::format("invalid commodity key '{}'", fullPid);
			break;
		}
		WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(ay[0].c_str(), ay[1].c_str());
		if (commInfo == NULL)
		{
			WTSLogger::warn("Fee template item '{}' skipped because the commodity is not registered", fullPid);
			continue;
		}

		WTSVariant* cfgItem = cfg->get(fullPid.c_str());
		// The config loader omits top-level nulls from memberNames(). The raw
		// key list preserves them, so explicitly keep the old behavior and skip
		// an omitted value without changing the active fee snapshot.
		if (cfgItem == NULL)
			continue;
		if (cfgItem->type() != WTSVariant::VT_Object)
		{
			validationError = fmt::format("fee entry '{}' must be an object", fullPid);
			break;
		}

		StagedFee& staged = stagedFees[fullPid];
		staged.commodity = commInfo;
		if (!readNumber(cfgItem->get("open"), staged.open))
			validationError = fmt::format("fee entry '{}' has an invalid numeric 'open'", fullPid);
		else if (!readNumber(cfgItem->get("close"), staged.close))
			validationError = fmt::format("fee entry '{}' has an invalid numeric 'close'", fullPid);
		else if (!readNumber(cfgItem->get("closetoday"), staged.closeToday))
			validationError = fmt::format("fee entry '{}' has an invalid numeric 'closetoday'", fullPid);
		else
			staged.byVolume = readBoolean(cfgItem->get("byvolume"));

		WTSVariant* marginItem = cfgItem->get("margin");
		if (validationError.empty() && !readNumber(marginItem, staged.margin))
			validationError = fmt::format("fee entry '{}' has an invalid numeric 'margin'", fullPid);

		// minfee is a supported optional field in shared fee templates. This
		// engine does not apply it, but reject malformed values rather than
		// accepting a template that another fee consumer would interpret differently.
		WTSVariant* minFeeItem = cfgItem->get("minfee");
		double minFee = 0.0;
		if (validationError.empty() && minFeeItem != NULL && !readNumber(minFeeItem, minFee))
			validationError = fmt::format("fee entry '{}' has an invalid numeric 'minfee'", fullPid);
		else if (validationError.empty() && minFee < 0)
			validationError = fmt::format("fee entry '{}' has a negative 'minfee'", fullPid);
		staged.minFee = minFee;

		if (!validationError.empty())
			break;
	}
	if (!validationError.empty())
	{
		WTSLogger::error("Fee templates file {} is invalid: {}", filename, validationError);
		_fee_config_valid = false;
		cfg->release();
		return;
	}

	// Legacy files keep their historical merge semantics (entries absent from
	// the file, or null, keep their previous rates). They carry no
	// contract-level rows, so contract rates from an earlier versioned file
	// do not survive a legacy reload.
	_contract_fee_map.clear();
	for (const auto& stagedFee : stagedFees)
	{
		const StagedFee& staged = stagedFee.second;
		staged.commodity->setFeeRates(staged.open, staged.close, staged.closeToday, staged.byVolume);
		staged.commodity->setMarginRate(staged.margin);

		FeeItem& fItem = _fee_map[stagedFee.first];
		fItem._open = staged.open;
		fItem._close = staged.close;
		fItem._close_today = staged.closeToday;
		fItem._by_volume = staged.byVolume;
		fItem._min_fee = staged.minFee;
	}

	cfg->release();
	_fee_config_valid = true;

	WTSLogger::info("{} fee templates loaded", _fee_map.size());
}

bool WtEngine::calc_fee_checked(const char* stdCode, double price, double qty, uint32_t offset,
	double& fee, std::string& error)
{
	fee = 0.0;
	error.clear();
	if (stdCode == NULL || stdCode[0] == '\0')
	{
		error = "missing code";
		return false;
	}
	if (_base_data_mgr == NULL)
	{
		error = "engine base data is unavailable";
		return false;
	}

	// Card 13d: fees belong to the actual fill contract. A continuous code is
	// mapped to the contract of the current trading day.
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	std::string rawCode = cInfo._code;
	if (cInfo.hasRule())
	{
		const char* mapped = (_hot_mgr != NULL) ?
			_hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate) : NULL;
		if (mapped == NULL || mapped[0] == '\0')
		{
			error = "continuous code has no actual contract on the current trading day";
			return false;
		}
		rawCode = mapped;
	}
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(cInfo._exchg, cInfo._product);
	WTSContractInfo* contract = _base_data_mgr->getContract(rawCode.c_str(), cInfo._exchg);
	if (commInfo == NULL || contract == NULL || contract->getCommInfo() != commInfo)
	{
		error = "fill contract does not match registered commodity metadata";
		return false;
	}

	wt_fee::Context context;
	context.physical_contract = contract->getFullCode();
	context.product = commInfo->getFullPid();
	// Same rule as WTSBaseDataMgr::loadContracts: a contract without its own
	// volume scale uses its commodity's.
	context.volume_scale = contract->getVolScale() != 0 ? contract->getVolScale() : commInfo->getVolScale();
	const FeeItem* item = NULL;
	auto cit = _contract_fee_map.find(context.physical_contract);
	if (cit != _contract_fee_map.end())
		item = &cit->second;
	else
	{
		auto pit = _fee_map.find(context.product);
		if (pit != _fee_map.end())
			item = &pit->second;
	}
	if (item == NULL)
	{
		error = "no fee template for " + context.physical_contract;
		return false;
	}
	context.rates.open = item->_open;
	context.rates.close = item->_close;
	context.rates.close_today = item->_close_today;
	context.rates.by_volume = item->_by_volume;
	// Decision K10=A (2026-11-04): live fills apply the optional per-fill
	// minimum exactly like the backtest FeeSchedule rule (qty > 0, rate > 0).
	context.rates.min_fee = item->_min_fee;

	const wt_fee::CalculationResult result = wt_fee::calculate(context, price, qty, offset);
	switch (result.status)
	{
	case wt_fee::CalculationStatus::ok:
		fee = result.value;
		return true;
	case wt_fee::CalculationStatus::invalid_input:
		error = "invalid price, quantity or offset";
		return false;
	case wt_fee::CalculationStatus::invalid_context:
		error = "invalid fee template or contract metadata for " + context.physical_contract;
		return false;
	default:
		error = "fee calculation overflow for " + context.physical_contract;
		return false;
	}
}

void WtEngine::report_fee_error(const char* stdCode, const std::string& message)
{
	const std::string code = stdCode ? stdCode : "";
	const std::string text = code + ": " + message;
	bool firstForCode = false;
	{
		StdUniqueLock lock(_fee_error_mtx);
		_last_fee_error = text;
		firstForCode = _fee_error_codes.insert(code).second;
	}
	_fee_error_count.fetch_add(1);
	// The fill itself is kept; the accounting gap is made observable.
	if (firstForCode)
		WTSLogger::error("Fee accounting failed, fill kept with zero fee: {}", text);
}

std::string WtEngine::last_fee_error() const
{
	StdUniqueLock lock(_fee_error_mtx);
	return _last_fee_error;
}

double WtEngine::calc_fee(const char* stdCode, double price, double qty, uint32_t offset)
{
	// Legacy signature: it cannot carry a failure, so a failure is reported
	// through report_fee_error/fee_error_count and 0.0 is returned.
	double fee = 0.0;
	std::string error;
	if (!calc_fee_checked(stdCode, price, qty, offset, fee, error))
	{
		report_fee_error(stdCode, error);
		return 0.0;
	}
	return fee;
}

void WtEngine::append_signal(const char* stdCode, double qty, bool bStandBy /* = true */)
{
	// I03-NEW-124: a target change is one step under _sig_mtx.
	SpinLock sigLock(_sig_mtx);
	append_signal_locked(stdCode, qty, bStandBy);
}

void WtEngine::append_signal_locked(const char* stdCode, double qty, bool bStandBy)
{
	/*
	 *	By Wesley @ 2021.12.16
	 *	这里发现一个问题，就是组合的理论成交价和策略的理论成交价不一致
	 *	检查以后发现，策略的理论成交价会在下一个tick更新
	 *	但是组合的理论成交价这一个tick就直接更新了
	 *	这就导致组合成交价永远比策略提前一个tick
	 *	这里做一个修正，等下一个tick进来，触发signal
	 *	如果是bar内触发的，bStandBy为false，则直接修改持仓
	 */
	double curPx = get_cur_price(stdCode);
	if(bStandBy || decimal::eq(curPx, 0.0))
	{
		SigInfo& sInfo = _sig_map[stdCode];
		sInfo._volume = qty;
		sInfo._gentime = (uint64_t)_cur_date * 1000000000 + (uint64_t)_cur_raw_time * 100000 + _cur_secs;
	}
	else
	{
		do_set_position(stdCode, qty);
	}

	/*
	double curPx = get_cur_price(stdCode);
	if(decimal::eq(curPx, 0.0))
	{
		SigInfo& sInfo = _sig_map[stdCode];
		sInfo._volume = qty;
		sInfo._gentime = (uint64_t)_cur_date * 1000000000 + (uint64_t)_cur_raw_time * 100000 + _cur_secs;
	}
	else
	{
		do_set_position(stdCode, qty);
	}
	*/
}

void WtEngine::do_set_position(const char* stdCode, double qty, double curPx /* = -1 */)
{
	// I03-NEW-115: find/insert and copy the owner under _pos_mtx; a reference
	// into the map would dangle once another insert relocates its storage.
	PosInfoPtr pInfo = get_or_create_position(stdCode);

	SpinLock lock(pInfo->_mtx);

	if(decimal::lt(curPx, 0))
		curPx = get_cur_price(stdCode);

	uint64_t curTm = (uint64_t)_cur_date * 10000 + _cur_time;
	uint32_t curTDate = _cur_tdate;

	if (decimal::eq(pInfo->_volume, qty))
		return;

	double diff = qty - pInfo->_volume;

	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);

	// I03-NEW-115: fundInfo is only touched inside short _fund_mtx scopes
	// (lock order: PosInfo::_mtx -> _fund_mtx).
	WTSFundStruct& fundInfo = _port_fund->fundInfo();

	if (decimal::gt(pInfo->_volume*diff, 0))//当前持仓和目标仓位方向一致, 增加一条明细, 增加数量即可
	{
		pInfo->_volume = qty;

		DetailInfo dInfo;
		dInfo._long = decimal::gt(qty, 0);
		dInfo._price = curPx;
		dInfo._volume = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		pInfo->_details.emplace_back(dInfo);

		// Fee of the opened increment (the logged quantity), not of the target.
		double fee = calc_fee(stdCode, curPx, abs(diff), 0);
		{
			SpinLock fundLock(_fund_mtx);
			fundInfo._fees += fee;
			fundInfo._balance -= fee;
		}

		log_trade(stdCode, dInfo._long, true, curTm, curPx, abs(diff), fee);
	}
	else
	{//持仓方向和目标仓位方向不一致, 需要平仓
		double left = abs(diff);

		pInfo->_volume = qty;
		if (decimal::eq(pInfo->_volume, 0))
			pInfo->_dynprofit = 0;
		uint32_t count = 0;
		for (auto it = pInfo->_details.begin(); it != pInfo->_details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			if (decimal::eq(dInfo._volume, 0))
			{
				count++;
				continue;
			}

			double maxQty = min(dInfo._volume, left);
			if (decimal::eq(maxQty, 0))
				continue;

			dInfo._volume -= maxQty;
			left -= maxQty;

			//if (dInfo._volume == 0)
			if (decimal::eq(dInfo._volume, 0))
				count++;

			double profit = (curPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;
			pInfo->_closeprofit += profit;
			pInfo->_dynprofit = pInfo->_dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放

			double fee = calc_fee(stdCode, curPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			{
				SpinLock fundLock(_fund_mtx);
				fundInfo._profit += profit;
				fundInfo._balance += profit;
				fundInfo._fees += fee;
				fundInfo._balance -= fee;
			}

			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, curPx, maxQty, fee);
			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, curPx, maxQty, profit, pInfo->_closeprofit);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		while (count > 0)
		{
			auto it = pInfo->_details.begin();
			pInfo->_details.erase(it);
			count--;
		}

		//最后, 如果还有剩余的, 则需要反手了
		//if (left > 0)
		if(decimal::gt(left, 0))
		{
			left = left * qty / abs(qty);

			DetailInfo dInfo;
			dInfo._long = qty > 0;
			dInfo._price = curPx;
			dInfo._volume = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			pInfo->_details.emplace_back(dInfo);

			//这里还需要写一笔成交记录
			double fee = calc_fee(stdCode, curPx, abs(left), 0);
			{
				SpinLock fundLock(_fund_mtx);
				fundInfo._fees += fee;
				fundInfo._balance -= fee;
			}

			log_trade(stdCode, dInfo._long, true, curTm, curPx, abs(left), fee);
		}
	}
}

void WtEngine::push_task(TaskItem task)
{
	{
		StdUniqueLock lock(_mtx_task);
		if (_terminated.load())
			return;

		_task_queue.push(std::move(task));
		if (_thrd_task == NULL)
		{
			_thrd_task.reset(new StdThread([this]{
				task_loop();
			}));
		}
	}

	_cond_task.notify_one();
}

void WtEngine::task_loop()
{
	TaskLoopThreadMarker threadMarker(this);
	for (;;)
	{
		TaskItem item;
		{
			StdUniqueLock lock(_mtx_task);
			_cond_task.wait(lock, [this]() {
				return _terminated.load() || !_task_queue.empty();
			});

			if (_task_queue.empty())
				return;

			item = std::move(_task_queue.front());
			_task_queue.pop();
		}

		item();
	}
}

void WtEngine::release()
{
	// The worker cannot wait for the external join lock: an external release
	// may already hold it while joining this worker. It only requests stop here;
	// the owning thread must later call release() to join the worker.
	if (g_task_loop_engine == this)
	{
		{
			StdUniqueLock lock(_mtx_task);
			_terminated.store(true);
		}
		_cond_task.notify_all();
		return;
	}

	StdUniqueLock releaseLock(_mtx_task_release);
	StdThreadPtr taskThread;
	{
		StdUniqueLock lock(_mtx_task);
		_terminated.store(true);
		taskThread = _thrd_task;
	}
	_cond_task.notify_all();

	if (taskThread != NULL && taskThread->joinable())
		taskThread->join();

	{
		StdUniqueLock lock(_mtx_task);
		if (_thrd_task == taskThread)
			_thrd_task.reset();
	}

	// After the task worker: stop the risk monitor before any derived engine
	// member it may reach through its context is torn down.
	stop_risk_monitor();
}

bool WtEngine::init_riskmon(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	if (!cfg->getBoolean("active"))
		return false;

	if (_risk_mon)
	{
		// Card 9c=A: a repeated initialization keeps the original monitor.
		WTSLogger::log_by_cat("risk", LL_ERROR, "Risk monitor is already initialized; repeated initialization rejected");
		return false;
	}

	std::string module = DLLHelper::wrap_module(cfg->getCString("module"));
	//先看工作目录下是否有对应模块
	std::string dllpath = WtHelper::getCWD() + module;
	//如果没有,则再看模块目录,即dll同目录下
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getInstDir() + module;

	DllHandle hInst = DLLHelper::load_library(dllpath.c_str());
	if (hInst == NULL)
	{
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon module {} loading failed", dllpath.c_str());
		return false;
	}

	FuncCreateRiskMonFact creator = (FuncCreateRiskMonFact)DLLHelper::get_symbol(hInst, "createRiskMonFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon module {} is not compatible", module.c_str());
		return false;
	}

	_risk_fact._module_inst = hInst;
	_risk_fact._module_path = module;
	_risk_fact._creator = creator;
	_risk_fact._remover = (FuncDeleteRiskMonFact)DLLHelper::get_symbol(hInst, "deleteRiskMonFact");
	_risk_fact._fact = _risk_fact._creator();
	if (_risk_fact._fact == NULL)
	{
		// The module stays resident with the other retained plugin owners; no
		// factory exists, so no monitor or wrapper can be installed.
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon module {} returned no factory", module.c_str());
		return false;
	}

	// Card 9a: only a module declaring a synchronous stop may later have its
	// monitor destroyed and the fund released; others are quarantined at stop.
	WtRiskMonPluginV2GetterFn capsGetter = (WtRiskMonPluginV2GetterFn)DLLHelper::get_symbol(hInst, WT_RISKMON_PLUGIN_V2_GETTER_SYMBOL);
	const WtRiskMonPluginCapsV2* caps = NULL;
	if (capsGetter != NULL)
	{
		try
		{
			caps = capsGetter(WT_RISKMON_PLUGIN_V2_ABI_MAJOR);
		}
		catch (...)
		{
			caps = NULL;
		}
	}
	_risk_caps = wt_riskmon_plugin::wellFormed(caps) ? caps : NULL;
	_risk_mon_stop_proven = wt_riskmon_plugin::declaresSyncStop(caps);
	_risk_mon_stopped = false;
	if (!_risk_mon_stop_proven)
		WTSLogger::log_by_cat("risk", LL_WARN,
			"Risk monitor module {} does not declare a synchronous stop (IRiskMonPluginV2); it will be quarantined at shutdown",
			module.c_str());

	const char* name = cfg->getCString("name");
	WtRiskMonitor* monitor = _risk_fact._fact->createRiskMonotor(name);
	if (monitor == NULL)
	{
		// Same failure contract as an incompatible module: report false and keep
		// the engine without a risk wrapper instead of calling init on NULL.
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon {} is not provided by module {}", name, module.c_str());
		// I03-NEW-062: no monitor exists; a verified deleter may free the factory.
		WtRiskMonPluginV2DestroyFactoryFn destroyFactory = wt_riskmon_plugin::factoryDestroyer(_risk_caps);
		if (destroyFactory != NULL)
		{
			try
			{
				destroyFactory(static_cast<void*>(_risk_fact._fact));
			}
			catch (...)
			{
			}
			_risk_fact._fact = NULL;
		}
		return false;
	}

	_risk_ctx = new WtRiskContextProxy(this);
	_risk_mon.reset(new WtRiskMonWrapper(monitor, _risk_fact._fact));
	_risk_mon->self()->init(_risk_ctx, cfg);

	return true;
}

void WtEngine::init_outputs()
{
	std::string folder = WtHelper::getPortifolioDir();
	std::string filename = folder + "trades.csv";
	_trade_logs.reset(new BoostFile());
	{
		bool isNewFile = !StdFile::exists(filename.c_str());
		_trade_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_trade_logs->write_file("code,time,direct,action,price,qty,fee\n");
		}
		else
		{
			_trade_logs->seek_to_end();
		}
	}

	filename = folder + "closes.csv";
	_close_logs.reset(new BoostFile());
	{
		bool isNewFile = !StdFile::exists(filename.c_str());
		_close_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_close_logs->write_file("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit\n");
		}
		else
		{
			_close_logs->seek_to_end();
		}
	}
}

void WtEngine::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee /* = 0.0 */)
{
	if (_trade_logs)
	{
		_trade_logs->write_file(fmt::format("{},{},{},{},{},{},{:.2f}\n", stdCode, curTime,
			(isLong ? "LONG" : "SHORT"), (isOpen ? "OPEN" : "CLOSE"), price, qty, fee));
	}
}

void WtEngine::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double totalprofit /* = 0 */)
{
	if (_close_logs)
	{
		_close_logs->write_file(fmt::format("{},{},{},{},{},{},{},{:.2f},{:.2f}\n", stdCode,
			(isLong ? "LONG" : "SHORT"), openTime, openpx, closeTime, closepx, qty, profit, totalprofit));
	}
}
