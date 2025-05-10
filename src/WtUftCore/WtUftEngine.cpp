/*!
 * \file WtUftEngine.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief UFT策略引擎实现文件
 * \details 实现了WtUftEngine类的各种方法，负责超高频交易(UFT)策略的执行和管理
 */
#define WIN32_LEAN_AND_MEAN

#include "WtUftEngine.h"
#include "WtUftTicker.h"
#include "WtUftDtMgr.h"
#include "TraderAdapter.h"
#include "WtHelper.h"

#include "../Share/decimal.h"
#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSContractInfo.hpp"

#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

/**
 * @brief 构造函数
 * @details 初始化UFT引擎对象，获取当前系统时间并设置初始化参数
 */
WtUftEngine::WtUftEngine()
	: _cfg(NULL)
	, _tm_ticker(NULL)
	, _notifier(NULL)
{
	// 获取当前系统日期和时间
	TimeUtils::getDateTime(_cur_date, _cur_time);
	// 提取秒数部分
	_cur_secs = _cur_time % 100000;
	// 计算分钟时间
	_cur_time /= 100000;
	_cur_raw_time = _cur_time;
	_cur_tdate = _cur_date;

	// 设置全局时间
	WtHelper::setTime(_cur_date, _cur_time, _cur_secs);
}


/**
 * @brief 析构函数
 * @details 清理引擎资源，释放内存，关闭时间计时器
 */
WtUftEngine::~WtUftEngine()
{
	// 停止并释放时间计时器
	if (_tm_ticker)
	{
		_tm_ticker->stop();
		delete _tm_ticker;
		_tm_ticker = NULL;
	}

	// 释放配置对象
	if (_cfg)
		_cfg->release();
}

/**
 * @brief 设置当前日期和时间
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMM
 * @param curSecs 当前秒数，包含毫秒，默认为0
 * @param rawTime 原始时间，默认为0（使用curTime）
 * @details 设置引擎内部时间并更新全局时间
 */
void WtUftEngine::set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs /* = 0 */, uint32_t rawTime /* = 0 */)
{
	// 设置引擎内部日期和时间
	_cur_date = curDate;
	_cur_time = curTime;
	_cur_secs = curSecs;

	// 如果原始时间未指定，使用当前时间
	if (rawTime == 0)
		rawTime = curTime;

	_cur_raw_time = rawTime;

	// 更新全局时间
	WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs);
}

/**
 * @brief 设置交易日期
 * @param curTDate 当前交易日期，格式YYYYMMDD
 * @details 设置引擎内部交易日期并更新全局交易日期
 */
void WtUftEngine::set_trading_date(uint32_t curTDate)
{
	// 设置引擎内部交易日期
	_cur_tdate = curTDate;

	// 更新全局交易日期
	WtHelper::setTDate(curTDate);
}

/**
 * @brief 获取品种信息
 * @param stdCode 标准化合约代码，格式为“交易所.合约代码”
 * @return 品种信息指针，如果找不到则返回NULL
 * @details 根据标准化合约代码解析出交易所和合约代码，然后获取对应的品种信息
 */
WTSCommodityInfo* WtUftEngine::get_commodity_info(const char* stdCode)
{
	// 拆分标准化合约代码，格式为“交易所.合约代码”
	const StringVector& ay = StrUtil::split(stdCode, ".");
	// 根据交易所和合约代码获取合约信息
	WTSContractInfo* cInfo = _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
	if (cInfo == NULL)
		return NULL;

	// 从合约信息中获取品种信息
	return cInfo->getCommInfo();
}

/**
 * @brief 获取合约信息
 * @param stdCode 标准化合约代码，格式为“交易所.合约代码”
 * @return 合约信息指针，如果找不到则返回NULL
 * @details 根据标准化合约代码解析出交易所和合约代码，然后返回对应的合约信息
 */
WTSContractInfo* WtUftEngine::get_contract_info(const char* stdCode)
{
	// 拆分标准化合约代码，格式为“交易所.合约代码”
	const StringVector& ay = StrUtil::split(stdCode, ".");
	// 直接从基础数据管理器中获取合约信息
	return _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
}

/**
 * @brief 获取交易时段信息
 * @param sid 交易时段ID或标准化合约代码
 * @param isCode 是否为合约代码，默认为false
 * @return 交易时段信息指针，如果找不到则返回NULL
 * @details 根据交易时段ID或标准化合约代码获取对应的交易时段信息
 */
WTSSessionInfo* WtUftEngine::get_session_info(const char* sid, bool isCode /* = false */)
{
	// 如果不是合约代码，直接根据交易时段ID获取交易时段信息
	if (!isCode)
		return _base_data_mgr->getSession(sid);

	// 如果是合约代码，先拆分标准化合约代码
	const StringVector& ay = StrUtil::split(sid, ".");
	// 获取合约信息
	WTSContractInfo* cInfo = _base_data_mgr->getContract(ay[1].c_str(), ay[0].c_str());
	if (cInfo == NULL)
		return NULL;

	// 从合约信息中获取品种信息，再从品种信息中获取交易时段信息
	WTSCommodityInfo* commInfo = cInfo->getCommInfo();
	return commInfo->getSessionInfo();
}

/**
 * @brief 获取Tick切片数据
 * @param sid 策略ID
 * @param code 标准化合约代码
 * @param count 请求的数据条数
 * @return Tick切片数据指针
 * @details 根据策略ID、合约代码和请求数量获取Tick切片数据，但当前实现为空
 * @note 当前实现直接返回NULL，后续的数据获取代码被不可达（注释了执行但未删除）
 */
WTSTickSlice* WtUftEngine::get_tick_slice(uint32_t sid, const char* code, uint32_t count)
{
	return NULL;
	return _data_mgr->get_tick_slice(code, count);
}

/**
 * @brief 获取最新的Tick数据
 * @param sid 策略ID
 * @param stdCode 标准化合约代码
 * @return 最新的Tick数据指针，如果找不到则返回NULL
 * @details 从数据管理器中获取指定合约的最新Tick数据
 */
WTSTickData* WtUftEngine::get_last_tick(uint32_t sid, const char* stdCode)
{
	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取K线切片数据
 * @param sid 策略ID
 * @param stdCode 标准化合约代码
 * @param period 周期标识（如"m"代表分钟，"d"代表天）
 * @param count 请求的数据条数
 * @param times 周期倍数，默认为1
 * @param etime 结束时间戳，默认为0（当前时间）
 * @return K线切片数据指针
 * @details 根据策略ID、合约代码、周期等参数获取K线切片数据
 * @note 当前实现直接返回NULL，后续的处理代码被不可达（注释了执行但未删除）
 */
WTSKlineSlice* WtUftEngine::get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */, uint64_t etime /* = 0 */)
{
	return NULL;
	// 下面代码因为上面的return语句而不可达
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(stdCode);
	if (cInfo == NULL)
		return NULL;

	WTSSessionInfo* sInfo = cInfo->getSessionInfo();

	// 构造唯一的订阅键值
	std::string key = fmt::format("{}-{}-{}", stdCode, period, times);
	SubList& sids = _bar_sub_map[key];
	sids.insert(sid);

	// 根据周期标识和倍数确定实际的K线周期
	WTSKlinePeriod kp;
	if (strcmp(period, "m") == 0)
	{
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			times /= 5;
		}
		else
			kp = KP_Minute1;
	}
	else
	{
		kp = KP_DAY;
	}

	// 从数据管理器中获取K线切片
	return _data_mgr->get_kline_slice(stdCode, kp, times, count, etime);
}

/**
 * @brief 订阅Tick数据
 * @param sid 策略ID
 * @param stdCode 标准化合约代码
 * @details 将指定策略添加到特定合约的Tick数据订阅列表中
 */
void WtUftEngine::sub_tick(uint32_t sid, const char* stdCode)
{
	// 获取该合约的订阅列表，如果不存在会自动创建
	SubList& sids = _tick_sub_map[stdCode];
	// 将策略ID添加到订阅列表中
	sids.insert(sid);
}

/**
 * @brief 获取当前价格
 * @param stdCode 标准化合约代码
 * @return 当前价格，如果没有Tick数据则返回0.0
 * @details 从最新的Tick数据中提取当前价格
 */
double WtUftEngine::get_cur_price(const char* stdCode)
{
	// 获取最新的Tick数据
	WTSTickData* lastTick = _data_mgr->grab_last_tick(stdCode);
	if (lastTick == NULL)
		return 0.0;

	// 提取价格信息
	double ret = lastTick->price();
	// 释放数据对象
	lastTick->release();
	return ret;
}

/**
 * @brief 通知策略参数更新
 * @param name 策略名称
 * @details 当策略参数发生更新时，通知并触发对应策略的参数更新回调
 */
void WtUftEngine::notify_params_update(const char* name)
{
	// 遍历所有策略上下文
	for(auto& v : _ctx_map)
	{
		const UftContextPtr& context = v.second;
		// 找到匹配名称的策略
		if(strcmp(context->name(), name) == 0)
		{
			// 触发策略的参数更新回调
			context->on_params_updated();
			break;
		}
	}
}

/**
 * @brief 初始化UFT引擎
 * @param cfg 配置项变量集
 * @param bdMgr 基础数据管理器指针
 * @param dataMgr UFT数据管理器指针
 * @param notifier 事件通知器指针
 * @details 设置各类管理器指针和加载配置项，为引擎运行做准备
 */
void WtUftEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtUftDtMgr* dataMgr, EventNotifier* notifier)
{
	// 设置基础数据管理器
	_base_data_mgr = bdMgr;
	// 设置UFT数据管理器
	_data_mgr = dataMgr;
	// 设置事件通知器
	_notifier = notifier;

	// 保存配置项并增加引用计数
	_cfg = cfg;
	if(_cfg) _cfg->retain();
}

void WtUftEngine::run()
{
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_init();
	}

	_tm_ticker = new WtUftRtTicker(this);
	if(_cfg && _cfg->has("product"))
	{
		WTSVariant* cfgProd = _cfg->get("product");
		_tm_ticker->init(cfgProd->getCString("session"));
	}
	else
	{
		_tm_ticker->init("ALLDAY");
	}

	_tm_ticker->run();
}

void WtUftEngine::handle_push_quote(WTSTickData* newTick)
{
	if (_tm_ticker)
		_tm_ticker->on_tick(newTick);
}

void WtUftEngine::handle_push_order_detail(WTSOrdDtlData* curOrdDtl)
{
	const char* stdCode = curOrdDtl->code();
	auto sit = _orddtl_sub_map.find(stdCode);
	if (sit != _orddtl_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_order_detail(stdCode, curOrdDtl);
			}
		}
	}
}

void WtUftEngine::handle_push_order_queue(WTSOrdQueData* curOrdQue)
{
	const char* stdCode = curOrdQue->code();
	auto sit = _ordque_sub_map.find(stdCode);
	if (sit != _ordque_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_order_queue(stdCode, curOrdQue);
			}
		}
	}
}

void WtUftEngine::handle_push_transaction(WTSTransData* curTrans)
{
	const char* stdCode = curTrans->code();
	auto sit = _trans_sub_map.find(stdCode);
	if (sit != _trans_sub_map.end())
	{
		const SubList& sids = sit->second;
		for (auto it = sids.begin(); it != sids.end(); it++)
		{
			//By Wesley @ 2022.02.07
			//Level2数据一般用于HFT场景，所以不做复权处理
			//所以不读取订阅标记
			uint32_t sid = *it;
			auto cit = _ctx_map.find(sid);
			if (cit != _ctx_map.end())
			{
				UftContextPtr& ctx = (UftContextPtr&)cit->second;
				ctx->on_transaction(stdCode, curTrans);
			}
		}
	}
}

void WtUftEngine::sub_order_detail(uint32_t sid, const char* stdCode)
{
	SubList& sids = _orddtl_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::sub_order_queue(uint32_t sid, const char* stdCode)
{
	SubList& sids = _ordque_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::sub_transaction(uint32_t sid, const char* stdCode)
{
	SubList& sids = _trans_sub_map[stdCode];
	sids.insert(sid);
}

void WtUftEngine::on_session_begin()
{
	WTSLogger::info("Trading day {} begun", _cur_tdate);

	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_session_begin(_cur_tdate);
	}
}

void WtUftEngine::on_session_end()
{
	for (auto it = _ctx_map.begin(); it != _ctx_map.end(); it++)
	{
		UftContextPtr& ctx = (UftContextPtr&)it->second;
		ctx->on_session_end(_cur_tdate);
	}

	WTSLogger::info("Trading day {} ended", _cur_tdate);
}

void WtUftEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	if(_data_mgr)
		_data_mgr->handle_push_quote(stdCode, curTick);

	{
		auto sit = _tick_sub_map.find(stdCode);
		if (sit != _tick_sub_map.end())
		{
			const SubList& sids = sit->second;
			for (auto it = sids.begin(); it != sids.end(); it++)
			{
				uint32_t sid = *it;

				auto cit = _ctx_map.find(sid);
				if (cit != _ctx_map.end())
				{
					UftContextPtr& ctx = (UftContextPtr&)cit->second;
					ctx->on_tick(stdCode, curTick);
				}
			}
		}
	}
}

void WtUftEngine::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	std::string key = fmt::format("{}-{}-{}", stdCode, period, times);
	const SubList& sids = _bar_sub_map[key];
	for (auto it = sids.begin(); it != sids.end(); it++)
	{
		uint32_t sid = *it;
		auto cit = _ctx_map.find(sid);
		if (cit != _ctx_map.end())
		{
			UftContextPtr& ctx = (UftContextPtr&)cit->second;
			ctx->on_bar(stdCode, period, times, newBar);
		}
	}
}

void WtUftEngine::on_minute_end(uint32_t curDate, uint32_t curTime)
{

}

void WtUftEngine::addContext(UftContextPtr ctx)
{
	uint32_t sid = ctx->id();
	_ctx_map[sid] = ctx;
}

UftContextPtr WtUftEngine::getContext(uint32_t id)
{
	auto it = _ctx_map.find(id);
	if (it == _ctx_map.end())
		return UftContextPtr();

	return it->second;
}

WTSOrdQueSlice* WtUftEngine::get_order_queue_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_queue_slice(code, count);
}

WTSOrdDtlSlice* WtUftEngine::get_order_detail_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_order_detail_slice(code, count);
}

WTSTransSlice* WtUftEngine::get_transaction_slice(uint32_t sid, const char* code, uint32_t count)
{
	return _data_mgr->get_transaction_slice(code, count);
}