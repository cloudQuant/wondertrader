/*!
 * \file WTSHotMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 主力合约管理器实现
 *
 * 该文件定义了WTSHotMgr类，用于管理期货市场中的主力合约切换规则。
 * 主要功能包括加载主力合约配置文件、识别当前主力和次主力合约、计算复权因子等。
 * 支持自定义规则，满足不同交易所和品种的主力合约切换需求。
 */
#pragma once
#include "../Includes/IHotMgr.h"
#include "../Includes/FasterDefs.h"
#include "../Includes/WTSCollection.hpp"
#include <string>

NS_WTP_BEGIN
	class WTSSwitchItem;
NS_WTP_END

USING_NS_WTP;

//! 换月主力映射，日期到主力合约切换项的映射
typedef WTSMap<uint32_t>			WTSDateHotMap;
//! 品种主力映射，品种代码到日期主力映射的哈希表
typedef WTSHashMap<std::string>		WTSProductHotMap;
//! 分市场主力映射，交易所代码到品种主力映射的哈希表
typedef WTSHashMap<std::string>		WTSExchgHotMap;

//! 自定义切换规则映射，规则标签到品种主力映射的哈希表
typedef WTSHashMap<std::string>		WTSCustomSwitchMap;

/**
 * @brief 主力合约管理器类
 * 
 * 实现IHotMgr接口，用于管理期货合约的主力合约切换、次主力合约切换以及自定义切换规则。
 * 支持根据日期查询特定时间点的主力合约和次主力合约代码，
 * 以及判断某合约是否为特定时间点的主力合约或次主力合约。
 * 还提供了计算复权因子的功能，用于连续合约数据的生成。
 */
class WTSHotMgr : public IHotMgr
{
public:
	/**
	 * @brief 构造函数
	 */
	WTSHotMgr();

	/**
	 * @brief 析构函数
	 */
	~WTSHotMgr();

public:
	/**
	 * @brief 加载主力合约切换规则配置文件
	 * 
	 * @param filename 配置文件路径
	 * @return bool 加载成功返回true，失败返回false
	 */
	bool loadHots(const char* filename);

	/**
	 * @brief 加载次主力合约切换规则配置文件
	 * 
	 * @param filename 配置文件路径
	 * @return bool 加载成功返回true，失败返回false
	 */
	bool loadSeconds(const char* filename);

	/**
	 * @brief 释放内存资源
	 */
	void release();

	/**
	 * @brief 加载自定义切换规则配置文件
	 * 
	 * @param tag 规则标识符，用于区分不同规则
	 * @param filename 配置文件路径
	 * @return bool 加载成功返回true，失败返回false
	 */
	bool loadCustomRules(const char* tag, const char* filename);

	/**
	 * @brief 检查是否已初始化完成
	 * 
	 * @return bool 已初始化返回true，未初始化返回false
	 */
	inline bool isInitialized() const {return m_bInitialized;}

public:
	/**
	 * @brief 获取标准合约代码对应的规则标签
	 * 
	 * @param stdCode 标准合约代码
	 * @return const char* 规则标签，如果不存在则返回空字符串
	 */
	virtual const char* getRuleTag(const char* stdCode) override;

	/**
	 * @brief 获取指定日期的复权因子
	 * 
	 * @param ruleTag 规则标签
	 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
	 * @param uDate 日期，格式YYYYMMDD，默认为0表示取最新数据
	 * @return double 复权因子，默认为1.0
	 */
	virtual double		getRuleFactor(const char* ruleTag, const char* fullPid, uint32_t uDate  = 0 ) override;

	//////////////////////////////////////////////////////////////////////////
	//主力接口
	/**
	 * @brief 获取指定日期的主力合约代码
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return const char* 原始合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override;

	/**
	 * @brief 获取指定日期的前一主力合约代码
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return const char* 前一主力合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getPrevRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override;

	/**
	 * @brief 判断指定合约是否为特定日期的主力合约
	 * 
	 * @param exchg 交易所代码
	 * @param rawCode 原始合约代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return bool 是主力合约返回true，不是返回false
	 */
	virtual bool	isHot(const char* exchg, const char* rawCode, uint32_t dt = 0) override;

	/**
	 * @brief 分割指定时间范围内的主力合约切换区间
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param sDt 开始日期，格式YYYYMMDD
	 * @param eDt 结束日期，格式YYYYMMDD
	 * @param sections 输出的主力合约切换区间
	 * @return bool 分割成功返回true，失败返回false
	 */
	virtual bool	splitHotSecions(const char* exchg, const char* pid, uint32_t sDt, uint32_t eDt, HotSections& sections) override;

	//////////////////////////////////////////////////////////////////////////
	//次主力接口
	/**
	 * @brief 获取指定日期的次主力合约代码
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return const char* 次主力合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getSecondRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override;

	/**
	 * @brief 获取指定日期的前一次主力合约代码
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return const char* 前一次主力合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt = 0) override;

	/**
	 * @brief 判断指定合约是否为特定日期的次主力合约
	 * 
	 * @param exchg 交易所代码
	 * @param rawCode 原始合约代码
	 * @param dt 日期，格式YYYYMMDD，默认为0表示取当前日期
	 * @return bool 是次主力合约返回true，不是返回false
	 */
	virtual bool		isSecond(const char* exchg, const char* rawCode, uint32_t dt = 0) override;

	/**
	 * @brief 分割指定时间范围内的次主力合约切换区间
	 * 
	 * @param exchg 交易所代码
	 * @param pid 品种代码
	 * @param sDt 开始日期，格式YYYYMMDD
	 * @param eDt 结束日期，格式YYYYMMDD
	 * @param sections 输出的次主力合约切换区间
	 * @return bool 分割成功返回true，失败返回false
	 */
	virtual bool		splitSecondSecions(const char* exchg, const char* pid, uint32_t sDt, uint32_t eDt, HotSections& sections) override;

	//////////////////////////////////////////////////////////////////////////
	//通用接口
	/**
	 * @brief 获取指定日期的自定义规则合约代码
	 * 
	 * @param tag 规则标识符
	 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
	 * @param dt 日期，格式YYYYMMDD
	 * @return const char* 合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt) override;

	/**
	 * @brief 获取指定日期的前一自定义规则合约代码
	 * 
	 * @param tag 规则标识符
	 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
	 * @param dt 日期，格式YYYYMMDD
	 * @return const char* 前一合约代码，如果不存在则返回空字符串
	 */
	virtual const char* getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt) override;

	/**
	 * @brief 判断指定合约是否为特定日期的自定义规则合约
	 * 
	 * @param tag 规则标识符
	 * @param fullCode 完整合约代码，格式为“交易所.合约代码”
	 * @param dt 日期，格式YYYYMMDD
	 * @return bool 是定制化合约返回true，不是返回false
	 */
	virtual bool		isCustomHot(const char* tag, const char* fullCode, uint32_t dt) override;

	/**
	 * @brief 分割指定时间范围内的自定义合约切换区间
	 * 
	 * @param tag 规则标识符
	 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
	 * @param sDt 开始日期，格式YYYYMMDD
	 * @param eDt 结束日期，格式YYYYMMDD
	 * @param sections 输出的合约切换区间
	 * @return bool 分割成功返回true，失败返回false
	 */
	virtual bool		splitCustomSections(const char* tag, const char* fullPid, uint32_t sDt, uint32_t eDt, HotSections& sections) override;


private:
	//WTSExchgHotMap*	m_pExchgHotMap;
	//WTSExchgHotMap*	m_pExchgScndMap;
	//wt_hashset<std::string>	m_curHotCodes;
	//wt_hashset<std::string>	m_curSecCodes;
	
	//! 是否已初始化标记
	bool			m_bInitialized;

	//! 自定义规则映射表，将规则标签映射到品种主力映射
	WTSCustomSwitchMap*	m_mapCustRules;
	
	//! 自定义规则的当前主力合约代码存储类型
	typedef wt_hashmap<std::string, wt_hashset<std::string>>	CustomSwitchCodes;
	
	//! 自定义规则的当前主力合约代码列表，按规则标签分组
	CustomSwitchCodes	m_mapCustCodes;
};

