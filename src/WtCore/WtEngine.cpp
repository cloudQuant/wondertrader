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

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;


USING_NS_WTP;

WtEngine::WtEngine()
	: _port_fund(NULL)
	, _risk_volscale(1.0)
	, _risk_date(0)
	, _terminated(false)
	, _evt_listener(NULL)
	, _adapter_mgr(NULL)
	, _notifier(NULL)
	, _fund_udt_span(0)
	, _ready(false)
{
	TimeUtils::getDateTime(_cur_date, _cur_time);
	_cur_secs = _cur_time % 100000;
	_cur_time /= 100000;
	_cur_raw_time = _cur_time;
	_cur_tdate = _cur_date;

	WtHelper::setTime(_cur_date, _cur_time, _cur_secs);
}

/**
 * @brief 设置当前日期和时间
 * @details 设置交易引擎的当前日期、时间、秒数和原始时间
 *          这些时间信息用于交易引擎的时间控制和交易日志记录
 *          同时会将时间信息设置到全局的WtHelper中，供其他模块使用
 * 
 * @param curDate 当前日期，格式为YYYYMMDD，如 20250426
 * @param curTime 当前时间，格式为HHMM，如 1530 表示15点30分
 * @param curSecs 当前秒数，默认为0
 * @param rawTime 原始时间，默认为0，如果为0则使用curTime
 */
void WtEngine::set_date_time(uint32_t curDate, uint32_t curTime, uint32_t curSecs /* = 0 */, uint32_t rawTime /* = 0 */)
{
	// 设置当前日期
	_cur_date = curDate;
	// 设置当前时间
	_cur_time = curTime;
	// 设置当前秒数
	_cur_secs = curSecs;

	// 如果原始时间为0，则使用当前时间
	if (rawTime == 0)
		rawTime = curTime;

	// 设置原始时间
	_cur_raw_time = rawTime;

	// 将时间信息设置到全局的WtHelper中
	WtHelper::setTime(_cur_date, _cur_raw_time, _cur_secs);
}

/**
 * @brief 设置当前交易日期
 * @details 设置交易引擎的当前交易日期，并同步到全局的WtHelper中
 *          交易日期与实际日历日期可能不同，如夜盘交易时间属于下一个交易日
 *          交易日期用于结算、清算和复权计算等
 * 
 * @param curTDate 当前交易日期，格式为YYYYMMDD，如 20250426
 */
void WtEngine::set_trading_date(uint32_t curTDate)
{
	// 设置当前交易日期
	_cur_tdate = curTDate; 

	// 将交易日期设置到全局的WtHelper中
	WtHelper::setTDate(curTDate);
}

/**
 * @brief 获取商品信息
 * @details 根据标准化合约代码获取对应的商品信息对象
 *          首先使用CodeHelper解析标准化合约代码，获取交易所和品种信息
 *          然后从基础数据管理器中获取对应的商品信息
 * 
 * @param stdCode 标准化合约代码
 * @return WTSCommodityInfo* 商品信息对象指针
 */
WTSCommodityInfo* WtEngine::get_commodity_info(const char* stdCode)
{
	// 解析标准化合约代码，获取交易所和品种信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	// 从基础数据管理器中获取对应的商品信息
	return _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
}

/**
 * @brief 获取合约信息
 * @details 根据标准化合约代码获取对应的合约信息对象
 *          首先使用CodeHelper解析标准化合约代码，获取合约代码和交易所信息
 *          然后从基础数据管理器中获取对应的合约信息
 * 
 * @param stdCode 标准化合约代码
 * @return WTSContractInfo* 合约信息对象指针
 */
WTSContractInfo* WtEngine::get_contract_info(const char* stdCode)
{
	// 解析标准化合约代码，获取合约代码和交易所信息
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	// 从基础数据管理器中获取对应的合约信息
	return _base_data_mgr->getContract(cInfo._code, cInfo._exchg);
}

/**
 * @brief 获取原始合约代码
 * @details 根据标准化合约代码获取对应的原始合约代码
 *          如果是主力合约或其他带规则的合约，则转换为实际的原始合约代码
 *          例如，将SHFE.ag.HOT转换为FHFE.ag.2306等实际合约
 * 
 * @param stdCode 标准化合约代码
 * @return std::string 原始合约代码，如果无法转换则返回空字符串
 */
std::string WtEngine::get_rawcode(const char* stdCode)
{
	// 解析标准化合约代码
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	// 如果合约有规则标签（如主力合约、次主力合约等）
	if (cInfo.hasRule())
	{
		// 从主力合约管理器中获取自定义原始代码
		std::string code = _hot_mgr->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _cur_tdate);
		// 将原始月份代码转换为标准化合约代码
		return CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);
	}

	// 如果没有规则标签或无法转换，返回空字符串
	return "";
}

/**
 * @brief 获取交易时段信息
 * @details 根据会话ID或合约代码获取对应的交易时段信息
 *          如果传入的是会话ID，直接从基础数据管理器中获取对应的交易时段信息
 *          如果传入的是合约代码，则首先获取对应的商品信息，再获取商品对应的交易时段信息
 * 
 * @param sid 会话ID或合约代码
 * @param isCode 是否为合约代码，默认为false（表示传入的是会话ID）
 * @return WTSSessionInfo* 交易时段信息对象指针，如果找不到则返回NULL
 */
WTSSessionInfo* WtEngine::get_session_info(const char* sid, bool isCode /* = false */)
{
	// 如果传入的不是合约代码，直接从基础数据管理器中获取交易时段信息
	if (!isCode)
		return _base_data_mgr->getSession(sid);

	// 解析标准化合约代码
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(sid, _hot_mgr);
	// 获取对应的商品信息
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	// 如果找不到商品信息，返回NULL
	if (cInfo == NULL)
		return NULL;

	// 从基础数据管理器中获取商品对应的交易时段信息
	return _base_data_mgr->getSession(cInfo->getSession());
}

/**
 * @brief 处理Tick行情数据
 * @details 处理收到的Tick行情数据，包括触发信号、更新价格缓存、计算浮动盈亏和更新资金信息
 * 
 * @param stdCode 标准化合约代码
 * @param curTick 当前Tick数据对象
 */
void WtEngine::on_tick(const char* stdCode, WTSTickData* curTick)
{
	// 更新合约的当前价格缓存
	_price_map[stdCode] = curTick->price();

	// 检查是否有待触发的交易信号
	{
		bool bTriggered = false;
		// 在信号映射表中查找当前合约的信号
		auto it = _sig_map.find(stdCode);
		if (it != _sig_map.end())
		{
			// 获取合约的交易时段信息
			WTSSessionInfo* sInfo = get_session_info(stdCode, true);

			// 检查当前时间是否在交易时段内
			if (sInfo->isInTradingTime(_cur_raw_time, true))
			{
				// 获取信号信息
				const SigInfo& sInfo = it->second;
				double pos = sInfo._volume;
				std::string code = stdCode;
				// 执行持仓调整，使用当前的Tick价格
				do_set_position(code.c_str(), pos, curTick->price());
				// 从信号映射表中移除已触发的信号
				_sig_map.erase(it);
				bTriggered = true;
			}

		}

		// 如果触发了信号，则保存数据
		if(bTriggered)
			save_datas();
	}

	// 如果成交量为0，表示价格没有变动，直接返回
	if (curTick->volume() == 0)
		return;

	// 将合约代码和价格信息复制到本地变量，以便在任务中使用
	std::string code = stdCode;
	double price = curTick->price();
	// 添加任务更新指定合约的浮动盈亏
	push_task([this, code, price]{
		// 查找合约的持仓信息
		auto it = _pos_map.find(code);
		if (it == _pos_map.end())
			return;

		// 获取持仓信息并加锁
		PosInfoPtr& pInfo = it->second;
		SpinLock lock(pInfo->_mtx);
		// 如果持仓量为0，则浮动盈亏也为0
		if (pInfo->_volume == 0)
		{
			pInfo->_dynprofit = 0;
		}
		else
		{
			// 获取商品信息
			WTSCommodityInfo* commInfo = get_commodity_info(code.c_str());
			double dynprofit = 0;
			// 遍历所有持仓明细，计算每笔持仓的浮动盈亏
			for (auto pit = pInfo->_details.begin(); pit != pInfo->_details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				// 计算浮动盈亏 = 持仓量 * (当前价格 - 开仓价格) * 合约乘数 * 方向系数(多头为1，空头为-1)
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				dynprofit += dInfo._profit;
			}

			// 更新总浮动盈亏
			pInfo->_dynprofit = dynprofit;
		}
	});

	// 添加任务更新资金的浮动盈亏
	push_task([this]() {
		update_fund_dynprofit();
	});
}

/**
 * @brief 更新资金账户的浮动盈亏信息
 * @details 根据当前持仓的浮动盈亏计算资金账户的总浮动盈亏，并更新资金账户的最大/最小动态权益等信息
 *          该函数会根据配置的更新时间间隔执行，避免频繁更新造成性能问题
 */
void WtEngine::update_fund_dynprofit()
{
	// 获取资金账户信息
	WTSFundStruct& fundInfo = _port_fund->fundInfo();
	// 如果上次结算日期等于当前交易日，说明今天已经结算过，不再更新
	if (fundInfo._last_date == _cur_tdate)
	{
		//上次结算日期等于当前交易日,说明已经结算,不再更新了
		return;
	}

	// 获取当前时间戳
	int64_t now = TimeUtils::getLocalTimeNow();
	// 如果设置了更新时间间隔，并且距离上次更新时间未超过间隔，则不更新
	if(_fund_udt_span != 0)
	{
		if (now - fundInfo._update_time < _fund_udt_span * 1000)
			return;
	}

	// 计算所有持仓的浮动盈亏总和
	double profit = 0.0;
	for(const auto& v : _pos_map)
	{
		const PosInfoPtr& pItem = v.second;
		profit += pItem->_dynprofit;
	}

	// 更新资金账户的浮动盈亏
	fundInfo._dynprofit = profit;
	// 计算动态权益 = 静态权益 + 浮动盈亏
	double dynbal = fundInfo._balance + profit;
	// 更新当日最大动态权益及其对应时间
	if (fundInfo._max_dyn_bal == DBL_MAX || decimal::gt(dynbal, fundInfo._max_dyn_bal))
	{
		fundInfo._max_dyn_bal = dynbal;
		fundInfo._max_time = _cur_raw_time * 100000 + _cur_secs;
	}

	// 更新当日最小动态权益及其对应时间
	if (fundInfo._min_dyn_bal == DBL_MAX || decimal::lt(dynbal, fundInfo._min_dyn_bal))
	{
		fundInfo._min_dyn_bal = dynbal;
		fundInfo._min_time = _cur_raw_time * 100000 + _cur_secs;;
	}

	// 再次计算动态权益（与上面的dynbal相同，可能是代码冗余）
	double dynbalance = fundInfo._balance + profit;
	// 更新多日最大动态权益及其对应日期
	if (fundInfo._max_md_dyn_bal._date == 0 || decimal::gt(dynbalance, fundInfo._max_md_dyn_bal._dyn_balance))
	{
		fundInfo._max_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._max_md_dyn_bal._date = _cur_tdate;
	}

	// 更新多日最小动态权益及其对应日期
	if (fundInfo._min_md_dyn_bal._date == 0 || decimal::lt(dynbalance, fundInfo._min_md_dyn_bal._dyn_balance))
	{
		fundInfo._min_md_dyn_bal._dyn_balance = dynbalance;
		fundInfo._min_md_dyn_bal._date = _cur_tdate;
	}

	// 更新最后更新时间
	fundInfo._update_time = now;
}

/**
 * @brief 写入风控日志
 * @details 将风控相关的日志信息写入日志文件，并添加风控标识前缀
 *          使用thread_local存储日志缓冲区，提高多线程环境下的性能
 * 
 * @param message 要写入的风控日志消息
 */
void WtEngine::writeRiskLog(const char* message)
{
	// 使用线程本地存储的日志缓冲区
	static thread_local char szBuf[2048] = { 0 };
	// 先写入风控标识前缀
	auto len = wt_strcpy(szBuf, "[RiskControl] ");
	// 再写入实际的日志消息
	wt_strcpy(szBuf + len, message);
	// 使用日志器将消息写入风控类别的日志
	WTSLogger::log_raw_by_cat("risk", LL_INFO, szBuf);
}

/**
 * @brief 获取当前日期
 * @details 获取当前日期，格式为YYYYMMDD，如20250426
 *          这是实际日历日期，与交易日期可能不同
 * 
 * @return uint32_t 当前日期，格式为YYYYMMDD
 */
uint32_t WtEngine::getCurDate()
{
	// 返回当前日期
	return _cur_date;
}

/**
 * @brief 获取当前时间
 * @details 获取当前时间，格式为HHMM，如1530表示15点30分
 *          这是当前的交易时间，用于交易策略和风控判断
 * 
 * @return uint32_t 当前时间，格式为HHMM
 */
uint32_t WtEngine::getCurTime()
{
	// 返回当前时间
	return _cur_time;
}

/**
 * @brief 获取当前交易日期
 * @details 获取当前交易日期，格式为YYYYMMDD，如20250426
 *          交易日期与实际日历日期可能不同，如夜盘交易时间属于下一个交易日
 *          交易日期用于结算、清算和复权计算等
 * 
 * @return uint32_t 当前交易日期，格式为YYYYMMDD
 */
uint32_t WtEngine::getTradingDate()
{
	// 返回当前交易日期
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

/**
 * @brief 获取资金账户信息
 * @details 获取当前资金账户信息，包括可用资金、浮动盈亏、手续费等
 *          获取前会先更新资金浮动盈亏并保存数据
 * 
 * @return WTSPortFundInfo* 资金账户信息对象指针
 */
WTSPortFundInfo* WtEngine::getFundInfo()
{
	// 更新资金账户的浮动盈亏信息
	update_fund_dynprofit();
	// 保存交易数据
	save_datas();

	// 返回资金账户信息对象
	return _port_fund;
}

/**
 * @brief 初始化交易引擎
 * @details 初始化交易引擎的各个组件，包括数据管理器、主力合约管理器、事件通知器等
 *          加载过滤器、手续费配置、交易数据和初始化输出文件
 *          如果配置了风控模块，则初始化风控线程
 * 
 * @param cfg 配置对象
 * @param bdMgr 基础数据管理器接口
 * @param dataMgr 数据管理器
 * @param hotMgr 主力合约管理器接口
 * @param notifier 事件通知器
 */
void WtEngine::init(WTSVariant* cfg, IBaseDataMgr* bdMgr, WtDtMgr* dataMgr, IHotMgr* hotMgr, EventNotifier* notifier)
{
	// 设置基础数据管理器
	_base_data_mgr = bdMgr;
	// 设置数据管理器
	_data_mgr = dataMgr;
	// 设置主力合约管理器
	_hot_mgr = hotMgr;
	// 设置事件通知器
	_notifier = notifier;

	// 记录运行模式为生产模式
	WTSLogger::info("Running mode: Production");

	// 为过滤器管理器设置通知器
	_filter_mgr.set_notifier(notifier);

	// 加载过滤器配置
	_filter_mgr.load_filters(cfg->getCString("filters"));

	// 加载手续费配置
	load_fees(cfg->getCString("fees"));

	// 加载交易数据
	load_datas();

	// 初始化输出文件
	init_outputs();

	// 获取风控配置
	WTSVariant* cfgRisk = cfg->get("riskmon");
	// 如果配置了风控模块
	if(cfgRisk)
	{
		// 初始化风控线程
		init_riskmon(cfgRisk);
	}
	else
	{
		// 如果没有配置风控线程，则需要自己更新浮动盈亏
		// 把更新时间间隔设置为5s
		_fund_udt_span = 5;
		WTSLogger::log_raw(LL_WARN, "RiskMon is not configured, portfilio fund will be updated every 5s");
	}
}

/**
 * @brief 处理交易日结束时的资金结算
 * @details 在交易日结束时进行资金结算，包括记录当日资金状况、更新资金信息并将资金数据写入CSV文件保存
 *          结算过程中会记录各种资金指标，如动态权益、静态权益、平仓盈亏、浮动盈亏、手续费等
 */
void WtEngine::on_session_end()
{
	// 获取资金账户信息
	WTSFundStruct& fundInfo = _port_fund->fundInfo();
	// 如果上次结算日期小于当前交易日，说明需要进行结算
	if (fundInfo._last_date < _cur_tdate)
	{
		// 构造资金记录文件路径
		std::string filename = WtHelper::getPortifolioDir();
		filename += "funds.csv";
		// 创建文件操作对象
		BoostFilePtr fund_log(new BoostFile());
		{
			// 检查文件是否存在
			bool isNewFile = !BoostFile::exists(filename.c_str());
			// 创建或打开文件
			fund_log->create_or_open_file(filename.c_str());
			// 如果是新文件，则写入CSV头部
			if (isNewFile)
			{
				fund_log->write_file("date,predynbalance,prebalance,balance,closeprofit,dynprofit,fee,maxdynbalance,maxtime,mindynbalance,mintime,mdmaxbalance,mdmaxdate,mdminbalance,mdmindate\n");
			}
			else
			{
				// 如果文件已存在，则将文件指针移动到文件末尾
				fund_log->seek_to_end();
			}
		}

		// 写入当日资金记录
		// 各字段含义：日期、前一日动态权益、前一日静态权益、当前静态权益、平仓盈亏、浮动盈亏、手续费、
		// 最大动态权益、最大权益时间、最小动态权益、最小权益时间、多日最大动态权益、多日最大权益日期、多日最小动态权益、多日最小权益日期
		fund_log->write_file(fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", 
			_cur_tdate, fundInfo._predynbal, fundInfo._prebalance, fundInfo._balance, 
			fundInfo._profit, fundInfo._dynprofit, fundInfo._fees, fundInfo._max_dyn_bal,
			fundInfo._max_time, fundInfo._min_dyn_bal, fundInfo._min_time,
			fundInfo._max_md_dyn_bal._dyn_balance, fundInfo._max_md_dyn_bal._date,
			fundInfo._min_md_dyn_bal._dyn_balance, fundInfo._min_md_dyn_bal._date));

		// 更新资金信息，为下一交易日做准备
		// 记录最后结算日期
		fundInfo._last_date = _cur_tdate;
		// 设置前一日动态权益 = 当前静态权益 + 当前浮动盈亏
		fundInfo._predynbal = fundInfo._balance + fundInfo._dynprofit;
		// 设置前一日静态权益 = 当前静态权益
		fundInfo._prebalance = fundInfo._balance;
		// 重置平仓盈亏
		fundInfo._profit = 0;
		// 重置手续费
		fundInfo._fees = 0;
		// 重置最大动态权益
		fundInfo._max_dyn_bal = DBL_MAX;
		// 重置最小动态权益
		fundInfo._min_dyn_bal = DBL_MAX;
		// 重置最大权益时间
		fundInfo._max_time = 0;
		// 重置最小权益时间
		fundInfo._min_time = 0;
	}
	// 保存所有数据
	save_datas();
}

/**
 * @brief 处理交易日开始时的操作
 * @details 在交易日开始时调用，用于初始化交易日相关的状态
 *          该函数在当前实现中为空，可由子类重写以实现特定的交易日开始时的操作
 */
void WtEngine::on_session_begin()
{
	// 当前实现为空，可由子类重写
}

/**
 * @brief 保存交易引擎的数据
 * @details 将交易引擎的资金数据、持仓数据、风控参数等信息保存到JSON文件中
 *          保存的数据包括资金账户信息、所有合约的持仓信息及明细、风控参数等
 */
void WtEngine::save_datas()
{
    // 创建JSON文档对象
    rj::Document root(rj::kObjectType);
    rj::Document::AllocatorType &allocator = root.GetAllocator();

    // 如果资金对象存在，则保存资金数据
    if (_port_fund != NULL)
    {//保存资金数据
        // 获取资金结构信息
        const WTSFundStruct& fundInfo = _port_fund->fundInfo();
        // 创建资金JSON对象
        rj::Value jFund(rj::kObjectType);
        // 添加前一日动态权益
        jFund.AddMember("predynbal", fundInfo._predynbal, allocator);
        // 添加当前静态权益
        jFund.AddMember("balance", fundInfo._balance, allocator);
        // 添加前一日静态权益
        jFund.AddMember("prebalance", fundInfo._prebalance, allocator);
        // 添加平仓盈亏
        jFund.AddMember("profit", fundInfo._profit, allocator);
        // 添加浮动盈亏
        jFund.AddMember("dynprofit", fundInfo._dynprofit, allocator);
        // 添加手续费
        jFund.AddMember("fees", fundInfo._fees, allocator);

        // 添加当日最大动态权益
        jFund.AddMember("max_dyn_bal", fundInfo._max_dyn_bal, allocator);
        // 添加最大权益时间
        jFund.AddMember("max_time", fundInfo._max_time, allocator);
        // 添加当日最小动态权益
        jFund.AddMember("min_dyn_bal", fundInfo._min_dyn_bal, allocator);
        // 添加最小权益时间
        jFund.AddMember("min_time", fundInfo._min_time, allocator);

        // 添加最后结算日期
        jFund.AddMember("last_date", fundInfo._last_date, allocator);
        // 添加当前交易日
        jFund.AddMember("date", _cur_tdate, allocator);

        // 创建多日最大动态权益对象
        rj::Value jMaxMD(rj::kObjectType);
        jMaxMD.AddMember("date", fundInfo._max_md_dyn_bal._date, allocator);
        jMaxMD.AddMember("dyn_balance", fundInfo._max_md_dyn_bal._dyn_balance, allocator);

        // 创建多日最小动态权益对象
        rj::Value jMinMD(rj::kObjectType);
        jMinMD.AddMember("date", fundInfo._min_md_dyn_bal._date, allocator);
        jMinMD.AddMember("dyn_balance", fundInfo._min_md_dyn_bal._dyn_balance, allocator);

        // 将多日最大/最小动态权益对象添加到资金对象中
        jFund.AddMember("maxmd", jMaxMD, allocator);
        jFund.AddMember("minmd", jMinMD, allocator);

        // 添加最后更新时间
        jFund.AddMember("update_time", fundInfo._update_time, allocator);

        // 将资金对象添加到根文档
        root.AddMember("fund", jFund, allocator);
    }

    {// 保存持仓数据
        // 创建持仓数组
        rj::Value jPos(rj::kArrayType);

        // 遍历所有持仓
        for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
        {
            // 获取合约代码和持仓信息
            const char* stdCode = it->first.c_str();
            const PosInfoPtr& pInfo = it->second;

            // 创建单个持仓项目
            rj::Value pItem(rj::kObjectType);
            // 添加合约代码
            pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);
            // 添加持仓量
            pItem.AddMember("volume", pInfo->_volume, allocator);
            // 添加平仓盈亏
            pItem.AddMember("closeprofit", pInfo->_closeprofit, allocator);
            // 添加浮动盈亏
            pItem.AddMember("dynprofit", pInfo->_dynprofit, allocator);

            // 创建持仓明细数组
            rj::Value details(rj::kArrayType);
            // 遍历所有持仓明细
            for (auto dit = pInfo->_details.begin(); dit != pInfo->_details.end(); dit++)
            {
                const DetailInfo& dInfo = *dit;
                // 如果持仓量为0，则跳过
                if(decimal::eq(dInfo._volume, 0))
                    continue;
                // 创建明细项目
                rj::Value dItem(rj::kObjectType);
                // 添加多空方向标记（true为多头，false为空头）
                dItem.AddMember("long", dInfo._long, allocator);
                // 添加开仓价格
                dItem.AddMember("price", dInfo._price, allocator);
                // 添加持仓量
                dItem.AddMember("volume", dInfo._volume, allocator);
                // 添加开仓时间
                dItem.AddMember("opentime", dInfo._opentime, allocator);
                // 添加开仓交易日
                dItem.AddMember("opentdate", dInfo._opentdate, allocator);

                // 添加浮动盈亏
                dItem.AddMember("profit", dInfo._profit, allocator);

                // 将明细项目添加到明细数组
                details.PushBack(dItem, allocator);
            }

            // 将明细数组添加到持仓项目
            pItem.AddMember("details", details, allocator);

            // 将持仓项目添加到持仓数组
            jPos.PushBack(pItem, allocator);
        }

        // 将持仓数组添加到根文档
        root.AddMember("positions", jPos, allocator);
    }

    // 保存风控参数设置
    {
        // 创建风控对象
        rj::Value jRisk(rj::kObjectType);

        // 添加风控仓位比例
        jRisk.AddMember("scale", _risk_volscale, allocator);
        // 添加风控参数更新日期
        jRisk.AddMember("date", _risk_date, allocator);

        // 将风控对象添加到根文档
        root.AddMember("riskmon", jRisk, allocator);
    }

    // 将数据写入文件
    {
        // 构造数据文件路径
        std::string filename = WtHelper::getPortifolioDir();
        filename += "datas.json";

        // 创建文件对象
        BoostFile bf;
        // 创建新文件
        if (bf.create_new_file(filename.c_str()))
        {
            // 创建字符串缓冲区
            rj::StringBuffer sb;
            // 创建JSON格式化写入器
            rj::PrettyWriter<rj::StringBuffer> writer(sb);
            // 将JSON文档写入缓冲区
            root.Accept(writer);
            // 将缓冲区内容写入文件
            bf.write_file(sb.GetString());
            // 关闭文件
            bf.close_file();
        }
    }
}

/**
 * @brief 加载交易引擎的数据
 * @details 从数据文件中加载交易引擎的资金数据、持仓数据和风控参数等信息
 *          在交易引擎初始化时调用，用于恢复交易状态
 */
void WtEngine::load_datas()
{
	// 创建资金对象
	_port_fund = WTSPortFundInfo::create();

	// 构造数据文件路径
	std::string filename = WtHelper::getPortifolioDir();
	filename += "datas.json";

	// 如果文件不存在，直接返回
	if (!StdFile::exists(filename.c_str()))
	{
		return;
	}

	// 读取文件内容
	std::string content;
	StdFile::read_file_content(filename.c_str(), content);
	// 如果文件内容为空，直接返回
	if (content.empty())
		return;

	// 解析JSON文档
	rj::Document root;
	root.Parse(content.c_str());

	// 如果解析出错，直接返回
	if (root.HasParseError())
		return;

	// 读取资金信息
	{
		// 获取资金对象
		const rj::Value& jFund = root["fund"];
		// 如果资金对象存在且有效
		if (!jFund.IsNull() && jFund.IsObject())
		{
			// 获取资金结构信息
			WTSFundStruct& fundInfo = _port_fund->fundInfo();
			// 读取前一日动态权益
			fundInfo._predynbal = jFund["predynbal"].GetDouble();
			// 读取当前静态权益
			fundInfo._balance = jFund["balance"].GetDouble();
			// 读取前一日静态权益
			fundInfo._prebalance = jFund["prebalance"].GetDouble();
			// 读取平仓盈亏
			fundInfo._profit = jFund["profit"].GetDouble();
			// 读取浮动盈亏
			fundInfo._dynprofit = jFund["dynprofit"].GetDouble();
			// 读取手续费
			fundInfo._fees = jFund["fees"].GetDouble();
			// 读取最后结算日期
			fundInfo._last_date = jFund["last_date"].GetUint();
			// 读取最大动态权益
			fundInfo._max_dyn_bal = jFund["max_dyn_bal"].GetDouble();
			// 读取最大权益时间
			fundInfo._max_time = jFund["max_time"].GetUint();
			// 读取最小动态权益
			fundInfo._min_dyn_bal = jFund["min_dyn_bal"].GetDouble();
			// 读取最小权益时间
			fundInfo._min_time = jFund["min_time"].GetUint();

			// 读取多日最大动态权益信息
			const rj::Value& jMaxMD = jFund["maxmd"];
			fundInfo._max_md_dyn_bal._dyn_balance = jMaxMD["dyn_balance"].GetDouble();
			fundInfo._max_md_dyn_bal._date = jMaxMD["date"].GetUint();

			// 读取多日最小动态权益信息
			const rj::Value& jMinMD = jFund["minmd"];
			fundInfo._min_md_dyn_bal._dyn_balance = jMinMD["dyn_balance"].GetDouble();
			fundInfo._min_md_dyn_bal._date = jMinMD["date"].GetUint();

			// 如果有更新时间字段，则读取更新时间
			if(jFund.HasMember("update_time"))
			{
				fundInfo._update_time = jFund["update_time"].GetInt64();
			}
		}
	}

	{// 读取持仓数据
		// 初始化总平仓盈亏和总浮动盈亏
		double total_profit = 0;
		double total_dynprofit = 0;
		// 获取持仓数组
		const rj::Value& jPos = root["positions"];
		// 如果持仓数组存在且有效
		if (!jPos.IsNull() && jPos.IsArray())
		{
			// 遍历所有持仓项目
			for (const rj::Value& pItem : jPos.GetArray())
			{
				// 获取合约代码
				const char* stdCode = pItem["code"].GetString();
				// 获取或创建持仓信息对象
				PosInfoPtr& pInfo = _pos_map[stdCode];
				if (pInfo == NULL)
					pInfo.reset(new PosInfo);
				// 读取平仓盈亏
				pInfo->_closeprofit = pItem["closeprofit"].GetDouble();
				// 读取持仓量
				pInfo->_volume = pItem["volume"].GetDouble();
				// 如果持仓量为0，则浮动盈亏也为0，否则读取浮动盈亏
				if (pInfo->_volume == 0)
					pInfo->_dynprofit = 0;
				else
					pInfo->_dynprofit = pItem["dynprofit"].GetDouble();

				// 累计总平仓盈亏和总浮动盈亏
				total_profit += pInfo->_closeprofit;
				total_dynprofit += pInfo->_dynprofit;

				// 获取持仓明细数组
				const rj::Value& details = pItem["details"];
				// 如果明细数组不存在或为空，则跳过
				if (details.IsNull() || !details.IsArray() || details.Size() == 0)
					continue;

				// 遍历所有持仓明细
				for (uint32_t i = 0; i < details.Size(); i++)
				{
					// 获取单个明细项目
					const rj::Value& dItem = details[i];
					// 创建明细信息对象
					DetailInfo dInfo;
					// 读取多空方向标记（true为多头，false为空头）
					dInfo._long = dItem["long"].GetBool();
					// 读取开仓价格
					dInfo._price = dItem["price"].GetDouble();
					// 读取持仓量
					dInfo._volume = dItem["volume"].GetDouble();
					// 读取开仓时间
					dInfo._opentime = dItem["opentime"].GetUint64();
					// 如果有开仓交易日字段，则读取
					if (dItem.HasMember("opentdate"))
						dInfo._opentdate = dItem["opentdate"].GetUint();

					// 读取浮动盈亏
					dInfo._profit = dItem["profit"].GetDouble();
					// 将明细信息添加到持仓信息中
					pInfo->_details.emplace_back(dInfo);
				}

				// 输出持仓确认日志
				WTSLogger::debug("Porfolio position confirmed,{} -> {}", stdCode, pInfo->_volume);
			}
		}

		// 更新资金账户的总浮动盈亏
		WTSFundStruct& fundInfo = _port_fund->fundInfo();
		fundInfo._dynprofit = total_dynprofit;

		// 输出持仓加载完成日志
		WTSLogger::debug("{} position info of portfolio loaded", _pos_map.size());
	}

	// 检查是否有风控参数配置
	if(root.HasMember("riskmon"))
	{
		// 读取风控参数
		const rj::Value& jRisk = root["riskmon"];
		// 读取风控仓位比例，用于调整交易仓位的大小
		_risk_volscale = jRisk["scale"].GetDouble();
		// 读取风控参数更新日期
		_risk_date = jRisk["date"].GetUint();
		// 风控参数用于控制交易风险，包括仓位比例和更新日期
		// 仓位比例用于调整交易仓位的大小，更新日期用于记录风控参数的更新时间
	}
}

/**
 * @brief 获取Tick数据切片
 * @details 根据合约代码和数量获取指定数量的Tick数据切片
 *          这个函数通常用于获取历史Tick数据，进行行情分析和策略计算
 * 
 * @param sid 订阅者ID
 * @param code 合约代码
 * @param count 请求的Tick数量
 * @return WTSTickSlice* Tick数据切片对象指针
 */
WTSTickSlice* WtEngine::get_tick_slice(uint32_t sid, const char* code, uint32_t count)
{
	// 从数据管理器中获取Tick数据切片
	return _data_mgr->get_tick_slice(code, count);
}

/**
 * @brief 获取最新的Tick行情数据
 * @details 根据标准化合约代码获取最新的Tick行情数据
 *          这个函数通常用于获取当前市场状态和价格信息
 * 
 * @param sid 订阅者ID
 * @param stdCode 标准化合约代码
 * @return WTSTickData* Tick数据对象指针，如果找不到则返回NULL
 */
WTSTickData* WtEngine::get_last_tick(uint32_t sid, const char* stdCode)
{
	// 从数据管理器中获取最新的Tick数据
	return _data_mgr->grab_last_tick(stdCode);
}

/**
 * @brief 获取K线切片数据
 * @details 根据标准化合约代码、周期类型和数量获取K线切片数据
 *          同时将订阅者ID添加到订阅映射表中，以便后续推送新的K线数据
 *          支持分钟和日线周期，并可以指定倍数和结束时间
 * 
 * @param sid 订阅者ID
 * @param stdCode 标准化合约代码
 * @param period 周期类型，如"m"(分钟)、"d"(日线)等
 * @param count 请求的K线数量
 * @param times 周期倍数，默认为1，如当period为"m"时，times为5表示5分钟线
 * @param etime 结束时间，默认为0表示当前时间
 * @return WTSKlineSlice* K线切片数据对象指针，如果找不到商品信息则返回NULL
 */
WTSKlineSlice* WtEngine::get_kline_slice(uint32_t sid, const char* stdCode, const char* period, uint32_t count, uint32_t times /* = 1 */, uint64_t etime /* = 0 */)
{
	// 解析标准化合约代码
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	// 获取商品信息
	WTSCommodityInfo* cInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);
	// 如果找不到商品信息，返回NULL
	if (cInfo == NULL)
		return NULL;

	// 使用线程本地存储的键值缓冲区
	thread_local static char key[64] = { 0 };
	// 生成唯一的订阅键，包含合约代码、周期和倍数
	fmtutil::format_to(key, "{}-{}-{}", stdCode, period, times);

	// 将订阅者ID添加到订阅映射表中
	SubList& sids = _bar_sub_map[key];
	sids[sid] = std::make_pair(sid, 0);

	// 定义K线周期类型
	WTSKlinePeriod kp;
	// 如果是分钟周期
	if (period[0] == 'm')
	{
		// 如果倍数是5的倍数，则使用基于5分钟线的周期
		if (times % 5 == 0)
		{
			kp = KP_Minute5;
			times /= 5;
		}
		else
			// 否则使用基于1分钟线的周期
			kp = KP_Minute1;
	}
	else
	{
		// 如果不是分钟周期，则默认使用日线周期
		kp = KP_DAY;
	}

	// 从数据管理器中获取K线切片数据
	return _data_mgr->get_kline_slice(stdCode, kp, times, count, etime);
}


/**
 * @brief 处理推送的行情数据
 * @details 处理接收到的Tick行情数据，包括处理原始合约和主力合约的行情
 *          对于非平合约，会同时生成主力合约的Tick数据并进行处理
 * 
 * @param curTick 当前Tick数据对象
 */
void WtEngine::handle_push_quote(WTSTickData* curTick)
{
	// 获取标准化合约代码
	std::string stdCode = curTick->code();
	// 将Tick数据传递给数据管理器处理
	_data_mgr->handle_push_quote(stdCode.c_str(), curTick);
	// 调用on_tick函数处理原始合约的Tick数据
	on_tick(stdCode.c_str(), curTick);

	// 获取当前价格
	double price = curTick->price();
	// 获取合约信息
	WTSContractInfo* cInfo = curTick->getContractInfo();

	// 判断是否为非平合约（即可以生成主力合约的合约）
	//if(hotFlag == 1)
	if(!cInfo->isFlat())
	{
		// 获取主力合约代码
		const char* hotCode = cInfo->getHotCode();
		// 创建主力合约的Tick数据对象
		WTSTickData* hotTick = WTSTickData::create(curTick->getTickStruct());
		// 设置主力合约代码
		hotTick->setCode(hotCode);
		// 设置合约信息
		hotTick->setContractInfo(curTick->getContractInfo());

		// 将主力合约的Tick数据传递给数据管理器处理
		_data_mgr->handle_push_quote(hotCode, hotTick);
		// 调用on_tick函数处理主力合约的Tick数据
		on_tick(hotCode, hotTick);

		// 释放主力合约的Tick数据对象
		hotTick->release();
	}
	// 以下注释代码是旧的实现方式，用于处理次主力合约
	//else if (hotFlag == 2)
	//{
	//	std::string scndCode = CodeHelper::stdCodeToStd2ndCode(stdCode.c_str());
	//	WTSTickData* scndTick = WTSTickData::create(curTick->getTickStruct());
	//	scndTick->setCode(scndCode.c_str());
	//	scndTick->setContractInfo(curTick->getContractInfo());

	//	_data_mgr->handle_push_quote(scndCode.c_str(), scndTick);
	//	on_tick(scndCode.c_str(), scndTick);

	//	scndTick->release();
	//}
}

/**
 * @brief 获取合约当前价格
 * @details 根据标准化合约代码获取当前价格，并处理复权相关的逻辑
 *          支持前复权（QFQ）和后复权（HFQ）两种复权方式
 * 
 * @param stdCode 标准化合约代码，可能带有复权后缀（如“-Q”表示前复权，“-H”表示后复权）
 * @return double 当前价格，如果找不到则返回0
 */
double WtEngine::get_cur_price(const char* stdCode)
{
	// 获取合约代码长度
	auto len = strlen(stdCode);
	// 获取合约代码的最后一个字符，用于判断是否需要复权
	char lastChar = stdCode[len - 1];
	//判断是否需要复权
	bool bAdjusted = (lastChar == SUFFIX_QFQ || lastChar == SUFFIX_HFQ);
	//前复权需要去掉后缀，后复权和未复权都直接使用原始代码
	std::string sCode = (lastChar == SUFFIX_QFQ) ? std::string(stdCode, len - 1) : stdCode;
	// 在价格缓存中查找
	auto it = _price_map.find(sCode);
	// 如果缓存中没有找到，需要从数据管理器中获取
	if(it == _price_map.end())
	{
		// 如果是复权代码，先去掉后缀获取原始代码
		std::string fCode = bAdjusted ? std::string(stdCode, len - 1) : stdCode;
		// 从数据管理器中获取最新的Tick数据
		WTSTickData* lastTick = _data_mgr->grab_last_tick(fCode.c_str());
		// 如果没有找到Tick数据，返回0
		if (lastTick == NULL)
			return 0.0;

		// 获取合约信息
		WTSContractInfo* cInfo = lastTick->getContractInfo();

		// 获取原始价格
		double ret = lastTick->price();
		// 释放Tick数据对象
		lastTick->release();

		// 如果是后复权，需要将价格乘以除权因子
		if (lastChar == SUFFIX_HFQ)
		{
			// 获取除权因子并进行价格调整
			ret *= get_exright_factor(stdCode, cInfo->getCommInfo());
		}

		// 将计算后的价格存入缓存
		_price_map[sCode] = ret;
		return ret;
	}
	else
	{
		// 如果缓存中找到了，直接返回缓存的价格
		return it->second;
	}
}

/**
 * @brief 获取日线价格数据
 * @details 根据标准化合约代码和指定的价格类型获取日线价格数据，支持复权处理
 *          价格类型包括开盘价、最高价、最低价和收盘价
 * 
 * @param stdCode 标准化合约代码，可能带有复权后缀
 * @param flag 价格类型标志：0-开盘价，1-最高价，2-最低价，3-收盘价，默认为0（开盘价）
 * @return double 指定类型的价格，如果找不到则返回0
 */
double WtEngine::get_day_price(const char* stdCode, int flag /* = 0 */)
{
	// 获取合约代码长度
	auto len = strlen(stdCode);
	// 获取合约代码的最后一个字符，用于判断是否需要复权
	char lastChar = stdCode[len - 1];
	//判断是否需要复权
	bool bAdjusted = (lastChar == SUFFIX_QFQ || lastChar == SUFFIX_HFQ);
	//前复权需要去掉后缀，后复权和未复权都直接使用原始代码
	std::string sCode = (lastChar == SUFFIX_QFQ) ? std::string(stdCode, len - 1) : stdCode;

	// 如果是复权代码，先去掉后缀获取原始代码
	std::string fCode = bAdjusted ? std::string(stdCode, len - 1) : stdCode;
	// 从数据管理器中获取最新的Tick数据
	WTSTickData* lastTick = _data_mgr->grab_last_tick(fCode.c_str());
	// 如果没有找到Tick数据，返回0
	if (lastTick == NULL)
		return 0.0;

	// 获取商品信息
	WTSCommodityInfo* commInfo = get_commodity_info(fCode.c_str());

	// 根据价格类型标志获取相应的价格
	double ret = 0.0;
	switch (flag)
	{
	case 0: // 开盘价
		ret = lastTick->open(); break;
	case 1: // 最高价
		ret = lastTick->high(); break;
	case 2: // 最低价
		ret = lastTick->low(); break;
	case 3: // 收盘价/当前价
		ret = lastTick->price(); break;
	default:
		break;
	}
	// 释放Tick数据对象
	lastTick->release();

	// 如果是后复权，需要将价格乘以除权因子
	if (lastChar == SUFFIX_HFQ)
	{
		// 获取除权因子并进行价格调整
		ret *= get_exright_factor(stdCode, commInfo);
	}

	return ret;
}

/**
 * @brief 获取除权因子
 * @details 根据合约代码和商品信息获取除权因子，用于价格复权计算
 *          对于股票，从数据管理器中获取除权因子
 *          对于期货，从主力合约管理器中获取规则因子
 * 
 * @param stdCode 标准化合约代码
 * @param commInfo 商品信息对象，如果为NULL则会自动获取
 * @return double 除权因子，默认为1.0（不需要复权）
 */
double WtEngine::get_exright_factor(const char* stdCode, WTSCommodityInfo* commInfo /* = NULL */)
{
	// 如果没有提供商品信息，则自动获取
	if (commInfo == NULL)
		commInfo = get_commodity_info(stdCode);

	// 如果仍然无法获取商品信息，则返回默认因子1.0（不复权）
	if (commInfo == NULL)
		return 1.0;

	// 如果是股票，从数据管理器中获取除权因子
	if (commInfo->isStock())
		return _data_mgr->get_adjusting_factor(stdCode, get_trading_date());
	else
	{
		// 如果是期货，则获取规则标签
		const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
		// 如果有规则标签，则从主力合约管理器中获取规则因子
		if(strlen(ruleTag) > 0)
			return _hot_mgr->getRuleFactor(ruleTag, commInfo->getFullPid(), get_trading_date());
	}

	// 默认返回1.0（不复权）
	return 1.0;
}

/**
 * @brief 获取复权调整标志
 * @details 判断当前是否需要进行复权调整。当当前日期与交易日期不同时，需要进行复权调整
 * 
 * @return uint32_t 复权调整标志：0-不需要复权，1-需要复权
 */
uint32_t WtEngine::get_adjusting_flag()
{
	// 当当前日期与交易日期相同时，不需要复权
	if (_cur_date == _cur_tdate)
		return 0;

	// 当当前日期与交易日期不同时，需要复权
	return 1;
}

/**
 * @brief 订阅行情数据
 * @details 订阅指定合约的Tick行情数据，支持主力合约和复权合约代码
 *          对于主力合约代码，会自动转换为实际的合约代码
 *          对于复权合约代码，会记录复权标志以便后续处理
 * 
 * @param sid 订阅者ID
 * @param stdCode 标准化合约代码
 */
void WtEngine::sub_tick(uint32_t sid, const char* stdCode)
{
	// 如果是主力合约代码, 如SHFE.ag.HOT, 那么要转换成原合约代码, 如SHFE.ag.1912
	// 因为执行器只识别原合约代码
	// 获取合约的规则标签，用于判断是否为主力合约
	const char* ruleTag = _hot_mgr->getRuleTag(stdCode);
	// 如果有规则标签，说明是主力合约
	if(strlen(ruleTag) > 0)
	{
		// 以下注释代码是旧的实现方式
		//SubList& sids = _tick_sub_map[stdCode];
		//sids[sid] = std::make_pair(sid, 0);

		// 获取合约代码长度
		std::size_t length = strlen(stdCode);
		// 复权标志：0-不复权，1-前复权，2-后复权
		uint32_t flag = 0;
		// 判断是否有复权后缀
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			// 如果有复权后缀，则去掉后缀
			length--;

			// 设置复权标志：1-前复权，2-后复权
			flag = (stdCode[length] == SUFFIX_QFQ) ? 1 : 2;
		}

		// 将订阅者ID和复权标志添加到订阅映射表中
		SubList& sids = _tick_sub_map[std::string(stdCode, length)];
		sids[sid] = std::make_pair(sid, flag);

		// 解析标准化合约代码
		CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
		// 获取实际的原始合约代码
		std::string rawCode = _hot_mgr->getCustomRawCode(ruleTag, cInfo.stdCommID(), _cur_tdate);
		// 将原始合约代码转换为标准化合约代码
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
	// 如果不是主力合约，则直接订阅原始合约
	else
	{
		// 获取合约代码长度
		std::size_t length = strlen(stdCode);
		// 复权标志：0-不复权，1-前复权，2-后复权
		uint32_t flag = 0;
		// 判断是否有复权后缀
		if (stdCode[length - 1] == SUFFIX_QFQ || stdCode[length - 1] == SUFFIX_HFQ)
		{
			// 如果有复权后缀，则去掉后缀
			length--;

			// 设置复权标志：1-前复权，2-后复权
			flag = (stdCode[length - 1] == SUFFIX_QFQ) ? 1 : 2;
		}

		// 将订阅者ID和复权标志添加到订阅映射表中
		SubList& sids = _tick_sub_map[std::string(stdCode, length)];
		sids[sid] = std::make_pair(sid, flag);

		// 以下注释代码是旧的实现方式，用于记录订阅的原始合约代码
		//_ticksubed_raw_codes.insert(std::string(stdCode, length));
	}
}

/**
 * @brief 加载交易费用配置
 * @details 从指定的配置文件中加载交易费用模板，包括开仓手续费、平仓手续费、平今手续费和保证金率等
 *          这些费用将应用于交易过程中的成本计算
 * 
 * @param filename 费用模板配置文件路径
 */
void WtEngine::load_fees(const char* filename)
{
	// 如果文件名为空，直接返回
	if (strlen(filename) == 0)
		return;

	// 检查文件是否存在
	if (!StdFile::exists(filename))
	{
		// 如果文件不存在，记录错误日志
		WTSLogger::error("Fee templates file {} not exists", filename);
		return;
	}

	// 从文件加载配置
	WTSVariant* cfg = WTSCfgLoader::load_from_file(filename);
	if (cfg == NULL)
	{
		// 如果加载失败，记录错误日志
		WTSLogger::error("Fee templates file {} loading failed", filename);
		return;
	}

	// 获取所有配置项的键名（商品ID）
	auto keys = cfg->memberNames();
	// 遍历所有商品配置
	for (const std::string& fullPid : keys)
	{
		// 获取当前商品的费用配置
		WTSVariant* cfgItem = cfg->get(fullPid.c_str());
		// 将商品ID按点分隔，分为交易所和品种代码
		const StringVector& ay = StrUtil::split(fullPid, ".");
		// 从基础数据管理器中获取商品信息
		WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(ay[0].c_str(), ay[1].c_str());
		// 如果商品信息不存在，跳过当前项
		if (commInfo == NULL)
			continue;

		// 设置费用率：开仓手续费、平仓手续费、平今手续费，以及是否按手数计算
		commInfo->setFeeRates(cfgItem->getDouble("open"), cfgItem->getDouble("close"), cfgItem->getDouble("closetoday"), cfgItem->getBoolean("byvolume"));
		// 设置保证金率
		commInfo->setMarginRate(cfgItem->getDouble("margin"));
	}

	// 释放配置对象
	cfg->release();

	// 记录加载完成日志
	WTSLogger::info("{} fee templates loaded", _fee_map.size());
}

double WtEngine::calc_fee(const char* stdCode, double price, double qty, uint32_t offset)
{
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	const char* stdPID = codeInfo.stdCommID();
	auto it = _fee_map.find(stdPID);
	if (it == _fee_map.end())
	{
		WTSLogger::warn("Fee template of {} not found, return 0.0 as default", stdPID);
		return 0.0;
	}

	double ret = 0.0;
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(stdPID);
	const FeeItem& fItem = it->second;
	if(fItem._by_volume)
	{
		switch (offset)
		{
		case 0: ret = fItem._open*qty; break;
		case 1: ret = fItem._close*qty; break;
		case 2: ret = fItem._close_today*qty; break;
		default: ret = 0.0; break;
		}
	}
	else
	{
		double amount = price*qty*commInfo->getVolScale();
		switch (offset)
		{
		case 0: ret = fItem._open*amount; break;
		case 1: ret = fItem._close*amount; break;
		case 2: ret = fItem._close_today*amount; break;
		default: ret = 0.0; break;
		}
	}

	return (int32_t)(ret * 100 + 0.5) / 100.0;
}

/**
 * @brief 添加交易信号
 * @details 添加策略生成的交易信号，并根据情况决定是立即执行还是等待下一个Tick数据到来再执行。
 *          这种设计可以确保策略和组合的理论成交价一致，避免组合成交价永远比策略提前一个Tick的问题
 * 
 * @param stdCode 标准化合约代码
 * @param qty 目标持仓量，正数表示多头持仓，负数表示空头持仓
 * @param bStandBy 是否等待下一个Tick数据再执行，默认为true。如果是bar内触发的信号，可以设置为false直接执行
 */
void WtEngine::append_signal(const char* stdCode, double qty, bool bStandBy /* = true */)
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
	// 获取当前合约价格
	double curPx = get_cur_price(stdCode);
	// 如果需要等待下一个Tick或者当前价格为0，则将信号存入信号映射表
	if(bStandBy || decimal::eq(curPx, 0.0))
	{
		// 获取或创建合约的信号信息
		SigInfo& sInfo = _sig_map[stdCode];
		// 设置目标持仓量
		sInfo._volume = qty;
		// 记录信号生成时间
		sInfo._gentime = (uint64_t)_cur_date * 1000000000 + (uint64_t)_cur_raw_time * 100000 + _cur_secs;
	}
	else
	{
		// 如果不需要等待，则直接调用do_set_position设置持仓
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

/**
 * @brief 设置合约持仓量
 * @details 根据目标仓位调整当前持仓，包括平仓、开仓和反手操作。
 *          该函数是交易引擎的核心函数，负责实际执行交易操作并更新资金、持仓和日志信息
 * 
 * @param stdCode 标准化合约代码
 * @param qty 目标持仓量，正数表示多头持仓，负数表示空头持仓
 * @param curPx 当前价格，默认为-1，表示需要自动获取当前价格
 */
void WtEngine::do_set_position(const char* stdCode, double qty, double curPx /* = -1 */)
{
	// 获取或创建合约的持仓信息对象
	PosInfoPtr& pInfo = _pos_map[stdCode];
	if (pInfo == NULL)
		pInfo.reset(new PosInfo);

	// 加锁保护持仓信息，防止多线程并发修改
	SpinLock lock(pInfo->_mtx);

	// 如果传入的价格小于0，则自动获取当前合约价格
	if(decimal::lt(curPx, 0))
		curPx = get_cur_price(stdCode);

	// 生成当前时间戳和交易日期
	uint64_t curTm = (uint64_t)_cur_date * 10000 + _cur_time;
	uint32_t curTDate = _cur_tdate;

	// 如果目标仓位与当前仓位相等，则无需操作，直接返回
	if (decimal::eq(pInfo->_volume, qty))
		return;

	// 计算目标仓位与当前仓位的差值，决定是增加还是减少持仓
	double diff = qty - pInfo->_volume;

	// 提取合约信息和商品信息
	CodeHelper::CodeInfo codeInfo = CodeHelper::extractStdCode(stdCode, _hot_mgr);
	WTSCommodityInfo* commInfo = _base_data_mgr->getCommodity(codeInfo._exchg, codeInfo._product);

	WTSFundStruct& fundInfo = _port_fund->fundInfo();

	// 如果当前持仓和目标仓位方向一致，则增加一条明细，增加数量即可
	if (decimal::gt(pInfo->_volume*diff, 0))
	{
		pInfo->_volume = qty;

		DetailInfo dInfo;
		dInfo._long = decimal::gt(qty, 0);
		dInfo._price = curPx;
		dInfo._volume = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		pInfo->_details.emplace_back(dInfo);

		double fee = commInfo->calcFee(curPx, abs(qty), 0);
		fundInfo._fees += fee;
		fundInfo._balance -= fee;

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
			fundInfo._profit += profit;
			fundInfo._balance += profit;

			double fee = commInfo->calcFee(curPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			fundInfo._fees += fee;
			fundInfo._balance -= fee;

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
			double fee = commInfo->calcFee(curPx, abs(qty), 0);
			fundInfo._fees += fee;
			fundInfo._balance -= fee;

			log_trade(stdCode, dInfo._long, true, curTm, curPx, abs(left), fee);
		}
	}
}

/**
 * @brief 将任务添加到后台任务队列
 * @details 将任务添加到任务队列中，并在需要时创建后台任务线程。该函数用于异步处理风控、资金、持仓等更新任务
 * 
 * @param task 要添加的任务项，类型为函数对象
 */
void WtEngine::push_task(TaskItem task)
{
	{
		// 加锁保护任务队列
		StdUniqueLock lock(_mtx_task);
		// 将任务添加到队列中
		_task_queue.push(task);
	}
	

	// 如果后台任务线程还没有创建，则创建一个
	if (_thrd_task == NULL)
	{
		_thrd_task.reset(new StdThread([this]{
			task_loop();
		}));
	}

	// 通知所有等待任务的线程
	_cond_task.notify_all();
}

/**
 * @brief 后台任务处理循环
 * @details 在单独的线程中处理任务队列中的任务，包括风控、资金、持仓等更新操作。
 *          通过这种方式将这些可能耗时的操作放到后台处理，避免影响主线程的响应速度
 */
void WtEngine::task_loop()
{
	// 循环处理，直到终止信号被设置
	while (!_terminated)
	{
		TaskQueue temp;
		{
			// 加锁保护任务队列
			StdUniqueLock lock(_mtx_task);
			// 如果任务队列为空，则等待条件变量通知
			if(_task_queue.empty())
			{
				_cond_task.wait(_mtx_task);
				continue;
			}

			// 交换任务队列，将当前队列中的所有任务移到临时队列中处理
			// 这样可以快速释放锁，不影响其他线程添加新任务
			temp.swap(_task_queue);
		}

		// 处理临时队列中的所有任务
		for (;;)
		{
			// 如果临时队列为空，则退出循环
			if(temp.empty())
				break;

			// 获取并执行队列中的第一个任务
			TaskItem& item = temp.front();
			item();  // 调用函数对象
			temp.pop();  // 从队列中移除已执行的任务
		}
	}
}

/**
 * @brief 初始化风险监控模块
 * @details 根据配置加载风控模块动态库，创建风控工厂和风控器实例
 *          风控模块负责监控交易过程中的风险，如持仓量、浮动盈亏等
 * 
 * @param cfg 风控模块配置
 * @return bool 初始化是否成功
 */
bool WtEngine::init_riskmon(WTSVariant* cfg)
{
	// 检查配置是否为空
	if (cfg == NULL)
		return false;

	// 检查风控模块是否激活
	if (!cfg->getBoolean("active"))
		return false;

	// 获取风控模块名称，并进行平台适配（添加正确的文件后缀如.dll或.so）
	std::string module = DLLHelper::wrap_module(cfg->getCString("module"));
	//先看工作目录下是否有对应模块
	std::string dllpath = WtHelper::getCWD() + module;
	//如果没有,则再看模块目录,即dll同目录下
	if (!StdFile::exists(dllpath.c_str()))
		dllpath = WtHelper::getInstDir() + module;

	// 加载风控模块动态库
	DllHandle hInst = DLLHelper::load_library(dllpath.c_str());
	if (hInst == NULL)
	{
		// 如果加载失败，记录错误日志
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon module {} loading failed", dllpath.c_str());
		return false;
	}

	// 获取风控工厂创建函数
	FuncCreateRiskMonFact creator = (FuncCreateRiskMonFact)DLLHelper::get_symbol(hInst, "createRiskMonFact");
	if (creator == NULL)
	{
		// 如果获取失败，释放动态库并记录错误日志
		DLLHelper::free_library(hInst);
		WTSLogger::log_by_cat("risk", LL_ERROR, "Riskmon module {} is not compatible", module.c_str());
		return false;
	}

	// 设置风控工厂相关信息
	_risk_fact._module_inst = hInst;  // 动态库句柄
	_risk_fact._module_path = module;  // 模块路径
	_risk_fact._creator = creator;     // 创建函数
	// 获取风控工厂删除函数
	_risk_fact._remover = (FuncDeleteRiskMonFact)DLLHelper::get_symbol(hInst, "deleteRiskMonFact");
	// 创建风控工厂实例
	_risk_fact._fact = _risk_fact._creator();

	// 获取风控器名称
	const char* name = cfg->getCString("name");
	
	// 创建风控器包装器，包含风控器实例和对应的工厂
	_risk_mon.reset(new WtRiskMonWrapper(_risk_fact._fact->createRiskMonotor(name), _risk_fact._fact));
	// 初始化风控器，传入引擎实例和配置
	_risk_mon->self()->init(this, cfg);

	return true;
}

/**
 * @brief 初始化交易日志输出
 * @details 初始化交易日志和平仓日志文件，用于记录交易执行和平仓结果。
 *          如果文件不存在则创建新文件并写入列头，如果已存在则在文件末尾继续追加
 */
void WtEngine::init_outputs()
{
	// 获取组合目录
	std::string folder = WtHelper::getPortifolioDir();
	
	// 初始化交易日志文件
	std::string filename = folder + "trades.csv";
	_trade_logs.reset(new BoostFile());
	{
		// 检查文件是否存在
		bool isNewFile = !BoostFile::exists(filename.c_str());
		// 创建或打开文件
		_trade_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			// 如果是新文件，则写入CSV列头
			_trade_logs->write_file("code,time,direct,action,price,qty,fee\n");
		}
		else
		{
			// 如果是已存在的文件，则定位到文件末尾准备追加
			_trade_logs->seek_to_end();
		}
	}

	// 初始化平仓日志文件
	filename = folder + "closes.csv";
	_close_logs.reset(new BoostFile());
	{
		// 检查文件是否存在
		bool isNewFile = !BoostFile::exists(filename.c_str());
		// 创建或打开文件
		_close_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			// 如果是新文件，则写入CSV列头
			_close_logs->write_file("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit\n");
		}
		else
		{
			// 如果是已存在的文件，则定位到文件末尾准备追加
			_close_logs->seek_to_end();
		}
	}
}

/**
 * @brief 记录交易日志
 * @details 将交易信息写入交易日志文件，包括合约代码、时间、方向、操作类型、价格、数量、手续费等信息
 * 
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头交易
 * @param isOpen 是否为开仓操作，true表示开仓，false表示平仓
 * @param curTime 当前交易时间戳
 * @param price 交易价格
 * @param qty 交易数量
 * @param fee 交易手续费，默认为0.0
 */
void WtEngine::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, double fee /* = 0.0 */)
{
	if (_trade_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE") << "," << price << "," << qty << "," << fee << "\n";
		_trade_logs->write_file(ss.str());
	}
}

/**
 * @brief 记录平仓日志
 * @details 将平仓信息写入平仓日志文件，包括合约代码、方向、开仓时间、开仓价格、平仓时间、平仓价格、数量、盈亏等信息
 * 
 * @param stdCode 标准化合约代码
 * @param isLong 是否为多头持仓
 * @param openTime 开仓时间戳
 * @param openpx 开仓价格
 * @param closeTime 平仓时间戳
 * @param closepx 平仓价格
 * @param qty 平仓数量
 * @param profit 平仓盈亏
 * @param totalprofit 累计盈亏，默认为0
 */
void WtEngine::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double totalprofit /* = 0 */)
{
	if (_close_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
			<< "," << closeTime << "," << closepx << "," << qty << "," << profit << ","
			<< totalprofit << "\n";
		_close_logs->write_file(ss.str());
	}
}