/*!
 * \file IDataCaster.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据广播器接口定义
 * \details 该文件定义了WonderTrader数据广播器接口IDataCaster，用于将各类行情数据广播到外部系统
 */
#pragma once
#include "../Includes/WTSMarcos.h"

NS_WTP_BEGIN
class WTSTickData;
class WTSVariant;
class WTSOrdDtlData;
class WTSOrdQueData;
class WTSTransData;

/**
 * @brief 数据广播器接口
 * @details 定义了数据广播器的接口，用于将各类行情数据（Tick数据、委托队列、委托明细、成交数据）广播到外部系统
 * 实现该接口的类需要至少实现broadcast(WTSTickData*)方法，其他方法有默认空实现
 */
class IDataCaster
{
public:
	/**
	 * @brief 广播Tick数据
	 * @details 将Tick数据广播到外部系统，该方法必须由子类实现
	 * @param curTick 当前的Tick数据指针
	 */
	virtual void	broadcast(WTSTickData* curTick) = 0;
	/**
	 * @brief 广播委托队列数据
	 * @details 将委托队列数据广播到外部系统，默认为空实现，子类可以根据需要重写
	 * @param curOrdQue 当前的委托队列数据指针
	 */
	virtual void	broadcast(WTSOrdQueData* curOrdQue){}
	/**
	 * @brief 广播委托明细数据
	 * @details 将委托明细数据广播到外部系统，默认为空实现，子类可以根据需要重写
	 * @param curOrdDtl 当前的委托明细数据指针
	 */
	virtual void	broadcast(WTSOrdDtlData* curOrdDtl){}
	/**
	 * @brief 广播成交数据
	 * @details 将成交数据广播到外部系统，默认为空实现，子类可以根据需要重写
	 * @param curTrans 当前的成交数据指针
	 */
	virtual void	broadcast(WTSTransData* curTrans){}
};

NS_WTP_END
