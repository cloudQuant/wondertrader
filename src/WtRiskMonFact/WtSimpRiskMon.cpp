/*!
 * \file WtSimpRiskMon.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader简单风险监控器实现文件
 * \details 该文件实现了简单风险监控器WtSimpleRiskMon的各个方法
 *          包括日内和多日回撤风控策略的具体实现
 *          用于监控交易组合的风险状况并触发风控措施
 */
#include "WtSimpRiskMon.h"

#include "../Includes/WTSRiskDef.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"
#include "../Share/fmtlib.h"

extern const char* FACT_NAME;

/**
 * @brief 获取风控模块名称
 * @return const char* 返回风控模块名称"WtSimpleRiskMon"
 * @details 该方法用于获取风控模块的唯一标识名称，方便上层应用识别
 */
const char* WtSimpleRiskMon::getName()
{
	return "WtSimpleRiskMon";
}

/**
 * @brief 获取风控工厂名称
 * @return const char* 返回风控工厂名称
 * @details 该方法返回所属的风控工厂名称，用于工厂模式下的对象创建
 */
const char* WtSimpleRiskMon::getFactName()
{
	return FACT_NAME;
}

/**
 * @brief 初始化风险监控器
 * @param ctx 交易组合上下文指针，提供资金、持仓等信息
 * @param cfg 配置项，包含风控参数
 * @details 该方法从配置中读取各项风控参数并初始化风控器
 *          主要配置项包括：
 *          - calc_span: 风控计算间隔(秒)
 *          - risk_span: 风险计算时间跨度(分钟)
 *          - basic_ratio: 基础收益率，设定盈利边界
 *          - inner_day_fd: 日内最大回撤比例
 *          - inner_day_active: 日内风控是否激活
 *          - multi_day_fd: 多日最大回撤比例
 *          - multi_day_active: 多日风控是否激活
 *          - base_amount: 基础资金量
 *          - risk_scale: 风控触发后的仓位比例
 */
void WtSimpleRiskMon::init(WtPortContext* ctx, WTSVariant* cfg)
{
	WtRiskMonitor::init(ctx, cfg);

	_calc_span = cfg->getUInt32("calc_span");
	_risk_span = cfg->getUInt32("risk_span");
	_basic_ratio = cfg->getUInt32("basic_ratio");
	_inner_day_fd = cfg->getDouble("inner_day_fd");
	_inner_day_active = cfg->getBoolean("inner_day_active");
	_multi_day_fd = cfg->getDouble("multi_day_fd");
	_multi_day_active = cfg->getBoolean("multi_day_active");
	_base_amount = cfg->getDouble("base_amount");
	_risk_scale = cfg->getDouble("risk_scale");

	ctx->writeRiskLog(fmt::format("Params inited, Checking frequency: {} s, MaxIDD: {}({:.2f}%), MaxMDD: {}({:.2f}%), Capital: {:.1f}, Profit Boudary: {:.2f}%, Calc Span: {} mins, Risk Scale: {:.2f}",
		_calc_span, _inner_day_active ? "ON" : "OFF", _inner_day_fd, _multi_day_active ? "ON" : "OFF", _multi_day_fd, _base_amount, _basic_ratio, _risk_span, _risk_scale).c_str());
}

/**
 * @brief 启动风控监控线程
 * @details 该方法创建并启动一个新的风控监控线程，定期执行以下风险监控策略：
 *          1. 日内回撤风控：监控当日收益的回撤情况，超过设定阈值时降低仓位
 *          2. 多日回撤风控：监控多日累计最高收益的回撤比例，超过设定阈值时平仓
 *          风控线程基于时间间隔周期性施行检查，间隔由_calc_span参数控制
 */
void WtSimpleRiskMon::run()
{
	if (_thrd)
		return;

	_thrd.reset(new std::thread([this](){
		while (!_stopped)
		{
			if (_ctx && _ctx->isInTrading())
			{
				WTSPortFundInfo* fundInfo = _ctx->getFundInfo();
				const WTSFundStruct& fs = fundInfo->fundInfo();
				/*
				* 条件1: 整体盘子的浮动收益比上一交易日结束时（收盘价计）, 增长 1% 以上
				*		组合盘的动态权益 ≥ 上日收盘时的动态权益的 101%
				* 条件2: 30min以内, 从今日高点回调到 80%以下
				*		30min以内, 今日收益从高点回调到 80%以下
				*
				* 动作: 
				* 方式A:  所有品种减仓（减少到 30% 仓位）, 下一交易日重新按策略新仓位补齐
				* 方式B:  所有盈利品种都 平仓, 下一交易日重新按策略新仓位补齐
				*/

				if (_inner_day_active && fs._max_dyn_bal != DBL_MAX)
				{
					double predynbal = fundInfo->predynbalance() + _base_amount;	//上日动态权益
					double maxBal = fs._max_dyn_bal + _base_amount;					//当日最大动态权益
					double curBal = fs._balance + fs._dynprofit + _base_amount;		//当前动态权益

					double rate = 0.0;
					if(!decimal::eq(maxBal, predynbal))
						rate = (maxBal - curBal) * 100 / (maxBal - predynbal);	//当日盈利回撤比例

					//如果当日最大权益超过止盈边界条件
					if (maxBal > (_basic_ratio*predynbal / 100.0))
					{
						
						/*
						 *	这里要转成日内分钟数处理
						 *	不然如果遇到午盘启动或早盘启动, 
						 *	可能会因为中途休息时间过长, 而不触发风控
						 *	导致更大风险的发生
						 */
						uint32_t maxTime = _ctx->transTimeToMin(fundInfo->max_dynbal_time());	
						uint32_t curTime = _ctx->transTimeToMin(_ctx->getCurTime());			//转成日内分钟数

						if (rate >= _inner_day_fd && curTime - maxTime <= _risk_span && !_limited)
						{
							_ctx->writeRiskLog(fmt::format("Current IDD {:.2f}%, ≥MaxIDD {:.2f}%, Position down to {:.1f}%", rate, _inner_day_fd, _risk_scale).c_str());
							_ctx->setVolScale(_risk_scale);
							_limited = true;
						}
						else
						{
							_ctx->writeRiskLog(fmt::format("Current Balance Ratio: {:.2f}%, Current IDD: {:.2f}%", curBal*100.0 / predynbal, rate).c_str());
							//_limited = false;
						}
					}
					else
					{
						//如果当日最大权益没有超过盈利边界条件
						_ctx->writeRiskLog(fmt::format("Current Balance Ratio: {:.2f}%", curBal*100.0 / predynbal).c_str());
						//_limited = false;
					}
				}

				if (_multi_day_active && fs._max_md_dyn_bal._date != 0)
				{
					double maxBal = fs._max_md_dyn_bal._dyn_balance + _base_amount;
					double curBal = fs._balance + fs._dynprofit + _base_amount;

					if (curBal < maxBal)
					{
						double rate = (maxBal - curBal) * 100 / maxBal;
						if (rate >= _multi_day_fd)
						{
							_ctx->writeRiskLog(fmt::format("Current MDD {:.2f}%, >= MaxMDD {:.2f}%, Position down to 0.0%", rate, _multi_day_fd).c_str());
							_ctx->setVolScale(0.0);
						}
					}
				}
			}

			_last_time = TimeUtils::getLocalTimeNow();

			while (!_stopped)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				uint64_t now = TimeUtils::getLocalTimeNow();;
				if(now - _last_time >= _calc_span*1000)
					break;
			}
		}
	}));
}

/**
 * @brief 停止风控监控线程
 * @details 该方法安全停止风控监控线程的运行
 *          首先设置停止标记_stopped为true，
 *          然后等待线程执行完当前操作并安全退出
 *          调用join()确保线程安全终止并回收相关资源
 */
void WtSimpleRiskMon::stop()
{
	_stopped = true;
	if (_thrd)
		_thrd->join();
}