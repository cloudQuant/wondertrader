#include "TraderMocker.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSTradeDef.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/IBaseDataMgr.h"

#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"

#include <boost/bind.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdlib>
#include <filesystem>
namespace fs = std::filesystem;

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

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
// BUG-T6-009: identifies the adapter whose io worker or matcher is running on
// the current thread, so stop/join never self-joins (card 9d).
thread_local const TraderMocker* t_mocker_owner = NULL;

// Card 9a/9b: an instance that the factory remover cannot prove has stopped
// (removal requested from one of its own callback threads) is retained until
// process exit instead of being destroyed underneath the running thread. The
// registry is intentionally never destroyed so the retained objects stay
// reachable.
void quarantine_mocker(ITraderApi* trader)
{
	static std::mutex* mtx = new std::mutex();
	static std::vector<ITraderApi*>* retained = new std::vector<ITraderApi*>();
	std::lock_guard<std::mutex> lock(*mtx);
	retained->push_back(trader);
}
}

extern "C"
{
	EXPORT_FLAG ITraderApi* createTrader()
	{
		TraderMocker *instance = new TraderMocker();
		return instance;
	}

	EXPORT_FLAG void deleteTrader(ITraderApi* &trader)
	{
		if (NULL != trader)
		{
			TraderMocker* mocker = dynamic_cast<TraderMocker*>(trader);
			if (mocker != NULL && mocker->is_callback_thread())
			{
				// Thread exit cannot be confirmed from the thread itself: close
				// admission, request stop and keep the instance alive.
				mocker->release();
				mocker->detach_for_quarantine();
				quarantine_mocker(trader);
				std::fprintf(stderr, "[TraderMocker] deleteTrader called on a callback thread; instance quarantined until process exit\n");
				trader = NULL;
				return;
			}
			delete trader;
			trader = NULL;
		}
	}
}

namespace
{
uint32_t makeSplitSeed()
{
	try
	{
		return std::random_device{}();
	}
	catch (...)
	{
		return static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
	}
}

std::vector<uint32_t> splitVolumeWithRng(uint32_t vol, uint32_t minQty, uint32_t maxQty, std::mt19937& rng)
{
	std::vector<uint32_t> ret;
	// Invalid ranges have no valid split. In particular, minQty == 0 can
	// otherwise spin forever and the full uint32 range makes length wrap to 0.
	if (minQty == 0 || maxQty < minQty)
		return ret;
	uint32_t length = maxQty - minQty + 1;
	if (vol <= minQty)
	{
		ret.emplace_back(vol);
	}
	else
	{
		uint32_t left = vol;
		while (left > 0)
		{
			uint32_t curVol = minQty + rng() % length;

			if (curVol >= left)
				curVol = left;

			if (curVol == 0)
				continue;

			ret.emplace_back(curVol);
			left -= curVol;
		}
	}

	return ret;
}
}

// Preserve the pre-existing free symbol used by standalone callers. Real
// matching uses the per-TraderMocker engine below.
std::vector<uint32_t> splitVolume(uint32_t vol, uint32_t minQty = 1, uint32_t maxQty = 100)
{
	std::mt19937 rng(makeSplitSeed());
	return splitVolumeWithRng(vol, minQty, maxQty, rng);
}

std::vector<uint32_t> TraderMocker::split_volume(uint32_t vol, uint32_t minQty, uint32_t maxQty)
{
	std::lock_guard<std::mutex> lock(_split_rng_mutex);
	return splitVolumeWithRng(vol, minQty, maxQty, _split_rng);
}

TraderMocker::TraderMocker()
	: _split_rng(makeSplitSeed())
	, _terminated(false)
	, _listener(NULL)
	, _orders(NULL)
	, _trades(NULL)
	, _ticks(NULL)
	, _awaits(NULL)
	, _max_tick_time(0)
	, _last_match_time(0)
, _b_socket(NULL)
	{
	_auto_order_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
	_auto_trade_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 300);
	_auto_entrust_id = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(20200101, 0)) / 1000 * 100);
}


TraderMocker::~TraderMocker()
{
	if (on_own_thread())
	{
		// Destroying the object from its own io worker or matcher would leave that
		// thread running on freed memory and cannot join it. Fail fast; the factory
		// remover deleteTrader quarantines such a request instead.
		std::fprintf(stderr, "[TraderMocker] Direct delete on a callback thread is unsupported; use deleteTrader\n");
		std::abort();
	}
	release();

	if (_orders)
		_orders->release();

	if (_trades)
		_trades->release();

	if (_ticks)
		_ticks->release();
}

uint32_t TraderMocker::makeTradeID()
{
	return ++_auto_trade_id;
}

uint32_t TraderMocker::makeOrderID()
{
	return ++_auto_order_id;
}

bool TraderMocker::makeEntrustID(char* buffer, int length)
{
	if (buffer == NULL || length == 0)
		return false;

	try
	{
		fmtutil::format_to(buffer, "me.{}.{}.{}", TimeUtils::getCurDate(), _mocker_id, _auto_entrust_id++);
		return true;
	}
	catch (...)
	{

	}

	return false;
}

int TraderMocker::orderInsert(WTSEntrust* entrust)
{	
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	if (entrust == NULL)
	{
		return 0;
	}

	entrust->retain();
	// A stopped io_context can destroy queued handlers without running them.
	std::shared_ptr<WTSEntrust> retained(entrust, [](WTSEntrust* item) { item->release(); });
	boost::asio::post(_io_service, [this, retained](){
		WTSEntrust* entrust = retained.get();
		// Lock order for state shared with the matcher: _mtx_awaits, then _mutex_api.
		StdUniqueLock stateLock(_mtx_awaits);
		StdUniqueLock lock(_mutex_api);

		WTSContractInfo* ct = entrust->getContractInfo();
		if(ct == NULL) 
			ct = _bd_mgr->getContract(entrust->getCode(), entrust->getExchg());

		/*
		 *	1、开仓无需检查
		 *	2、平仓要先检查可平
		 *	3、检查通过了,平仓还要冻结持仓
		 *	4、还要考虑国际期货的问题
		 */

		bool bPass = false;
		std::string msg;
		do 
		{
			if (ct == NULL)
			{
				bPass = false;
				msg = "品种不存在";
				break;
			}
			WTSCommodityInfo* commInfo = ct->getCommInfo();

			//检查价格类型的合法性
			if (entrust->getPriceType() == WPT_ANYPRICE && commInfo->getPriceMode() == PM_Limit)
			{
				bPass = false;
				msg = "价格类型不合法";
				break;
			}

			//检查数量的合法性
			if ((commInfo->getCategoty() == CC_Stock) && (entrust->getOffsetType() == WOT_OPEN) && !decimal::eq(decimal::mod(entrust->getVolume(), 100), 0))
			{
				bPass = false;
				msg = "股票买入数量必须为100的整数倍";
				break;
			}

			//检查方向的合法性
			if(!commInfo->canShort() && entrust->getDirection() == WDT_SHORT)
			{
				bPass = false;
				msg = "股票不能做空";
				break;
			}

			//检查价格的合法性
			if(!decimal::eq(entrust->getPrice(), 0))
			{
				double pricetick = commInfo->getPriceTick();
				[[maybe_unused]] double v = entrust->getPrice() / pricetick;

				if (!decimal::eq(decimal::mod(entrust->getPrice(), pricetick), 0))	//整除的检查方式,先小数相除得到商,然后商取整以后,再跟原来的商相减,如果等于0,则是整除,否则是
				{
					bPass = false;
					msg = "委托价格不合法";
					break;
				}
			}


			//开仓直接通过,不检查资金
			if (entrust->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}

			//如果不需要开平,则直接通过,主要针对国际期货
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}
			
			//如果区分平昨平今,而委托的是平昨,则直接拒绝,因为mocker为了简化处理,不考虑昨仓
			if (commInfo->getCoverMode() == CM_CoverToday && (entrust->getOffsetType() == WOT_CLOSE || entrust->getOffsetType() == WOT_CLOSEYESTERDAY))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//如果没有持仓或者持仓不够,也要
			auto it = _positions.find(ct->getFullCode());
			if(it == _positions.end())
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			PosItem& pItem = (PosItem&)it->second;
			bool isLong = entrust->getDirection() == WDT_LONG;

			double validQty = isLong ? (pItem._long._volume - pItem._long._frozen) : (pItem._short._volume - pItem._short._frozen);
			if(decimal::lt(validQty, entrust->getVolume()))
			{
				bPass = false;
				msg = "没有足够的可平仓位";
				break;
			}

			//冻结持仓
			if(isLong)
			{
				pItem._long._frozen += entrust->getVolume();
			}
			else
			{
				pItem._short._frozen += entrust->getVolume();
			}

			bPass = true;
			msg = "下单成功";

		} while (false);
		
		if(bPass)
		{
			WTSOrderInfo* ordInfo = WTSOrderInfo::create();
			ordInfo->setContractInfo(ct);
			ordInfo->setCode(entrust->getCode());
			ordInfo->setExchange(entrust->getExchg());
			ordInfo->setDirection(entrust->getDirection());
			ordInfo->setOffsetType(entrust->getOffsetType());
			ordInfo->setUserTag(entrust->getUserTag());
			ordInfo->setPrice(entrust->getPrice());
			thread_local static char str[64];
			fmtutil::format_to(str, "mo.{}.{}", _mocker_id, makeOrderID());
			ordInfo->setOrderID(str);
			ordInfo->setStateMsg(msg.c_str());
			ordInfo->setOrderState(WOS_NotTraded_Queuing);
			ordInfo->setOrderTime(TimeUtils::getLocalTimeNow());
			ordInfo->setVolume(entrust->getVolume());
			ordInfo->setVolLeft(entrust->getVolume());
			ordInfo->setPriceType(entrust->getPriceType());
			ordInfo->setOrderFlag(entrust->getOrderFlag());

			ITraderSpi* listener = get_listener();
			if (listener != NULL)
			{
				listener->onRspEntrust(entrust, NULL);
				listener->onPushOrder(ordInfo);
			}

			_codes.insert(ct->getFullCode());

			write_log(listener, LL_INFO, "共有{}个品种有待撮合订单", _codes.size());

			if (_orders == NULL)
				_orders = WTSArray::create();
			_orders->append(ordInfo, false);

			if (_awaits == NULL)
				_awaits = OrderCache::create();

			_awaits->add(ordInfo->getOrderID(), ordInfo, true);

			save_positions();
		}
		else
		{
			WTSError* err = WTSError::create(WEC_ORDERINSERT, msg.c_str());
			ITraderSpi* listener = get_listener();
			if (listener != NULL)
			{
				listener->onRspEntrust(entrust, err);
			}
			err->release();
		}
	});

	return 0;
}

int32_t TraderMocker::match_once()
{
	// The io worker mutates _orders/_codes/_ticks/_trades/_positions/_awaits
	// under the same state lock, so hold it for the whole matching pass.
	StdUniqueLock stateLock(_mtx_awaits);
	if (_terminated.load(std::memory_order_acquire) || _orders == NULL || _orders->size() == 0 || _ticks == NULL)
		return 0;
	
	int32_t count = 0;
	for (const std::string& fullcode : _codes)
	{
		if (_terminated.load(std::memory_order_acquire))
			return count;

		std::string code, exchg;
		auto pos = fullcode.find(".");
		exchg = fullcode.substr(0, pos);
		code = fullcode.substr(pos + 1);

		WTSContractInfo* ct = _bd_mgr->getContract(code.c_str(), exchg.c_str());
		if (ct == NULL)
			continue;

		WTSCommodityInfo* commInfo = ct->getCommInfo();

		WTSTickData* curTick = (WTSTickData*)_ticks->grab(fullcode);
		if (curTick && strcmp(curTick->code(), ct->getCode())==0)
		{
			[[maybe_unused]] uint64_t tickTime = (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime();
			if (decimal::gt(curTick->price(), 0) /*&& tickTime >= _last_match_time*/)
			{
				//开始处理订单
				//处理记录
				std::vector<std::string> to_erase;

				for (auto it = _awaits->begin(); it != _awaits->end(); it++)
				{
					WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
					if (ordInfo->getVolLeft() == 0 ||
						strcmp(ordInfo->getCode(), ct->getCode()) != 0 ||
						strcmp(ordInfo->getExchg(), ct->getExchg()) != 0 ||
						strcmp(ct->getCode(), curTick->code()) != 0 ||
						strcmp(ct->getExchg(), curTick->exchg()) != 0)
						continue;

					bool isBuy = (ordInfo->getDirection() == WDT_LONG && ordInfo->getOffsetType() == WOT_OPEN) || (ordInfo->getDirection() != WDT_LONG && ordInfo->getOffsetType() != WOT_OPEN);

					double uPrice, uVolume;
					if (isBuy)
					{
						uPrice = curTick->askprice(0);
						uVolume = curTick->askqty(0);
					}
					else
					{
						uPrice = curTick->bidprice(0);
						uVolume = curTick->bidqty(0);
					}

					if (decimal::eq(uVolume, 0))
						continue;					

					if (_use_newpx)
					{
						uPrice = curTick->price();
					}

					if (decimal::eq(uPrice, 0))
						continue;

					double target = ordInfo->getPrice();
					//买入的时候,委托价格小于最新价则不成交,卖出的时候,委托价大于最新价则不成交
					if (ordInfo->getPriceType() == WPT_LIMITPRICE && ((isBuy && decimal::lt(target, uPrice)) || (!isBuy && decimal::gt(target, uPrice))))
						continue;

					count++;

					double maxVolume = min(uVolume, ordInfo->getVolLeft());
					std::vector<uint32_t> ayVol = split_volume((uint32_t)maxVolume, (uint32_t)_min_qty, (uint32_t)_max_qty);
					for (uint32_t curVol : ayVol)
					{

						WTSTradeInfo* trade = WTSTradeInfo::create(curTick->code(), curTick->exchg());
						trade->setDirection(ordInfo->getDirection());
						trade->setOffsetType(ordInfo->getOffsetType());
						trade->setContractInfo(ct);

						trade->setPrice(uPrice);
						trade->setVolume(curVol);

						trade->setRefOrder(ordInfo->getOrderID());

						char str[64];
						fmtutil::format_to(str, "mt.{}.{}", _mocker_id, makeTradeID());
						trade->setTradeID(str);

						trade->setTradeTime(TimeUtils::getLocalTimeNow());
						trade->setUserTag(ordInfo->getUserTag());

						//更新订单数据
						ordInfo->setVolLeft(ordInfo->getVolLeft() - curVol);
						ordInfo->setVolTraded(ordInfo->getVolTraded() + curVol);
						if (decimal::eq(ordInfo->getVolLeft(), 0))
						{
							ordInfo->setOrderState(WOS_AllTraded);
							ordInfo->setStateMsg("AllTrd");
							to_erase.emplace_back(ordInfo->getOrderID());
						}
						else
						{
							ordInfo->setOrderState(WOS_PartTraded_Queuing);
							ordInfo->setStateMsg("PartTrd");
						}

						PosItem& pItem = _positions[ct->getFullCode()];
						//第一次的话要给代码和交易所赋值
						if(strlen(pItem._code) == 0)
						{
							strcpy(pItem._code, ct->getCode());
							strcpy(pItem._exchg, ct->getExchg());
						}

						if(commInfo->getCoverMode() == CM_None)
						{

						}
						else
						{
							if (ordInfo->getDirection() == WDT_LONG)
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._long._volume += curVol;
								}
								else
								{
									pItem._long._volume -= curVol;
									pItem._long._frozen -= curVol;
								}
							}
							else
							{
								if (ordInfo->getOffsetType() == WOT_OPEN)
								{
									pItem._short._volume += curVol;
								}
								else
								{
									pItem._short._volume -= curVol;
									pItem._short._frozen -= curVol;
								}
							}
						}
						

		ITraderSpi* listener = get_listener();
		if (listener)
						{
							StdUniqueLock lock(_mutex_api);
			listener->onPushOrder(ordInfo);
			listener->onPushTrade(trade);
						}

						if (_trades == NULL)
							_trades = WTSArray::create();

						_trades->append(trade, false);
					}
				}

				if (count > 0)
				{
					//write_log(_listener,LL_INFO, "[TraderMocker]触发 %s.%s 开多 %u 条,价格:%u", tick->exchg(), tick->code(), iCount, uPrice);
					for (const std::string& oid : to_erase)
					{
						_awaits->remove(oid);
					}
				}
			}
		}

		if(curTick)
			curTick->release();
	}

	_last_match_time = _max_tick_time;

	_ticks->clear();

	if(count > 0)
		save_positions();

	return count;
}

//////////////////////////////////////////////////////////////////////////

bool TraderMocker::init(WTSVariant *params)
{
	{
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		if (_requests_closed)
			return false;
		// Card 9c: reject repeated initialization and keep the original instance.
		if (_initialized)
		{
			std::fprintf(stderr, "[TraderMocker] Repeated init rejected\n");
			return false;
		}
	}
	if (params == NULL)
		return false;

	double maxQty = params->getDouble("maxqty");
	double minQty = params->getDouble("minqty");
	// Preserve the existing near-zero defaults, then require all other values
	// to convert safely to the nonzero integer range used by match_once.
	const double uint32Limit = 4294967296.0;
	const bool finiteQty = std::isfinite(minQty) && std::isfinite(maxQty);
	if (finiteQty && decimal::eq(maxQty, 0)) maxQty = 100.0;
	if (finiteQty && decimal::eq(minQty, 0)) minQty = 1.0;
	if (!finiteQty ||
		minQty < 1.0 || maxQty < 1.0 || minQty >= uint32Limit ||
		maxQty >= uint32Limit ||
		static_cast<uint32_t>(maxQty) < static_cast<uint32_t>(minQty))
	{
		ITraderSpi* listener = get_listener();
		if (listener != NULL)
			write_log(listener, LL_ERROR, "[TraderMocker] Invalid quantity range: minqty={} maxqty={}", minQty, maxQty);
		else
			std::fprintf(stderr, "[TraderMocker] Invalid quantity range: minqty=%g maxqty=%g\n", minQty, maxQty);
		return false;
	}

	// Optional deterministic sequence for a fresh mocker. With no seed, the
	// constructor's per-instance non-fixed seed remains in use.
	if (params->has("rng_seed"))
	{
		std::lock_guard<std::mutex> lock(_split_rng_mutex);
		_split_rng.seed(params->getUInt32("rng_seed"));
	}

	_millisecs = params->getUInt32("span");
	_use_newpx = params->getBoolean("newpx");
	_mocker_id = params->getUInt32("mockerid");
	_max_qty = maxQty;
	_min_qty = minQty;

	_udp_port = params->getInt32("udp_port");

	boost::asio::ip::address addr = boost::asio::ip::make_address("0.0.0.0");
	_broad_ep = boost::asio::ip::udp::endpoint(addr, _udp_port);

	//加载持仓数据
	std::stringstream ss;
	ss << "./mocker_" << _mocker_id << "/";
	std::string path = ss.str();
	fs::create_directories(path.c_str());

	_pos_file = path;
	_pos_file += "positions.json";

	// Release can run while validation or directory creation is in progress.
	// Never report a successful reinitialization after admission was closed.
	{
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		if (_requests_closed || _initialized)
			return false;
		_initialized = true;
	}
	return true;
}

void TraderMocker::load_positions()
{
	if (!fs::exists(_pos_file.c_str()))
		return;

	std::string json;
	StdFile::read_file_content(_pos_file.c_str(), json);

	rj::Document root;
	root.Parse(json.c_str());
	if (root.HasParseError())
		return;

	if(root.HasMember("positions"))
	{//读取仓位
		[[maybe_unused]] double total_profit = 0;
		[[maybe_unused]] double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* exchg = pItem["exchg"].GetString();
				const char* code = pItem["code"].GetString();
				WTSContractInfo* ct = _bd_mgr->getContract(code, exchg);
				if (ct == NULL)
					continue;

				PosItem& pInfo = _positions[ct->getFullCode()];
				strcpy(pInfo._code, ct->getCode());
				strcpy(pInfo._exchg, ct->getExchg());
				
				pInfo._long._volume = pItem["long"]["volume"].GetDouble();

				pInfo._short._volume = pItem["short"]["volume"].GetDouble();
			}
		}
	}

	write_log(get_listener(), LL_INFO, "[TraderMocker]共加载{}条持仓数据", _positions.size());
}

void TraderMocker::save_positions()
{
	rj::Document root(rj::kObjectType);

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& v : _positions)
		{
			[[maybe_unused]] const char* fullcode = v.first.c_str();
			const PosItem& pInfo = v.second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("exchg", rj::Value(pInfo._exchg, allocator), allocator);
			pItem.AddMember("code", rj::Value(pInfo._code, allocator), allocator);

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._long._volume, allocator);

				pItem.AddMember("long", dItem, allocator);
			}

			{
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("volume", pInfo._short._volume, allocator);

				pItem.AddMember("short", dItem, allocator);
			}


			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{
        rj::StringBuffer sb;
        rj::PrettyWriter<rj::StringBuffer> writer(sb);
        root.Accept(writer);
        StdFile::write_file_content(_pos_file.c_str(), sb.GetString());

	}
}

bool TraderMocker::on_own_thread() const
{
	return t_mocker_owner == this;
}

void TraderMocker::request_stop_from_callback()
{
	// Card 9d: a synchronous re-entry from a callback only closes admission and
	// requests stop. The join is completed by a later release() or destructor on
	// an external thread, or the instance is quarantined by deleteTrader.
	{
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		_requests_closed = true;
	}
	_terminated.store(true, std::memory_order_release);
	_io_service.stop();
}

void TraderMocker::join_matcher_locked()
{
	// Caller holds _release_mutex and is not the matcher thread.
	StdThreadPtr matcher;
	{
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		matcher.swap(_thrd_match);
	}
	if (matcher && matcher->joinable())
		matcher->join();
	_match_started.store(false, std::memory_order_release);
}

void TraderMocker::detach_for_quarantine()
{
	// Never block here: an external release() may hold the lock while joining
	// this very thread, in which case that release() completes the join.
	std::unique_lock<std::mutex> releaseLock(_release_mutex, std::try_to_lock);
	if (!releaseLock.owns_lock())
		return;
	StdThreadPtr matcher;
	{
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		matcher.swap(_thrd_match);
	}
	if (matcher && matcher->joinable())
		matcher->detach();
	if (_thrd_worker && _thrd_worker->joinable())
		_thrd_worker->detach();
	_match_started.store(false, std::memory_order_release);
}

void TraderMocker::release()
{
	if (on_own_thread())
	{
		request_stop_from_callback();
		return;
	}

	// Concurrent external release()/disconnect()/destructor calls are serialized;
	// the first completes the stop/join, later ones are no-ops.
	std::lock_guard<std::mutex> releaseLock(_release_mutex);
	{
		// Every admitted request retains and posts while holding this mutex, so
		// once it is closed no new work can reach the io worker.
		std::lock_guard<std::mutex> requestLock(_request_mutex);
		_requests_closed = true;
	}
	_terminated.store(true, std::memory_order_release);
	if (_released)
		return;

	join_matcher_locked();

	if (_thrd_worker)
	{
		_io_service.stop();
		if (_thrd_worker->joinable())
			_thrd_worker->join();
	}
	_thrd_worker.reset();

	// The socket belongs to this adapter. Stop and join the io worker before
	// closing it so no in-flight receive handler can access the socket object.
	if (_b_socket != NULL)
	{
		boost::system::error_code ec;
		_b_socket->close(ec);
		delete _b_socket;
		_b_socket = NULL;
	}

	// BUG-T6-009: both threads are joined, so no matcher pass or cancel handler
	// can touch the await cache. Return its references to the still-pending
	// orders exactly once; orders already matched or canceled were removed (and
	// released) by match_once()/orderAction() and are no longer in the cache.
	// The creation references remain owned by _orders and are released by the
	// destructor.
	{
		StdUniqueLock lock(_mtx_awaits);
		if (_awaits != NULL)
		{
			_awaits->release();
			_awaits = NULL;
		}
	}
	_released = true;
}

void TraderMocker::registerSpi(ITraderSpi *listener)
{
	_listener = listener;
	// Keep the base-data manager until release() has stopped the worker threads.
	// In-flight mocker tasks still use it after the SPI is detached.
	if (listener)
		_bd_mgr = listener->getBaseDataMgr();
}

void TraderMocker::reconn_udp()
{
	if (_b_socket != NULL)
	{
		_b_socket->close();
		delete _b_socket;
		_b_socket = NULL;
	}

	_b_socket = new boost::asio::ip::udp::socket(_io_service);

	_b_socket->open(_broad_ep.protocol());
	_b_socket->set_option(boost::asio::ip::udp::socket::reuse_address(true));
	_b_socket->set_option(boost::asio::ip::udp::socket::broadcast(true));
	_b_socket->bind(_broad_ep);


	_b_socket->async_receive_from(boost::asio::buffer(_b_buffer.data(), _b_buffer.size()), _broad_ep,
		boost::bind(&TraderMocker::handle_read, this,
		boost::asio::placeholders::error,
		boost::asio::placeholders::bytes_transferred, true));
}

void TraderMocker::connect()
{
	std::unique_lock<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
	{
		requestLock.unlock();
		std::fprintf(stderr, "[TraderMocker] Cannot connect after release\n");
		return;
	}
	if (_thrd_worker)
	{
		requestLock.unlock();
		std::fprintf(stderr, "[TraderMocker] Already connected\n");
		return;
	}
	reconn_udp();

	_thrd_worker.reset(new StdThread([this]() {
		t_mocker_owner = this;
		_io_service.run();
		t_mocker_owner = NULL;
	}));

	boost::asio::post(_io_service, [this](){
		StdUniqueLock stateLock(_mtx_awaits);
		StdUniqueLock lock(_mutex_api);

		load_positions();

		if (ITraderSpi* listener = get_listener())
			listener->handleEvent(WTE_Connect, 0);
	});
}

void TraderMocker::disconnect()
{
	_terminated.store(true, std::memory_order_release);
	// Card 9d: from a callback the matcher only observes the stop request; it is
	// joined later from an external thread.
	if (on_own_thread())
		return;

	std::lock_guard<std::mutex> releaseLock(_release_mutex);
	join_matcher_locked();
}

bool TraderMocker::isConnected()
{
	return _match_started.load(std::memory_order_acquire);
}

int TraderMocker::login(const char* /*user*/, const char* /*pass*/, const char* /*productInfo*/)
{
	// Do not start a matcher or queue a successful login after release has
	// closed admission.  Replacing a live std::thread would terminate.
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed || _terminated.load(std::memory_order_acquire) || _thrd_match)
		return -1;
	_thrd_match.reset(new StdThread([this]() {
		t_mocker_owner = this;
		while (!_terminated.load(std::memory_order_acquire))
		{
			match_once();

			//等待5毫秒
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		t_mocker_owner = NULL;
	}));
	_match_started.store(true, std::memory_order_release);

	boost::asio::post(_io_service, [this](){
		StdUniqueLock lock(_mutex_api);

		if (ITraderSpi* listener = get_listener())
			listener->onLoginResult(true, "", TimeUtils::getCurDate());
	});

	return 0;
}

int TraderMocker::logout()
{
	return 0;
}

int TraderMocker::orderAction(WTSEntrustAction* action)
{
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	action->retain();
	std::shared_ptr<WTSEntrustAction> retained(action, [](WTSEntrustAction* item) { item->release(); });
	
	boost::asio::post(_io_service, [this, retained](){
		WTSEntrustAction* action = retained.get();
		StdUniqueLock lck(_mtx_awaits);	//一定要把awaits锁起来,不然可能会导致一边撮合一边撤单
		const std::string order_id = action->getOrderID();
		WTSOrderInfo* ordInfo = _awaits == NULL ? NULL : (WTSOrderInfo*)_awaits->grab(order_id.c_str());

		/*
		 *	撤单也要考虑几个问题
		 *	1、是否处于可以撤销的状态
		 *	2、如果是开仓,则直接撤销
		 *	3、如果是平仓,要释放冻结
		 */
		if(ordInfo == NULL)
		{
			ITraderSpi* listener = get_listener();
			write_log(listener, LL_ERROR, "订单{}不存在或者已完成", order_id);
			WTSError* err = WTSError::create(WEC_ORDERCANCEL, "订单不存在或者处于不可撤销状态");
			if (listener)
				listener->onTraderError(err);
			err->release();
			return;
		}

		WTSContractInfo* ct = ordInfo->getContractInfo();
		WTSCommodityInfo* commInfo = ct->getCommInfo();

		[[maybe_unused]] bool bPass = false;
		do 
		{
			//开仓委托直接撤单
			if (ordInfo->getOffsetType() == WOT_OPEN)
			{
				bPass = true;
				break;
			}
			
			//不区分开平的,也直接撤销
			if (commInfo->getCoverMode() == CM_None)
			{
				bPass = true;
				break;
			}

			//释放冻结持仓
			PosItem& pItem = _positions[ct->getFullCode()];
			bool isLong = ordInfo->getDirection() == WDT_LONG;
			if(isLong)
			{
				pItem._long._frozen -= ordInfo->getVolLeft();
			}
			else
			{
				pItem._short._frozen -= ordInfo->getVolLeft();
			}
			bPass = true;

		} while (false);

		ordInfo->setStateMsg("撤单成功");
		ordInfo->setOrderState(WOS_Canceled);
		//ordInfo->setVolLeft(0);	

		if (ITraderSpi* listener = get_listener())
		{
			StdUniqueLock lock(_mutex_api);
			listener->onPushOrder(ordInfo);
		}

		ordInfo->release();
		_awaits->remove(order_id.c_str());

		save_positions();
	});

	return 0;
}

int TraderMocker::queryAccount()
{
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	boost::asio::post(_io_service, [this](){
		WTSArray* ay = WTSArray::create();
		WTSAccountInfo* accountInfo = WTSAccountInfo::create();
		accountInfo->setCurrency("CNY");
		accountInfo->setBalance(0);
		accountInfo->setPreBalance(0);
		accountInfo->setCloseProfit(0);
		accountInfo->setMargin(0);
		accountInfo->setAvailable(0);
		accountInfo->setCommission(0);
		accountInfo->setFrozenMargin(0);
		accountInfo->setFrozenCommission(0);
		accountInfo->setDeposit(0);
		accountInfo->setWithdraw(0);
		accountInfo->setDynProfit(0);

		ay->append(accountInfo, false);

		if (ITraderSpi* listener = get_listener())
		{
			StdUniqueLock lock(_mutex_api);
			listener->onRspAccount(ay);
		}

		ay->release();

	});
	return 0;
}

int TraderMocker::queryPositions()
{
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	boost::asio::post(_io_service, [this](){
		StdUniqueLock stateLock(_mtx_awaits);
		WTSArray* ayPos = WTSArray::create();

		for(auto& v : _positions)
		{
			const PosItem& pItem = v.second;

			WTSContractInfo* ct = _bd_mgr->getContract(pItem._code, pItem._exchg);
			if(ct == NULL)
				continue;

			WTSCommodityInfo* commInfo = ct->getCommInfo();

			if(pItem._long._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_LONG);
				pInfo->setNewPosition(pItem._long._volume);
				pInfo->setAvailNewPos(pItem._long._volume - pItem._long._frozen);

				ayPos->append(pInfo, false);
			}

			if (pItem._short._volume > 0)
			{
				WTSPositionItem* pInfo = WTSPositionItem::create(pItem._code, commInfo->getCurrency(), pItem._exchg);
				pInfo->setContractInfo(ct);
				pInfo->setDirection(WDT_SHORT);
				pInfo->setNewPosition(pItem._short._volume);
				pInfo->setAvailNewPos(pItem._short._volume - pItem._short._frozen);

				ayPos->append(pInfo, false);
			}
		}

		if (ITraderSpi* listener = get_listener())
		{
			StdUniqueLock lock(_mutex_api);
			listener->onRspPosition(ayPos);
		}
		ayPos->release();
	});

	return 0;
}

int TraderMocker::queryOrders()
{
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	boost::asio::post(_io_service, [this](){
		StdUniqueLock stateLock(_mtx_awaits);
		StdUniqueLock lock(_mutex_api);

		if (ITraderSpi* listener = get_listener())
			listener->onRspOrders(_orders);
	});

	return 0;
}

int TraderMocker::queryTrades()
{
	std::lock_guard<std::mutex> requestLock(_request_mutex);
	if (_requests_closed)
		return -1;
	boost::asio::post(_io_service, [this](){
		StdUniqueLock stateLock(_mtx_awaits);
		StdUniqueLock lock(_mutex_api);

		if (ITraderSpi* listener = get_listener())
			listener->onRspTrades(_trades);
	});

	return 0;
}

void TraderMocker::handle_read(const boost::system::error_code& e, std::size_t bytes_transferred, bool isBroad /* = true */)
{
	if (e)
	{
	write_log(get_listener(), LL_ERROR, "[TraderMocker]UDP行情接收出错:{}({})", e.message().c_str(), e.value());

		if (!_terminated.load(std::memory_order_acquire))
		{
			std::this_thread::sleep_for(std::chrono::seconds(2));
			reconn_udp();
			return;
		}
	}

	if (_terminated.load(std::memory_order_acquire))
		return;

	// Empty UDP datagrams are valid transport events, but contain no packet to parse.
	// Keep the receive chain alive so the next market-data packet is still delivered.
	if (bytes_transferred > 0)
		extract_buffer(bytes_transferred, isBroad);

	if (isBroad && _b_socket && !_terminated.load(std::memory_order_acquire))
	{
		_b_socket->async_receive_from(boost::asio::buffer(_b_buffer.data(), _b_buffer.size()), _broad_ep,
			boost::bind(&TraderMocker::handle_read, this,
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred, true));
	}
}

#define UDP_MSG_PUSHTICK	0x200
#pragma pack(push,1)

typedef struct UDPPacketHead
{
	uint32_t		_type;
} UDPPacketHead;
//UDP请求包
typedef struct _UDPReqPacket : UDPPacketHead
{
	char			_data[1020];
} UDPReqPacket;

//UDPTick数据包
template <typename T>
struct UDPDataPacket : UDPPacketHead
{
	T			_data;
};
#pragma pack(pop)
typedef UDPDataPacket<WTSTickStruct>	UDPTickPacket;
void TraderMocker::extract_buffer(uint32_t length, bool /*isBroad*/ /* = true */)
{
	if (length < sizeof(UDPPacketHead))
		return;

	UDPPacketHead header;
	memcpy(&header, _b_buffer.data(), sizeof(header));

	if (header._type == UDP_MSG_PUSHTICK)
	{
		// A short UDP datagram leaves the rest of _b_buffer untouched from the
		// preceding datagram. Never interpret that stale tail as part of a tick.
		if (length < sizeof(UDPTickPacket))
			return;

		// The packed wire payload starts four bytes after the header and is not
		// necessarily aligned for WTSTickStruct. Copy into an aligned object.
		WTSTickStruct tick;
		memcpy(&tick, _b_buffer.data() + sizeof(UDPPacketHead), sizeof(tick));
		thread_local static char fullcode[64] = { 0 };
		fmtutil::format_to(fullcode, "{}.{}", tick.exchg, tick.code);
		StdUniqueLock stateLock(_mtx_awaits);
		auto it = _codes.find(fullcode);
		if (it == _codes.end())
			return;

		WTSTickData* curTick = WTSTickData::create(tick);
		
		if (_ticks == NULL)
			_ticks = TickCache::create();

		_ticks->add(fullcode, curTick, false);

		_max_tick_time = max(_max_tick_time, (uint64_t)curTick->actiondate() * 1000000000 + curTick->actiontime());
	}
}
