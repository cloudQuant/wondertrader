/*!
 * \file IHotMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 主力合约管理器接口定义
 * \details 该文件定义了WonderTrader框架中管理期货主力合约、次主力合约切换和连续合约的接口
 */
#pragma once
#include "WTSMarcos.h"
#include <vector>
#include <string>
#include <stdint.h>

/**
 * @brief 主力合约切段结构体
 * @details 定义了主力合约在某一时间段内的分月合约信息，包括合约代码、开始日期、结束日期和对应的连续因子
 */
typedef struct _HotSection
{
	std::string	_code;    ///< 分月合约代码，如"au2106"
	uint32_t	_s_date;   ///< 该合约作为主力合约的开始日期，格式为YYYYMMDD
	uint32_t	_e_date;   ///< 该合约作为主力合约的结束日期，格式为YYYYMMDD
	double		_factor;   ///< 连续合约计算因子，用于计算连续合约价格

	/**
	 * @brief 构造函数
	 * @param code 分月合约代码
	 * @param sdate 开始日期，格式为YYYYMMDD
	 * @param edate 结束日期，格式为YYYYMMDD
	 * @param factor 连续因子
	 */
	_HotSection(const char* code, uint32_t sdate, uint32_t edate, double factor)
		: _s_date(sdate), _e_date(edate), _code(code),_factor(factor)
	{
	
	}

} HotSection;
/**
 * @brief 主力合约切段列表类型
 * @details 用于存储一系列主力合约切段，表示连续合约中不同时间段的分月合约信息
 */
typedef std::vector<HotSection>	HotSections;

NS_WTP_BEGIN

/**
 * @brief 主力合约市场标识符
 * @details 用于标识主力合约市场，在构造主力合约代码时使用
 */
#define HOTS_MARKET		"HOTS_MARKET"

/**
 * @brief 次主力合约市场标识符
 * @details 用于标识次主力合约市场，在构造次主力合约代码时使用
 */
#define SECONDS_MARKET	"SECONDS_MARKET"

/**
 * @brief 主力合约管理器接口
 * @details 管理期货主力合约、次主力合约的切换和连续合约的生成，提供查询各时间点主力和次主力合约的方法
 */
class IHotMgr
{
public:
	/**
	 * @brief 获取主力合约分月代码
	 * @details 获取指定交易所和品种在特定交易日的主力合约分月代码
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param pid 品种代码，如"au"（黄金）
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 分月合约代码，如"au2106"
	 */
	virtual const char* getRawCode(const char* exchg, const char* pid, uint32_t dt)	= 0;

	/**
	 * @brief 获取上一个主力合约分月代码
	 * @details 获取指定交易所和品种在特定交易日的上一个主力合约的分月代码
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param pid 品种代码，如"au"（黄金）
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 上一个主力合约的分月代码，如"au2103"
	 */
	virtual const char* getPrevRawCode(const char* exchg, const char* pid, uint32_t dt) = 0;

	/**
	 * @brief 判断是否为主力合约
	 * @details 判断指定交易所的某个分月合约在特定交易日是否为主力合约
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param rawCode 分月合约代码，如"au2106"
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 如果是主力合约返回true，否则返回false
	 */
	virtual bool		isHot(const char* exchg, const char* rawCode, uint32_t dt) = 0;

	/**
	 * @brief 分割主力合约切段
	 * @details 获取指定时间范围内主力合约的全部分月合约切段，用于连续合约的生成
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param hotCode 主力合约代码，如"SHFE.au.HOT"
	 * @param sDt 开始交易日，格式为YYYYMMDD
	 * @param eDt 结束交易日，格式为YYYYMMDD
	 * @param sections 输出参数，用于返回主力合约切段列表
	 * @return 如果成功分割切段返回true，否则返回false
	 */
	virtual bool		splitHotSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) = 0;

	/**
	 * @brief 获取次主力合约分月代码
	 * @details 获取指定交易所和品种在特定交易日的次主力合约分月代码
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param pid 品种代码，如"au"（黄金）
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 次主力合约的分月代码，如"au2109"
	 */
	virtual const char* getSecondRawCode(const char* exchg, const char* pid, uint32_t dt) = 0;

	/**
	 * @brief 获取上一个次主力合约分月代码
	 * @details 获取指定交易所和品种在特定交易日的上一个次主力合约的分月代码
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param pid 品种代码，如"au"（黄金）
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 上一个次主力合约的分月代码，如"au2106"
	 */
	virtual const char* getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt) = 0;

	/**
	 * @brief 判断是否为次主力合约
	 * @details 判断指定交易所的某个分月合约在特定交易日是否为次主力合约
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param rawCode 分月合约代码，如"au2109"
	 * @param dt 交易日期，格式为YYYYMMDD
	 * @return 如果是次主力合约返回true，否则返回false
	 */
	virtual bool		isSecond(const char* exchg, const char* rawCode, uint32_t dt) = 0;

	/**
	 * @brief 分割次主力合约切段
	 * @details 获取指定时间范围内次主力合约的全部分月合约切段，用于连续合约的生成
	 * 
	 * @param exchg 交易所代码，如"SHFE"（上期）
	 * @param hotCode 次主力合约代码，如"SHFE.au.2ND"
	 * @param sDt 开始交易日，格式为YYYYMMDD
	 * @param eDt 结束交易日，格式为YYYYMMDD
	 * @param sections 输出参数，用于返回次主力合约切段列表
	 * @return 如果成功分割切段返回true，否则返回false
	 */
	virtual bool		splitSecondSecions(const char* exchg, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) = 0;

	/**
	 * @brief 获取自定义主力合约的分月代码
	 * @details 根据自定义规则标签和品种代码获取特定交易日的分月合约代码
	 * 
	 * @param tag 自定义规则标签，如"MONTHLY"、"WEEKLY"等
	 * @param fullPid 带交易所的品种代码，如"SHFE.au"（上期黄金）
	 * @param dt 交易日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 分月合约代码，如"au2106"
	 */
	virtual const char* getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) = 0;

	/**
	 * @brief 获取自定义连续合约的上一期主力分月代码
	 * @details 根据自定义规则标签和品种代码获取特定交易日的上一期主力合约的分月代码
	 * 
	 * @param tag 自定义规则标签，如"MONTHLY"、"WEEKLY"等
	 * @param fullPid 带交易所的品种代码，如"SHFE.au"（上期黄金）
	 * @param dt 交易日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 上一期主力合约的分月代码，如"au2103"
	 */
	virtual const char* getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt = 0) = 0;

	/**
	 * @brief 判断是否是自定义主力合约
	 * @details 判断指定的合约代码在特定交易日是否满足自定义规则的主力合约条件
	 * 
	 * @param tag 自定义规则标签，如"MONTHLY"、"WEEKLY"等
	 * @param fullCode 标准化合约代码，如"SHFE.au.au2106"
	 * @param d 交易日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 如果是自定义主力合约返回true，否则返回false
	 */
	virtual bool		isCustomHot(const char* tag, const char* fullCode, uint32_t d = 0) = 0;

	/**
	 * @brief 分割自定义主力合约切段
	 * @details 获取指定时间范围内自定义连续合约的全部分月合约切段，用于连续合约的生成
	 * 
	 * @param tag 自定义规则标签，如"MONTHLY"、"WEEKLY"等
	 * @param hotCode 自定义连续合约代码，如"SHFE.au@MONTHLY"
	 * @param sDt 开始交易日，格式为YYYYMMDD
	 * @param eDt 结束交易日，格式为YYYYMMDD
	 * @param sections 输出参数，用于返回自定义主力合约切段列表
	 * @return 如果成功分割切段返回true，否则返回false
	 */
	virtual bool		splitCustomSections(const char* tag, const char* hotCode, uint32_t sDt, uint32_t eDt, HotSections& sections) = 0;

	/**
	 * @brief 根据标准合约代码获取规则标签
	 * @details 从标准化合约代码中提取自定义规则标签，用于确定连续合约的计算规则
	 * 
	 * @param stdCode 标准化合约代码，如"SHFE.au@MONTHLY"
	 * @return 规则标签，如"MONTHLY"
	 */
	virtual const char* getRuleTag(const char* stdCode) = 0;

	/**
	 * @brief 获取规则因子
	 * @details 根据规则标签、品种代码和日期获取连续合约计算因子，用于计算连续合约价格
	 * 
	 * @param ruleTag 规则标签，如"MONTHLY"、"WEEKLY"等
	 * @param fullPid 带交易所的品种代码，如"SHFE.au"（上期黄金）
	 * @param uDate 交易日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 规则因子，用于计算连续合约价格
	 */
	virtual double		getRuleFactor(const char* ruleTag, const char* fullPid, uint32_t uDate = 0) = 0;
};
NS_WTP_END
