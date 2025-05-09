/*!
 * \file WTSDataFactory.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 数据拼接工厂类定义
 * 
 * WTSDataFactory类实现了IDataFactory接口，负责处理各种周期K线数据的生成、更新和合并。
 * 该类提供了从Tick数据构建各种周期K线、从基础周期K线提取更高周期K线的功能，
 * 以及K线数据的合并等核心功能，是WonderTrader中数据处理的重要组件。
 */
#pragma once
#include "../Includes/IDataFactory.h"

USING_NS_WTP;

/**
 * @brief 数据工厂类，负责K线数据的生成、更新和合并
 * 
 * WTSDataFactory实现了IDataFactory接口，提供了丰富的数据处理功能，包括：
 * - 从Tick数据构建和更新各种周期的K线数据
 * - 从基础周期K线提取更高周期的K线数据
 * - 合并不同时间段的K线数据
 * 该类是WonderTrader中数据处理的核心组件
 */
class WTSDataFactory : public IDataFactory
{
public:
	/**
	 * @brief 利用tick数据更新K线
	 * 
	 * 根据最新的tick数据更新现有的K线数据，支持秒、分钟和日线级别的K线更新
	 * 
	 * @param klineData K线数据对象，存储当前已有的K线数据
	 * @param tick 最新的tick数据
	 * @param sInfo 交易时间模板，用于确定交易时段
	 * @param bAlignSec 是否按照交易小节对齐，默认为false
	 * @return WTSBarStruct* 返回更新后的K线结构指针，如果更新失败则返回NULL
	 */
	virtual WTSBarStruct*	updateKlineData(WTSKlineData* klineData, WTSTickData* tick, WTSSessionInfo* sInfo, bool bAlignSec = false);

	/**
	 * @brief 利用基础周期K线数据更新K线
	 * 
	 * 根据新的基础周期K线数据更新现有的K线数据，主要用于K线周期的转换
	 * 
	 * @param klineData K线数据对象，存储当前已有的K线数据
	 * @param newBasicBar 新的基础周期K线数据
	 * @param sInfo 交易时间模板，用于确定交易时段
	 * @param bAlignSec 是否按照交易小节对齐，默认为false
	 * @return WTSBarStruct* 返回更新后的K线结构指针，如果更新失败则返回NULL
	 */
	virtual WTSBarStruct*	updateKlineData(WTSKlineData* klineData, WTSBarStruct* newBasicBar, WTSSessionInfo* sInfo, bool bAlignSec = false);

	/**
	 * @brief 从基础周期K线数据提取非基础周期的K线数据
	 * 
	 * 根据基础周期K线数据，提取生成更高周期的K线数据，如从1分钟K线生成5分钟K线
	 * 
	 * @param baseKline 基础周期K线数据切片
	 * @param period 基础周期类型，如m1/m5/day
	 * @param times 周期倍数，用于计算目标周期
	 * @param sInfo 交易时间模板，用于确定交易时段
	 * @param bIncludeOpen 是否包含未闭合的K线，默认为true
	 * @param bAlignSec 是否按交易小节对齐，默认为false
	 * @return WTSKlineData* 返回提取的K线数据对象，如果提取失败则返回NULL
	 */
	virtual WTSKlineData*	extractKlineData(WTSKlineSlice* baseKline, WTSKlinePeriod period, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false);

	/**
	 * @brief 从tick数据提取秒周期的K线数据
	 * 
	 * 根据tick数据序列，生成指定秒数周期的K线数据
	 * 
	 * @param ayTicks tick数据切片
	 * @param seconds 目标周期的秒数
	 * @param sInfo 交易时间模板，用于确定交易时段
	 * @param bUnixTime tick时间戳是否是Unix时间戳格式，默认为false
	 * @param bAlignSec 是否按交易小节对齐，默认为false
	 * @return WTSKlineData* 返回提取的K线数据对象，如果提取失败则返回NULL
	 */
	virtual WTSKlineData*	extractKlineData(WTSTickSlice* ayTicks, uint32_t seconds, WTSSessionInfo* sInfo, bool bUnixTime = false, bool bAlignSec = false);

	/**
	 * @brief 合并K线数据
	 * 
	 * 将新的K线数据合并到目标K线数据中，通常用于合并不同时间段的K线数据
	 * 
	 * @param klineData 目标K线数据对象，合并结果将保存在此对象中
	 * @param newKline 待合并的新K线数据
	 * @return bool 合并是否成功
	 */
	virtual bool			mergeKlineData(WTSKlineData* klineData, WTSKlineData* newKline);

protected:
	/**
	 * @brief 更新1分钟K线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param tick 最新tick数据
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec = false);
	
	/**
	 * @brief 更新5分钟K线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param tick 最新tick数据
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick, bool bAlignSec = false);
	
	/**
	 * @brief 更新日线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param tick 最新tick数据
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateDayData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick);
	
	/**
	 * @brief 更新秒级K线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param tick 最新tick数据
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateSecData(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSTickData* tick);

	/**
	 * @brief 使用基础K线更新1分钟K线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param newBasicBar 新的基础K线数据
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateMin1Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec = false);
	
	/**
	 * @brief 使用基础K线更新5分钟K线数据
	 * 
	 * @param sInfo 交易时间模板
	 * @param klineData 现有K线数据
	 * @param newBasicBar 新的基础K线数据
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSBarStruct* 更新后的K线结构
	 */
	WTSBarStruct* updateMin5Data(WTSSessionInfo* sInfo, WTSKlineData* klineData, WTSBarStruct* newBasicBar, bool bAlignSec = false);

	/**
	 * @brief 从基础K线提取1分钟K线数据
	 * 
	 * @param baseKline 基础K线切片
	 * @param times 周期倍数
	 * @param sInfo 交易时间模板
	 * @param bIncludeOpen 是否包含未闭合K线
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSKlineData* 提取的K线数据
	 */
	WTSKlineData* extractMin1Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false);
	
	/**
	 * @brief 从基础K线提取5分钟K线数据
	 * 
	 * @param baseKline 基础K线切片
	 * @param times 周期倍数
	 * @param sInfo 交易时间模板
	 * @param bIncludeOpen 是否包含未闭合K线
	 * @param bAlignSec 是否按交易小节对齐
	 * @return WTSKlineData* 提取的K线数据
	 */
	WTSKlineData* extractMin5Data(WTSKlineSlice* baseKline, uint32_t times, WTSSessionInfo* sInfo, bool bIncludeOpen = true, bool bAlignSec = false);
	
	/**
	 * @brief 从基础K线提取日线数据
	 * 
	 * @param baseKline 基础K线切片
	 * @param times 周期倍数
	 * @param bIncludeOpen 是否包含未闭合K线
	 * @return WTSKlineData* 提取的K线数据
	 */
	WTSKlineData* extractDayData(WTSKlineSlice* baseKline, uint32_t times, bool bIncludeOpen = true);

protected:
	/**
	 * @brief 获取前一个分钟时间
	 * 
	 * 根据当前分钟和周期，计算前一个周期的分钟时间
	 * 
	 * @param curMinute 当前分钟时间
	 * @param period 周期，默认为1
	 * @return uint32_t 前一个周期的分钟时间
	 */
	static uint32_t getPrevMinute(uint32_t curMinute, int period = 1);
};

