/*!
 * \file IBaseDataMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 基础数据管理器接口定义
 * \details 该文件定义了WonderTrader框架中基础数据管理的接口，包括合约、品种、交易时间等静态数据的管理
 */
#pragma once
#include <string>
#include <stdint.h>

#include "WTSMarcos.h"
#include "FasterDefs.h"

NS_WTP_BEGIN
/**
 * @brief 合约集合类型
 * @details 用于存储一组合约代码，基于CodeSet（代码集合）实现
 */
typedef CodeSet ContractSet;

// 前向声明
/**
 * @brief 合约信息类
 * @details 定义了合约的详细信息，如代码、名称、合约乘数等
 */
class WTSContractInfo;

/**
 * @brief WonderTrader数组类
 * @details 用于存储同类型对象的数组
 */
class WTSArray;

/**
 * @brief 交易时段信息类
 * @details 定义了交易时段的信息，如交易时间段、休市时间等
 */
class WTSSessionInfo;

/**
 * @brief 品种信息类
 * @details 定义了品种的详细信息，如交易所、品种代码、手续费率等
 */
class WTSCommodityInfo;

/**
 * @brief 假日集合类型
 * @details 用于存储交易日历中的假日日期，格式为YYYYMMDD的整数
 */
typedef wt_hashset<uint32_t> HolidaySet;
/**
 * @brief 交易日历模板结构体
 * @details 定义了交易日历的基本模板，包含当前交易日和假日集合
 */
typedef struct _TradingDayTpl
{
	uint32_t	_cur_tdate;   ///< 当前交易日，格式为YYYYMMDD
	HolidaySet	_holidays;   ///< 假日集合，包含所有不交易的日期

	/**
	 * @brief 构造函数
	 * @details 初始化交易日历模板，将当前交易日设置为0
	 */
	_TradingDayTpl() :_cur_tdate(0){}
} TradingDayTpl;

/**
 * @brief 基础数据管理器接口
 * @details 该接口定义了管理和访问交易所、品种、合约、交易时段等静态基础数据的方法
 */
class IBaseDataMgr
{
public:
	/**
	 * @brief 获取品种信息
	 * @details 通过带交易所的品种代码获取品种信息
	 * 
	 * @param exchgpid 品种代码，格式为“交易所.品种代码”，如“SHFE.au”（上期黄金）
	 * @return 品种信息对象指针，如果不存在返回NULL
	 */
	virtual WTSCommodityInfo*	getCommodity(const char* exchgpid)						= 0;

	/**
	 * @brief 获取品种信息
	 * @details 通过分别指定交易所和品种代码获取品种信息
	 * 
	 * @param exchg 交易所代码，如“SHFE”（上期）
	 * @param pid 品种代码，如“au”（黄金）
	 * @return 品种信息对象指针，如果不存在返回NULL
	 */
	virtual WTSCommodityInfo*	getCommodity(const char* exchg, const char* pid)		= 0;

	/**
	 * @brief 获取合约信息
	 * @details 通过合约代码和交易所获取合约的详细信息
	 * 
	 * @param code 合约代码，如“au2106”（黄金2106合约）
	 * @param exchg 交易所代码，如“SHFE”（上期），默认为空字符串
	 * @param uDate 日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 合约信息对象指针，如果不存在返回NULL
	 */
	virtual WTSContractInfo*	getContract(const char* code, const char* exchg = "", uint32_t uDate = 0)	= 0;

	/**
	 * @brief 获取合约列表
	 * @details 获取指定交易所在指定日期的所有合约
	 * 
	 * @param exchg 交易所代码，如“SHFE”（上期），默认为空字符串，表示所有交易所
	 * @param uDate 日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 合约信息列表（WTSArray对象），元素为WTSContractInfo对象
	 */
	virtual WTSArray*			getContracts(const char* exchg = "", uint32_t uDate = 0)					= 0;

	/**
	 * @brief 获取交易时段信息
	 * @details 通过交易时段ID获取交易时段的详细信息
	 * 
	 * @param sid 交易时段ID，如“SHFE01”（上期第一交易时段）
	 * @return 交易时段信息对象指针，如果不存在返回NULL
	 */
	virtual WTSSessionInfo*		getSession(const char* sid)						= 0;

	/**
	 * @brief 根据合约获取交易时段信息
	 * @details 通过合约代码和交易所获取对应的交易时段信息
	 * 
	 * @param code 合约代码，如“au2106”（黄金2106合约）
	 * @param exchg 交易所代码，如“SHFE”（上期），默认为空字符串
	 * @return 交易时段信息对象指针，如果不存在返回NULL
	 */
	virtual WTSSessionInfo*		getSessionByCode(const char* code, const char* exchg = "") = 0;

	/**
	 * @brief 获取所有交易时段
	 * @details 获取系统中配置的所有交易时段信息
	 * 
	 * @return 交易时段信息列表（WTSArray对象），元素为WTSSessionInfo对象
	 */
	virtual WTSArray*			getAllSessions() = 0;

	/**
	 * @brief 判断指定日期是否为假日
	 * @details 检查指定品种在指定日期是否为交易所休市日
	 * 
	 * @param pid 品种代码或带交易所的品种代码，如“SHFE.au”（上期黄金）
	 * @param uDate 日期，格式为YYYYMMDD
	 * @param isTpl 是否使用模板，默认为false
	 * @return 如果是假日返回true，否则返回false
	 */
	virtual bool				isHoliday(const char* pid, uint32_t uDate, bool isTpl = false) = 0;

	/**
	 * @brief 计算交易日期
	 * @details 根据实际日期、时间和品种信息计算对应的交易日期
	 * 
	 * @param stdPID 标准化品种代码，格式为“交易所.品种代码”，如“SHFE.au”（上期黄金）
	 * @param uDate 当前日期，格式为YYYYMMDD
	 * @param uTime 当前时间，格式为HHMMSS
	 * @param isSession 是否使用交易时段，默认为false
	 * @return 计算出的交易日期，格式为YYYYMMDD
	 */
	virtual uint32_t			calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) = 0;

	/**
	 * @brief 获取交易时段边界时间
	 * @details 获取指定品种在指定交易日的交易时段边界时间（开始或结束时间）
	 * 
	 * @param stdPID 标准化品种代码，格式为“交易所.品种代码”，如“SHFE.au”（上期黄金）
	 * @param tDate 交易日期，格式为YYYYMMDD
	 * @param isSession 是否使用交易时段，默认为false
	 * @param isStart 是否获取开始时间，如果为false则获取结束时间，默认为true
	 * @return 边界时间，格式为YYYYMMDDHHMMSSmmm
	 */
	virtual uint64_t			getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) = 0;

	/**
	 * @brief 获取合约数量
	 * @details 获取指定交易所在指定日期的合约总数
	 * 
	 * @param exchg 交易所代码，如“SHFE”（上期），默认为空字符串，表示所有交易所
	 * @param uDate 日期，格式为YYYYMMDD，默认为0，表示当前日期
	 * @return 合约数量，子类必须实现此接口，基类中默认返回0
	 */
	virtual uint32_t			getContractSize(const char* exchg = "", uint32_t uDate = 0) { return 0; }
};
NS_WTP_END