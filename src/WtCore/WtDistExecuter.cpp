/*!
 * \file WtDistExecuter.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 分布式执行器实现
 * \details 实现分布式执行器的核心功能，包括初始化、目标仓位设置和通知
 */

#include "WtDistExecuter.h"

#include "../Includes/WTSVariant.hpp"

#include "../Share/decimal.h"
#include "../WTSTools/WTSLogger.h"

USING_NS_WTP;

/**
 * \brief 分布式执行器构造函数
 * \details 初始化分布式执行器，调用父类构造函数
 * \param name 执行器名称
 */
WtDistExecuter::WtDistExecuter(const char* name)
	: IExecCommand(name)
{

}

/**
 * \brief 分布式执行器析构函数
 * \details 资源清理
 */
WtDistExecuter::~WtDistExecuter()
{

}

/**
 * \brief 初始化分布式执行器
 * \details 从配置参数中读取如缩放比例等设置
 * \param params 配置参数集合
 * \return 是否初始化成功
 */
bool WtDistExecuter::init(WTSVariant* params)
{
	if (params == NULL)
		return false;

	_config = params;
	_config->retain();

	_scale = params->getUInt32("scale");

	return true;
}

/**
 * \brief 设置目标仓位
 * \details 批量更新多个品种的目标仓位，并广播变化
 * \param targets 目标仓位映射表，键为品种代码，值为目标仓位
 */
void WtDistExecuter::set_position(const wt_hashmap<std::string, double>& targets)
{
	for (auto it = targets.begin(); it != targets.end(); it++)
	{
		const char* stdCode = it->first.c_str();
		double newVol = it->second;

		// 应用仓位缩放比例
		newVol *= _scale;
		double oldVol = _target_pos[stdCode];
		_target_pos[stdCode] = newVol;
		if (!decimal::eq(oldVol, newVol))
		{
			WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}]{}目标仓位更新: {} -> {}", _name.c_str(), stdCode, oldVol, newVol);
		}

		// TODO: 这里广播目标仓位到其他节点
	}
}

/**
 * \brief 处理单个品种的目标仓位变化
 * \details 更新单个品种的目标仓位，并广播到其他节点
 * \param stdCode 品种代码
 * \param targetPos 目标仓位
 */
void WtDistExecuter::on_position_changed(const char* stdCode, double targetPos)
{
	// 应用仓位缩放比例
	targetPos *= _scale;

	double oldVol = _target_pos[stdCode];
	_target_pos[stdCode] = targetPos;

	if (!decimal::eq(oldVol, targetPos))
	{
		WTSLogger::log_dyn("executer", _name.c_str(), LL_INFO, "[{}]{}目标仓位更新: {} -> {}", _name.c_str(), stdCode, oldVol, targetPos);
	}

	// TODO: 这里广播目标仓位到其他节点
}

/**
 * \brief 处理行情数据回调
 * \details 分布式执行器不需要直接处理行情数据，这个方法是接口实现的空方法
 * \param stdCode 品种代码
 * \param newTick 最新的行情数据
 */
void WtDistExecuter::on_tick(const char* stdCode, WTSTickData* newTick)
{
	// 分布式执行器不需要处理ontick
}
