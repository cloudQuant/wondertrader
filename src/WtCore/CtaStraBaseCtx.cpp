/*!
 * \file CtaStraBaseCtx.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief CTA策略基础上下文实现文件
 * \details 实现了CTA策略的基础上下文类，为策略提供交易接口、数据访问、信号处理、
 *          持仓管理以及图表和日志输出等功能，是策略与交易引擎之间的桥梁
 */
#include "CtaStraBaseCtx.h"
#include "WtCtaEngine.h"
#include "WtHelper.h"

#include <exception>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

#include "../Share/StrUtil.hpp"
#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/IHotMgr.h"
#include "../Includes/WTSTradeDef.hpp"
#include "../Share/decimal.h"
#include "../Share/CodeHelper.hpp"

#include "../WTSTools/WTSLogger.h"

/**
 * @brief 条件判断操作符名称数组
 * @details 用于策略条件表达式的比较操作符映射，如等于、大于、小于等
 */
const char* CMP_ALG_NAMES[] =
{
	"＝",
	">",
	"<",
	">=",
	"<="
};

/**
 * @brief 策略动作名称数组
 * @details 用于标识策略执行的具体动作，如开多、平多、开空、平空、同步
 */
const char* ACTION_NAMES[] =
{
	"OL",
	"CL",
	"OS",
	"CS",
	"SYN"
};


/**
 * @brief 生成唯一的策略上下文ID
 * @return 返回自增的上下文ID，保证每个策略实例唯一
 */
inline uint32_t makeCtaCtxId()
{
	static std::atomic<uint32_t> _auto_context_id{ 1 };
	return _auto_context_id.fetch_add(1);
}


/**
 * @brief CtaStraBaseCtx构造函数
 * @param engine CTA引擎实例，提供策略运行环境
 * @param name 策略名称
 * @param slippage 滑点值，模拟交易成本
 * @note 初始化策略上下文，设置相关参数
 */
CtaStraBaseCtx::CtaStraBaseCtx(WtCtaEngine* engine, const char* name, int32_t slippage)
	: ICtaStraCtx(name)
	, _engine(engine)
	, _total_calc_time(0)
	, _emit_times(0)
	, _last_cond_min(0)
	, _is_in_schedule(false)
	, _ud_modified(false)
	, _last_barno(0)
	, _slippage(slippage)
{
	_context_id = makeCtaCtxId();
}


/**
 * @brief CtaStraBaseCtx析构函数
 * @note 释放策略上下文资源
 */
CtaStraBaseCtx::~CtaStraBaseCtx()
{
}

/**
 * @brief 初始化日志输出文件
 * @details 创建trades.csv、closes.csv、funds.csv等日志文件，用于记录策略运行过程中的交易、资金、信号等信息。
 * @note 日志文件有助于策略回测和实盘分析。
 */
void CtaStraBaseCtx::init_outputs()
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
			_trade_logs->write_file("code,time,direct,action,price,qty,tag,fee,barno\n");
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
			_close_logs->write_file("code,direct,opentime,openprice,closetime,closeprice,qty,profit,totalprofit,entertag,exittag,openbarno,closebarno\n");
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

	filename = folder + "indice.csv";
	_idx_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_idx_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_idx_logs->write_file("bartime,index_name,line_name,value\n");
		}
		else
		{
			_idx_logs->seek_to_end();
		}
	}

	filename = folder + "marks.csv";
	_mark_logs.reset(new BoostFile());
	{
		bool isNewFile = !BoostFile::exists(filename.c_str());
		_mark_logs->create_or_open_file(filename.c_str());
		if (isNewFile)
		{
			_mark_logs->write_file("bartime,price,icon,tag\n");
		}
		else
		{
			_mark_logs->seek_to_end();
		}
	}
}

/**
 * @brief 记录交易信号日志
 * @param stdCode 标准化合约代码
 * @param target 信号目标仓位
 * @param price 信号价格
 * @param gentime 信号生成时间戳
 * @param usertag 用户自定义标签
 * @note 写入signals.csv，便于策略信号追踪和复盘分析
 */
void CtaStraBaseCtx::log_signal(const char* stdCode, double target, double price, uint64_t gentime, const char* usertag /* = "" */)
{
	if (_sig_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << target << "," << price << "," << gentime << "," << usertag << "\n";
		_sig_logs->write_file(ss.str());
	}
}

/**
 * @brief 记录单笔交易明细
 * @param stdCode 标准化合约代码
 * @param isLong 多空方向，true为多头，false为空头
 * @param isOpen 开平仓标志，true为开仓，false为平仓
 * @param curTime 交易时间戳
 * @param price 成交价格
 * @param qty 成交数量
 * @param userTag 用户自定义标签
 * @param fee 手续费
 * @param barNo K线编号
 * @note 写入trades.csv，便于交易明细统计和回测分析
 */
void CtaStraBaseCtx::log_trade(const char* stdCode, bool isLong, bool isOpen, uint64_t curTime, double price, double qty, const char* userTag /* = "" */, double fee /* = 0.0 */, uint32_t barNo /* = 0 */)
{
	if (_trade_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << curTime << "," << (isLong ? "LONG" : "SHORT") << "," << (isOpen ? "OPEN" : "CLOSE") << "," << price << "," << qty << "," << userTag << "," << fee << "," << barNo << "\n";
		_trade_logs->write_file(ss.str());
	}

	_engine->notify_trade(this->name(),stdCode, isLong, isOpen, curTime, price, userTag);
}

/**
 * @brief 记录平仓明细日志
 * @param stdCode 标准化合约代码
 * @param isLong 多空方向，true为多头，false为空头
 * @param openTime 开仓时间戳
 * @param openpx 开仓价格
 * @param closeTime 平仓时间戳
 * @param closepx 平仓价格
 * @param qty 平仓数量
 * @param profit 本次平仓盈亏
 * @param totalprofit 累计平仓盈亏
 * @param enterTag 开仓信号标签
 * @param exitTag 平仓信号标签
 * @param openBarNo 开仓K线编号
 * @param closeBarNo 平仓K线编号
 * @note 写入closes.csv，便于完整记录每笔交易生命周期
 */
void CtaStraBaseCtx::log_close(const char* stdCode, bool isLong, uint64_t openTime, double openpx, uint64_t closeTime, double closepx, double qty, double profit, double totalprofit /* = 0 */, 
	const char* enterTag /* = "" */, const char* exitTag /* = "" */, uint32_t openBarNo /* = 0 */, uint32_t closeBarNo /* = 0 */)
{
	if (_close_logs)
	{
		std::stringstream ss;
		ss << stdCode << "," << (isLong ? "LONG" : "SHORT") << "," << openTime << "," << openpx
			<< "," << closeTime << "," << closepx << "," << qty << "," << profit << "," 
			<< totalprofit << "," << enterTag << "," << exitTag << "," << openBarNo << "," << closeBarNo << "\n";
		_close_logs->write_file(ss.str());
	}
}
/**
 * @brief 保存用户自定义数据到JSON文件
 * @details 将_user_datas映射写入ud_[策略名].json，持久化策略状态
 * @note 策略退出或定期保存用户数据时调用
 */
void CtaStraBaseCtx::save_userdata()
{
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
 * @brief 从JSON文件加载用户自定义数据
 * @details 读取用户数据文件ud_[策略名].json，将内容解析为_user_datas映射
 * @note 文件不存在或解析错误时自动跳过
 */
void CtaStraBaseCtx::load_userdata()
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
 * @brief 加载策略历史数据
 * @param flag 数据加载标志（按位控制资金、持仓、信号等子模块）
 * @details 从[name].json读取策略资金、持仓及明细，并恢复到运行上下文
 * @note 在策略初始化或回测开始阶段调用
 */
void CtaStraBaseCtx::load_data(uint32_t flag /* = 0xFFFFFFFF */)
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

	if(root.HasMember("fund"))
	{
		//读取资金
		const rj::Value& jFund = root["fund"];
		if(!jFund.IsNull() && jFund.IsObject())
		{
			_fund_info._total_profit = jFund["total_profit"].GetDouble();
			_fund_info._total_dynprofit = jFund["total_dynprofit"].GetDouble();
			uint32_t tdate = jFund["tdate"].GetUint();
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

				if(isExpired)
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
					//By Wesley @ 2023.02.21
					//加这一行的原因是，有些期权合约经常会持有到交割日
					//所以如果合约过期了，那么需要把浮动盈亏当做平仓盈亏累加一下
					//处理完以后，下一次加载，浮动盈亏就是0了
					pInfo._closeprofit += pInfo._dynprofit;

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

				for (uint32_t i = 0; i < details.Size(); i++)
				{
					const rj::Value& dItem = details[i];
					double vol = dItem["volume"].GetDouble();
					if(decimal::eq(vol, 0))
						continue;

					DetailInfo dInfo;
					dInfo._long = dItem["long"].GetBool();
					dInfo._price = dItem["price"].GetDouble();
					dInfo._volume = dItem["volume"].GetDouble();
					dInfo._opentime = dItem["opentime"].GetUint64();
					if(dItem.HasMember("opentdate"))
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
					if (dItem.HasMember("openbarno"))
						dInfo._open_barno = dItem["openbarno"].GetUint();
					else
						dInfo._open_barno = 0;

					pInfo._details.emplace_back(dInfo);
				}

				if(!isExpired)
				{
					log_info("Position confirmed,{} -> {}", stdCode, pInfo._volume);
					stra_sub_ticks(stdCode);
				}
			}
		}

		_fund_info._total_profit = total_profit;
		_fund_info._total_dynprofit = total_dynprofit;
	}

	{//读取条件单
		uint32_t count = 0;
		const rj::Value& jCond = root["conditions"];
		if (!jCond.IsNull() && jCond.IsObject())
		{
			_last_cond_min = jCond["settime"].GetUint64();
			const rj::Value& jItems = jCond["items"];
			for (auto& m : jItems.GetObject())
			{
				const char* stdCode = m.name.GetString();
				const char* ruleTag = _engine->get_hot_mgr()->getRuleTag(stdCode);
				if (strlen(ruleTag) == 0 && _engine->get_contract_info(stdCode) == NULL)
				{
					log_info("{} not exists or expired, condition ignored", stdCode);
					continue;
				}

				const rj::Value& cListItem = m.value;

				CondList& condList = _condtions[stdCode];

				for(auto& cItem : cListItem.GetArray())
				{
					CondEntrust condInfo;
					strcpy(condInfo._code, stdCode);
					strcpy(condInfo._usertag, cItem["usertag"].GetString());

					condInfo._field = (WTSCompareField)cItem["field"].GetUint();
					condInfo._alg = (WTSCompareType)cItem["alg"].GetUint();
					condInfo._target = cItem["target"].GetDouble();
					condInfo._qty = cItem["qty"].GetDouble();
					condInfo._action = (char)cItem["action"].GetUint();

					condList.emplace_back(condInfo);

					log_info("{} condition recovered, {} {}, condition: newprice {} {}",
						stdCode, ACTION_NAMES[condInfo._action], condInfo._qty, CMP_ALG_NAMES[condInfo._alg], condInfo._target);
					count++;
				}
			}

			log_info("{} conditions recovered, setup time: {}", count, _last_cond_min);
		}
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

	if (root.HasMember("utils"))
	{
		//读取杂项
		const rj::Value& jUtils = root["utils"];
		if (!jUtils.IsNull() && jUtils.IsObject())
		{
			_last_barno = jUtils["lastbarno"].GetUint();
		}
	}
}

/**
 * @brief 保存策略数据到JSON文件
 * @param flag 数据保存标志，按位控制不同模块的保存
 * @details 将策略的持仓、资金、信号等数据写入[name].json
 * @note 策略退出或定期保存时调用，保证数据持久化
 */
void CtaStraBaseCtx::save_data(uint32_t flag /* = 0xFFFFFFFF */)
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
				dItem.AddMember("openbarno", dInfo._open_barno, allocator);

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

		for (auto& m:_sig_map)
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

	{//条件单保存
		rj::Value jCond(rj::kObjectType);
		rj::Value jItems(rj::kObjectType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		for (auto it = _condtions.begin(); it != _condtions.end(); it++)
		{
			const char* code = it->first.c_str();
			const CondList& condList = it->second;

			rj::Value cArray(rj::kArrayType);
			for(auto& condInfo : condList)
			{
				rj::Value cItem(rj::kObjectType);
				cItem.AddMember("code", rj::Value(code, allocator), allocator);
				cItem.AddMember("usertag", rj::Value(condInfo._usertag, allocator), allocator);

				cItem.AddMember("field", (uint32_t)condInfo._field, allocator);
				cItem.AddMember("alg", (uint32_t)condInfo._alg, allocator);
				cItem.AddMember("target", condInfo._target, allocator);
				cItem.AddMember("qty", condInfo._qty, allocator);
				cItem.AddMember("action", (uint32_t)condInfo._action, allocator);

				cArray.PushBack(cItem, allocator);
			}

			jItems.AddMember(rj::Value(code, allocator), cArray, allocator);
		}
		jCond.AddMember("settime", _last_cond_min, allocator);
		jCond.AddMember("items", jItems, allocator);

		root.AddMember("conditions", jCond, allocator);
	}

	{//杂项保存
		rj::Value jUtils(rj::kObjectType);

		rj::Document::AllocatorType &allocator = root.GetAllocator();

		jUtils.AddMember("lastbarno", _last_barno, allocator);

		root.AddMember("utils", jUtils, allocator);
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
 * @brief K线周期回调函数
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1、m5、d1等
 * @param times 周期倍数，用于复合周期，如m1周期时times=2表示2分钟
 * @param newBar 新K线数据结构指针
 * @details 当新的K线周期到来时触发，用于处理K线周期事件
 * @note 策略可以在此函数中实现周期性的数据分析和交易决策
 */
void CtaStraBaseCtx::on_bar(const char* stdCode, const char* period, uint32_t times, WTSBarStruct* newBar)
{
	if (newBar == NULL)
		return;

	thread_local static char realPeriod[8] = { 0 };
	fmtutil::format_to(realPeriod, "{}{}", period, times);

	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, realPeriod);

	KlineTag& tag = _kline_tags[key];
	tag._closed = true;

	if(tag._notify)
		on_bar_close(stdCode, realPeriod, newBar);

	if(key == _main_key)
		log_debug("Main KBars {} closed", key);
}

/**
 * @brief 策略初始化函数
 * @details 完成策略初始化所需的各项工作，包括初始化日志输出、加载历史数据和用户数据
 * @note 在策略实例创建后自动调用，是策略生命周期的入口点
 */
void CtaStraBaseCtx::on_init()
{
	init_outputs();

	//读取数据
	load_data();

	//加载用户数据
	load_userdata();
}

/**
 * @brief 导出图表信息到JSON
 * @details 将策略中设置的K线、指标、线条等图表元素导出为JSON格式
 * @note 用于图表展示和策略可视化分析
 */
void CtaStraBaseCtx::dump_chart_info()
{
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();

	rj::Value klineItem(rj::kObjectType);
	if (_chart_code.empty())
	{
		//如果没有设置主K线，就用主K线落地
		klineItem.AddMember("code", rj::Value(_main_code.c_str(), allocator), allocator);
		klineItem.AddMember("period", rj::Value(_main_period.c_str(), allocator), allocator);
	}
	else
	{
		klineItem.AddMember("code", rj::Value(_chart_code.c_str(), allocator), allocator);
		klineItem.AddMember("period", rj::Value(_chart_period.c_str(), allocator), allocator);
	}

	root.AddMember("kline", klineItem, allocator);

	if (!_chart_indice.empty())
	{
		rj::Value jIndice(rj::kArrayType);
		for (const auto& v : _chart_indice)
		{
			const ChartIndex& cIndex = v.second;
			rj::Value jIndex(rj::kObjectType);
			jIndex.AddMember("name", rj::Value(cIndex._name.c_str(), allocator), allocator);
			jIndex.AddMember("index_type", cIndex._indexType, allocator);

			rj::Value jLines(rj::kArrayType);
			for (const auto& v2 : cIndex._lines)
			{
				const ChartLine& cLine = v2.second;
				rj::Value jLine(rj::kObjectType);
				jLine.AddMember("name", rj::Value(cLine._name.c_str(), allocator), allocator);
				jLine.AddMember("line_type", cLine._lineType, allocator);

				jLines.PushBack(jLine, allocator);
			}

			jIndex.AddMember("lines", jLines, allocator);

			rj::Value jBaseLines(rj::kObjectType);
			for (const auto& v3 : cIndex._base_lines)
			{
				jBaseLines.AddMember(rj::Value(v3.first.c_str(), allocator), rj::Value(v3.second), allocator);
			}

			jIndex.AddMember("baselines", jBaseLines, allocator);

			jIndice.PushBack(jIndex, allocator);
		}

		root.AddMember("index", jIndice, allocator);
	}

	std::string folder = WtHelper::getOutputDir();
	folder += _name;
	folder += "/";

	if (!StdFile::exists(folder.c_str()))
		boost::filesystem::create_directories(folder.c_str());

	std::string filename = folder;
	filename += "rtchart.json";

	rj::StringBuffer sb;
	rj::PrettyWriter<rj::StringBuffer> writer(sb);
	root.Accept(writer);
	StdFile::write_file_content(filename.c_str(), sb.GetString());
}

/**
 * @brief 更新合约浮动盈亏
 * @param stdCode 标准化合约代码
 * @param price 最新价格
 * @details 根据当前行情更新指定合约的浮动盈亏，并跟踪最大盈利、最大亏损、最高价和最低价
 * @note 在每次行情更新时调用，会同时更新策略总的浮动盈亏
 */
void CtaStraBaseCtx::update_dyn_profit(const char* stdCode, double price)
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
	for(auto& v : _pos_map)
	{
		const PosInfo& pInfo = v.second;
		total_dynprofit += pInfo._dynprofit;
	}

	_fund_info._total_dynprofit = total_dynprofit;
}

/**
 * @brief Tick数据回调函数
 * @param stdCode 标准化合约代码
 * @param newTick 新到的Tick数据
 * @param bEmitStrategy 是否触发策略回调
 * @details 处理实时行情数据，包括更新价格缓存、检查信号触发、更新浮动盈亏、检查条件单
 * @note 每次收到新的Tick数据时触发，是策略实时响应市场的主要入口
 */
void CtaStraBaseCtx::on_tick(const char* stdCode, WTSTickData* newTick, bool bEmitStrategy /* = true */)
{
	_price_map[stdCode] = newTick->price();

	//先检查是否要信号要触发
	{
		auto it = _sig_map.find(stdCode);
		if(it != _sig_map.end())
		{
			WTSSessionInfo* sInfo = _engine->get_session_info(stdCode, true);
			if (sInfo->isInTradingTime(_engine->get_raw_time(), true))
			{
				const SigInfo sInfo = it->second;
				//只有当信号类型不为0，即bar内信号或者条件单触发信号时，且信号没有触发过
				do_set_position(stdCode, sInfo._volume, sInfo._usertag.c_str(), (sInfo._sigtype != 0 && !sInfo._triggered));
				_sig_map.erase(it);

				//如果是条件单触发，则回调on_condition_triggered
				if(sInfo._sigtype == 2)
					on_condition_triggered(stdCode, sInfo._volume, newTick->price(), sInfo._usertag.c_str());
			}
			
		}
	}

	//更新浮动盈亏
	update_dyn_profit(stdCode, newTick->price());

	//////////////////////////////////////////////////////////////////////////
	//检查条件单
	if(!_condtions.empty())
	{
		auto it = _condtions.find(stdCode);
		if (it == _condtions.end())
			return;

		const CondList& condList = it->second;
		for (const CondEntrust& entrust : condList)
		{
			double curPrice = newTick->price();

			bool isMatched = false;
			switch (entrust._alg)
			{
			case WCT_Equal:
				isMatched = decimal::eq(curPrice, entrust._target);
				break;
			case WCT_Larger:
				isMatched = decimal::gt(curPrice, entrust._target);
				break;
			case WCT_LargerOrEqual:
				isMatched = decimal::ge(curPrice, entrust._target);
				break;
			case WCT_Smaller:
				isMatched = decimal::lt(curPrice, entrust._target);
				break;
			case WCT_SmallerOrEqual:
				isMatched = decimal::le(curPrice, entrust._target);
				break;
			default:
				break;
			}

			if (isMatched)
			{
				log_info("Condition triggered[newprice {}{} targetprice {}], instrument: {}, {} {}",
					curPrice, CMP_ALG_NAMES[entrust._alg], entrust._target, stdCode, ACTION_NAMES[entrust._action], entrust._qty);

				switch (entrust._action)
				{
				case COND_ACTION_OL:
				{
					double curQty = stra_get_position(stdCode);
					double desQty = 0;
					if (decimal::lt(curQty, 0))
						desQty = entrust._qty;
					else
						desQty = curQty + entrust._qty;

					append_signal(stdCode, desQty, entrust._usertag, 2);
				}
				break;
				case COND_ACTION_CL:
				{
					double curQty = stra_get_position(stdCode);
					if (decimal::gt(curQty, 0))
					{
						double maxQty = min(curQty, entrust._qty);
						double desQty = curQty - maxQty;
						append_signal(stdCode, desQty, entrust._usertag, 2);
					}
				}
				break;
				case COND_ACTION_OS:
				{
					double curQty = stra_get_position(stdCode);
					double desQty = 0;
					if (decimal::gt(curQty, 0))
						desQty = -entrust._qty;
					else
						desQty = curQty - entrust._qty;

					append_signal(stdCode, desQty, entrust._usertag, 2);
				}
				break;
				case COND_ACTION_CS:
				{
					double curQty = stra_get_position(stdCode);
					if (decimal::lt(curQty, 0))
					{
						double maxQty = min(abs(curQty), entrust._qty);
						double desQty = curQty + maxQty;
						append_signal(stdCode, desQty, entrust._usertag, 2);
					}
				}
				break;
				case COND_ACTION_SP: 
				{
					append_signal(stdCode, entrust._qty, entrust._usertag, 2);
				}
				break;
				default: break;
				}

				//同一个bar设置针对同一个合约的条件单, 只可能触发一条
				//所以这里直接清理掉即可
				_condtions.erase(it);
				break;
			}
		}
	}

	if (bEmitStrategy)
		on_tick_updated(stdCode, newTick);

	if(_ud_modified)
	{
		save_userdata();
		_ud_modified = false;
	}
}

/**
 * @brief 策略定时调度函数
 * @param curDate 当前日期，格式YYYYMMDD
 * @param curTime 当前时间，格式HHMMSS
 * @return 是否成功触发策略计算
 * @details 根据K线周期定时触发策略计算，处理主周期更新和条件单清理
 * @note 策略引擎定时调用，是策略周期性计算的入口点
 */
bool CtaStraBaseCtx::on_schedule(uint32_t curDate, uint32_t curTime)
{
	_is_in_schedule = true;//开始调度, 修改标记

	//主要用于保存浮动盈亏的
	save_data();

	bool isMainUdt = false;
	bool emmited = false;

	for (auto it = _kline_tags.begin(); it != _kline_tags.end(); it++)
	{
		const char* key = it->first.c_str();
		KlineTag& marker = (KlineTag&)it->second;

		auto idx = StrUtil::findFirst(key, '#');

		std::string stdCode(key, idx);

		if (key == _main_key)
		{
			if (marker._closed)
			{
				isMainUdt = true;
				marker._closed = false;
			}
			else
			{
				isMainUdt = false;
				break;
			}
		}

		WTSSessionInfo* sInfo = _engine->get_session_info(stdCode.c_str(), true);

		if (isMainUdt || _kline_tags.empty())
		{	
			TimeUtils::Ticker ticker;

			uint32_t offTime = sInfo->offsetTime(curTime, true);
			if(offTime <= sInfo->getCloseTime(true))
			{
				_condtions.clear();
				on_calculate(curDate, curTime);
				log_debug("Strategy {} scheduled @ {}", _name, curTime);
				emmited = true;

				_emit_times++;
				_total_calc_time += ticker.micro_seconds();

				if (_emit_times % 20 == 0)
				{
					log_info("Strategy has been scheduled {} times, totally taking {} us, {:.3f} us each time",
						_emit_times, _total_calc_time, _total_calc_time*1.0 / _emit_times);
				}

				if (_ud_modified)
				{
					save_userdata();
					_ud_modified = false;
				}

				if(!_condtions.empty())
				{
					_last_cond_min = (uint64_t)curDate * 10000 + curTime;
					save_data();
				}
			}
			else
			{
				log_info("{} not in trading time, schedule canceled", curTime);
			}
			break;
		}
	}

	_is_in_schedule = false;//调度结束, 修改标记
	_last_barno++;	//每次计算，barno加1
	return emmited;
}

/**
 * @brief 交易日开始事件回调
 * @param uTDate 交易日期，格式YYYYMMDD
 * @details 在每个交易日开始时触发，清理过期的冻结持仓
 * @note 用于处理跨交易日的持仓状态更新，确保冻结仓位在新交易日释放
 */
void CtaStraBaseCtx::on_session_begin(uint32_t uTDate)
{
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if(pInfo._frozen_date!=0 && pInfo._frozen_date < uTDate && !decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of %s frozen on {} released on {}", pInfo._frozen, stdCode, pInfo._frozen_date, uTDate);

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
 * @brief 枚举当前策略所有持仓
 * @param cb 持仓处理回调函数，用于接收并处理每个持仓信息
 * @param bForExecute 是否为执行模式，如果为true则会标记信号为已触发
 * @details 遍历所有的实际持仓和信号持仓，并将每个持仓信息通过回调函数返回
 * @note 使用互斥锁保证线程安全，避免组合轧差同步与信号同时触发导致的问题
 */
void CtaStraBaseCtx::enum_position(FuncEnumCtaPosCallBack cb, bool bForExecute /* = false */)
{
	/* By HeJ @ 2023.03.14
	 * 读取理论持仓时，要加个锁，避免出现组合轧差同步与信号同时触发，导致的反复发单和信号覆盖
	 */
	std::unordered_map<std::string, double> desPos;
	{
		SpinLock lock(_mutex);		
		for (auto& it : _pos_map)
		{
			const char* stdCode = it.first.c_str();
			const PosInfo& pInfo = it.second;
			//cb(stdCode, pInfo._volume);
			desPos[stdCode] = pInfo._volume;
		}

		for (auto& sit : _sig_map)
		{
			const char* stdCode = sit.first.c_str();
			SigInfo& sInfo = (SigInfo&)sit.second;
			desPos[stdCode] = sInfo._volume;
			if (bForExecute)
				sInfo._triggered = true;
		}
	}	

	for(auto v:desPos)
	{
		cb(v.first.c_str(), v.second);
	}
}

/**
 * @brief 交易日结束事件回调
 * @param uTDate 交易日期，格式YYYYMMDD
 * @details 在每个交易日结束时触发，记录当日持仓和资金状况，保存策略数据
 * @note 用于日结算和数据持久化，确保策略状态正确保存
 */
void CtaStraBaseCtx::on_session_end(uint32_t uTDate)
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

		if(_pos_logs)
			_pos_logs->write_file(fmt::format("{},{},{},{:.2f},{:.2f}\n", curDate, stdCode,
				pInfo._volume, pInfo._closeprofit, pInfo._dynprofit));
	}

	//这里要把当日结算的数据写到日志文件里
	//而且这里回测和实盘写法不同, 先留着, 后面来做
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

/**
 * @brief 获取指定合约的条件单列表
 * @param stdCode 标准化合约代码
 * @return 返回指定合约的条件单列表引用
 * @details 根据合约代码获取相应的条件单列表，如果不存在则创建新的空列表
 * @note 内部函数，用于条件单管理，被各个交易接口调用
 */
CondList& CtaStraBaseCtx::get_cond_entrusts(const char* stdCode)
{
	CondList& ce = _condtions[stdCode];
	return ce;
}

//////////////////////////////////////////////////////////////////////////
//策略接口
/**
 * @brief 策略开多接口
 * @param stdCode 标准化合约代码
 * @param qty 开仓数量
 * @param userTag 用户自定义标签
 * @param limitprice 限价，当行情价格低于此价格时触发，默认0表示市价单
 * @param stopprice 止损价，当行情价格高于此价格时触发，默认0表示市价单
 * @details 策略开多入场接口，支持市价单和条件单两种模式
 * @note 如果当前持空，则会先平空再开多；如果已有多仓，则会增加多仓
 */
void CtaStraBaseCtx::stra_enter_long(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	_engine->sub_tick(id(), stdCode);
	
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式, 则直接触发
	{
		double curQty = stra_get_position(stdCode);
		if (decimal::lt(curQty, 0))
		{
			//当前持仓小于0,逻辑是反手到qty,所以设置信号目标仓位为qty
			append_signal(stdCode, qty, userTag, _is_in_schedule ? 0 : 1);
		}
		else
		{
			//当前持仓大于等于0,则要增加多仓qty
			append_signal(stdCode, curQty + qty, userTag, _is_in_schedule ? 0 : 1);
		}
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		wt_strcpy(entrust._code, stdCode);
		wt_strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if(!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_SmallerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_LargerOrEqual;
		}
		
		entrust._action = COND_ACTION_OL;

		condList.emplace_back(entrust);
	}
}

/**
 * @brief 策略开空接口
 * @param stdCode 标准化合约代码
 * @param qty 开仓数量
 * @param userTag 用户自定义标签
 * @param limitprice 限价，当行情价格高于此价格时触发，默认0表示市价单
 * @param stopprice 止损价，当行情价格低于此价格时触发，默认0表示市价单
 * @details 策略开空入场接口，支持市价单和条件单两种模式
 * @note 需要先检查合约是否支持做空；如果当前持多，则会先平多再开空；如果已有空仓，则会增加空仓
 */
void CtaStraBaseCtx::stra_enter_short(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	if (!commInfo->canShort())
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	_engine->sub_tick(id(), stdCode);
	
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式, 则直接触发
	{
		double curQty = stra_get_position(stdCode);
		if (decimal::gt(curQty, 0))
		{
			//当前仓位大于0,逻辑是反手到qty手,所以设置信号目标仓位为-qty手
			append_signal(stdCode, -qty, userTag, _is_in_schedule ? 0 : 1);
		}
		else
		{
			//当前仓位小于等于0,则是追加空方手数
			append_signal(stdCode, curQty - qty, userTag, _is_in_schedule ? 0 : 1);
		}
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		wt_strcpy(entrust._code, stdCode);
		wt_strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_OS;

		condList.emplace_back(entrust);
	}
}

/**
 * @brief 策略平多接口
 * @param stdCode 标准化合约代码
 * @param qty 平仓数量
 * @param userTag 用户自定义标签
 * @param limitprice 限价，当行情价格低于此价格时触发，默认0表示市价单
 * @param stopprice 止损价，当行情价格高于此价格时触发，默认0表示市价单
 * @details 策略平多出场接口，支持市价单和条件单两种模式
 * @note 会自动判断是否是收盘时段，收盘时会读取全部持仓，否则只读取可平仓位
 */
void CtaStraBaseCtx::stra_exit_long(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	WTSSessionInfo* sInfo = commInfo->getSessionInfo();
	uint32_t offTime = sInfo->offsetTime(_engine->get_min_time(), true);
	bool isLastBarOfDay = (offTime == sInfo->getCloseTime(true));

	//读取可平持仓,如果是收盘那根bar，则直接读取全部持仓
	double curQty = stra_get_position(stdCode, !isLastBarOfDay);
	if (decimal::le(curQty, 0))
		return;
	
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式, 则直接触发
	{
		double maxQty = min(curQty, qty);
		double totalQty = stra_get_position(stdCode, false);
		append_signal(stdCode, totalQty - maxQty, userTag, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		wt_strcpy(entrust._code, stdCode);
		wt_strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_CL;

		condList.emplace_back(entrust);
	}
}

/**
 * @brief 策略平空接口
 * @param stdCode 标准化合约代码
 * @param qty 平仓数量
 * @param userTag 用户自定义标签
 * @param limitprice 限价，当行情价格高于此价格时触发，默认0表示市价单
 * @param stopprice 止损价，当行情价格低于此价格时触发，默认0表示市价单
 * @details 策略平空出场接口，支持市价单和条件单两种模式
 * @note 会自动判断是否是收盘时段，收盘时会读取全部持仓，否则只读取可平仓位
 */
void CtaStraBaseCtx::stra_exit_short(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice, double stopprice)
{
	WTSCommodityInfo* commInfo = _engine->get_commodity_info(stdCode);
	if (commInfo == NULL)
	{
		log_error("Cannot find corresponding commodity info of {}", stdCode);
		return;
	}

	if (!commInfo->canShort())
	{
		log_error("Cannot short on {}", stdCode);
		return;
	}

	double curQty = stra_get_position(stdCode);
	//如果持仓是多,则不需要执行退出空头的逻辑了
	if (decimal::ge(curQty, 0))
		return;
	
	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式, 则直接触发
	{
		double maxQty = min(abs(curQty), qty);
		append_signal(stdCode, curQty + maxQty, userTag, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		CondEntrust entrust;
		wt_strcpy(entrust._code, stdCode);
		wt_strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = WCT_SmallerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = WCT_LargerOrEqual;
		}

		entrust._action = COND_ACTION_CS;
		
		condList.emplace_back(entrust);
	}
}

/**
 * @brief 获取合约最新价格
 * @param stdCode 标准化合约代码
 * @return 合约最新价格，如果不存在则返回0
 * @details 先从价格缓存中获取，如果缓存中不存在则从引擎中获取
 * @note 策略计算和交易决策时经常需要查询最新价格
 */
double CtaStraBaseCtx::stra_get_price(const char* stdCode)
{
	auto it = _price_map.find(stdCode);
	if (it != _price_map.end())
		return it->second;

	if (_engine)
		return _engine->get_cur_price(stdCode);
	
	return 0.0;
}

double CtaStraBaseCtx::stra_get_day_price(const char* stdCode, int flag /* = 0 */)
{
	if (_engine)
		return _engine->get_day_price(stdCode, flag);

	return 0.0;
}

/**
 * @brief 设置目标仓位
 * @param stdCode 标准化合约代码
 * @param qty 目标仓位量，正数表示多头，负数表示空头，0表示平仓
 * @param userTag 用户自定义标签
 * @param limitprice 限价，默认0表示市价单
 * @param stopprice 止损价，默认0表示市价单
 * @details 设置策略目标仓位，对外的通用持仓设置接口，自动处理多空切换、开平逻辑
 * @note 支持市价单和条件单两种模式，内部调用append_signal或者添加条件单
 */
void CtaStraBaseCtx::stra_set_position(const char* stdCode, double qty, const char* userTag /* = "" */, double limitprice /* = 0.0 */, double stopprice /* = 0.0 */)
{
	_engine->sub_tick(id(), stdCode);

	if (decimal::eq(limitprice, 0.0) && decimal::eq(stopprice, 0.0))	//如果不是动态下单模式, 则直接触发
	{
		append_signal(stdCode, qty, userTag, _is_in_schedule ? 0 : 1);
	}
	else
	{
		CondList& condList = get_cond_entrusts(stdCode);

		double curVol = stra_get_position(stdCode);
		//如果目标仓位和当前仓位是一致的，则不再设置条件单
		if (decimal::eq(curVol, qty))
			return;

		//根据目标仓位和当前仓位,判断是买还是卖
		bool isBuy = decimal::gt(qty, curVol);

		CondEntrust entrust;
		wt_strcpy(entrust._code, stdCode);
		wt_strcpy(entrust._usertag, userTag);

		entrust._qty = qty;
		entrust._field = WCF_NEWPRICE;
		if (!decimal::eq(limitprice))
		{
			entrust._target = limitprice;
			entrust._alg = isBuy ? WCT_SmallerOrEqual : WCT_LargerOrEqual;
		}
		else if (!decimal::eq(stopprice))
		{
			entrust._target = stopprice;
			entrust._alg = isBuy ? WCT_LargerOrEqual : WCT_SmallerOrEqual;
		}

		entrust._action = COND_ACTION_SP;

		condList.emplace_back(entrust);
	}
}

/**
 * @brief 添加交易信号
 * @param stdCode 标准化合约代码
 * @param qty 目标仓位量，正数表示多头，负数表示空头
 * @param userTag 用户自定义标签
 * @param sigType 信号类型（0-定时计算信号，1-实时行情触发信号，2-条件单触发信号）
 * @details 生成交易信号并记录到信号映射表中，同时写入信号日志
 * @note 内部函数，由各种交易接口调用，用于生成统一的交易信号
 */
void CtaStraBaseCtx::append_signal(const char* stdCode, double qty, const char* userTag /* = "" */, uint32_t sigType)
{
	double curPx = _price_map[stdCode];

	SigInfo& sInfo = _sig_map[stdCode];
	sInfo._volume = qty;
	sInfo._sigprice = curPx;
	sInfo._usertag = userTag;
	sInfo._gentime = (uint64_t)_engine->get_date() * 1000000000 + (uint64_t)_engine->get_raw_time() * 100000 + _engine->get_secs();
	sInfo._sigtype = sigType;

	log_signal(stdCode, qty, curPx, sInfo._gentime, userTag);

	save_data();
}

/**
 * @brief 设置策略持仓量
 * @param stdCode 标准化合约代码
 * @param qty 目标持仓量，正数表示多头，负数表示空头
 * @param userTag 用户自定义标签，用于跟踪交易来源
 * @param bFireAtOnce 是否立即触发仓位变更通知
 * @details 核心交易执行函数，计算仓位差值并生成交易明细、更新资金信息
 * @note 内部使用互斥锁保证线程安全，处理开仓、平仓、反手等多种交易情况
 */
void CtaStraBaseCtx::do_set_position(const char* stdCode, double qty, const char* userTag /* = "" */, bool bFireAtOnce /* = false */)
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
	/* By HeJ @ 2023.03.14
	 * 设置理论持仓时，要加个锁，避免出现组合轧差同步与信号同时触发，导致的反复发单和信号覆盖
	 */
	SpinLock lock(_mutex);
	bool isBuy = decimal::gt(diff, 0.0);
	if (decimal::gt(pInfo._volume*diff, 0))
	{//当前持仓和仓位变化方向一致, 增加一条明细, 增加数量即可
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
		dInfo._open_barno = _last_barno;
		wt_strcpy(dInfo._opentag, userTag);
		pInfo._details.emplace_back(dInfo);
		pInfo._last_entertime = curTm;

		//double fee = _engine->calc_fee(stdCode, trdPx, abs(diff), 0);
		double fee = commInfo->calcFee(trdPx, abs(diff), 0);
		_fund_info._total_fees += fee;
		log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(diff), userTag, fee, _last_barno);
	}
	else
	{//持仓方向和仓位变化方向不一致, 需要平仓
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

			if (decimal::eq(dInfo._volume, 0))
				count++;

			//计算平仓盈亏
			double profit = (trdPx - dInfo._price) * maxQty * commInfo->getVolScale();
			if (!dInfo._long)
				profit *= -1;
			pInfo._closeprofit += profit;

			//浮盈也要做等比缩放
			pInfo._dynprofit = pInfo._dynprofit*dInfo._volume / (dInfo._volume + maxQty);
			pInfo._last_exittime = curTm;
			_fund_info._total_profit += profit;

			//计算手续费
			//double fee = _engine->calc_fee(stdCode, trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			double fee = commInfo->calcFee(trdPx, maxQty, dInfo._opentdate == curTDate ? 2 : 1);
			_fund_info._total_fees += fee;

			//这里写平仓记录
			log_close(stdCode, dInfo._long, dInfo._opentime, dInfo._price, curTm, trdPx, maxQty, profit, pInfo._closeprofit, dInfo._opentag, userTag, dInfo._open_barno, _last_barno);

			//这里写成交记录
			log_trade(stdCode, dInfo._long, false, curTm, trdPx, maxQty, userTag, fee, _last_barno);

			if (decimal::eq(left,0))
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
		if (decimal::gt(left, 0))
		{
			left = left * qty / abs(qty);

			//如果T+1，则冻结仓位要增加
			if (commInfo->isT1())
			{
				pInfo._frozen += left;
				pInfo._frozen_date = curTDate;
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
			dInfo._open_barno = _last_barno;
			wt_strcpy(dInfo._opentag, userTag);
			pInfo._details.emplace_back(dInfo);
			pInfo._last_entertime = curTm;

			//这里还需要写一笔成交记录
			double fee = commInfo->calcFee(trdPx, abs(left), 0);
			_fund_info._total_fees += fee;
			log_trade(stdCode, dInfo._long, true, curTm, trdPx, abs(left), userTag, fee, _last_barno);
		}
	}


	//存储数据
	save_data();

	if (bFireAtOnce)	//如果是条件单触发, 则向引擎提交变化量
	{
		_engine->handle_pos_change(_name.c_str(), stdCode, diff);
	}
}

/**
 * @brief 获取合约历史K线数据
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1、5m、d1等
 * @param count 请求的K线数量
 * @param isMain 是否为主K线，默认为false
 * @return 返回K线数据切片指针，如果得不到则返回NULL
 * @details 策略获取历史K线数据的主要接口，每个策略只能有一个主K线设置
 * @note 主K线会影响策略的计算周期，只有当主K线更新时策略才会执行
 */
WTSKlineSlice* CtaStraBaseCtx::stra_get_bars(const char* stdCode, const char* period, uint32_t count, bool isMain /* = false */)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	if (isMain)
	{
		if (_main_key.empty())
		{
			_main_key = key;
			log_debug("Main KBars confirmed: {}", key);
		}
		else if (_main_key != key)
		{
			log_error("Main KBars already confirmed");
			return NULL;
		}

		/*
		 *	By Wesley @ 2022.12.07
		 */
		_main_code = stdCode;
		_main_period = period;
	}

	thread_local static char basePeriod[2] = { 0 };
	basePeriod[0] = period[0];
	uint32_t times = 1;
	if (strlen(period) > 1)
		times = strtoul(period + 1, NULL, 10);

	WTSKlineSlice* kline = _engine->get_kline_slice(_context_id, stdCode, basePeriod, count, times);
	if(kline)
	{
		//如果K线获取不到,说明也不会有闭合事件发生,所以不更新本地标记
		bool isFirst = (_kline_tags.find(key) == _kline_tags.end());	//如果没有保存标记,说明是第一次拉取该K线
		KlineTag& tag = _kline_tags[key];
		tag._closed = false;

		double lastClose = kline->at(-1)->close;
		_price_map[stdCode] = lastClose;

		if(isMain && isFirst && !_condtions.empty())
		{
			//如果是第一次拉取主K线,则检查条件单触发时间
			bool isDay = basePeriod[0] == 'd';
			uint64_t lastBartime = isDay ? kline->at(-1)->date : kline->at(-1)->time;
			if(!isDay)
				lastBartime += 199000000000;

			//如果最后一条已闭合的K线的时间大于条件单设置时间，说明条件单已经过期了，则需要清理
			if(lastBartime > _last_cond_min)
			{
				log_info("Conditions expired, setup time: {}, time of last bar of main kbars: {}, all cleared", _last_cond_min, lastBartime);
				_condtions.clear();
			}
		}

		_engine->sub_tick(id(), stdCode);

		//如果是主K线，并且最后一根bar的编号为0
		//则将最后一根bar的编号设置为主K线的长度
		if(isMain && _last_barno == 0)
		{
			_last_barno = kline->size();
		}
	}

	return kline;
}

/**
 * @brief 获取合约Tick数据
 * @param stdCode 标准化合约代码
 * @param count 请求的Tick数量
 * @return 返回Tick数据切片指针，如果得不到则返回NULL
 * @details 获取指定合约的历史Tick数据，同时自动订阅该合约的行情
 * @note 当策略需要基于Tick级别数据进行决策时使用
 */
WTSTickSlice* CtaStraBaseCtx::stra_get_ticks(const char* stdCode, uint32_t count)
{
	WTSTickSlice* ret = _engine->get_tick_slice(_context_id, stdCode, count);
	if (ret)
		_engine->sub_tick(id(), stdCode);

	return ret;
}

/**
 * @brief 获取合约最新的Tick数据
 * @param stdCode 标准化合约代码
 * @return 返回Tick数据指针，如果得不到则返回NULL
 * @details 从引擎中获取指定合约的最新Tick数据
 * @note 这个函数常用于实时获取行情数据，与stra_get_ticks不同，只返回最新一条
 */
WTSTickData* CtaStraBaseCtx::stra_get_last_tick(const char* stdCode)
{
	return _engine->get_last_tick(_context_id, stdCode);
}

/**
 * @brief 主动订阅合约的Tick数据
 * @param code 标准化合约代码
 * @details 将合约编码添加到订阅列表中并通知引擎订阅相应的Tick数据
 * @note 当策略需要实时监控某些合约的行情，但不一定要交易该合约时使用
 */
void CtaStraBaseCtx::stra_sub_ticks(const char* code)
{
	/*
	 *	By Wesley @ 2022.03.01
	 *	主动订阅tick会在本地记一下
	 *	tick数据回调的时候先检查一下
	 */
	_tick_subs.insert(code);

	_engine->sub_tick(_context_id, code);
	log_info("Market data subscribed: {}", code);
}

/**
 * @brief 订阅K线事件通知
 * @param stdCode 标准化合约代码
 * @param period K线周期，如m1、5m、d1等
 * @details 将指定合约和周期的K线标记为需要通知的状态
 * @note 当策略需要在某些周期的K线更新时收到通知进行响应时使用
 */
void CtaStraBaseCtx::stra_sub_bar_events(const char* stdCode, const char* period)
{
	thread_local static char key[64] = { 0 };
	fmtutil::format_to(key, "{}#{}", stdCode, period);

	KlineTag& tag = _kline_tags[key];
	tag._notify = true;
}

/**
 * @brief 获取合约品种信息
 * @param stdCode 标准化合约代码
 * @return 返回品种信息指针，如果不存在则返回NULL
 * @details 从引擎中获取指定合约的品种信息，包含手续费、合约乘数、交易时段等
 * @note 策略计算手续费、判断交易时间、确定是否可以做空时使用
 */
WTSCommodityInfo* CtaStraBaseCtx::stra_get_comminfo(const char* stdCode)
{
	return _engine->get_commodity_info(stdCode);
}

/**
 * @brief 获取合约原始代码
 * @param stdCode 标准化合约代码
 * @return 返回合约原始代码字符串
 * @details 将标准化合约代码转换为原始交易所代码
 * @note 当需要将内部的标准化代码转换为交易所原始代码时使用
 */
std::string CtaStraBaseCtx::stra_get_rawcode(const char* stdCode)
{
	return _engine->get_rawcode(stdCode);
}

/**
 * @brief 获取当前交易日期
 * @return 返回交易日期数值，格式YYYYMMDD
 * @details 从引擎中获取当前的交易日期
 * @note 交易日期与自然日期不同，对于夜盘品种，晚上的交易属于下一个交易日
 */
uint32_t CtaStraBaseCtx::stra_get_tdate()
{
	return _engine->get_trading_date();
}

/**
 * @brief 获取当前自然日期
 * @return 返回自然日期数值，格式YYYYMMDD
 * @details 从引擎中获取当前的日历日期
 * @note 自然日期是指实际日历日期，与交易日期不同
 */
uint32_t CtaStraBaseCtx::stra_get_date()
{
	return _engine->get_date();
}

/**
 * @brief 获取当前交易时间
 * @return 返回当前时间数值，格式HHMM（小时分钟）
 * @details 从引擎中获取当前的分钟时间
 * @note 主要用于策略判断当前交易时间点，进行时间相关的交易决策
 */
uint32_t CtaStraBaseCtx::stra_get_time()
{
	return _engine->get_min_time();
}

/**
 * @brief 获取资金相关数据
 * @param flag 资金数据标记，0-总盈亏（0包含实现盈亏、浮动盈亏和费用），1-实现盈亏，2-浮动盈亏，3-手续费
 * @return 返回策略相应的资金数据
 * @details 根据传入的标记返回不同类型的资金数据信息
 * @note 常用于策略统计和风险控制，监控策略盈亏状况
 */
double CtaStraBaseCtx::stra_get_fund_data(int flag )
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
 * @brief 记录信息级别的日志
 * @param message 日志信息内容
 * @details 将信息级别的日志写入策略日志文件
 * @note 信息级别日志用于记录策略的一般性操作和状态信息
 */
void CtaStraBaseCtx::stra_log_info(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_INFO, message);
}

/**
 * @brief 记录调试级别的日志
 * @param message 日志信息内容
 * @details 将调试级别的日志写入策略日志文件
 * @note 调试级别日志用于记录详细的运行信息，日志量较大，仅在调试时使用
 */
void CtaStraBaseCtx::stra_log_debug(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_DEBUG, message);
}

/**
 * @brief 记录警告级别的日志
 * @param message 日志信息内容
 * @details 将警告级别的日志写入策略日志文件
 * @note 警告级别日志用于记录策略中的异常情况和需要注意的问题
 */
void CtaStraBaseCtx::stra_log_warn(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_WARN, message);
}

/**
 * @brief 记录错误级别的日志
 * @param message 日志信息内容
 * @details 将错误级别的日志写入策略日志文件
 * @note 错误级别日志用于记录策略运行中的严重问题和异常，需要立即关注和处理
 */
void CtaStraBaseCtx::stra_log_error(const char* message)
{
	WTSLogger::log_dyn_raw("strategy", _name.c_str(), LL_ERROR, message);
}

/**
 * @brief 加载策略用户数据
 * @param key 数据键名
 * @param defVal 默认值，当指定键不存在时返回
 * @return 返回指定键名对应的数据值，如果不存在则返回默认值
 * @details 从策略的用户数据存储中根据键名检索数据
 * @note 用于存储和检索策略相关的自定义设置和运行状态
 */
const char* CtaStraBaseCtx::stra_load_user_data(const char* key, const char* defVal /*= ""*/)
{
	auto it = _user_datas.find(key);
	if (it != _user_datas.end())
		return it->second.c_str();

	return defVal;
}

/**
 * @brief 保存策略用户数据
 * @param key 数据键名
 * @param val 数据值
 * @details 将指定的键值对保存到策略的用户数据存储中
 * @note 保存数据时会标记数据已修改状态，策略调度完成后会自动存盘
 */
void CtaStraBaseCtx::stra_save_user_data(const char* key, const char* val)
{
	_user_datas[key] = val;
	_ud_modified = true;
}

/**
 * @brief 获取合约首次入场时间
 * @param stdCode 标准化合约代码
 * @return 返回合约首次入场时间，如果没有持仓则返回0
 * @details 获取指定合约当前持仓的首次入场时间
 * @note 可用于计算持仓时间并制定基于时间的交易策略
 */
uint64_t CtaStraBaseCtx::stra_get_first_entertime(const char* stdCode)
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
 * @brief 获取合约最近一次入场的标签
 * @param stdCode 标准化合约代码
 * @return 返回最近入场的标签字符串，如果没有持仓则返回空字符串
 * @details 获取指定合约当前持仓的最后一笔明细的入场标签
 * @note 可用于跟踪和识别不同策略信号生成的交易
 */
const char* CtaStraBaseCtx::stra_get_last_entertag(const char* stdCode)
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
 * @brief 获取合约最近一次出场时间
 * @param stdCode 标准化合约代码
 * @return 返回最近一次出场时间，如果没有持仓或未出场过则返回0
 * @details 获取指定合约的最后一次平仓时间
 * @note 可用于识别交易频率和计算交易间隔，在再入场判断中非常有用
 */
uint64_t CtaStraBaseCtx::stra_get_last_exittime(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._last_exittime;
}

/**
 * @brief 获取合约最后一次入场时间
 * @param stdCode 标准化合约代码
 * @return 返回最近一次入场时间，如果没有持仓则返回0
 * @details 获取指定合约当前持仓的最后一笔明细的开仓时间
 * @note 与stra_get_first_entertime不同，这个函数返回最后一笔明细的开仓时间，而非第一笔
 */
uint64_t CtaStraBaseCtx::stra_get_last_entertime(const char* stdCode)
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
 * @brief 获取合约最后一次入场价格
 * @param stdCode 标准化合约代码
 * @return 返回最近一次入场价格，如果没有持仓则返回0
 * @details 获取指定合约当前持仓的最后一笔明细的开仓价格
 * @note 在做出场决策时，经常需要知道开仓价格以计算收益或止损点
 */
double CtaStraBaseCtx::stra_get_last_enterprice(const char* stdCode)
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
 * @brief 获取合约持仓量
 * @param stdCode 标准化合约代码
 * @param bOnlyValid 是否只返回有效持仓（即非冻结部分），默认为false
 * @param userTag 用户自定义标签，如果非空则只返回指定标签的持仓
 * @return 持仓数量，正数表示多头，负数表示空头
 * @details 获取指定合约的持仓量，包括未触发信号的仓位和实际持仓
 * @note 当userTag非空时，bOnlyValid参数无效；对空头持仓，冻结仓位始终为0
 */
double CtaStraBaseCtx::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, const char* userTag /* = "" */)
{
	double totalPos = 0;
	auto sit = _sig_map.find(stdCode);
	if(sit != _sig_map.end())
	{
		WTSLogger::warn("{} has untouched signal, [userTag] will be ignored", stdCode);
		totalPos = sit->second._volume;
	}

	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return totalPos;

	const PosInfo& pInfo = it->second;
	totalPos = pInfo._volume;
	if (strlen(userTag) == 0)
	{
		//只有userTag为空的时候时候，才会用bOnlyValid
		if (bOnlyValid)
		{
			//这里理论上，只有多头才会进到这里
			//其他地方要保证，空头持仓的话，_frozen要为0
			return totalPos - pInfo._frozen;
		}
		else
			return totalPos;
	}
	else
	{
		for (auto it = pInfo._details.begin(); it != pInfo._details.end(); it++)
		{
			const DetailInfo& dInfo = (*it);
			if (strcmp(dInfo._opentag, userTag) != 0)
				continue;

			return dInfo._volume;
		}
	}

	return 0;
}

/**
 * @brief 获取持仓均价
 * @param stdCode 标准化合约代码
 * @return 持仓均价，无持仓时返回0
 * @details 计算指定合约的所有明细持仓的加权平均价
 * @note 必须有实际持仓才会返回有效均价，否则返回0
 */
double CtaStraBaseCtx::stra_get_position_avgpx(const char* stdCode)
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
 * @brief 获取持仓浮动盈亏
 * @param stdCode 标准化合约代码
 * @return 浮动盈亏值，无持仓或不存在时返回0
 * @details 返回指定合约的当前浮动盈亏（未实现盈亏）
 * @note 浮动盈亏基于当前行情价格计算，会随行情波动而变化
 */
double CtaStraBaseCtx::stra_get_position_profit(const char* stdCode)
{
	auto it = _pos_map.find(stdCode);
	if (it == _pos_map.end())
		return 0;

	const PosInfo& pInfo = it->second;
	return pInfo._dynprofit;
}

/**
 * @brief 获取指定标签持仓明细的入场时间
 * @param stdCode 标准化合约代码
 * @param userTag 用户自定义标签
 * @return 返回指定标签的入场时间，如果不存在则返回0
 * @details 根据用户标签查找对应的持仓明细，并返回其入场时间
 * @note 当需要跟踪特定信号生成的持仓时间时非常有用
 */
uint64_t CtaStraBaseCtx::stra_get_detail_entertime(const char* stdCode, const char* userTag)
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
 * @brief 获取指定标签持仓明细的开仓成本
 * @param stdCode 标准化合约代码
 * @param userTag 用户自定义标签
 * @return 返回指定标签的开仓成本（入场价格），如果不存在则返回0
 * @details 根据用户标签查找对应的持仓明细，并返回其开仓价格
 * @note 当需要计算特定信号生成的持仓收益率或止盈止损点时非常有用
 */
double CtaStraBaseCtx::stra_get_detail_cost(const char* stdCode, const char* userTag)
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
 * @brief 获取指定标签持仓明细的盈亏
 * @param stdCode 标准化合约代码
 * @param userTag 用户自定义标签
 * @param flag 盈亏标记，0-当前盈亏，1-最大盈利，2-最大亏损
 * @return 返回指定标签的交易盈亏，如果不存在则返回0
 * @details 根据用户标签和盈亏标记查找对应的持仓明细，并返回其盈亏情况
 * @note 可用于分析特定信号生成的持仓盈亏状况和最大回撤信息
 */
double CtaStraBaseCtx::stra_get_detail_profit(const char* stdCode, const char* userTag, int flag /* = 0 */)
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

/**
 * @brief 设置打印图表的K线信息
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @details 设置策略在输出图表时使用的合约和周期
 * @note 当需要不同于主K线的图表展示时使用，如果不调用则使用主K线
 */
void CtaStraBaseCtx::set_chart_kline(const char* stdCode, const char* period)
{
	_chart_code = stdCode;
	_chart_period = period;
}

/**
 * @brief 在图表上添加标记
 * @param price 标记的价格位置
 * @param icon 标记图标类型
 * @param tag 标记的描述文字
 * @details 将特定标记添加到图表上，常用于标记交易信号、实际交易点或者重要的分析点
 * @note 仅在定时调度期间可以添加标记，否则会报错
 */
void CtaStraBaseCtx::add_chart_mark(double price, const char* icon, const char* tag)
{
	if (!_is_in_schedule)
	{
		WTSLogger::error("Marks can be added only during schedule");
		return;
	}

	uint64_t curTime = stra_get_date();
	curTime = curTime * 10000 + stra_get_time();

	if (_mark_logs)
	{
		std::stringstream ss;
		ss << curTime << "," << price << "," << icon << "," << tag << std::endl;;
		_mark_logs->write_file(ss.str());
	}

	_engine->notify_chart_marker(curTime, _name.c_str(), price, icon, tag);
}

/**
 * @brief 注册图表指标
 * @param idxName 指标名称
 * @param indexType 指标类型
 * @details 在图表上创建一个新的指标，用于后续添加多条指标线
 * @note 必须先注册指标，才能通过register_index_line添加指标线
 */
void CtaStraBaseCtx::register_index(const char* idxName, uint32_t indexType)
{
	ChartIndex& cIndex = _chart_indice[idxName];
	cIndex._name = idxName;
	cIndex._indexType = indexType;
}

/**
 * @brief 注册指标线
 * @param idxName 指标名称，必须先用register_index注册
 * @param lineName 指标线名称
 * @param lineType 指标线类型
 * @return 注册成功返回true，失败返回false
 * @details 在已注册的指标上创建一条新的指标线
 * @note 必须先用register_index注册指标，才能添加指标线
 */
bool CtaStraBaseCtx::register_index_line(const char* idxName, const char* lineName, uint32_t lineType)
{
	auto it = _chart_indice.find(idxName);
	if (it == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = (ChartIndex&)it->second;
	ChartLine& cLine = cIndex._lines[lineName];
	cLine._name = lineName;
	cLine._lineType = lineType;
	return true;
}

/**
 * @brief 添加指标线基准值
 * @param idxName 指标名称
 * @param lineName 指标线名称
 * @param val 基准值
 * @return 添加成功返回true，失败返回false
 * @details 为指定的指标线设置一个基准值，用于绘制参考线
 * @note 通常用于绘制水平线或上下轨等静态线
 */
bool CtaStraBaseCtx::add_index_baseline(const char* idxName, const char* lineName, double val)
{
	auto it = _chart_indice.find(idxName);
	if (it == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = (ChartIndex&)it->second;
	cIndex._base_lines[lineName] = val;
	return true;
}

/**
 * @brief 设置指标线数据点值
 * @param idxName 指标名称
 * @param lineName 指标线名称
 * @param val 数据值
 * @return 设置成功返回true，失败返回false
 * @details 为指定的指标线添加当前时间点的数据值
 * @note 仅在定时调度期间可以添加指标数据，用于绘制动态指标线
 */
bool CtaStraBaseCtx::set_index_value(const char* idxName, const char* lineName, double val)
{
	if (!_is_in_schedule)
	{
		WTSLogger::error("Marks can be added only during schedule");
		return false;
	}

	auto ait = _chart_indice.find(idxName);
	if (ait == _chart_indice.end())
	{
		WTSLogger::error("Index {} not registered", idxName);
		return false;
	}

	ChartIndex& cIndex = (ChartIndex&)ait->second;
	auto bit = cIndex._lines.find(lineName);
	if (bit == cIndex._lines.end())
	{
		WTSLogger::error("Line {} of index {} not registered", lineName, idxName);
		return false;
	}

	uint64_t curTime = stra_get_date();
	curTime = curTime * 10000 + stra_get_time();

	if (_idx_logs)
	{
		std::stringstream ss;
		ss << curTime << "," << idxName << "," << lineName << "," << val << std::endl;;
		_idx_logs->write_file(ss.str());
	}

	_engine->notify_chart_index(curTime, _name.c_str(), idxName, lineName, val);

	return true;
}
