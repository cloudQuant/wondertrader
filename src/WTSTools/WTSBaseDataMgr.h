/*!
 * \file WTSBaseDataMgr.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 基础数据管理器实现
 * 
 * 该文件定义了交易系统的基础数据管理器，负责管理交易品种、合约、交易时段和交易日历等基础数据
 * 为交易系统提供基础数据支持，包括获取品种信息、合约信息、交易时段信息以及交易日历计算等功能
 */
#pragma once
#include "../Includes/IBaseDataMgr.h"
#include "../Includes/WTSCollection.hpp"
#include "../Includes/FasterDefs.h"

USING_NS_WTP;

/*!
 * \brief 交易日历模板映射表类型
 * 用于存储不同交易日历模板的映射关系
 */
typedef wt_hashmap<std::string, TradingDayTpl>	TradingDayTplMap;

/*!
 * \brief 合约列表类型
 * 用于存储合约信息的哈希映射
 */
typedef WTSHashMap<std::string>		WTSContractList;

/*!
 * \brief 交易所合约映射类型
 * 用于存储交易所与其合约列表的映射关系
 */
typedef WTSHashMap<std::string>		WTSExchgContract;

/*!
 * \brief 合约映射类型
 * 用于存储合约代码到合约信息的映射
 */
typedef WTSHashMap<std::string>		WTSContractMap;

/*!
 * \brief 交易时段映射类型
 * 用于存储交易时段ID到交易时段信息的映射
 */
typedef WTSHashMap<std::string>		WTSSessionMap;

/*!
 * \brief 品种映射类型
 * 用于存储品种ID到品种信息的映射
 */
typedef WTSHashMap<std::string>		WTSCommodityMap;

/*!
 * \brief 交易时段代码集合映射类型
 * 用于存储交易时段ID到该时段包含的代码集合的映射
 */
typedef wt_hashmap<std::string, CodeSet> SessionCodeMap;

/*!
 * \brief 基础数据管理器类
 * 
 * 负责管理交易系统所需的各类基础数据，包括：
 * - 交易品种信息
 * - 合约信息
 * - 交易时段信息
 * - 交易日历信息
 * 
 * 提供数据加载、查询和交易日期计算等功能
 */
class WTSBaseDataMgr : public IBaseDataMgr
{
public:
	/*!
	 * \brief 构造函数
	 * 
	 * 初始化基础数据管理器，创建各类数据容器
	 */
	WTSBaseDataMgr();

	/*!
	 * \brief 析构函数
	 * 
	 * 释放所有数据资源
	 */
	~WTSBaseDataMgr();

public:
	/*!
	 * \brief 根据标准品种ID获取品种信息
	 * \param stdPID 标准品种ID，格式为“交易所.品种代码”
	 * \return 品种信息指针，如果不存在则返回NULL
	 */
	virtual WTSCommodityInfo*	getCommodity(const char* stdPID) override;

	/*!
	 * \brief 根据交易所和品种代码获取品种信息
	 * \param exchg 交易所代码
	 * \param pid 品种代码
	 * \return 品种信息指针，如果不存在则返回NULL
	 */
	virtual WTSCommodityInfo*	getCommodity(const char* exchg, const char* pid) override;

	/*!
	 * \brief 获取合约信息
	 * \param code 合约代码
	 * \param exchg 交易所代码，默认为空字符串
	 * \param uDate 日期，默认为0，如果非0则检查合约在该日期是否有效
	 * \return 合约信息指针，如果不存在则返回NULL
	 */
	virtual WTSContractInfo*	getContract(const char* code, const char* exchg = "", uint32_t uDate = 0) override;

	/*!
	 * \brief 获取合约列表
	 * \param exchg 交易所代码，默认为空字符串，表示获取所有交易所的合约
	 * \param uDate 日期，默认为0，如果非0则只返回在该日期有效的合约
	 * \return 合约信息数组
	 */
	virtual WTSArray*			getContracts(const char* exchg = "", uint32_t uDate = 0) override;

	/*!
	 * \brief 根据交易时段ID获取交易时段信息
	 * \param sid 交易时段ID
	 * \return 交易时段信息指针，如果不存在则返回NULL
	 */
	virtual WTSSessionInfo*		getSession(const char* sid) override;

	/*!
	 * \brief 根据合约代码获取其对应的交易时段信息
	 * \param code 合约代码
	 * \param exchg 交易所代码，默认为空字符串
	 * \return 交易时段信息指针，如果不存在则返回NULL
	 */
	virtual WTSSessionInfo*		getSessionByCode(const char* code, const char* exchg = "") override;

	/*!
	 * \brief 获取所有交易时段信息
	 * \return 交易时段信息数组
	 */
	virtual WTSArray*			getAllSessions() override;

	/*!
	 * \brief 检查指定日期是否为节假日
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uDate 日期，格式为YYYYMMDD
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 * \return 如果是节假日则返回true，否则返回false
	 */
	virtual bool				isHoliday(const char* stdPID, uint32_t uDate, bool isTpl = false) override;

	/*!
	 * \brief 计算交易日期
	 * \param stdPID 标准品种ID或交易时段ID
	 * \param uDate 自然日期，格式为YYYYMMDD
	 * \param uTime 时间，格式为HHMM或HHMMSS
	 * \param isSession 是否为交易时段ID，默认为false
	 * \return 交易日期，格式为YYYYMMDD
	 */
	virtual uint32_t			calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession = false) override;

	/*!
	 * \brief 获取交易时段边界时间
	 * \param stdPID 标准品种ID或交易时段ID
	 * \param tDate 交易日期，格式为YYYYMMDD
	 * \param isSession 是否为交易时段ID，默认为false
	 * \param isStart 是否为开始时间，默认为true，否则为结束时间
	 * \return 边界时间，格式为YYYYMMDDHHMMSS
	 */
	virtual uint64_t			getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession = false, bool isStart = true) override;

	/*!
	 * \brief 获取合约数量
	 * \param exchg 交易所代码，默认为空字符串，表示获取所有交易所的合约数量
	 * \param uDate 日期，默认为0，如果非0则只计算在该日期有效的合约
	 * \return 合约数量
	 */
	virtual uint32_t			getContractSize(const char* exchg = "", uint32_t uDate = 0) override;

	/*!
	 * \brief 释放资源
	 * 
	 * 释放基础数据管理器所持有的所有资源
	 */
	void		release();

	/*!
	 * \brief 加载交易时段数据
	 * \param filename 交易时段配置文件路径
	 * \return 加载是否成功
	 */
	bool		loadSessions(const char* filename);

	/*!
	 * \brief 加载品种数据
	 * \param filename 品种配置文件路径
	 * \return 加载是否成功
	 */
	bool		loadCommodities(const char* filename);

	/*!
	 * \brief 加载合约数据
	 * \param filename 合约配置文件路径
	 * \return 加载是否成功
	 */
	bool		loadContracts(const char* filename);

	/*!
	 * \brief 加载节假日数据
	 * \param filename 节假日配置文件路径
	 * \return 加载是否成功
	 */
	bool		loadHolidays(const char* filename);

public:
	/*!
	 * \brief 获取交易日期
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uOffDate 偏移日期，默认为0，表示使用当前日期
	 * \param uOffMinute 偏移分钟数，默认为0
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 * \return 交易日期，格式为YYYYMMDD
	 */
	uint32_t	getTradingDate(const char* stdPID, uint32_t uOffDate = 0, uint32_t uOffMinute = 0, bool isTpl = false);

	/*!
	 * \brief 获取下一个交易日
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uDate 起始日期，格式为YYYYMMDD
	 * \param days 前进的交易日数，默认为1
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 * \return 下一个交易日，格式为YYYYMMDD
	 */
	uint32_t	getNextTDate(const char* stdPID, uint32_t uDate, int days = 1, bool isTpl = false);

	/*!
	 * \brief 获取上一个交易日
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uDate 起始日期，格式为YYYYMMDD
	 * \param days 后退的交易日数，默认为1
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 * \return 上一个交易日，格式为YYYYMMDD
	 */
	uint32_t	getPrevTDate(const char* stdPID, uint32_t uDate, int days = 1, bool isTpl = false);

	/*!
	 * \brief 检查指定日期是否为交易日
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uDate 日期，格式为YYYYMMDD
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 * \return 如果是交易日则返回true，否则返回false
	 */
	bool		isTradingDate(const char* stdPID, uint32_t uDate, bool isTpl = false);

	/*!
	 * \brief 设置当前交易日
	 * \param stdPID 标准品种ID或交易日历模板ID
	 * \param uDate 交易日期，格式为YYYYMMDD
	 * \param isTpl 是否为交易日历模板ID，默认为false
	 */
	void		setTradingDate(const char* stdPID, uint32_t uDate, bool isTpl = false);

	/*!
	 * \brief 获取交易时段对应的代码集合
	 * \param sid 交易时段ID
	 * \return 代码集合指针，如果不存在则返回NULL
	 */
	CodeSet*	getSessionComms(const char* sid);

private:
	/*!
	 * \brief 根据标准品种ID获取交易日历模板ID
	 * \param stdPID 标准品种ID，格式为“交易所.品种代码”
	 * \return 交易日历模板ID，如果不存在则返回空字符串
	 */
	const char* getTplIDByPID(const char* stdPID);

private:
	/*! 交易日历模板映射表 */
	TradingDayTplMap	m_mapTradingDay;

	/*! 交易时段代码集合映射表 */
	SessionCodeMap		m_mapSessionCode;

	/*! 交易所合约映射表 */
	WTSExchgContract*	m_mapExchgContract;
	/*! 交易时段映射表 */
	WTSSessionMap*		m_mapSessions;
	/*! 品种映射表 */
	WTSCommodityMap*	m_mapCommodities;
	/*! 合约映射表 */
	WTSContractMap*		m_mapContracts;
};

