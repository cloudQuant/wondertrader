/*!
* \file SelMocker.cpp
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略回测模拟器实现
* \details 该文件实现了WonderTrader中的选股策略回测模拟器，
*          负责模拟选股策略的执行、信号处理、仓位管理以及回测结果的计算与导出。
*          主要功能包括：
*          1. 处理各类事件（K线、Tick、调度等）
*          2. 管理策略持仓和信号
*          3. 计算动态盈亏
*          4. 生成回测报告和输出结果
*/
#include "SelMocker.h"
#include "WtHelper.h"

#include <exception>
#include <boost/filesystem.hpp>

#include "../Share/StdUtils.hpp"
#include "../Share/StrUtil.hpp"
#include "../Share/decimal.h"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../WTSTools/WTSLogger.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

/**
 * @brief 生成选股策略上下文ID
 * @return 返回新生成的上下文ID
 * @details 该函数用于立选股策略上下文对象生成唯一的ID
 *          使用原子变量保证线程安全，从3000开始递增
 *          每个选股策略实例都会分配一个唯一的上下文ID
 */
inline uint32_t makeSelCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 3000 };
	return _auto_context_id.fetch_add(1);
}


/**
 * @brief 选股策略模拟器构造函数
 * @param replayer 历史数据回放器指针
 * @param name 策略名称
 * @param slippage 滑点数值，默认为0
 * @param isRatioSlp 是否为比例滑点，默认为false
 * @details 初始化选股策略模拟器对象，设置基本参数和初始状态
 *          初始化所有计数器和标志位，并生成唯一的上下文ID
 *          滑点参数用于模拟实际交易中的滑点成本
 */
SelMocker::SelMocker(HisDataReplayer* replayer, const char* name, int32_t slippage /* = 0 */, bool isRatioSlp /* = false */)
	: ISelStraCtx(name)
	, _replayer(replayer)
	, _total_calc_time(0)
	, _emit_times(0)
	, _is_in_schedule(false)
	, _ud_modified(false)
	, _strategy(NULL)
	, _slippage(slippage)
	, _ratio_slippage(isRatioSlp)
	, _schedule_times(0)
{
	_context_id = makeSelCtxId();
}


/**
 * @brief 选股策略模拟器析构函数
 * @details 析构选股策略模拟器对象，负责资源清理
 *          在当前版本中，所有的资源都由外部管理，所以析构函数为空
 */
SelMocker::~SelMocker()
{
}

/**
 * @brief 将策略数据导出到JSON文件
 * @details 将选股策略的关键数据（持仓信息、资金信息、信号信息等）序列化为JSON格式并写入文件
 */
void SelMocker::dump_stradata()
{
	rj::Document root(rj::kObjectType);

	{//持仓数据保存
		rj::Value jPos(rj::kArrayType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
		{
			const char* stdCode = it->first.c_str();
			const PosInfo& pInfo = it->second;

			rj::Value pItem(rj::kObjectType);
			pItem.AddMember("code", rj::Value(stdCode, allocator), allocator);
			pItem.AddMember("volume", pInfo._volume, allocator);
			pItem.AddMember("closeprofit", pInfo._closeprofit, allocator);
			pItem.AddMember("dynprofit", pInfo._dynprofit, allocator);
			pItem.AddMember("lastentertime", pInfo._last_entertime, allocator);
			pItem.AddMember("lastexittime", pInfo._last_exittime, allocator);

			rj::Value details(rj::kArrayType);
			for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
			{
				const DetailInfo& dInfo = *dit;
				rj::Value dItem(rj::kObjectType);
				dItem.AddMember("long", dInfo._long, allocator);
				dItem.AddMember("price", dInfo._price, allocator);
				dItem.AddMember("maxprice", dInfo._max_price, allocator);
				dItem.AddMember("minprice", dInfo._min_price, allocator);
				dItem.AddMember("volume", dInfo._volume, allocator);
				dItem.AddMember("opentime", dInfo._opentime, allocator);
				dItem.AddMember("opentdate", dInfo._opentdate, allocator);

				dItem.AddMember("profit", dInfo._profit, allocator);
				dItem.AddMember("maxprofit", dInfo._max_profit, allocator);
				dItem.AddMember("maxloss", dInfo._max_loss, allocator);
				dItem.AddMember("opentag", rj::Value(dInfo._opentag, allocator), allocator);

				details.PushBack(dItem, allocator);
			}

			pItem.AddMember("details", details, allocator);

			jPos.PushBack(pItem, allocator);
		}

		root.AddMember("positions", jPos, allocator);
	}

	{//资金保存
		rj::Value jFund(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		jFund.AddMember("total_profit", _fund_info._total_profit, allocator);
		jFund.AddMember("total_dynprofit", _fund_info._total_dynprofit, allocator);
		jFund.AddMember("total_fees", _fund_info._total_fees, allocator);
		jFund.AddMember("tdate", _cur_tdate, allocator);

		root.AddMember("fund", jFund, allocator);
	}

	{//信号保存
		rj::Value jSigs(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto& m : _sig_map)
		{
			const char* stdCode = m.first.c_str();
			const SigInfo& sInfo = m.second;

			rj::Value jItem(rj::kObjectType);
			jItem.AddMember("usertag", rj::Value(sInfo._usertag.c_str(), allocator), allocator);

			jItem.AddMember("volume", sInfo._volume, allocator);
			jItem.AddMember("sigprice", sInfo._sigprice, allocator);
			jItem.AddMember("gentime", sInfo._gentime, allocator);

			jSigs.AddMember(rj::Value(stdCode, allocator), jItem, allocator);
		}

		root.AddMember("signals", jSigs, allocator);
	}

	{
		std::string folder = WtHelper::getOutputDir();
		folder += _name;
		folder += "/";

		std::string filename = folder;
		filename += _name;
		filename += ".json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}
}


/**
 * @brief 导出策略回测结果
 * @details 将选股策略的回测结果导出到CSV文件，包括以下文件：
 *          1. trades.csv - 交易明细，包含合约、时间、方向、操作、价格、数量、标签和费用
 *          2. closes.csv - 平仓记录，包含合约、开仓时间、平仓时间、开仓价、平仓价等
 *          3. signals.csv - 信号记录，包含合约、时间、价格、仓位变化、标签
 *          4. positions.csv - 持仓汇总，包含日期、合约、持仓量、平仓盈亏和浮动盈亏
 *          5. funds.csv - 资金汇总，包含日期、总盈亏、浮动盈亏、净值和交易费用
 *          这些文件可以用于后续的分析和生成回测报告
 */
void SelMocker::dump_outputs()
{
	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "/";
	boost::filesystem::create_directories(folder.c_str());

	std::string filename = folder + "trades.csv";
	std::string content = "code,time,direct,action,price,qty,tag,fee\n";
	content += _trade_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "closes.csv";
	content = "code,direct,opentime,openprice,closetime,closeprice,qty,profit,maxprofit,maxloss,totalprofit,entertag,exittag,openbarno,closebarno\n";
	content += _close_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());


	filename = folder + "funds.csv";
	content = "date,closeprofit,positionprofit,dynbalance,fee\n";
	content += _fund_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());


	filename = folder + "signals.csv";
	content = "code,target,sigprice,gentime,usertag\n";
	content += _sig_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	filename = folder + "positions.csv";
	content = "date,code,volume,closeprofit,dynprofit\n";
	if (!_pos_logs.str().empty()) content += _pos_logs.str();
	StdFile::write_file_content(filename.c_str(), (void*)content.c_str(), content.size());

	{
		rj::Document root(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();
		for (auto it = _user_datas.begin(); it != _user_datas.end(); it++)
		{
			root.AddMember(rj::Value(it->first.c_str(), allocator), rj::Value(it->second.c_str(), allocator), allocator);
		}

		filename = folder;
		filename += "ud_";
		filename += _name;
		filename += ".json";

		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		root.Accept(writer);
		StdFile::write_file_content(filename.c_str(), sb.GetString());
	}
}

/**
 * @brief 记录策略信号
 * @param stdCode 标准合约代码
 * @param target 目标仓位
 * @param price 信号价格
 * @param gentime 信号生成时间
 * @param usertag 用户标签，默认为空字符串
 * @details 将策略生成的信号记录到日志流中
 *          记录格式为：时间,合约代码,价格,目标仓位,用户标签,策略ID
 *          这些日志用于后续生成signals.csv文件
 */
void SelMocker::log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag /* = "" */)
{
	_sig_logs << fmt::format("{},{},{},{},{},{}\n", gentime, stdCode, price, target, usertag, _strategy->id());
}

/**
 * @brief 记录交易记录
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头，true表示多头，false表示空头
 * @param isOpen 是否为开仓，true表示开仓，false表示平仓
 * @param curTime 交易时间
 * @param price 交易价格
 * @param qty 交易数量
 * @param userTag 用户标签
 * @param fee 交易费用
 * @details 记录交易明细到日志流中
 *          记录格式为：时间,合约代码,方向(LONG/SHORT),操作(OPEN/CLOSE),价格,数量,标签,费用
 *          这些日志用于后续生成trades.csv文件
 */
void SelMocker::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag, double fee)
{
	_trade_logs << fmt::format("{},{},{},{},{},{},{},{}\n", curTime, stdCode, isLong ? "LONG" : "SHORT", isOpen ? "OPEN" : "CLOSE", price, qty, userTag, fee);
}

/**
 * @brief 记录平仓记录
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头，true表示多头，false表示空头
 * @param openTime 开仓时间
 * @param openpx 开仓价格
 * @param closeTime 平仓时间
 * @param closepx 平仓价格
 * @param qty 交易数量
 * @param profit 交易盈亏
 * @param maxprofit 最大盈利
 * @param maxloss 最大亏损
 * @param totalprofit 总盈亏，默认为0
 * @param enterTag 开仓标签，默认为空字符串
 * @param exitTag 平仓标签，默认为空字符串
 * @param openBarNo 开仓操作K线编号，默认为0
 * @param closeBarNo 平仓操作K线编号，默认为0
 * @details 记录平仓明细到日志流中，包含开平仓信息和盈亏统计
 *          记录格式包括：平仓时间,合约代码,方向,开仓时间,开仓价,平仓价,数量,盈亏,最大盈利,最大亏损,总盈亏等
 *          这些日志用于后续生成closes.csv文件，进行策略统计分析
 */
void SelMocker::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double maxprofit, double maxloss,
	double totalprofit /* = 0 */, const char* enterTag /* = "" */, const char* exitTag /* = "" */, uint32_t openBarNo/* = 0*/, uint32_t closeBarNo/* = 0*/)
{
	_close_logs << fmt::format("{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", closeTime, stdCode, isLong ? "LONG" : "SHORT", openTime, openpx, closepx, qty, profit, maxprofit, maxloss,
		totalprofit, enterTag, exitTag, openBarNo, closeBarNo);
}

/**
 * @brief 初始化选股策略工厂
 * @param cfg 策略配置
 * @return 初始化是否成功
 * @details 根据配置初始化选股策略工厂，主要流程如下：
 *          1. 检查配置是否有效
 *          2. 判断是否启用策略动态链接库
 *          3. 加载策略动态链接库并获取工厂创建函数
 *          4. 创建策略工厂实例
 *          5. 创建具体的策略实例并进行初始化
 *          这个函数是选股策略模拟器的关键初始化函数
 */
bool SelMocker::init_sel_factory(WTSVariant* cfg)
{
	if (cfg == NULL)
		return false;

	const char* module = cfg->getCString("module");

	DllHandle hInst = DLLHelper::load_library(module);
	if (hInst == NULL)
		return false;

	FuncCreateSelStraFact creator = (FuncCreateSelStraFact)DLLHelper::get_symbol(hInst, "createSelStrategyFact");
	if (creator == NULL)
	{
		DLLHelper::free_library(hInst);
		return false;
	}

	_factory._module_inst = hInst;
	_factory._module_path = module;
	_factory._creator = creator;
	_factory._remover = (FuncDeleteSelStraFact)DLLHelper::get_symbol(hInst, "deleteSelStrategyFact");
	_factory._fact = _factory._creator();

	WTSVariant* cfgStra = cfg->get("strategy");
	if (cfgStra)
	{
		_strategy = _factory._fact->createStrategy(cfgStra->getCString("name"), cfgStra->getCString("id"));
		if (_strategy)
		{
			WTSLogger::info("Strategy {}.{} created,strategy ID: {}", _factory._fact->getName(), _strategy->getName(), _strategy->id());
		}
		_strategy->init(cfgStra->get("params"));
		_name = _strategy->id();
	}

	return true;
}

//////////////////////////////////////////////////////////////////////////
//IDataSink
void SelMocker::handle_init()
{
	this->on_init();
}

/**
 * @brief 处理K线关闭事件
 * @param stdCode 标准合约代码
 * @param period 周期标识（如'd'表示日K，'m'表示分钟线）
 * @param times 周期倍数（如分钟周期为5，则表示5分钟线）
 * @param newBar 新的K线数据
 * @details 实现IDataSink接口的K线关闭事件处理函数，当新的K线完成时调用
 *          该函数将K线数据转发给on_bar函数处理，触发策略的K线事件
 */
void SelMocker::handle_bar_close(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	this->on_bar(stdCode, period, times, newBar);
}

/**
 * @brief 处理定时调度事件
 * @param uDate 当前日期（格式YYYYMMDD）
 * @param uTime 当前时间（格式HHMMSS）
 * @details 实现IDataSink接口的定时调度事件处理函数，当回测器触发定时调度时调用
 *          该函数计算下一分钟的时间，处理跨日情况，并将调度事件转发给on_schedule函数
 *          定时调度是策略执行的重要触发机制
 */
void SelMocker::handle_schedule(uint32_t uDate, uint32_t uTime)
{
	uint32_t nextTime = TimeUtils::getNextMinute(uTime, 1);
	if (nextTime < uTime)
		uDate = TimeUtils::getNextDate(uDate);
	this->on_schedule(uDate, uTime, nextTime);
}

/**
 * @brief 处理交易日开始事件
 * @param uCurDate 当前交易日（格式YYYYMMDD）
 * @details 实现IDataSink接口的交易日开始事件处理函数，当新的交易日开始时调用
 *          该函数将交易日开始事件转发给on_session_begin函数处理
 *          通常用于重置每日的交易状态和冻结仓位
 */
void SelMocker::handle_session_begin(uint32_t uCurDate)
{
	this->on_session_begin(uCurDate);
}

/**
 * @brief 处理交易日结束事件
 * @param uCurDate 当前交易日（格式YYYYMMDD）
 * @details 实现IDataSink接口的交易日结束事件处理函数，当交易日结束时调用
 *          该函数将交易日结束事件转发给on_session_end函数进行计算汇总
 *          用于汇总当日交易盈亏和记录日志
 */
void SelMocker::handle_session_end(uint32_t uCurDate)
{
	this->on_session_end(uCurDate);
}

/**
 * @brief 处理回测结束事件
 * @details 实现IDataSink接口的回测结束事件处理函数，当所有回测数据处理完毕时调用
 *          该函数主要完成以下工作：
 *          1. 记录策略调度统计信息（调度次数、总计算时间、平均计算时间）
 *          2. 导出策略运行结果
 *          3. 导出策略数据
 *          4. 触发回测结束回调
 *          这是回测结束时的最后一个处理函数
 */
void SelMocker::handle_replay_done()
{
	WTSLogger::log_dyn("strategy", _name.c_str(), LL_INFO, 
		"Strategy has been scheduled for {} times,totally taking {} microsecs,average of {} microsecs",
		_emit_times, _total_calc_time, _total_calc_time / _emit_times);

	dump_outputs();

	dump_stradata();

	this->on_bactest_end();
}

/**
 * @brief 处理Tick数据
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @param pxType 价格类型（0:实时价格，1:延迟行情，2:模拟价格，3:收盘价格）
 * @details 实现IDataSink接口的Tick数据处理函数，当新的Tick数据到达时调用
 *          主要函数包括：
 *          1. 获取当前价格和上一价格
 *          2. 更新价格缓存
 *          3. 处理信号触发和持仓盈亏计算
 *          4. 调用on_tick_updated通知策略
 *          5. 根据价格类型处理不同情况
 */
void SelMocker::handle_tick(const char* stdCode, WTSTickData* newTick, uint32_t pxType)
{
	double cur_px = newTick->price();

	/*
	 *	By Wesley @ 2022.04.19
	 *	这里的逻辑改了一下
	 *	如果缓存的价格不存在，则上一笔价格就用最新价
	 *	这里主要是为了应对跨日价格跳空的情况
	 */
	double last_px = cur_px;
	if (pxType != 0)
	{
		auto it = _price_map.find(stdCode);
		if (it != _price_map.end())
			last_px = it->second.first;
		else
			last_px = cur_px;
	}

	_price_map[stdCode].first = cur_px;
	_price_map[stdCode].second = (uint64_t)newTick->actiondate() * 1000000000 + newTick->actiontime();

	//先检查是否要信号要触发
	proc_tick(stdCode, last_px, cur_px);

	on_tick_updated(stdCode, newTick);

	/*
	 *	By Wesley @ 2022.04.19
	 *	isBarEnd，如果是逐tick回放，这个永远都是true，永远也不会触发下面这段逻辑
	 *	如果是模拟的tick数据，用收盘价模拟tick的时候，isBarEnd才会为true
	 *	如果不是收盘价模拟的tick，那么直接在当前tick触发撮合逻辑
	 *	这样做的目的是为了让在模拟tick触发的ontick中下单的信号能够正常处理
	 *	而不至于在回测的时候成交价偏离太远
	 */
	if (pxType != 3)
		proc_tick(stdCode, last_px, cur_px);
}

/**
 * @brief 处理合约的Tick数据
 * @param stdCode 标准合约代码
 * @param last_px 上一次价格
 * @param cur_px 当前最新价格
 * @details 根据Tick数据处理信号和持仓信息，主要功能包括：
 *          1. 检查信号列表中是否有当前合约的信号
 *          2. 如果有信号，根据设置的期望价格或当前市场价格执行仓位操作
 *          3. 执行完成后从信号列表中移除该信号
 *          4. 更新合约的动态盈亏
 *          这个函数是实际处理信号触发和持仓盈亏计算的核心逻辑
 */
void SelMocker::proc_tick(const char* stdCode, double last_px, double cur_px)
{
	{
		auto it = _sig_map.find(stdCode);
		if (it != _sig_map.end())
		{
			//if (sInfo->isInTradingTime(_replayer->get_raw_time(), true))
			{
				const SigInfo& sInfo = it->second;
				double price;
				if (decimal::eq(sInfo._desprice, 0.0))
					price = cur_px;
				else
					price = sInfo._desprice;
				do_set_position(stdCode, sInfo._volume, price, sInfo._usertag.c_str());
				_sig_map.erase(it);
			}
		}
	}

	update_dyn_profit(stdCode, cur_px);
}


//////////////////////////////////////////////////////////////////////////
//回调函数
/**
 * @brief 处理K线数据回调
 * @param stdCode 标准合约代码
 * @param period 周期标识（如'd'表示日线，'m'表示分钟线）
 * @param times 周期倍数
 * @param newBar 新的K线数据
 * @details 处理新到达K线数据的核心函数，主要功能包括：
 *          1. 检验新K线数据是否有效
 *          2. 根据周期类型构造真实周期标识（例如'd1'表示日线，'m5'表示5分钟线）
 *          3. 生成唯一的K线标识键，并更新K线标签信息
 *          4. 调用on_bar_close函数处理已完成K线
 *          这个函数是连接K线数据库和策略回调的关键桥梁
 */
void SelMocker::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (newBar == NULL)
		return;

	std::string realPeriod;
	if (period[0] == 'd')
		realPeriod = StrUtil::printf("%s%u", period, times);
	else
		realPeriod = StrUtil::printf("m%u", times);

	std::string key = StrUtil::printf("%s#%s", stdCode, realPeriod.c_str());
	KlineTag& tag = _kline_tags[key];
	tag._closed = true;
	tag._count++;

	on_bar_close(stdCode, realPeriod.c_str(), newBar);
}

/**
 * @brief 策略初始化回调
 * @details 当策略模拟器初始化时调用此函数，主要功能包括：
 *          1. 检查策略实例是否存在
 *          2. 如果策略实例存在，则调用策略的on_init方法进行初始化
 *          3. 记录滑点设置日志信息
 *          这个函数是选股策略模拟器启动时的第一个回调函数
 */
void SelMocker::on_init()
{
	if (_strategy)
		_strategy->on_init(this);

	WTSLogger::info("SEL Strategy initialized with {} slippage: {}", _ratio_slippage ? "ratio" : "absolute", _slippage);
}

/**
 * @brief 更新持仓的动态盈亏
 * @param stdCode 标准合约代码
 * @param price 当前价格
 * @details 根据最新市场价格计算持仓的动态盈亏，主要流程如下：
 *          1. 查找标的合约目前的持仓情况
 *          2. 如果持仓量为零，则直接将动态盈亏设为零
 *          3. 如果有持仓，则遍历该合约的所有持仓明细，计算每笔交易的当前盈亏：
 *             - 计算当前盈亏 = 持仓量 * (当前价格 - 开仓价格) * 合约乘数 * 方向系数
 *             - 更新最大盈利和最大亏损记录
 *             - 记录持仓期间遇到的最高价和最低价
 *          4. 汇总合约的全部明细盈亏，得到该合约的总动态盈亏
 *          5. 计算所有合约的总动态盈亏
 *          6. 更新资金账户的总动态盈亏
 *          这个函数是盈亏计算的核心部分，保证了回测系统的盈亏计算准确性
 */
void SelMocker::update_dyn_profit(const char* stdCode, double price)
{
	auto it = _pos_map.find(stdCode);
	if (it != _pos_map.end())
	{
		PosInfo& pInfo = (PosInfo&)it->second;
		if (pInfo._volume == 0)
		{
			pInfo._dynprofit = 0;
		}
		else
		{
			WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
			double dynprofit = 0;
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				if (dInfo._profit > 0)
					dInfo._max_profit = max(dInfo._profit, dInfo._max_profit);
				else if (dInfo._profit < 0)
					dInfo._max_loss = min(dInfo._profit, dInfo._max_loss);

				dInfo._max_price = std::max(dInfo._max_price, price);
				dInfo._min_price = std::min(dInfo._min_price, price);

				dynprofit += dInfo._profit;
			}

			pInfo._dynprofit = dynprofit;
		}
	}

	double total_dynprofit = 0;
	for (auto& v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		total_dynprofit += pInfo._dynprofit;
	}

	_fund_info._total_dynprofit = total_dynprofit;
}

/**
 * @brief Tick数据回调函数
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据
 * @param bEmitStrategy 是否触发策略回调，默认为true
 * @details 当新的Tick数据到达时调用此函数
 *          注意：自2022.04.19版本起，此函数的核心逻辑已迁移到handle_tick函数中
 *          这个函数当前仅保留为历史兼容目的，不再包含实际处理逻辑
 */
void SelMocker::on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy /* = true */)
{
	//By Wesley @ 2022.04.19
	//这个逻辑迁移到handle_tick去了
}

/**
 * @brief K线关闭回调函数
 * @param code 标准合约代码
 * @param period 周期标识
 * @param newBar 新的K线数据
 * @details 当新的K线完成关闭时调用此函数，将K线数据传递给策略实例
 *          检查策略实例是否存在，如果存在则调用策略的on_bar方法
 *          这个函数是连接模拟器和用户策略的核心中介函数
 */
void SelMocker::on_bar_close(const char* code, const char* period, WTSBarStruct* newBar)
{
	if (_strategy)
		_strategy->on_bar(this, code, period, newBar);
}

/**
 * @brief Tick数据更新通知函数
 * @param code 标准合约代码
 * @param newTick 新的Tick数据
 * @details 当有新的Tick数据到达时，检查该合约是否已订阅
 *          如果合约未订阅，则直接返回不处理
 *          如果合约已订阅且策略实例存在，则调用策略的on_tick方法
 *          这确保只有被策略主动订阅的合约才会触发策略回调
 */
void SelMocker::on_tick_updated(const char* code, WTSTickData* newTick)
{
	auto it = _tick_subs.find(code);
	if (it == _tick_subs.end())
		return;

	if (_strategy)
		_strategy->on_tick(this, code, newTick);
}

/**
 * @brief 策略调度函数
 * @param curDate 当前日期（格式YYYYMMDD）
 * @param curTime 当前时间（格式HHMMSS）
 * @details 在定时调度事件中调用，检查策略实例是否存在
 *          如果策略实例存在，则调用策略的on_schedule方法
 *          这是实际触发用户策略定时调度逻辑的函数
 */
void SelMocker::on_strategy_schedule(uint32_t curDate, uint32_t curTime)
{
	if (_strategy)
		_strategy->on_schedule(this, curDate, curTime);
}


/**
 * @brief 执行定时调度
 * @param curDate 当前日期（格式YYYYMMDD）
 * @param curTime 当前时间（格式HHMMSS）
 * @param fireTime 触发时间（格式HHMMSS）
 * @return 调度是否成功
 * @details 策略定时调度的核心函数，主要流程如下：
 *          1. 设置调度状态标记
 *          2. 累计调度次数
 *          3. 开始计时并调用策略调度函数
 *          4. 检查当前所有持仓，收集需要清空的持仓
 *          5. 自动清空无信号的持仓
 *          6. 统计调度性能数据
 *          7. 重置调度状态标记
 *          这是策略模拟器中最重要的定时执行函数
 */
bool SelMocker::on_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime)
{
	_is_in_schedule = true;//开始调度,修改标记

	_schedule_times++;

	TimeUtils::Ticker ticker;
	on_strategy_schedule(curDate, curTime);

	wt_hashset<std::string> to_clear;
	for(auto& v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		const char* code = v.first.c_str();
		if(_sig_map.find(code) == _sig_map.end() && !decimal::eq(pInfo._volume, 0.0))
		{
			//新的信号中没有该持仓,则要清空
			to_clear.insert(code);
		}
	}

	for(const std::string& code : to_clear)
	{
		append_signal(code.c_str(), 0, "autoexit");
	}

	_emit_times++;
	_total_calc_time += ticker.micro_seconds();

	_is_in_schedule = false;//调度结束,修改标记
	return true;
}

/**
 * @brief 交易日开始事件处理
 * @param curTDate 当前交易日（格式YYYYMMDD）
 * @details 当新的交易日开始时执行的函数，主要工作包括：
 *          1. 更新当前交易日变量
 *          2. 重置所有合约的冻结持仓数量为零
 *          这个函数的主要目的是在每个新交易日开始时重置仓位的冻结状态
 *          确保前一交易日未完成的交易不会影响新交易日的操作
 */
void SelMocker::on_session_begin(uint32_t curTDate)
{
	_cur_tdate = curTDate;
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (!decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
			pInfo._frozen = 0;
		}
	}
}

/**
 * @brief 枚举策略持仓
 * @param cb 持仓回调函数
 * @details 该函数枚举当前策略所有的持仓信息，并通过回调函数返回
 *          收集逻辑如下：
 *          1. 首先收集当前实际持仓
 *          2. 再收集信号列表中的目标持仓
 *          3. 通过回调函数将每个合约的最终持仓量返回给调用者
 *          这个函数通常用于获取策略的当前持仓状态，包括已执行和待执行的目标仓位
 */
void SelMocker::enum_position(FuncEnumSelPositionCallBack cb)
{
	wt_hashmap<std::string, double> desPos;
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		const PosInfo& pInfo = it.second;
		desPos[stdCode] = pInfo._volume;
	}

	for (auto sit : _sig_map)
	{
		const char* stdCode = sit.first.c_str();
		const SigInfo& sInfo = sit.second;
		desPos[stdCode] = sInfo._volume;
	}

	for (auto v : desPos)
	{
		cb(v.first.c_str(), v.second);
	}
}

/**
 * @brief 交易日结束事件处理
 * @param curTDate 当前交易日（格式YYYYMMDD）
 * @details 当交易日结束时执行的函数，主要工作包括：
 *          1. 调用策略的on_session_end回调函数
 *          2. 遍历所有持仓，执行以下操作：
 *             - 初始化持仓快照，记录当天收盘持仓状态
 *             - 将当日平仓盈亏汇总到总盈亏
 *             - 重置当日平仓盈亏为零，准备下一交易日的计算
 *          这个函数的主要目的是完成当日盈亏的结算和持仓汇总
 */

void SelMocker::on_session_end(uint32_t curTDate)
{
	uint32_t curDate = curTDate;//_replayer->get_trading_date();

	double total_profit = 0;
	double total_dynprofit = 0;

	for (auto it = _pos_map.begin(); it != _pos_map.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		const PosInfo& pInfo = it->second;
		total_profit += pInfo._closeprofit;
		total_dynprofit += pInfo._dynprofit;

		if (decimal::eq(pInfo._volume, 0.0))
			continue;

		_pos_logs << fmt::format("{},{},{},{:.2f},{:.2f}\n", curDate, stdCode,
			pInfo._volume, pInfo._closeprofit, pInfo._dynprofit);
	}

	_fund_logs << fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curDate,
		_fund_info._total_profit, _fund_info._total_dynprofit,
		_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees);

	//save_data();
}

//////////////////////////////////////////////////////////////////////////
//策略接口
/**
 * @brief 获取合约当前价格
 * @param stdCode 标准合约代码
 * @return 当前合约的最新价格，如果存在则返回当前价格，否则返回0
 * @details 策略接口函数，用于获取指定合约的当前市场价格
 *          通过回测引擎返回当前最新的市场价格
 *          这是策略开发者可以使用的市场数据查询函数
 */
double SelMocker::stra_get_price(const char* stdCode)
{
	if (_replayer)
		return _replayer->get_cur_price(stdCode);

	return 0.0;
}

/**
 * @brief 设置合约目标持仓
 * @param stdCode 标准合约代码
 * @param qty 目标持仓量，正数表示多头持仓，负数表示空头持仓
 * @param userTag 用户标签，如信号标识或交易备注，可用于后续调试和分析
 * @details 策略接口函数，用于设置合约的目标持仓量，执行流程如下：
 *          1. 首先获取合约信息，包括疯狂模式、T+1规则等
 *          2. 检查目标仓位是否合法（例如是否能做空）
 *          3. 比较目标仓位与当前仓位，如果相等则直接返回
 *          4. 对于T+1合约，检查目标仓位是否满足冻结仓位的限制
 *          5. 订阅合约的Tick数据，并添加仓位变更信号
 *          这是策略开发者用于设置目标仓位的主要接口
 */
void SelMocker::stra_set_position(const char* stdCode, double qty, const char* userTag /* = "" */)
{
	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	//如果不能做空，则目标仓位不能设置负数
	if (!commInfo->canShort() && decimal::lt(qty, 0))
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	double total = stra_get_position(stdCode, false);
	//如果目标仓位和当前仓位是一致的，直接退出
	if (decimal::eq(total, qty))
		return;

	if (commInfo->isT1())
	{
		double valid = stra_get_position(stdCode, true);
		double frozen = total - valid;
		//如果是T+1规则，则目标仓位不能小于冻结仓位
		if (decimal::lt(qty, frozen))
		{
			WTSLogger::log_dyn("strategy", _name.c_str(), LL_ERROR, 
				"New position of {} cannot be set to {} due to {} being frozen", stdCode, qty, frozen);
			return;
		}
	}

	_replayer->sub_tick(id(), stdCode);
	append_signal(stdCode, qty, userTag);
}

/**
 * @brief 添加仓位变更信号
 * @param stdCode 标准合约代码
 * @param qty 目标持仓量
 * @param userTag 用户标签
 * @param price 期望价格（可选），如果为0则使用当前市场价格
 * @details 将仓位变更信号添加到信号列表中，执行流程如下：
 *          1. 获取当前市场价格
 *          2. 为给定合约创建或更新信号信息，包括：
 *             - 目标仓位量
 *             - 信号价格（当前市场价格）
 *             - 期望成交价格（如果指定）
 *             - 用户标签
 *             - 信号生成时间
 *             - 触发标志（在调度中生成的信号不立即触发）
 *          3. 记录信号日志
 *          信号会在下一个Tick处理中触发成交
 */
void SelMocker::append_signal(const char* stdCode, double qty, const char* userTag /* = "" */, double price/* = 0.0*/)
{
	double curPx = _price_map[stdCode].first;

	SigInfo& sInfo = _sig_map[stdCode];
	sInfo._volume = qty;
	sInfo._sigprice = curPx;
	sInfo._desprice = price;
	sInfo._usertag = userTag;
	sInfo._gentime = (uint64_t)_replayer->get_date() * 1000000000 + (uint64_t)_replayer->get_raw_time() * 100000 + _replayer->get_secs();
	sInfo._triggered = !_is_in_schedule;

	log_signal(stdCode, qty, curPx, sInfo._gentime, userTag);

	//save_data();
}

/**
 * @brief 实际执行仓位变更
 * @param stdCode 标准合约代码
 * @param qty 目标持仓量
 * @param price 成交价格，如果为0则使用当前市场价格
 * @param userTag 用户标签
 * @param bTriggered 是否被触发的信号
 * @details 实际执行仓位变更操作，包括开仓、平仓和仓位调整，执行流程如下：
 *          1. 获取或创建合约的持仓信息
 *          2. 确定成交价格（使用参数价格或当前市场价格）
 *          3. 获取当前交易时间和交易日
 *          4. 如果目标仓位与当前仓位相同，则不执行操作
 *          这是实际执行仓位变更的核心函数，后续处理包括仓位调整、盈亏计算、交易记录等
 */
void SelMocker::do_set_position(const char* stdCode, double qty, double price /* = 0.0 */, const char* userTag /* = "" */, bool bTriggered /* = false */)
{
	PosInfo& pInfo = _pos_map[stdCode];
	double curPx = price;
	if (decimal::eq(price, 0.0))
		curPx = _price_map[stdCode].first;
	uint64_t curTm = (uint64_t)_replayer->get_date() * 10000 + _replayer->get_min_time();
	uint32_t curTDate = _replayer->get_trading_date();

	if (decimal::eq(pInfo._volume, qty))
		return;

	WTSCommodityInfo* commInfo = _replayer->get_commodity_info(stdCode);
	if (commInfo == NULL)
		return;

	//成交价
	double trdPx = curPx;
	double diff = qty - pInfo._volume;
	bool isBuy = decimal::gt(diff, 0.0);
	if (decimal::gt(pInfo._volume*diff, 0))//当前持仓和仓位变动方向一致,增加一条明细,增加数量即可
	{
		pInfo._volume = qty;

		//如果T+1，则冻结仓位要增加
		if (commInfo->isT1())
		{
			//ASSERT(diff>0);
			pInfo._frozen += diff;
			log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
		}

		if (_slippage != 0)
		{
			if (_ratio_slippage)
			{
				//By Wesley @ 2023.05.05
				//如果是比率滑点，则要根据目标成交价计算
				//得到滑点以后，再根据pricetick做一个修正
				double slp = (_slippage * trdPx / 10000.0);
				slp = round(slp / commInfo->getPriceTick())*commInfo->getPriceTick();

				trdPx += slp * (isBuy ? 1 : -1);
			}
			else
				trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);
		}

		DetailInfo dInfo;
		dInfo._long = decimal::gt(qty, 0);
		dInfo._price = trdPx;
		dInfo._max_price = trdPx;
		dInfo._min_price = trdPx;
		dInfo._volume = abs(diff);
		dInfo._opentime = curTm;
		dInfo._opentdate = curTDate;
		strcpy(dInfo._opentag, userTag);
		dInfo._open_barno = _schedule_times;
		pInfo._details.emplace_back(dInfo);
		pInfo._last_entertime = curTm;

		double fee = _replayer->calc_fee(stdCode, trdPx, abs(diff), 0);
		_fund_info._total_fees += fee;

		log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(diff), userTag, fee);
	}
	else
	{//持仓方向和目标仓位方向不一致,需要平仓
		double left = abs(diff);
		bool isBuy = decimal::gt(diff, 0.0);

		if (_slippage != 0)
		{
			if (_ratio_slippage)
			{
				//By Wesley @ 2023.05.05
				//如果是比率滑点，则要根据目标成交价计算
				//得到滑点以后，再根据pricetick做一个修正
				double slp = (_slippage * trdPx / 10000.0);
				slp = round(slp / commInfo->getPriceTick())*commInfo->getPriceTick();

				trdPx += slp * (isBuy ? 1 : -1);
			}
			else
				trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);
		}

		pInfo._volume = qty;
		if (decimal::eq(pInfo._volume, 0))
			pInfo._dynprofit = 0;
		uint32_t count = 0;
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			double maxQty = min(dInfo._volume, left);
			if (decimal::eq(maxQty, 0))
				continue;

			double maxProf = dInfo._max_profit * maxQty / dInfo._volume;
			double maxLoss = dInfo._max_loss * maxQty / dInfo._volume;

			dInfo._volume -= maxQty;
			left -= maxQty;

			if (decimal::eq(dInfo._volume, 0))
				count++;

			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;

			pInfo._closeprofit += profit;
			pInfo._dynprofit = pInfo._dynprofit*dInfo._volume / (dInfo._volume + maxQty);//浮盈也要做等比缩放
			pInfo._last_exittime = curTm;
			_fund_info._total_profit += profit;

			double fee = _replayer->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, userTag, fee);
			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, 
				trdPx, maxQty, profit, dInfo._max_profit, dInfo._max_loss, pInfo._closeprofit, dInfo._opentag, userTag, dInfo._open_barno, _schedule_times);

			if (left == 0)
				break;
		}

		//需要清理掉已经平仓完的明细
		while (count > 0)
		{
			auto it = pInfo._details.begin();
			pInfo._details.erase(it);
			count--;
		}

		//最后,如果还有剩余的,则需要反手了
		if (left > 0)
		{
			left = left * qty / abs(qty);

			//如果T+1，则冻结仓位要增加
			if (commInfo->isT1())
			{
				pInfo._frozen += left;
				log_debug("{} frozen position up to {}", stdCode, pInfo._frozen);
			}

			DetailInfo dInfo;
			dInfo._long = decimal::gt(qty, 0);
			dInfo._price = trdPx;
			dInfo._max_price = trdPx;
			dInfo._min_price = trdPx;
			dInfo._volume = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			strcpy(dInfo._opentag, userTag);
			dInfo._open_barno = _schedule_times;
			pInfo._details.emplace_back(dInfo);
			pInfo._last_entertime = curTm;

			//这里还需要写一笔成交记录
			double fee = _replayer->calc_fee(stdCode, trdPx, abs(left), 0);
			_fund_info._total_fees += fee;
			log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), userTag, fee);
		}
	}
}

/**
 * @brief 获取K线切片数据
 * @param stdCode 标准合约代码
 * @param period 周期标识，如'd'表示日线，'m5'表示5分钟线
 * @param count 请求的K线条数
 * @return 返回K线切片指针，如果数据不存在则返回NULL
 * @details 获取指定合约的历史K线数据，主要流程如下：
 *          1. 生成K线标识键
 *          2. 提取基础周期和倍数
 *          3. 从回测器获取K线切片数据
 *          4. 更新K线标签状态
 *          5. 更新合约最新价格和时间
 *          这个函数是策略获取历史K线数据的主要接口
 */
WTSKlineSlice* SelMocker::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);
	else
		strcat(key, "1");

	WTSKlineSlice* kline = _replayer->get_kline_slice(stdCode, basePeriod, count, times, false);

	KlineTag& tag = _kline_tags[key];
	tag._closed = false;

	if (kline)
	{
		double lastClose = kline->at(-1)->close;
		uint64_t lastTime = 0;
		if(basePeriod[0] == 'd')
		{
			lastTime = kline->at(-1)->date;
			WTSSessionInfo* sInfo = _replayer->get_session_info(stdCode, true);
			lastTime *= 1000000000;
			lastTime += (uint64_t)sInfo->getCloseTime() * 100000;
		}
		else
		{
			lastTime = kline->at(-1)->time;
			lastTime += 199000000000;
			lastTime *= 100000;
		}

		if(lastTime > _price_map[stdCode].second)
		{
			_price_map[stdCode].second = lastTime;
			_price_map[stdCode].first = lastClose;
		}
	}

	return kline;
}

/**
 * @brief 获取Tick切片数据
 * @param stdCode 标准合约代码
 * @param count 请求的Tick数据条数
 * @return 返回Tick切片指针，如果数据不存在则返回NULL
 * @details 获取指定合约的历史Tick数据切片
 *          这个函数直接从回测器中获取Tick切片数据
 *          策略可以使用这个接口分析高频数据和循序信息
 */
WTSTickSlice* SelMocker::stra_get_ticks(const char* stdCode, uint32_t count)
{
	return _replayer->get_tick_slice(stdCode, count);
}

/**
 * @brief 获取最新Tick数据
 * @param stdCode 标准合约代码
 * @return 返回最新的Tick数据指针，如果数据不存在则返回NULL
 * @details 获取指定合约的最新Tick数据
 *          策略可以通过这个接口获取合约的实时行情数据
 *          包括最新价、买卖相关价格、成交量、深度数据等
 */
WTSTickData* SelMocker::stra_get_last_tick(const char* stdCode)
{
	return _replayer->get_last_tick(stdCode);
}

void SelMocker::stra_sub_ticks(const char* code)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	_tick_subs.insert(code);

	_replayer->sub_tick(_context_id, code);
}

/**
 * @brief 获取合约的商品信息
 * @details 返回指定合约的商品信息，包含合约乘数、价格单位等数据
 * @param stdCode 标准合约代码
 * @return 返回商品信息指针
 */
WTSCommodityInfo* SelMocker::stra_get_comminfo(const char* stdCode)
{
	return _replayer->get_commodity_info(stdCode);
}

/**
 * @brief 获取合约的原始代码
 * @details 返回标准合约代码对应的原始代码，即交易所原始编码
 * @param stdCode 标准合约代码
 * @return 返回原始代码字符串
 */
std::string SelMocker::stra_get_rawcode(const char* stdCode)
{
	return _replayer->get_rawcode(stdCode);
}

/**
 * @brief 获取合约的交易时段信息
 * @details 返回指定合约的交易时段信息，包含开盘时间、收盘时间等
 * @param stdCode 标准合约代码
 * @return 返回交易时段信息指针
 */
WTSSessionInfo* SelMocker::stra_get_sessinfo(const char* stdCode)
{
	return _replayer->get_session_info(stdCode, true);
}

/**
 * @brief 获取当前交易日期
 * @details 返回当前回测的交易日期，格式为YYYYMMDD
 * @return 交易日期整数
 */
/**
 * @brief 获取当前交易日
 * @return 返回当前交易日，格式YYYYMMDD
 * @details 该函数返回回测器中的当前交易日期
 *          交易日是结算日，用于持仓和资金的结算
 *          策略可以通过这个接口获取当前的交易日期
 */
uint32_t SelMocker::stra_get_tdate()
{
	return _replayer->get_trading_date();
}

/**
 * @brief 获取当前自然日期
 * @details 返回当前回测的自然日期，格式为YYYYMMDD
 * @return 自然日期整数
 */
/**
 * @brief 获取当前日历日期
 * @return 返回当前日历日期，格式YYYYMMDD
 * @details 该函数返回回测器中的当前日历日期
 *          日历日期可能与交易日不同，如周末的交易日为周五，但日历日可能是周六或周日
 *          策略开发者可以通过这个接口获取当前模拟的日历日期
 */
uint32_t SelMocker::stra_get_date()
{
	return _replayer->get_date();
}

/**
 * @brief 获取当前时间
 * @details 返回当前回测的分钟时间，格式为HHMM
 * @return 时间整数
 */
uint32_t SelMocker::stra_get_time()
{
	return _replayer->get_min_time();
}

/**
 * @brief 获取资金数据
 * @details 根据标志获取不同类型的资金数据
 * @param flag 数据标志，0-总收益（平仓盈亏+浮动盈亏-手续费），1-平仓盈亏，2-浮动盈亏，3-手续费
 * @return 对应类型的资金数据
 */
/**
 * @brief 获取资金数据
 * @param flag 资金数据标志，0-总盈亏，1-平仓盈亏，2-浮动盈亏，3-总手续费
 * @return 返回对应的资金数据
 * @details 该函数返回策略的各类资金数据，根据传入的flag参数返回不同类型的资金信息：
 *          0 - 总盈亏（平仓盈亏 + 浮动盈亏 - 手续费）
 *          1 - 平仓盈亏（已实现的盈亏）
 *          2 - 浮动盈亏（未实现的持仓盈亏）
 *          3 - 总手续费
 *          策略可以通过这个接口监控策略的盈亏情况
 */
double SelMocker::stra_get_fund_data(int flag)
{
	switch (flag)
	{
	case 0:
		return _fund_info._total_profit - _fund_info._total_fees + _fund_info._total_dynprofit;
	case 1:
		return _fund_info._total_profit;
	case 2:
		return _fund_info._total_dynprofit;
	case 3:
		return _fund_info._total_fees;
	default:
		return 0.0;
	}
}


/**
 * @brief 记录信息级别日志
 * @details 记录一条信息级别的日志消息
 * @param message 日志消息内容
 */
void SelMocker::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

/**
 * @brief 记录调试级别日志
 * @details 记录一条调试级别的日志消息
 * @param message 日志消息内容
 */
void SelMocker::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

/**
 * @brief 记录警告级别日志
 * @details 记录一条警告级别的日志消息
 * @param message 日志消息内容
 */
void SelMocker::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

/**
 * @brief 记录错误级别日志
 * @details 记录一条错误级别的日志消息
 * @param message 日志消息内容
 */
void SelMocker::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

/**
 * @brief 加载用户数据
 * @details 根据键名从用户数据映射中加载数据
 * @param key 数据键名
 * @param defVal 默认值，当指定的键不存在时返回此值，默认为空字符串
 * @return 返回键对应的值或默认值
 */
const char* SelMocker::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	return defVal;
}

/**
 * @brief 保存用户数据
 * @details 将数据以键值对的形式保存到用户数据映射中
 * @param key 数据键名
 * @param val 数据值
 */
void SelMocker::stra_save_user_data(const char* key, const char* val)
{
	_user_datas[key] = val;
	_ud_modified = true;
}

/**
 * @brief 获取合约仓位
 * @details 获取指定合约的当前持仓量
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只获取有效仓位（排除冻结部分），默认为false
 * @param userTag 用户标签，用于检索特定标记的仓位，默认为空字符串
 * @return 返回仓位量，正数表示多头，负数表示空头，0表示无仓位
 */
double SelMocker::stra_get_position(const char* stdCode, bool bOnlyValid/* = false*/, const char* userTag /* = "" */)
{
	//By Wesley @ 2023.04.17
	//如果有信号，说明刚下了指令，还没等到下一个tick进来，用户就在读取仓位
	//但是如果用户读取，还是要返回
	auto sit = _sig_map.find(stdCode);
	if (sit != _sig_map.end())
	{
		return sit->second._volume;
	}

	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (strlen(userTag) == 0)
	{
		//只有userTag为空的时候时候，才会用bOnlyValid
		if (bOnlyValid)
		{
			//这里理论上，只有多头才会进到这里
			//其他地方要保证，空头持仓的话，_frozen要为0
			return pInfo._volume - pInfo._frozen;
		}
		else
			return pInfo._volume;
	}

	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		return dInfo._volume;
	}

	return 0;
}

/**
 * @brief 获取合约的日线价格
 * @details 获取指定合约的日线价格，包括开盘价、收盘价、最高价、最低价等
 * @param stdCode 标准合约代码
 * @param flag 价格标志：0-收盘价（默认），1-开盘价，2-最高价，3-最低价
 * @return 返回对应的日线价格，如果不存在则返回0
 */
double SelMocker::stra_get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if (_replayer)
		return _replayer->get_day_price(stdCode, flag);

	return 0.0;
}

/**
 * @brief 获取首次入场时间
 * @details 获取指定合约当前持仓的首次入场时间
 * @param stdCode 标准合约代码
 * @return 返回首次入场时间，格式为YYYYMMDDHHMMSSsss，如果没有持仓则返回0
 */
uint64_t SelMocker::stra_get_first_entertime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[0]._opentime;
}

/**
 * @brief 获取最近入场时间
 * @details 获取指定合约当前持仓的最近入场时间
 * @param stdCode 标准合约代码
 * @return 返回最近入场时间，格式为YYYYMMDDHHMMSSsss，如果没有持仓则返回0
 */
uint64_t SelMocker::stra_get_last_entertime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[pInfo._details.size() - 1]._opentime;
}

/**
 * @brief 获取最近入场标签
 * @details 获取指定合约当前持仓的最近入场标签
 * @param stdCode 标准合约代码
 * @return 返回最近入场标签，如果没有持仓则返回空字符串
 */
const char* SelMocker::stra_get_last_entertag(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return "";

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return "";

	return pInfo._details[pInfo._details.size() - 1]._opentag;
}

/**
 * @brief 获取最近出场时间
 * @details 获取指定合约的最近出场时间
 * @param stdCode 标准合约代码
 * @return 返回最近出场时间，格式为YYYYMMDDHHMMSSsss，如果没有出场记录则返回0
 */
uint64_t SelMocker::stra_get_last_exittime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._last_exittime;
}

/**
 * @brief 获取最近入场价格
 * @details 获取指定合约当前持仓的最近入场价格
 * @param stdCode 标准合约代码
 * @return 返回最近入场价格，如果没有持仓则返回0
 */
double SelMocker::stra_get_last_enterprice(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return 0;

	return pInfo._details[pInfo._details.size() - 1]._price;
}

/**
 * @brief 获取仓位平均价格
 * @details 获取指定合约当前仓位的加权平均价格
 * @param stdCode 标准合约代码
 * @return 返回平均价格，如果无仓位则返回0
 */
double SelMocker::stra_get_position_avgpx(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	if (pInfo._volume == 0)
		return 0.0;

	double amount = 0.0;
	for (auto dit = pInfo._details.begin(); dit != pInfo._details.end(); dit++)
	{
		const DetailInfo& dInfo = *dit;
		amount += dInfo._price*dInfo._volume;
	}

	return amount / pInfo._volume;
}

/**
 * @brief 获取仓位浮动盈亏
 * @details 获取指定合约当前仓位的浮动盈亏
 * @param stdCode 标准合约代码
 * @return 返回浮动盈亏金额
 */
double SelMocker::stra_get_position_profit(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

/**
 * @brief 获取仓位明细开仓时间
 * @details 获取指定合约指定用户标签的仓位明细开仓时间
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @return 返回开仓时间，如果无仓位则返回0
 */
uint64_t SelMocker::stra_get_detail_entertime(const char* stdCode, const char* userTag)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		return dInfo._opentime;
	}

	return 0;
}

/**
 * @brief 获取仓位明细的开仓成本
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @return 返回开仓价格，如果没有找到对应的仓位明细则返回0
 * @details 获取指定合约和用户标签的仓位明细的开仓价格
 *          为策略提供细粒度成本管理，可以组合userTag实现多元信号管理
 */
double SelMocker::stra_get_detail_cost(const char* stdCode, const char* userTag)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		return dInfo._price;
	}

	return 0.0;
}

/**
 * @brief 获取仓位明细的利润信息
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @param flag 标志：0-当前盈亏，1-最大盈利，-1-最大亏损，2-最高价格，-2-最低价格，默认为0
 * @return 返回对应的利润信息，如果没有找到对应的仓位明细则返回0
 * @details 根据标志获取指定合约和用户标签的仓位明细的不同利润信息
 *          flag为0返回当前盈亏，flag为1返回最大盈利，flag为-1返回最大亏损
 *          flag为2返回持仓期间最高价格，flag为-2返回持仓期间最低价格
 */
double SelMocker::stra_get_detail_profit(const char* stdCode, const char* userTag, int flag /* = 0 */)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
	{
		const DetailInfo& dInfo = (*it);
		if (strcmp(dInfo._opentag, userTag) != 0)
			continue;

		switch (flag)
		{
		case 0:
			return dInfo._profit;
		case 1:
			return dInfo._max_profit;
		case -1:
			return dInfo._max_loss;
		case 2:
			return dInfo._max_price;
		case -2:
			return dInfo._min_price;
		}
	}

	return 0.0;
}