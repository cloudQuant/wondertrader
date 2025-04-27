/*!
* \file MfStraBaseCtx.cpp
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief 选股策略基础上下文实现文件
* \details 该文件实现了选股策略的基础上下文类，提供了策略运行所需的各种核心功能，
*          包括持仓管理、信号生成、资金计算、日志记录等。选股策略是WonderTrader中
*          用于股票市场的策略类型，专注于股票池的管理和调整。
*/
#include "SelStraBaseCtx.h"
#include "WtSelEngine.h"
#include "WtHelper.h"

#include <exception>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IHotMgr.h"
#include "../Share/decimal.h"
#include "../Share/CodeHelper.hpp"

#include "../WTSTools/WTSLogger.h"

namespace rj = rapidjson;

/**
* @brief 生成选股策略上下文ID
* @return 返回唯一的上下文ID
* @details 通过原子操作生成唯一的上下文ID，起始值为3000
*          每次调用此函数都会返回一个新的ID值（当前值），并将内部计数器加1
*          这确保了每个选股策略实例都有唯一的标识符
*/
inline uint32_t makeSelCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 3000 };
	return _auto_context_id.fetch_add(1);
}


/**
* @brief 选股策略基础上下文类构造函数
* @param engine 选股引擎指针
* @param name 策略名称
* @param slippage 滑点设置
* @details 初始化选股策略上下文，设置策略名称、引擎指针和滑点，
*          并初始化所有内部状态变量，通过makeSelCtxId生成唯一的上下文ID
*/
SelStraBaseCtx::SelStraBaseCtx(WtSelEngine* engine, const char* name, int32_t slippage)
	: ISelStraCtx(name)
	, _engine(engine)
	, _total_calc_time(0)
	, _emit_times(0)
	, _is_in_schedule(false)
	, _ud_modified(false)
	, _schedule_date(0)
	, _schedule_time(0)
	, _slippage(slippage)
{
	_context_id = makeSelCtxId();
}


/**
* @brief 选股策略基础上下文类析构函数
* @details 负责清理选股策略上下文实例，释放相关资源
*/
SelStraBaseCtx::~SelStraBaseCtx()
{
}

/**
* @brief 初始化策略输出文件
* @details 创建并初始化策略所需的所有日志输出文件，包括交易日志、平仓日志、资金日志、信号日志和持仓日志
*          所有输出文件都采用CSV格式，放置在以策略名称命名的专用文件夹中
*          如果是初次创建文件，会写入CSV标题行；如果文件已存在，则将文件指针移动到末尾追加内容
*/
void SelStraBaseCtx::init_outputs()
{
	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "//";
	BoostFile::create_directories(folder.c_str());

	std::string filename = folder + "trades.csv";
	_trade_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_trade_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_trade_logs->write_file("code,time,direct,action,price,qty,tag,fee\n");
		}
		else
		{
			_trade_logs->seek_to_end();
		}
	}

	filename = folder + "closes.csv";
	_close_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_close_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_close_logs->write_file("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit,entertag,exittag\n");
		}
		else
		{
			_close_logs->seek_to_end();
		}
	}

	filename = folder + "funds.csv";
	_fund_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_fund_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_fund_logs->write_file("date,closeprofit,positionprofit,dynbalance,fee\n");
		}
		else
		{
			_fund_logs->seek_to_end();
		}
	}

	filename = folder + "signals.csv";
	_sig_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_sig_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_sig_logs->write_file("code,target,sigprice,gentime,usertag\n");
		}
		else
		{
			_sig_logs->seek_to_end();
		}
	}

	filename = folder + "positions.csv";
	_pos_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_pos_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_pos_logs->write_file("date,code,volume,closeprofit,dynprofit\n");
		}
		else
		{
			_pos_logs->seek_to_end();
		}
	}
}

/**
 * @brief 记录信号日志
 * @param stdCode 标准合约代码
 * @param target 目标仓位
 * @param price 信号价格
 * @param gentime 信号生成时间
 * @param usertag 用户标签，默认为空字符串
 * @details 将信号信息写入信号日志文件，格式为CSV
 *          包含字段：合约代码、目标仓位、信号价格、生成时间和用户标签
 */
void SelStraBaseCtx::log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag /* = "" */)
{
	if (_sig_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << target << "," << price << "," << gentime << "," << usertag << "\n";
		_sig_logs->write_file(ss.str());
	}
}

/**
 * @brief 记录交易日志
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头
 * @param isOpen 是否为开仓
 * @param curTime 当前时间
 * @param price 成交价格
 * @param qty 成交数量
 * @param userTag 用户标签
 * @param fee 手续费
 * @details 将交易信息写入交易日志文件，格式为CSV
 *          包含字段：合约代码、时间、方向(LONG/SHORT)、动作(OPEN/CLOSE)、价格、数量、用户标签和手续费
 */
void SelStraBaseCtx::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag, double fee)
{
	if (_trade_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE") << "," << price << "," << qty << "," << userTag << "," << fee << "\n";
		_trade_logs->write_file(ss.str());
	}
}

/**
 * @brief 记录平仓日志
 * @param stdCode 标准合约代码
 * @param isLong 是否为多头
 * @param openTime 开仓时间
 * @param openpx 开仓价格
 * @param closeTime 平仓时间
 * @param closepx 平仓价格
 * @param qty 平仓数量
 * @param profit 平仓盈亏
 * @param totalprofit 累计盈亏，默认为0
 * @param enterTag 开仓标签，默认为空字符串
 * @param exitTag 平仓标签，默认为空字符串
 * @details 将平仓信息写入平仓日志文件，格式为CSV
 *          包含字段：合约代码、方向、开仓时间、开仓价格、平仓时间、平仓价格、数量、盈亏、累计盈亏、开仓标签和平仓标签
 */
void SelStraBaseCtx::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty,
	double profit, double totalprofit /* = 0 */, const char* enterTag /* = "" */, const char* exitTag /* = "" */)
{
	if (_close_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
			<< "," << closeTime << "," << closepx << "," << qty << "," << profit << ","
			<< totalprofit << "," << enterTag << "," << exitTag << "\n";
		_trade_logs->write_file(ss.str());
	}
}

/**
 * @brief 保存用户自定义数据
 * @details 将用户自定义数据保存到JSON文件中，文件名格式为"ud_策略名.json"
 *          每次调用此函数时，会将_user_datas映射中的所有键值对写入文件
 *          文件保存在WtHelper::getStraUsrDatDir()指定的目录中
 *          使用RapidJSON库将数据序列化为JSON格式
 */
void SelStraBaseCtx::save_userdata()
{
	//ini.save(filename.c_str());
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();
	for (auto it = _user_datas.begin(); it != _user_datas.end(); it++)
	{
		root.AddMember(rj::Value(it->first.c_str(), allocator), rj::Value(it->second.c_str(), allocator), allocator);
	}

	{
		std::string filename = WtHelper::getStraUsrDatDir();
		filename += "ud_";
		filename += _name;
		filename += ".json";

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

/**
 * @brief 加载用户自定义数据
 * @details 从JSON文件中加载用户自定义数据到_user_datas映射中
 *          文件名格式为"ud_策略名.json"，位于WtHelper::getStraUsrDatDir()指定的目录
 *          如果文件不存在或内容为空，则不进行任何操作
 *          使用RapidJSON库解析JSON格式的数据，并将每个键值对加载到_user_datas映射中
 */
void SelStraBaseCtx::load_userdata()
{
	std::string filename = WtHelper::getStraUsrDatDir();
	filename += "ud_";
	filename += _name;
	filename += ".json";

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

	for (auto& m : root.GetObject())
	{
		const char* key = m.name.GetString();
		const char* val = m.value.GetString();
		_user_datas[key] = val;
	}
}

/**
 * @brief 加载策略数据
 * @param flag 加载标志，默认为0xFFFFFFFF（加载所有数据）
 * @details 从JSON文件中加载策略数据，包括资金信息、持仓信息和信号信息
 *          文件名格式为"策略名.json"，位于WtHelper::getStraDataDir()指定的目录
 *          如果文件不存在或内容为空，或者JSON解析出错，则直接返回
 *          该函数会对过期合约进行特殊处理，忽略其持仓和信号
 */
void SelStraBaseCtx::load_data(uint32_t flag /* = 0xFFFFFFFF */)
{
	std::string filename = WtHelper::getStraDataDir();
	filename += _name;
	filename += ".json";

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

	if (root.HasMember("fund"))
	{
		//读取资金
		const rj::Value& jFund = root["fund"];
		if (!jFund.IsNull() && jFund.IsObject())
		{
			_fund_info._total_profit = jFund["total_profit"].GetDouble();
			_fund_info._total_dynprofit = jFund["total_dynprofit"].GetDouble();
			uint32_t tdate = jFund["tdate"].GetUint();
			if (tdate == _engine->get_trading_date())
				_fund_info._total_fees = jFund["total_fees"].GetDouble();
		}
	}

	{//读取仓位
		double total_profit = 0;
		double total_dynprofit = 0;
		const rj::Value& jPos = root["positions"];
		if (!jPos.IsNull() && jPos.IsArray())
		{
			for (const rj::Value& pItem : jPos.GetArray())
			{
				const char* stdCode = pItem["code"].GetString();
				const char* ruleTag = _engine->get_hot_mgr()->getRuleTag(stdCode);
				bool isExpired = (strlen(ruleTag) == 0 && _engine->get_contract_info(stdCode) == NULL);

				if (isExpired)
					log_info("{} not exists or expired, position ignored", stdCode);

				PosInfo& pInfo = _pos_map[stdCode];
				pInfo._closeprofit = pItem["closeprofit"].GetDouble();
				pInfo._last_entertime = pItem["lastentertime"].GetUint64();
				pInfo._last_exittime = pItem["lastexittime"].GetUint64();
				pInfo._volume = isExpired ? 0 : pItem["volume"].GetDouble();
				if (pItem.HasMember("frozen") && !isExpired)
				{
					pInfo._frozen = pItem["frozen"].GetDouble();
					pInfo._frozen_date = pItem["frozendate"].GetUint();
				}

				if (pInfo._volume == 0 || isExpired)
				{
					pInfo._dynprofit = 0;
					pInfo._frozen = 0;
				}
				else
					pInfo._dynprofit = pItem["dynprofit"].GetDouble();				

				total_profit += pInfo._closeprofit;
				total_dynprofit += pInfo._dynprofit;

				const rj::Value& details = pItem["details"];
				if (details.IsNull() || !details.IsArray() || details.Size() == 0 || isExpired)
					continue;

				pInfo._details.resize(details.Size());

				for (uint32_t i = 0; i < details.Size(); i++)
				{
					const rj::Value& dItem = details[i];
					DetailInfo& dInfo = pInfo._details[i];
					dInfo._long = dItem["long"].GetBool();
					dInfo._price = dItem["price"].GetDouble();
					dInfo._volume = dItem["volume"].GetDouble();
					dInfo._opentime = dItem["opentime"].GetUint64();
					if (dItem.HasMember("opentdate"))
						dInfo._opentdate = dItem["opentdate"].GetUint();

					if (dItem.HasMember("maxprice"))
						dInfo._max_price = dItem["maxprice"].GetDouble();
					else
						dInfo._max_price = dInfo._price;

					if (dItem.HasMember("minprice"))
						dInfo._min_price = dItem["minprice"].GetDouble();
					else
						dInfo._min_price = dInfo._price;

					dInfo._profit = dItem["profit"].GetDouble();
					dInfo._max_profit = dItem["maxprofit"].GetDouble();
					dInfo._max_loss = dItem["maxloss"].GetDouble();

					strcpy(dInfo._opentag, dItem["opentag"].GetString());
				}

				if (!isExpired)
				{
					log_info("Position confirmed,{} -> {}", stdCode, pInfo._volume);
					stra_sub_ticks(stdCode);
				}
			}
		}

		_fund_info._total_profit = total_profit;
		_fund_info._total_dynprofit = total_dynprofit;
	}

	if (root.HasMember("signals"))
	{
		//读取信号
		const rj::Value& jSignals = root["signals"];
		if (!jSignals.IsNull() && jSignals.IsObject())
		{
			for (auto& m : jSignals.GetObject())
			{
				const char* stdCode = m.name.GetString();
				const char* ruleTag = _engine->get_hot_mgr()->getRuleTag(stdCode);
				if (strlen(ruleTag) == 0 && _engine->get_contract_info(stdCode) == NULL)
				{
					log_info("{} not exists or expired, signal ignored", stdCode);
					continue;
				}

				const rj::Value& jItem = m.value;

				SigInfo& sInfo = _sig_map[stdCode];
				sInfo._usertag = jItem["usertag"].GetString();
				sInfo._volume = jItem["volume"].GetDouble();
				sInfo._sigprice = jItem["sigprice"].GetDouble();
				sInfo._gentime = jItem["gentime"].GetUint64();

				log_info("{} untouched signal recovered, target pos: {}", stdCode, sInfo._volume);
				stra_sub_ticks(stdCode);
			}
		}
	}
}

/**
 * @brief 保存策略数据
 * @param flag 保存标志，默认为0xFFFFFFFF（保存所有数据）
 * @details 将策略数据保存到JSON文件中，包括持仓数据、资金数据和信号数据
 *          文件名格式为"策略名.json"，位于WtHelper::getStraDataDir()指定的目录
 *          使用RapidJSON库将数据序列化为JSON格式
 *          持仓数据包含总量、平仓盈亏、动态盈亏、冻结仓位等信息
 *          每个持仓还包含详细信息，如方向、价格、最高价、最低价等
 */
void SelStraBaseCtx::save_data(uint32_t flag /* = 0xFFFFFFFF */)
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
			pItem.AddMember("frozen", pInfo._frozen, allocator);
			pItem.AddMember("frozendate", pInfo._frozen_date, allocator);

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
		jFund.AddMember("tdate", _engine->get_trading_date(), allocator);

		root.AddMember("fund", jFund, allocator);
	}

	{//信号保存
		rj::Value jSigs(rj::kObjectType);
		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto it : _sig_map)
		{
			const char* stdCode = it.first.c_str();
			const SigInfo& sInfo = it.second;

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
		std::string filename = WtHelper::getStraDataDir();
		filename += _name;
		filename += ".json";

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

//////////////////////////////////////////////////////////////////////////
//回调函数
/**
 * @brief K线数据回调函数
 * @param stdCode 标准合约代码
 * @param period 周期标识
 * @param times 周期倍数
 * @param newBar 新K线数据指针
 * @details 当新的K线数据到达时调用此函数，将调用on_bar_close处理K线闭合事件
 *          首先检查newBar是否为空，如果为空则直接返回
 *          然后生成实际周期标识（包含周期和倍数）和唯一键
 *          最后标记K线已闭合，并调用on_bar_close函数
 */
void SelStraBaseCtx::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (newBar == NULL)
		return;

	thread_local static char realPeriod[8] = { 0 };
	fmtutil::format_to(realPeriod, "{}{}", period, times);

	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, realPeriod);

	KlineTag& tag = _kline_tags[key];
	tag._closed = true;

	on_bar_close(stdCode, realPeriod, newBar);
}

/**
 * @brief 策略初始化回调函数
 * @details 在策略初始化时调用，完成以下初始化工作：
 *          1. 调用init_outputs()初始化所有输出文件
 *          2. 调用load_data()加载策略数据，包括持仓、资金和信号信息
 *          3. 调用load_userdata()加载用户自定义数据
 *          这些初始化操作确保策略在开始运行前加载必要的数据和设置
 */
void SelStraBaseCtx::on_init()
{
	init_outputs();

	//读取数据
	load_data();

	load_userdata();
}

/**
 * @brief 更新动态盈亏
 * @param stdCode 标准合约代码
 * @param price 最新价格
 * @details 根据最新价格更新指定合约持仓的动态盈亏
 *          如果持仓量为0，则动态盈亏为0
 *          否则遍历所有持仓明细，计算每一笔的盈亏并累计
 *          同时更新每笔持仓的最大盈利、最大亏损、最高价和最低价
 *          最后更新策略总的动态盈亏
 */
void SelStraBaseCtx::update_dyn_profit(const char* stdCode, double price)
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
			WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
			double dynprofit = 0;
			for (auto pit = pInfo._details.begin(); pit != pInfo._details.end(); pit++)
			{
				DetailInfo& dInfo = *pit;
				dInfo._profit = dInfo._volume*(price - dInfo._price)*commInfo->getVolScale()*(dInfo._long ? 1 : -1);
				if (dInfo._profit > 0)
					dInfo._max_profit = std::max(dInfo._profit, dInfo._max_profit);
				else if (dInfo._profit < 0)
					dInfo._max_loss = std::min(dInfo._profit, dInfo._max_loss);

				dInfo._max_price = std::max(dInfo._max_price, price);
				dInfo._min_price = std::min(dInfo._min_price, price);

				dynprofit += dInfo._profit;
			}

			pInfo._dynprofit = dynprofit;
		}
	}

	double total_dynprofit = 0;
	for (auto v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		total_dynprofit += pInfo._dynprofit;
	}

	_fund_info._total_dynprofit = total_dynprofit;
}

/**
 * @brief Tick数据回调函数
 * @param stdCode 标准合约代码
 * @param newTick 新的Tick数据指针
 * @param bEmitStrategy 是否触发策略回调，默认为true
 * @details 当新的Tick数据到达时调用此函数，完成以下处理：
 *          1. 更新合约的最新价格到价格映射中
 *          2. 检查并触发待执行的信号（如果在交易时间内）
 *          3. 更新合约的动态盈亏
 *          4. 调用用户定义的tick更新回调函数（如果bEmitStrategy为true）
 *          5. 如果用户数据已修改，则保存用户数据
 */
void SelStraBaseCtx::on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy /* = true */)
{
	_price_map[stdCode] = newTick->price();

	//先检查是否要信号要触发
	{
		auto it = _sig_map.find(stdCode);
		if (it != _sig_map.end())
		{
			WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);

			if (sInfo->isInTradingTime(_engine->get_raw_time(), true))
			{
				const SigInfo& sInfo = it->second;
				do_set_position(stdCode, sInfo._volume, sInfo._usertag.c_str(), sInfo._triggered);
				_sig_map.erase(it);
			}

		}
	}

	update_dyn_profit(stdCode, newTick->price());

	if (bEmitStrategy)
		on_tick_updated(stdCode, newTick);

	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 定时调度回调函数
 * @param curDate 当前日期，格式为YYYYMMDD
 * @param curTime 当前时间，格式为HHMMSS
 * @param fireTime 触发时间
 * @return 调度是否成功
 * @details 在定时调度时调用，执行策略调度逻辑，完成以下处理：
 *          1. 记录当前的日期和时间，并标记进入调度状态
 *          2. 保存策略数据（包括浮动盈亏）
 *          3. 调用用户定义的策略调度函数
 *          4. 清理无效持仓（新信号中不存在的持仓）
 *          5. 记录调度统计信息（调度次数和计算时间）
 *          6. 如果用户数据已修改，则保存用户数据
 *          7. 标记调度结束
 */
bool SelStraBaseCtx::on_schedule(uint32_t curDate, uint32_t curTime, uint32_t fireTime)
{
	_schedule_date = curDate;
	_schedule_time = curTime;

	_is_in_schedule = true;//开始调度, 修改标记	

	//主要用于保存浮动盈亏的
	save_data();

	TimeUtils::Ticker ticker;
	on_strategy_schedule(curDate, fireTime);
	log_debug("Strategy {} scheduled @ {}", _context_id, curTime);

	wt_hashset<std::string> to_clear;
	for (auto& v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		const char* code = v.first.c_str();
		if (_sig_map.find(code) == _sig_map.end() && !decimal::eq(pInfo._volume, 0.0))
		{
			//新的信号中没有该持仓,则要清空
			to_clear.insert(code);
		}
	}

	for (const std::string& code : to_clear)
	{
		append_signal(code.c_str(), 0, "autoexit");
	}

	_emit_times++;
	_total_calc_time += ticker.micro_seconds();

	if (_emit_times % 20 == 0)
		log_info("Strategy has been scheduled {} times, totally taking {} us, {:.3f} us each time",
			_emit_times, _total_calc_time, _total_calc_time*1.0 / _emit_times);

	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}

	_is_in_schedule = false;//调度结束, 修改标记
	return true;
}

/**
 * @brief 交易日开始回调函数
 * @param uTDate 交易日，格式为YYYYMMDD
 * @details 在每个交易日开始时调用，完成以下处理：
 *          1. 释放过期的冻结持仓（冻结日期早于当前交易日的持仓）
 *          2. 如果用户数据已修改，则保存用户数据
 *          这个函数主要处理T+1交易规则下的冻结仓位释放
 */
void SelStraBaseCtx::on_session_begin(uint32_t uTDate)
{
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (pInfo._frozen_date != 0 && pInfo._frozen_date < uTDate && !decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen on {} released on {}", pInfo._frozen, stdCode, pInfo._frozen_date, uTDate);

			pInfo._frozen = 0;
			pInfo._frozen_date = 0;
		}
	}

	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 枚举所有持仓和信号
 * @param cb 持仓回调函数，类型为FuncEnumSelPositionCallBack
 * @details 对所有持仓和信号进行枚举，并调用回调函数
 *          首先收集所有实际持仓信息，然后收集所有信号信息
 *          如果同一合约既有持仓又有信号，信号会覆盖持仓
 *          最后对每个合约调用回调函数，传入合约代码和目标仓位
 */
void SelStraBaseCtx::enum_position(FuncEnumSelPositionCallBack cb)
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
 * @brief 交易日结束回调函数
 * @param uTDate 交易日，格式为YYYYMMDD
 * @details 在每个交易日结束时调用，完成以下处理：
 *          1. 计算总平仓盈亏和总动态盈亏
 *          2. 记录每个非零持仓的日志，包括合约、数量、平仓盈亏和动态盈亏
 *          3. 记录资金日志，包括总平仓盈亏、总动态盈亏、净值和手续费
 *          4. 保存策略数据
 *          5. 如果用户数据已修改，则保存用户数据
 */
void SelStraBaseCtx::on_session_end(uint32_t uTDate)
{
	uint32_t curDate = uTDate;//_engine->get_trading_date();

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

		if (_pos_logs)
			_pos_logs->write_file(fmt::format("{},{},{},{:.2f},{:.2f}\n", curDate, stdCode,
				pInfo._volume, pInfo._closeprofit, pInfo._dynprofit));
	}

	if (_fund_logs)
		_fund_logs->write_file(fmt::format("{},{:.2f},{:.2f},{:.2f},{:.2f}\n", curDate,
		_fund_info._total_profit, _fund_info._total_dynprofit,
		_fund_info._total_profit + _fund_info._total_dynprofit - _fund_info._total_fees, _fund_info._total_fees));

	save_data();

	if (_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}


//////////////////////////////////////////////////////////////////////////
//策略接口
#pragma region "策略接口"
/**
 * @brief 获取合约当前价格
 * @param stdCode 标准合约代码
 * @return 合约当前价格，如果不存在则返回0
 * @details 首先从内部价格映射中查找，如果找不到则从引擎中获取
 *          这个函数是策略用来获取合约当前价格的主要接口
 */
double SelStraBaseCtx::stra_get_price(const char* stdCode)
{
	auto it = _price_map.find(stdCode);
	if (it != _price_map.end())
		return it->second;

	if (_engine)
		return _engine->get_cur_price(stdCode);

	return 0.0;
}

/**
 * @brief 设置合约目标仓位
 * @param stdCode 标准合约代码
 * @param qty 目标仓位数量，正数表示多头，负数表示空头
 * @param userTag 用户标签，默认为空字符串
 * @details 设置策略中合约的目标仓位，完成以下处理：
 *          1. 检查合约信息是否存在
 *          2. 检查合约是否支持做空
 *          3. 检查目标仓位是否与当前仓位一致
 *          4. 如果是T+1合约，检查目标仓位是否小于冻结仓位
 *          5. 最后调用append_signal生成信号
 */
void SelStraBaseCtx::stra_set_position(const char* stdCode, double qty, const char* userTag /* = "" */)
{
	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
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
			log_error("New position of {} cannot be set to {} due to {} being frozen", stdCode, qty, frozen);
			return;
		}
	}

	append_signal(stdCode, qty, userTag);
}

/**
 * @brief 添加交易信号
 * @param stdCode 标准合约代码
 * @param qty 目标仓位数量，正数表示多头，负数表示空头
 * @param userTag 用户标签，默认为空字符串
 * @details 生成并添加交易信号，完成以下处理：
 *          1. 获取当前合约价格
 *          2. 创建或更新信号信息，包括目标仓位、信号价格、用户标签、生成时间等
 *          3. 根据当前是否在调度中设置触发标记
 *          4. 记录信号日志
 *          5. 保存策略数据
 */
void SelStraBaseCtx::append_signal(const char* stdCode, double qty, const char* userTag /* = "" */)
{
	double curPx = _price_map[stdCode];

	SigInfo& sInfo = _sig_map[stdCode];
	sInfo._volume = qty;
	sInfo._sigprice = curPx;
	sInfo._usertag = userTag;
	sInfo._gentime = (uint64_t)_engine->get_date() * 1000000000 + (uint64_t)_engine->get_raw_time() * 100000 + _engine->get_secs();
	sInfo._triggered = !_is_in_schedule;

	log_signal(stdCode, qty, curPx, sInfo._gentime, userTag);

	save_data();
}

/**
 * @brief 实际设置仓位
 * @param stdCode 标准合约代码
 * @param qty 目标仓位数量
 * @param userTag 用户标签，默认为空字符串
 * @param bTriggered 是否已触发，默认为false
 * @details 实际执行仓位调整操作，完成以下处理：
 *          1. 如果目标仓位与当前仓位相同，直接返回
 *          2. 计算仓位差值并获取合约信息
 *          3. 如果当前持仓和目标仓位方向一致，则增加仓位
 *             - 如果是T+1合约，更新冻结仓位
 *             - 考虑滑点调整成交价
 *             - 添加仓位明细并记录交易日志
 *          4. 如果持仓方向和目标仓位方向不一致，则需要平仓
 *             - 考虑滑点调整成交价
 *             - 逐个处理持仓明细，计算平仓盈亏
 *             - 记录平仓日志
 */
void SelStraBaseCtx::do_set_position(const char* stdCode, double qty, const char* userTag /* = "" */, bool bTriggered /* = false */)
{
	PosInfo& pInfo = _pos_map[stdCode];
	double curPx = _price_map[stdCode];
	uint64_t curTm = (uint64_t)_engine->get_date() * 10000 + _engine->get_min_time();
	uint32_t curTDate = _engine->get_trading_date();

	if (decimal::eq(pInfo._volume, qty))
		return;

	double diff = qty - pInfo._volume;

	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
	if (commInfo == NULL)
		return;

	//成交价
	double trdPx = curPx;

	bool isBuy = decimal::gt(diff, 0.0);
	if (decimal::gt(pInfo._volume*diff, 0))//当前持仓和目标仓位方向一致, 增加一条明细, 增加数量即可
	{
		pInfo._volume = qty;
		//如果T+1，则冻结仓位要增加
		if (commInfo->isT1())
		{
			//ASSERT(diff>0);
			pInfo._frozen += diff;
			pInfo._frozen_date = curTDate;
			log_debug("{} frozen position updated to {}", stdCode, pInfo._frozen);
		}

		if (_slippage != 0)
		{
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
		wt_strcpy(dInfo._opentag, userTag);
		pInfo._details.push_back(dInfo);
		pInfo._last_entertime = curTm;

		double fee = commInfo->calcFee(trdPx, abs(qty), 0);
		_fund_info._total_fees += fee;
		//_engine->mutate_fund(fee, FFT_Fee);
		log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(qty), userTag, fee);
	}
	else
	{//持仓方向和目标仓位方向不一致, 需要平仓
		double left = abs(diff);

		if (_slippage != 0)
			trdPx += _slippage * commInfo->getPriceTick()*(isBuy ? 1 : -1);

		pInfo._volume = qty;
		if (decimal::eq(pInfo._volume, 0))
			pInfo._dynprofit = 0;
		uint32_t count = 0;
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			DetailInfo& dInfo = *it;
			double maxQty = min(dInfo._volume, left);
			//if (maxQty == 0)
			if (decimal::eq(maxQty, 0))
				continue;

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

			double fee = commInfo->calcFee(trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;
			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, userTag, fee);
			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, pInfo._closeprofit, dInfo._opentag, userTag);

			//if (left == 0)
			if (decimal::eq(left, 0))
				break;
		}

		//需要清理掉已经平仓完的明细
		while (count > 0)
		{
			auto it = pInfo._details.begin();
			pInfo._details.erase(it);
			count--;
		}

		//最后, 如果还有剩余的, 则需要反手了
		//if (left > 0)
		if (decimal::gt(left, 0))
		{
			left = left * qty / abs(qty);

			//如果T+1，则冻结仓位要增加
			if (commInfo->isT1())
			{
				//ASSERT(diff>0);
				pInfo._frozen += diff;
				pInfo._frozen_date = curTDate;
				log_debug("{} frozen position updated to {}", stdCode, pInfo._frozen);
			}

			DetailInfo dInfo;
			dInfo._long = decimal::gt(qty, 0);
			dInfo._price = trdPx;
			dInfo._max_price = trdPx;
			dInfo._min_price = trdPx;
			dInfo._volume = abs(left);
			dInfo._opentime = curTm;
			dInfo._opentdate = curTDate;
			wt_strcpy(dInfo._opentag, userTag);
			pInfo._details.push_back(dInfo);
			pInfo._last_entertime = curTm;

			//这里还需要写一笔成交记录
			double fee = commInfo->calcFee(trdPx, abs(qty), 0);
			_fund_info._total_fees += fee;
			//_engine->mutate_fund(fee, FFT_Fee);
			log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), userTag, fee);
		}
	}

	//存储数据
	save_data();

	_engine->handle_pos_change(_name.c_str(), stdCode, diff);
}

/**
 * @brief 获取K线切片数据
 * @param stdCode 标准合约代码
 * @param period 周期标识，如"m1"表示1分钟，"d1"表示日线
 * @param count 请求的K线数量
 * @return 返回K线切片指针，如果数据不存在则返回nullptr
 * @details 获取指定合约的K线数据，完成以下处理：
 *          1. 根据合约和周期生成唯一键
 *          2. 解析周期标识，提取基础周期和周期倍数
 *          3. 根据周期类型确定结束时间
 *          4. 从引擎中获取K线切片
 *          5. 标记K线未闭合
 *          6. 如果成功获取切片，更新合约的最新价格
 */
WTSKlineSlice* SelStraBaseCtx::stra_get_bars(const char* stdCode, const char* period, uint32_t count)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);
	
	uint64_t etime = 0;
	if (period[0] == 'd')
	{
		WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);
		etime = (uint64_t)_schedule_date * 10000 + sInfo->getCloseTime();
	}
	else
		etime = (uint64_t)_schedule_date * 10000 + _schedule_time;

	WTSKlineSlice* kline = _engine->get_kline_slice(_context_id, stdCode, basePeriod, count, times, etime);

	KlineTag& tag = _kline_tags[key];
	tag._closed = false;

	if (kline)
	{
		double lastClose = kline->at(-1)->close;
		_price_map[stdCode] = lastClose;
	}

	return kline;
}

/**
 * @brief 获取Tick切片数据
 * @param stdCode 标准合约代码
 * @param count 请求的Tick数量
 * @return 返回Tick切片指针，如果数据不存在则返回nullptr
 * @details 从引擎中获取指定合约的Tick数据
 *          这个函数是策略用来获取合约实时Tick数据的主要接口
 *          返回的是最新的count个Tick数据组成的切片
 */
WTSTickSlice* SelStraBaseCtx::stra_get_ticks(const char* stdCode, uint32_t count)
{
	return _engine->get_tick_slice(_context_id, stdCode, count);
}

/**
 * @brief 获取最新的Tick数据
 * @param stdCode 标准合约代码
 * @return 返回最新的Tick数据指针，如果数据不存在则返回nullptr
 * @details 从引擎中获取指定合约的最新Tick数据
 *          这个函数是策略用来获取合约最新市场数据的快捷方式
 *          与获取切片不同，这个函数只返回单个最新的Tick数据
 */
WTSTickData* SelStraBaseCtx::stra_get_last_tick(const char* stdCode)
{
	return _engine->get_last_tick(_context_id, stdCode);
}

/**
 * @brief 订阅合约的Tick数据
 * @param stdCode 标准合约代码
 * @details 订阅指定合约的Tick数据，完成以下处理：
 *          1. 将合约代码添加到本地订阅集合中
 *          2. 通知引擎订阅该合约的Tick数据
 *          3. 记录日志
 *          订阅后，引擎会将合约的Tick数据通过on_tick函数回调给策略
 */
void SelStraBaseCtx::stra_sub_ticks(const char* stdCode)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	_tick_subs.insert(stdCode);

	_engine->sub_tick(_context_id, stdCode);
	log_info("Market data subscribed: {}", stdCode);
}

/**
 * @brief 获取合约品种信息
 * @param stdCode 标准合约代码
 * @return 返回合约品种信息指针，如果不存在则返回nullptr
 * @details 从引擎中获取指定合约的品种信息
 *          品种信息包含合约的交易单位、点值、手续费率、交易时间等重要参数
 *          策略可以使用这些信息进行交易决策和风控计算
 */
WTSCommodityInfo* SelStraBaseCtx::stra_get_comminfo(const char* stdCode)
{
	return _engine->get_commodity_info(stdCode);
}

/**
 * @brief 获取合约的原始代码
 * @param stdCode 标准合约代码
 * @return 返回合约的原始代码
 * @details 从引擎中获取指定标准合约代码对应的原始代码
 *          标准合约代码是系统内部使用的统一格式代码，而原始代码是交易所或数据源的原始代码
 *          在某些场景下，策略可能需要知道合约的原始代码，例如输出日志或调试信息
 */
std::string SelStraBaseCtx::stra_get_rawcode(const char* stdCode)
{
	return _engine->get_rawcode(stdCode);
}

/**
 * @brief 获取合约的交易时段信息
 * @param stdCode 标准合约代码
 * @return 返回交易时段信息指针，如果不存在则返回nullptr
 * @details 从引擎中获取指定合约的交易时段信息
 *          交易时段信息包含开盘时间、收盘时间、交易时段等重要参数
 *          策略可以使用这些信息确定交易时间和交易规则
 */
WTSSessionInfo* SelStraBaseCtx::stra_get_sessinfo(const char* stdCode)
{
	return _engine->get_session_info(stdCode, true);
}

/**
 * @brief 获取合约的日线价格
 * @param stdCode 标准合约代码
 * @param flag 价格标志，0表示收盘价，1表示开盘价，2表示最高价，3表示最低价，默认为0
 * @return 返回指定类型的日线价格，如果不存在则返回0
 * @details 从引擎中获取指定合约的日线价格
 *          根据flag参数可以获取不同类型的价格，如收盘价、开盘价、最高价和最低价
 *          这个函数在策略中通常用于获取前一交易日的价格信息
 */
double SelStraBaseCtx::stra_get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if (_engine)
		return _engine->get_day_price(stdCode, flag);

	return 0.0;
}

/**
 * @brief 获取当前交易日
 * @return 返回当前交易日，格式为YYYYMMDD
 * @details 从引擎中获取当前的交易日期
 *          交易日是指当前的交易日期，可能与自然日不同
 *          例如，夜盘交易时，交易日是指下一个交易日
 */
uint32_t SelStraBaseCtx::stra_get_tdate()
{
	return _engine->get_trading_date();
}

/**
 * @brief 获取当前日期
 * @return 返回当前日期，格式为YYYYMMDD
 * @details 获取当前的日期，根据是否在调度中返回不同的值
 *          如果在调度中，返回调度日期；否则返回引擎当前日期
 *          与交易日不同，这个函数返回的是实际的自然日期
 */
uint32_t SelStraBaseCtx::stra_get_date()
{
	return _is_in_schedule ? _schedule_date : _engine->get_date();
}

/**
 * @brief 获取当前时间
 * @return 返回当前时间，格式为HHMMSS
 * @details 获取当前的时间，根据是否在调度中返回不同的值
 *          如果在调度中，返回调度时间；否则返回引擎当前分钟时间
 *          这个函数通常用于获取当前策略运行的时间点
 */
uint32_t SelStraBaseCtx::stra_get_time()
{
	return _is_in_schedule ? _schedule_time : _engine->get_min_time();
}

/**
 * @brief 获取资金数据
 * @param flag 资金数据标志，0表示总资金（0号资金 = 平仓盈亏 + 浮动盈亏 - 手续费），1表示平仓盈亏，2表示浮动盈亏，3表示手续费
 * @return 返回指定类型的资金数据
 * @details 获取策略的资金数据，根据flag参数返回不同类型的资金信息
 *          这个函数在策略中通常用于获取资金状况信息，进行风控计算或统计分析
 */
double SelStraBaseCtx::stra_get_fund_data(int flag)
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
 * @param message 日志消息内容
 * @details 记录信息级别的日志消息
 *          消息将以策略名称为标识，以INFO级别记录
 *          这个函数在策略中通常用于记录一般信息，如策略初始化、交易信号等
 */
void SelStraBaseCtx::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

/**
 * @brief 记录调试级别日志
 * @param message 日志消息内容
 * @details 记录调试级别的日志消息
 *          消息将以策略名称为标识，以DEBUG级别记录
 *          这个函数在策略中通常用于记录详细的调试信息，如中间计算结果、变量值等
 */
void SelStraBaseCtx::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

/**
 * @brief 记录警告级别日志
 * @param message 日志消息内容
 * @details 记录警告级别的日志消息
 *          消息将以策略名称为标识，以WARN级别记录
 *          这个函数在策略中通常用于记录可能存在问题的警告信息，如参数超过预期范围、数据异常等
 */
void SelStraBaseCtx::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

/**
 * @brief 记录错误级别日志
 * @param message 日志消息内容
 * @details 记录错误级别的日志消息
 *          消息将以策略名称为标识，以ERROR级别记录
 *          这个函数在策略中通常用于记录严重错误信息，如计算失败、交易失败等
 *          错误日志通常需要特别关注，因为它们可能表示策略运行出现了严重问题
 */
void SelStraBaseCtx::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

/**
 * @brief 加载用户数据
 * @param key 数据键名
 * @param defVal 默认值，当键不存在时返回，默认为空字符串
 * @return 返回与键关联的值，如果键不存在则返回默认值
 * @details 从_user_datas映射中检索指定键的值
 *          这是策略代码用来访问用户数据的主要接口
 */
const char* SelStraBaseCtx::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	return defVal;
}

/**
 * @brief 保存用户数据
 * @param key 数据键名
 * @param val 数据值
 * @details 将指定的键值对保存到_user_datas映射中，并标记数据已修改
 *          这是策略代码用来存储用户数据的主要接口
 *          注意：调用此函数后，数据不会立即写入文件，而是在策略调度结束或交易日结束时保存
 */
void SelStraBaseCtx::stra_save_user_data(const char* key, const char* val)
{
	_user_datas[key] = val;
	_ud_modified = true;
}

/**
 * @brief 获取合约的首次入场时间
 * @param stdCode 标准合约代码
 * @return 返回首次入场时间，格式为YYYYMMDDHHMM，如果没有持仓则返回0
 * @details 获取指定合约的首次入场时间
 *          首次入场时间指的是当前持仓明细中最早的入场时间
 *          这个函数在策略中通常用于获取持仓时间，计算持仓周期等
 */
uint64_t SelStraBaseCtx::stra_get_first_entertime(const char* stdCode)
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
 * @brief 获取合约的最后入场标签
 * @param stdCode 标准合约代码
 * @return 返回最后入场标签，如果没有持仓则返回空字符串
 * @details 获取指定合约的最后入场标签
 *          最后入场标签指的是当前持仓明细中第一笔入场的用户标签
 *          这个函数在策略中通常用于获取入场原因或入场标识
 */
const char* SelStraBaseCtx::stra_get_last_entertag(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return "";

	const PosInfo& pInfo = it->second;
	if (pInfo._details.empty())
		return "";

	return pInfo._details[0]._opentag;
}


/**
 * @brief 获取合约的最后出场时间
 * @param stdCode 标准合约代码
 * @return 返回最后出场时间，格式为YYYYMMDDHHMM，如果没有持仓则返回0
 * @details 获取指定合约的最后出场时间
 *          最后出场时间指的是当前持仓的最后一次减仓或平仓的时间
 *          这个函数在策略中通常用于获取上次出场时间，计算出场间隔等
 */
uint64_t SelStraBaseCtx::stra_get_last_exittime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._last_exittime;
}

/**
 * @brief 获取合约的最后入场时间
 * @param stdCode 标准合约代码
 * @return 返回最后入场时间，格式为YYYYMMDDHHMM，如果没有持仓则返回0
 * @details 获取指定合约的最后入场时间
 *          最后入场时间指的是当前持仓明细中最后一笔入场的时间
 *          这个函数在策略中通常用于获取最近入场时间，计算持仓时间等
 */
uint64_t SelStraBaseCtx::stra_get_last_entertime(const char* stdCode)
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
 * @brief 获取合约的最后入场价格
 * @param stdCode 标准合约代码
 * @return 返回最后入场价格，如果没有持仓则返回0
 * @details 获取指定合约的最后入场价格
 *          最后入场价格指的是当前持仓明细中最后一笔入场的价格
 *          这个函数在策略中通常用于获取最近入场价格，计算盈亏等
 */
double SelStraBaseCtx::stra_get_last_enterprice(const char* stdCode)
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
 * @brief 获取合约的持仓量
 * @param stdCode 标准合约代码
 * @param bOnlyValid 是否只返回有效仓位（总仓位 - 冻结仓位），默认为false
 * @param userTag 用户标签，如果指定了用户标签，则只返回对应标签的持仓量，默认为空字符串
 * @return 返回持仓量，如果没有持仓则返回0
 * @details 获取指定合约的持仓量，完成以下处理：
 *          1. 如果没有指定用户标签，则返回总持仓量或有效持仓量（根据bOnlyValid参数）
 *          2. 如果指定了用户标签，则遍历所有持仓明细，返回匹配标签的持仓量
 *          这个函数在策略中通常用于获取当前持仓状态，进行仓位管理和风控计算
 */
double SelStraBaseCtx::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, const char* userTag /* = "" */)
{
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
 * @brief 获取合约的持仓平均价格
 * @param stdCode 标准合约代码
 * @return 返回持仓平均价格，如果没有持仓则返回0
 * @details 获取指定合约的持仓平均价格，计算方式如下：
 *          1. 遍历所有持仓明细，计算总金额（每笔持仓的价格 * 数量的总和）
 *          2. 总金额除以总持仓量，得到平均价格
 *          这个函数在策略中通常用于获取当前持仓的平均成本，计算盈亏等
 */
double SelStraBaseCtx::stra_get_position_avgpx(const char* stdCode)
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
 * @brief 获取合约的持仓浮动盈亏
 * @param stdCode 标准合约代码
 * @return 返回持仓浮动盈亏，如果没有持仓则返回0
 * @details 获取指定合约的持仓浮动盈亏
 *          浮动盈亏是指根据当前市场价格计算的未实现盈亏
 *          这个函数在策略中通常用于获取当前持仓的盈亏状况，进行风控管理
 */
double SelStraBaseCtx::stra_get_position_profit(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

/**
 * @brief 获取指定标签的持仓明细入场时间
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @return 返回指定标签的持仓明细入场时间，格式为YYYYMMDDHHMM，如果没有匹配的持仓则返回0
 * @details 获取指定合约和标签的持仓明细入场时间
 *          遍历所有持仓明细，找到匹配标签的明细并返回其入场时间
 *          这个函数在策略中通常用于获取特定标签的持仓入场时间，计算持仓周期等
 */
uint64_t SelStraBaseCtx::stra_get_detail_entertime(const char* stdCode, const char* userTag)
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
 * @brief 获取指定标签的持仓明细成本价格
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @return 返回指定标签的持仓明细成本价格，如果没有匹配的持仓则返回0
 * @details 获取指定合约和标签的持仓明细成本价格
 *          遍历所有持仓明细，找到匹配标签的明细并返回其成本价格
 *          这个函数在策略中通常用于获取特定标签的持仓成本，计算盈亏等
 */
double SelStraBaseCtx::stra_get_detail_cost(const char* stdCode, const char* userTag)
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
 * @brief 获取指定标签的持仓明细盈亏信息
 * @param stdCode 标准合约代码
 * @param userTag 用户标签
 * @param flag 盈亏标志，0表示当前盈亏，1表示最大盈利，-1表示最大亏损，2表示最高价格，-2表示最低价格，默认为0
 * @return 返回指定标签的持仓明细盈亏信息，如果没有匹配的持仓则返回0
 * @details 获取指定合约和标签的持仓明细盈亏信息
 *          遍历所有持仓明细，找到匹配标签的明细，根据flag参数返回不同类型的盈亏信息：
 *          - flag=0：返回当前盈亏
 *          - flag=1：返回最大盈利
 *          - flag=-1：返回最大亏损
 *          - flag=2：返回最高价格
 *          - flag=-2：返回最低价格
 *          这个函数在策略中通常用于获取特定标签的持仓盈亏信息，进行风控管理
 */
double SelStraBaseCtx::stra_get_detail_profit(const char* stdCode, const char* userTag, int flag /* = 0 */)
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

#pragma endregion 