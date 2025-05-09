/*!
 * \file WTSBaseDataMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 基础数据管理器实现文件
 * 
 * 本文件实现了WTSBaseDataMgr类，该类负责管理交易系统所需的基础数据，
 * 包括交易品种、合约信息、交易时段、交易日历等。
 * 这些基础数据对于交易系统的正常运行至关重要。
 */
#include "WTSBaseDataMgr.h"
#include "../WTSUtils/WTSCfgLoader.h"
#include "WTSLogger.h"

#include "../Includes/WTSContractInfo.hpp"
#include "../Includes/WTSSessionInfo.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

/**
 * @brief 默认的节假日模板ID
 * 
 * 用于在没有指定节假日模板时使用的默认模板，默认为中国节假日模板
 */
const char* DEFAULT_HOLIDAY_TPL = "CHINA";

/**
 * @brief 构造函数
 * 
 * 初始化基础数据管理器，创建各种数据容器用于存储交易所、交易时段、品种和合约信息
 */
WTSBaseDataMgr::WTSBaseDataMgr()
	: m_mapExchgContract(NULL)
	, m_mapSessions(NULL)
	, m_mapCommodities(NULL)
	, m_mapContracts(NULL)
{
	// 创建交易所-合约映射容器
	m_mapExchgContract = WTSExchgContract::create();
	// 创建交易时段映射容器
	m_mapSessions = WTSSessionMap::create();
	// 创建品种映射容器
	m_mapCommodities = WTSCommodityMap::create();
	// 创建合约映射容器
	m_mapContracts = WTSContractMap::create();
}


/**
 * @brief 析构函数
 * 
 * 释放基础数据管理器所持有的各种数据容器资源
 */
WTSBaseDataMgr::~WTSBaseDataMgr()
{
	// 释放交易所-合约映射容器
	if (m_mapExchgContract)
	{
		m_mapExchgContract->release();
		m_mapExchgContract = NULL;
	}

	// 释放交易时段映射容器
	if (m_mapSessions)
	{
		m_mapSessions->release();
		m_mapSessions = NULL;
	}

	// 释放品种映射容器
	if (m_mapCommodities)
	{
		m_mapCommodities->release();
		m_mapCommodities = NULL;
	}

	// 释放合约映射容器
	if(m_mapContracts)
	{
		m_mapContracts->release();
		m_mapContracts = NULL;
	}
}

/**
 * @brief 获取品种信息
 * 
 * 根据标准品种ID获取相应的品种信息
 * 
 * @param exchgpid 标准品种ID，格式为"交易所.品种代码"，如"SHFE.au"
 * @return WTSCommodityInfo* 返回品种信息指针，如果不存在则返回NULL
 */
WTSCommodityInfo* WTSBaseDataMgr::getCommodity(const char* exchgpid)
{
	return (WTSCommodityInfo*)m_mapCommodities->get(exchgpid);
}


/**
 * @brief 获取品种信息
 * 
 * 根据交易所代码和品种代码获取相应的品种信息
 * 
 * @param exchg 交易所代码，如"SHFE"
 * @param pid 品种代码，如"au"
 * @return WTSCommodityInfo* 返回品种信息指针，如果不存在则返回NULL
 */
WTSCommodityInfo* WTSBaseDataMgr::getCommodity(const char* exchg, const char* pid)
{
	// 检查品种映射容器是否存在
	if (m_mapCommodities == NULL)
		return NULL;

	// 构造查询键值，格式为"交易所.品种代码"
	char key[64] = { 0 };
	fmt::format_to(key, "{}.{}", exchg, pid);

	// 从品种映射容器中获取品种信息
	return (WTSCommodityInfo*)m_mapCommodities->get(key);
}


/**
 * @brief 获取合约信息
 * 
 * 根据合约代码、交易所和日期获取合约信息
 * 
 * @param code 合约代码，如"au2106"
 * @param exchg 交易所代码，默认为空字符串
 * @param uDate 日期，格式为YYYYMMDD，如果非零，则检查合约在该日期是否有效
 * @return WTSContractInfo* 返回合约信息指针，如果不存在则返回NULL
 */
WTSContractInfo* WTSBaseDataMgr::getContract(const char* code, const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	//如果直接找到对应的市场代码,则直接
	
	// 将合约代码转换为字符串作为查询键
	auto lKey = std::string(code);

	// 如果没有指定交易所，则从合约映射容器中直接查找
	if (strlen(exchg) == 0)
	{
		// 在合约映射容器中查找合约代码
		auto it = m_mapContracts->find(lKey);
		if (it == m_mapContracts->end())
			return NULL;

		// 获取合约数组
		WTSArray* ayInst = (WTSArray*)it->second;
		if (ayInst == NULL || ayInst->size() == 0)
			return NULL;

		// 遍历合约数组，找到符合条件的合约
		for(std::size_t i = 0; i < ayInst->size(); i++)
		{
			WTSContractInfo* cInfo = (WTSContractInfo*)ayInst->at(i);
			/*
			 *	By Wesley @ 2023.10.23
			 *	if param uDate is not zero, need to check whether contract is valid
			 */
			// 如果指定了日期，检查合约在该日期是否有效
			if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
				return cInfo;
		}
		return NULL;
	}
	else
	{
		// 如果指定了交易所，则先从交易所-合约映射容器中查找交易所
		auto sKey = std::string(exchg);
		auto it = m_mapExchgContract->find(sKey);
		if (it != m_mapExchgContract->end())
		{
			// 获取交易所对应的合约列表
			WTSContractList* contractList = (WTSContractList*)it->second;
			// 在合约列表中查找指定的合约代码
			auto it = contractList->find(lKey);
			if (it != contractList->end())
			{
				WTSContractInfo* cInfo = (WTSContractInfo*)it->second;
				/*
				 *	By Wesley @ 2023.10.23
				 *	if param uDate is not zero, need to check whether contract is valid
				 */
				// 如果指定了日期，检查合约在该日期是否有效
				if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
					return cInfo;
			}

			return NULL;
		}
	}

	return NULL;
}

/**
 * @brief 获取合约数量
 * 
 * 获取指定交易所和日期的合约数量
 * 
 * @param exchg 交易所代码，默认为空字符串，如果为空则返回所有交易所的合约数量
 * @param uDate 日期，格式为YYYYMMDD，如果非零，则只计算在该日期有效的合约
 * @return uint32_t 返回合约数量
 */
uint32_t  WTSBaseDataMgr::getContractSize(const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	uint32_t ret = 0;
	if (strlen(exchg) > 0)
	{
		auto it = m_mapExchgContract->find(std::string(exchg));
		if (it != m_mapExchgContract->end())
		{
			WTSContractList* contractList = (WTSContractList*)it->second;
			auto it2 = contractList->begin();
			for (; it2 != contractList->end(); it2++)
			{
				WTSContractInfo* cInfo = (WTSContractInfo*)it2->second;
				if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
					ret++;
			}
		}
	}
	else
	{
		auto it = m_mapExchgContract->begin();
		for (; it != m_mapExchgContract->end(); it++)
		{
			WTSContractList* contractList = (WTSContractList*)it->second;
			auto it2 = contractList->begin();
			for (; it2 != contractList->end(); it2++)
			{
				WTSContractInfo* cInfo = (WTSContractInfo*)it2->second;
				if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
					ret++;
			}
		}
	}

	return ret;
}

/**
 * @brief 获取合约列表
 * 
 * 获取指定交易所和日期的合约列表，返回的列表按合约代码排序
 * 
 * @param exchg 交易所代码，默认为空字符串，如果为空则返回所有交易所的合约
 * @param uDate 日期，格式为YYYYMMDD，如果非零，则只返回在该日期有效的合约
 * @return WTSArray* 返回合约列表，其中包含符合条件的WTSContractInfo对象
 */
WTSArray* WTSBaseDataMgr::getContracts(const char* exchg /* = "" */, uint32_t uDate /* = 0 */)
{
	WTSArray* ay = WTSArray::create();
	if(strlen(exchg) > 0)
	{
		auto it = m_mapExchgContract->find(std::string(exchg));
		if (it != m_mapExchgContract->end())
		{
			WTSContractList* contractList = (WTSContractList*)it->second;
			auto it2 = contractList->begin();
			for (; it2 != contractList->end(); it2++)
			{
				WTSContractInfo* cInfo = (WTSContractInfo*)it2->second;
				/*
				 *	By Wesley @ 2023.10.23
				 *	if param uDate is not zero, need to check whether contract is valid
				 */
				if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
					ay->append(cInfo, true);
			}
		}
	}
	else
	{
		auto it = m_mapExchgContract->begin();
		for (; it != m_mapExchgContract->end(); it++)
		{
			WTSContractList* contractList = (WTSContractList*)it->second;
			auto it2 = contractList->begin();
			for (; it2 != contractList->end(); it2++)
			{
				WTSContractInfo* cInfo = (WTSContractInfo*)it2->second;
				/*
				 *	By Wesley @ 2023.10.23
				 *	if param uDate is not zero, need to check whether contract is valid
				 */
				if (uDate == 0 || (cInfo->getOpenDate() <= uDate && cInfo->getExpireDate() >= uDate))
					ay->append(cInfo, true);
			}
		}
	}

	return ay;
}

/**
 * @brief 获取所有交易时段
 * 
 * 返回系统中所有已加载的交易时段信息
 * 
 * @return WTSArray* 包含所有交易时段(WTSSessionInfo)的数组
 */
WTSArray* WTSBaseDataMgr::getAllSessions()
{
	WTSArray* ay = WTSArray::create();
	for (auto it = m_mapSessions->begin(); it != m_mapSessions->end(); it++)
	{
		ay->append(it->second, true);
	}
	return ay;
}

/**
 * @brief 获取指定的交易时段
 * 
 * 根据交易时段ID获取相应的交易时段信息
 * 
 * @param sid 交易时段ID
 * @return WTSSessionInfo* 交易时段信息对象，如果不存在则返回NULL
 */
WTSSessionInfo* WTSBaseDataMgr::getSession(const char* sid)
{
	return (WTSSessionInfo*)m_mapSessions->get(sid);
}

/**
 * @brief 根据合约代码获取交易时段
 * 
 * 根据合约代码和交易所获取相应的交易时段信息
 * 
 * @param code 合约代码
 * @param exchg 交易所代码，默认为空字符串
 * @return WTSSessionInfo* 交易时段信息对象，如果合约不存在或无相应交易时段则返回NULL
 */
WTSSessionInfo* WTSBaseDataMgr::getSessionByCode(const char* code, const char* exchg /* = "" */)
{
	// 先获取合约信息
	WTSContractInfo* ct = getContract(code, exchg);
	if (ct == NULL)
		return NULL;

	// 通过合约信息获取品种信息，再获取交易时段信息
	return ct->getCommInfo()->getSessionInfo();
}

/**
 * @brief 检查指定日期是否为节假日
 * 
 * 根据品种ID或交易日历模板ID检查指定日期是否为节假日
 * 
 * @param pid 品种ID或交易日历模板ID，取决于isTpl参数
 * @param uDate 要检查的日期，格式为YYYYMMDD
 * @param isTpl 指示pid参数是否为交易日历模板ID，默认为false表示为品种ID
 * @return bool 如果是节假日返回true，否则返回false
 */
bool WTSBaseDataMgr::isHoliday(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	// 先检查是否为周末，周六和周日算作节假日
	uint32_t wd = TimeUtils::getWeekDay(uDate);
	if (wd == 0 || wd == 6)
		return true;

	// 获取交易日历模板ID
	std::string tplid = pid;
	if (!isTpl)
		tplid = getTplIDByPID(pid);

	// 在交易日模板映射中查找指定的模板
	auto it = m_mapTradingDay.find(tplid.c_str());
	if(it != m_mapTradingDay.end())
	{
		// 获取模板并检查指定日期是否在节假日集合中
		const TradingDayTpl& tpl = it->second;
		return (tpl._holidays.find(uDate) != tpl._holidays.end());
	}

	return false;
}


/**
 * @brief 释放系统资源
 * 
 * 释放基础数据管理器中保存的关键容器和资源
 * 调用此函数后，不应再使用其他数据获取函数
 */
void WTSBaseDataMgr::release()
{
	// 释放交易所-合约映射
	if (m_mapExchgContract)
	{
		m_mapExchgContract->release();
		m_mapExchgContract = NULL;
	}

	// 释放交易时段映射
	if (m_mapSessions)
	{
		m_mapSessions->release();
		m_mapSessions = NULL;
	}

	// 释放品种映射
	if (m_mapCommodities)
	{
		m_mapCommodities->release();
		m_mapCommodities = NULL;
	}
}

/**
 * @brief 加载交易时段配置
 * 
 * 从指定的配置文件中加载交易时段信息，包括正常交易时段和集合端占时间
 * 
 * @param filename 交易时段配置文件路径
 * @return bool 加载成功返回true，否则返回false
 */
bool WTSBaseDataMgr::loadSessions(const char* filename)
{
	// 检查文件是否存在
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Trading sessions configuration file {} not exists", filename);
		return false;
	}

	// 加载配置文件
	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading session config file {} failed", filename);
		return false;
	}

	// 遍历所有交易时段定义
	for(const std::string& id : root->memberNames())
	{
		WTSVariant* jVal = root->get(id);

		// 获取时段名称和时区偏移
		const char* name = jVal->getCString("name");
		int32_t offset = jVal->getInt32("offset");

		// 创建交易时段对象
		WTSSessionInfo* sInfo = WTSSessionInfo::create(id.c_str(), name, offset);

		// 处理集合端占配置，支持单个时段和多个时段两种格式
		if (jVal->has("auction"))
		{
			WTSVariant* jAuc = jVal->get("auction");
			sInfo->setAuctionTime(jAuc->getUInt32("from"), jAuc->getUInt32("to"));
		}
		else if (jVal->has("auctions"))
		{
			WTSVariant* jAucs = jVal->get("auctions");
			for (uint32_t i = 0; i < jAucs->size(); i++)
			{
				WTSVariant* jSec = jAucs->get(i);
				sInfo->addAuctionTime(jSec->getUInt32("from"), jSec->getUInt32("to"));
			}
		}

		// 处理正常交易时段配置
		WTSVariant* jSecs = jVal->get("sections");
		if (jSecs == NULL || !jSecs->isArray())
			continue;

		// 遍历添加所有交易时段
		for (uint32_t i = 0; i < jSecs->size(); i++)
		{
			WTSVariant* jSec = jSecs->get(i);
			sInfo->addTradingSection(jSec->getUInt32("from"), jSec->getUInt32("to"));
		}

		// 将交易时段添加到全局映射中
		m_mapSessions->add(id.c_str(), sInfo);
	}

	// 释放配置对象
	root->release();

	return true;
}

/**
 * @brief 解析品种信息
 * 
 * 从配置对象中提取品种相关属性，并设置到品种信息对象中
 * 
 * @param pCommInfo 品种信息对象指针
 * @param jPInfo 包含品种配置的变体对象
 */
void parseCommodity(WTSCommodityInfo* pCommInfo, WTSVariant* jPInfo)
{
	// 设置最小变动单位
	pCommInfo->setPriceTick(jPInfo->getDouble("pricetick"));
	// 设置合约数量乘数
	pCommInfo->setVolScale(jPInfo->getUInt32("volscale"));

	// 设置合约类别，如期货、期权等，默认为期货
	if (jPInfo->has("category"))
		pCommInfo->setCategory((ContractCategory)jPInfo->getUInt32("category"));
	else
		pCommInfo->setCategory(CC_Future);

	// 设置平仓模式
	pCommInfo->setCoverMode((CoverMode)jPInfo->getUInt32("covermode"));
	// 设置价格模式
	pCommInfo->setPriceMode((PriceMode)jPInfo->getUInt32("pricemode"));

	// 设置交易模式，默认为多空都可交易
	if (jPInfo->has("trademode"))
		pCommInfo->setTradingMode((TradingMode)jPInfo->getUInt32("trademode"));
	else
		pCommInfo->setTradingMode(TM_Both);

	// 设置手数最小变动单位和最小交易手数
	double lotsTick = 1;
	double minLots = 1;
	if (jPInfo->has("lotstick"))
		lotsTick = jPInfo->getDouble("lotstick");
	if (jPInfo->has("minlots"))
		minLots = jPInfo->getDouble("minlots");
	pCommInfo->setLotsTick(lotsTick);
	pCommInfo->setMinLots(minLots);
}

/**
 * @brief 加载品种配置
 * 
 * 从指定的配置文件中加载交易品种信息，包括品种各种属性和相关交易参数
 * 
 * @param filename 品种配置文件路径
 * @return bool 加载成功返回true，否则返回false
 */
bool WTSBaseDataMgr::loadCommodities(const char* filename)
{
	// 检查配置文件是否存在
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Commodities configuration file {} not exists", filename);
		return false;
	}

	// 加载配置文件
	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading commodities config file {} failed", filename);
		return false;
	}

	// 遍历所有交易所
	for(const std::string& exchg : root->memberNames())
	{
		WTSVariant* jExchg = root->get(exchg);

		// 遍历当前交易所下的所有品种
		for (const std::string& pid : jExchg->memberNames())
		{
			WTSVariant* jPInfo = jExchg->get(pid);

			// 获取品种名称、交易时段ID和节假日模板ID
			const char* name = jPInfo->getCString("name");
			const char* sid = jPInfo->getCString("session");
			const char* hid = jPInfo->getCString("holiday");

			// 确保交易时段配置有效
			if (strlen(sid) == 0)
			{
				WTSLogger::warn("No session configured for {}.{}", exchg.c_str(), pid.c_str());
				continue;
			}

			// 创建品种信息对象并进行属性设置
			WTSCommodityInfo* pCommInfo = WTSCommodityInfo::create(pid.c_str(), name, exchg.c_str(), sid, hid);
			parseCommodity(pCommInfo, jPInfo);

			// 关联交易时段信息
			WTSSessionInfo* sInfo = getSession(sid);
			pCommInfo->setSessionInfo(sInfo);

			// 生成品种标准键值并添加到品种映射
			std::string key = fmt::format("{}.{}", exchg.c_str(), pid.c_str());
			if (m_mapCommodities == NULL)
				m_mapCommodities = WTSCommodityMap::create();

			m_mapCommodities->add(key, pCommInfo, false);

			// 建立交易时段和品种的双向关联
			m_mapSessionCode[sid].insert(key);
		}
	}

	WTSLogger::info("Commodities configuration file {} loaded", filename);
	root->release();
	return true;
}

/**
 * @brief 加载合约配置
 * 
 * 从指定的配置文件中加载合约信息，合约与品种关联
 * 如果合约没有关联品种，但有rules属性，会自动将合约作为单独品种添加
 * 
 * @param filename 合约配置文件路径
 * @return bool 加载成功返回true，否则返回false
 */
bool WTSBaseDataMgr::loadContracts(const char* filename)
{
	// 检查配置文件是否存在
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Contracts configuration file {} not exists", filename);
		return false;
	}

	// 加载配置文件
	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading contracts config file {} failed", filename);
		return false;
	}

	for(const std::string& exchg : root->memberNames())
	{
		WTSVariant* jExchg = root->get(exchg);

		for(const std::string& code : jExchg->memberNames())
		{
			WTSVariant* jcInfo = jExchg->get(code);

			/*
			 *	By Wesley @ 2021.12.28
			 *	这里做一个兼容，如果product为空,先检查是否配置了rules属性，如果配置了rules属性，把合约单独当成品种自动加入
			 *	如果没有配置rules，则直接跳过该合约
			 */
			WTSCommodityInfo* commInfo = NULL;
			std::string pid;
			if(jcInfo->has("product"))
			{
				pid = jcInfo->getCString("product");
				commInfo = getCommodity(jcInfo->getCString("exchg"), pid.c_str());
			}
			else if(jcInfo->has("rules"))
			{
				pid = code.c_str();
				WTSVariant* jPInfo = jcInfo->get("rules");
				const char* name = jcInfo->getCString("name");
				std::string sid = jPInfo->getCString("session");
				std::string hid;
				if(jPInfo->has("holiday"))
					hid = jPInfo->getCString("holiday");

				//这里不能像解析commodity那样处理，直接赋值为ALLDAY
				if (sid.empty())
					sid = "ALLDAY";

				commInfo = WTSCommodityInfo::create(pid.c_str(), name, exchg.c_str(), sid.c_str(), hid.c_str());
				parseCommodity(commInfo, jPInfo);
				WTSSessionInfo* sInfo = getSession(sid.c_str());
				commInfo->setSessionInfo(sInfo);

				std::string key = fmt::format("{}.{}", exchg.c_str(), pid.c_str());
				if (m_mapCommodities == NULL)
					m_mapCommodities = WTSCommodityMap::create();

				m_mapCommodities->add(key, commInfo, false);

				m_mapSessionCode[sid].insert(key);

				WTSLogger::debug("Commodity {} has been automatically added", key.c_str());
			}

			if (commInfo == NULL)
			{
				WTSLogger::warn("Commodity {}.{} not found, contract {} skipped", jcInfo->getCString("exchg"), jcInfo->getCString("product"), code.c_str());
				continue;
			}

			WTSContractInfo* cInfo = WTSContractInfo::create(code.c_str(),
				jcInfo->getCString("name"),
				jcInfo->getCString("exchg"),
				pid.c_str());

			cInfo->setCommInfo(commInfo);

			uint32_t maxMktQty = 1000000;
			uint32_t maxLmtQty = 1000000;
			uint32_t minMktQty = 1;
			uint32_t minLmtQty = 1;
			if (jcInfo->has("maxmarketqty"))
				maxMktQty = jcInfo->getUInt32("maxmarketqty");
			if (jcInfo->has("maxlimitqty"))
				maxLmtQty = jcInfo->getUInt32("maxlimitqty");
			if (jcInfo->has("minmarketqty"))
				minMktQty = jcInfo->getUInt32("minmarketqty");
			if (jcInfo->has("minlimitqty"))
				minLmtQty = jcInfo->getUInt32("minlimitqty");
			cInfo->setVolumeLimits(maxMktQty, maxLmtQty, minMktQty, minLmtQty);

			uint32_t opendate = 0;
			uint32_t expiredate = 0;
			if (jcInfo->has("opendate"))
				opendate = jcInfo->getUInt32("opendate");
			if (jcInfo->has("expiredate"))
				expiredate = jcInfo->getUInt32("expiredate");
			cInfo->setDates(opendate, expiredate);

			double lMargin = 0;
			double sMargin = 0;
			if (jcInfo->has("longmarginratio"))
				lMargin = jcInfo->getDouble("longmarginratio");
			if (jcInfo->has("shortmarginratio"))
				sMargin = jcInfo->getDouble("shortmarginratio");
			cInfo->setMarginRatios(lMargin, sMargin);

			WTSContractList* contractList = (WTSContractList*)m_mapExchgContract->get(std::string(cInfo->getExchg()));
			if (contractList == NULL)
			{
				contractList = WTSContractList::create();
				m_mapExchgContract->add(std::string(cInfo->getExchg()), contractList, false);
			}
			contractList->add(std::string(cInfo->getCode()), cInfo, false);

			commInfo->addCode(code.c_str());

			std::string key = std::string(cInfo->getCode());
			WTSArray* ayInst = (WTSArray*)m_mapContracts->get(key);
			if(ayInst == NULL)
			{
				ayInst = WTSArray::create();
				m_mapContracts->add(key, ayInst, false);
			}

			ayInst->append(cInfo, true);
		}
	}

	WTSLogger::info("Contracts configuration file {} loaded, {} exchanges", filename, m_mapExchgContract->size());
	root->release();
	return true;
}

/**
 * @brief 加载节假日配置
 * 
 * 从指定的配置文件中加载节假日信息，用于交易日历的计算
 * 每个节假日模板中包含一组具体的节假日日期
 * 
 * @param filename 节假日配置文件路径
 * @return bool 加载成功返回true，否则返回false
 */
bool WTSBaseDataMgr::loadHolidays(const char* filename)
{
	// 检查配置文件是否存在
	if (!StdFile::exists(filename))
	{
		WTSLogger::error("Holidays configuration file {} not exists", filename);
		return false;
	}

	// 加载配置文件
	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
	{
		WTSLogger::error("Loading holidays config file {} failed", filename);
		return false;
	}

	// 遍历节假日模板ID
	for (const std::string& hid : root->memberNames())
	{
		WTSVariant* jHolidays = root->get(hid);
		// 确保节假日数据是数组格式
		if(!jHolidays->isArray())
			continue;

		// 创建或获取对应模板的交易日结构
		TradingDayTpl& trdDayTpl = m_mapTradingDay[hid];
		// 遍历模板中的所有节假日日期
		for(uint32_t i = 0; i < jHolidays->size(); i++)
		{
			WTSVariant* hItem = jHolidays->get(i);
			// 将节假日日期添加到节假日集合中
			trdDayTpl._holidays.insert(hItem->asUInt32());
		}
	}

	root->release();

	return true;
}

/**
 * @brief 获取交易时段的边界时间
 * 
 * 计算指定品种或交易时段在特定日期的边界时间（开始或结束时间）
 * 能够处理不同的交易时段偏移情况，如国内期货的夜盘交易和国外市场的时区差异
 * 
 * @param stdPID 标准品种ID或交易时段ID，取决于isSession参数
 * @param tDate 交易日期，如果为0则使用当前日期
 * @param isSession 是否传入的是交易时段ID，true则stdPID是交易时段ID，false则stdPID是标准品种ID
 * @param isStart 是否获取开始时间，true为开盘时间，false为收盘时间
 * @return uint64_t 返回计算出的边界时间，格式为YYYYMMDDHHMM
 */
uint64_t WTSBaseDataMgr::getBoundaryTime(const char* stdPID, uint32_t tDate, bool isSession /* = false */, bool isStart /* = true */)
{
	// 如果日期为0，则使用当前日期
	if(tDate == 0)
		tDate = TimeUtils::getCurDate();
	
	// 设置节假日模板ID和待查询的交易时段信息
	std::string tplid = stdPID;
	bool isTpl = false;
	WTSSessionInfo* sInfo = NULL;
	
	// 如果传入的是交易时段ID
	if (isSession)
	{
		sInfo = getSession(stdPID);
		tplid = DEFAULT_HOLIDAY_TPL; // 使用默认的节假日模板
		isTpl = true;
	}
	else // 如果传入的是标准品种ID
	{
		// 获取品种信息并检查其有效性
		WTSCommodityInfo* cInfo = getCommodity(stdPID);
		if (cInfo == NULL)
			return 0;

		// 从品种信息中获取相关的交易时段信息
		sInfo = cInfo->getSessionInfo();
	}

	// 检查交易时段信息是否有效
	if (sInfo == NULL)
		return 0;

	// 处理周末情况，周六(6)和周日(0)需要调整到最近的交易日
	uint32_t weekday = TimeUtils::getWeekDay(tDate);
	if (weekday == 6 || weekday == 0)
	{
		// 根据是获取开始时间还是结束时间，分别调整到下一个或上一个交易日
		if (isStart)
			tDate = getNextTDate(tplid.c_str(), tDate, 1, isTpl);
		else
			tDate = getPrevTDate(tplid.c_str(), tDate, 1, isTpl);
	}

	// 情况1：没有时间偏移的交易时段（标准交易时段）
	// 不偏移的最简单,只需要直接返回开盘和收盘时间即可
	if (sInfo->getOffsetMins() == 0)
	{
		if (isStart)
			return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
		else
			return (uint64_t)tDate * 10000 + sInfo->getCloseTime();
	}

	// 情况2：负偏移的交易时段（往前偏移，一般用于外盘交易所）
	if(sInfo->getOffsetMins() < 0)
	{
		// 往前偏移,就是交易日推后,一般用于外盘
		// 这个比较简单,只需要按自然日取即可
		if (isStart)
			return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
		else
			// 收盘时间在下一个自然日
			return (uint64_t)TimeUtils::getNextDate(tDate) * 10000 + sInfo->getCloseTime();
	}
	// 情况3：正偏移的交易时段（往后偏移，如国内期货夜盘）
	else
	{
		// 往后偏移,一般国内期货夜盘都是这个,即夜盘算是第二个交易日
		// 这个比较复杂,主要节假日后第一天的边界很麻烦（如一般情况的周一）
		// 这种情况唯一方便的就是,收盘时间不需要处理
		if(!isStart)
			return (uint64_t)tDate * 10000 + sInfo->getCloseTime();

		// 想到一个简单的办法,就是不管怎么样,开始时间一定是上一个交易日的晚上
		// 所以我只需要拿到上一个交易日即可
		tDate = getPrevTDate(tplid.c_str(), tDate, 1, isTpl);
		return (uint64_t)tDate * 10000 + sInfo->getOpenTime();
	}
}

/**
 * @brief 计算交易日期
 * 
 * 根据特定时间点和品种/交易时段信息计算出实际的交易日期
 * 能够处理不同的交易时段偏移情况、节假日和周末
 * 
 * @param stdPID 标准品种ID或交易时段ID，取决于isSession参数
 * @param uDate 自然日期，格式YYYYMMDD，如果为0则使用当前日期
 * @param uTime 时间，格式HHMM或HHMMSS
 * @param isSession 是否传入的是交易时段ID，true则stdPID是交易时段ID，false则stdPID是标准品种ID
 * @return uint32_t 返回计算出的交易日期，格式YYYYMMDD
 */
uint32_t WTSBaseDataMgr::calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t uTime, bool isSession /* = false */)
{
	// 如果没有提供日期，则使用当前日期和时间
	if (uDate == 0)
	{
		TimeUtils::getDateTime(uDate, uTime);
		uTime /= 100000; // 将毫秒级时间转换为不带秒的时间
	}

	// 设置节假日模板ID和是否直接使用模板的标志
	std::string tplid = stdPID;
	bool isTpl = false;
	WTSSessionInfo* sInfo = NULL;
	
	// 根据参数确定使用交易时段ID还是品种ID
	if(isSession)
	{
		// 如果使用交易时段ID，直接获取交易时段信息
		sInfo = getSession(stdPID);
		tplid = DEFAULT_HOLIDAY_TPL; // 使用默认节假日模板
		isTpl = true;
	}
	else
	{
		// 如果使用品种ID，需要先获取品种信息
		WTSCommodityInfo* cInfo = getCommodity(stdPID);
		if (cInfo == NULL)
			return uDate; // 如果品种不存在，返回原始日期
		
		// 从品种信息中获取交易时段
		sInfo = cInfo->getSessionInfo();
	}

	// 如果未找到交易时段信息，返回原始日期
	if (sInfo == NULL)
		return uDate;
	
	// 计算时间偏移点，用于判断交易日的边界
	uint32_t offMin = sInfo->offsetTime(uTime, true);
	
	// 特殊情况7*24交易时间单独处理（全天候交易的设置）
	uint32_t total_mins = sInfo->getTradingMins();
	if(total_mins == 1440) // 1440分钟 = 24小时
	{
		// 正偏移情况下，如果当前时间大于偏移时间，交易日向后偏移
		if(sInfo->getOffsetMins() > 0 && uTime > offMin)
		{
			return TimeUtils::getNextDate(uDate, 1);
		}
		// 负偏移情况下，如果当前时间小于偏移时间，交易日向前偏移
		else if (sInfo->getOffsetMins() < 0 && uTime < offMin)
		{
			return TimeUtils::getNextDate(uDate, -1);
		}

		// 其他情况下交易日就是自然日
		return uDate;
	}

	// 获取当前日期的星期几，用于判断周末情况
	uint32_t weekday = TimeUtils::getWeekDay(uDate);
	
	// 情况1：正偏移情况（如国内期货夜盘）
	if (sInfo->getOffsetMins() > 0)
	{
		// 如果向后偏移,且当前时间大于偏移时间,说明向后跨日了
		// 这时交易日=下一个交易日
		if (uTime > offMin)
		{
			// 例如，20151016 23:00,偏移300分钟,时间边界为5:00
			// 因为23:00大于5:00，所以已经属于下一个交易日
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
		else if (weekday == 6 || weekday == 0)
		{
			// 如果是周末，例如，20151017 1:00,周六,交易日为20151019
			// 周末需要跳过到下一个交易日
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
	}
	// 情况2：负偏移情况（如一些外盘交易所）
	else if (sInfo->getOffsetMins() < 0)
	{
		// 如果向前偏移,且当前时间小于偏移时间,说明还是前一个交易日
		// 这时交易日=前一个交易日
		if (uTime < offMin)
		{
			// 例如20151017 1:00,偏移-300分钟,时间边界为20:00
			// 因1:00小于20:00，所以还属于前一个交易日
			return getPrevTDate(tplid.c_str(), uDate, 1, isTpl);
		}
		else if (weekday == 6 || weekday == 0)
		{
			// 因为向前偏移,如果在周末,则直接到下一个交易日
			// 周末无论如何都需要跳到下一个有效交易日
			return getNextTDate(tplid.c_str(), uDate, 1, isTpl);
		}
	}
	// 情况3：无偏移情况下的周末处理
	else if (weekday == 6 || weekday == 0)
	{
		// 如果没有偏移,且在周末,则直接读取下一个交易日
		// 周末需要跳过到下一个交易日
		return getNextTDate(tplid.c_str(), uDate, 1, isTpl);;
	}

	// 其他所有情况,交易日就等于自然日
	return uDate;
}

/**
 * @brief 获取交易日期
 * 
 * 根据品种ID或模板ID获取当前的交易日期
 * 处理周末情况，并支持缓存交易日期结果
 * 
 * @param pid 品种ID或节假日模板ID，取决于isTpl参数
 * @param uOffDate 偏移日期，默认为0表示使用当前日期
 * @param uOffMinute 时间偏移分钟数（保留参数，当前未使用）
 * @param isTpl 是否传入的是模板ID
 * @return uint32_t 返回计算出的交易日期，格式YYYYMMDD
 */
uint32_t WTSBaseDataMgr::getTradingDate(const char* pid, uint32_t uOffDate /* = 0 */, uint32_t uOffMinute /* = 0 */, bool isTpl /* = false */)
{
	// 获取节假日模板ID，如果传入的是品种ID，则需要转换
	const char* tplID = isTpl ? pid : getTplIDByPID(pid);

	// 获取当前日期
	uint32_t curDate = TimeUtils::getCurDate();
	// 在交易日模板映射中查找指定的模板ID
	auto it = m_mapTradingDay.find(tplID);
	if (it == m_mapTradingDay.end())
	{
		// 如果模板不存在，返回当前日期
		return curDate;
	}

	// 获取交易日模板
	TradingDayTpl* tpl = (TradingDayTpl*)&it->second;
	// 如果模板已经有缓存的交易日期且使用当前日期，直接返回
	if (tpl->_cur_tdate != 0 && uOffDate == 0)
		return tpl->_cur_tdate;

	// 如果没有指定偏移日期，使用当前日期
	if (uOffDate == 0)
		uOffDate = curDate;

	// 获取偏移日期的星期几
	uint32_t weekday = TimeUtils::getWeekDay(uOffDate);

	// 处理周末情况，周六(6)和周日(0)需要调整到下一个交易日
	if (weekday == 6 || weekday == 0)
	{
		// 如果没有偏移,且在周末,则直接读取下一个交易日
		tpl->_cur_tdate = getNextTDate(tplID, uOffDate, 1, true);
		uOffDate = tpl->_cur_tdate;
	}

	// 其他情况,交易日等于自然日
	return uOffDate;
}

uint32_t WTSBaseDataMgr::getNextTDate(const char* pid, uint32_t uDate, int days /* = 1 */, bool isTpl /* = false */)
{
	uint32_t curDate = uDate;
	int left = days;
	while (true)
	{
		tm t;
		memset(&t, 0, sizeof(tm));
		t.tm_year = curDate / 10000 - 1900;
		t.tm_mon = (curDate % 10000) / 100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts += 86400;

		tm* newT = localtime(&ts);
		curDate = (newT->tm_year + 1900) * 10000 + (newT->tm_mon + 1) * 100 + newT->tm_mday;
		if (newT->tm_wday != 0 && newT->tm_wday != 6 && !isHoliday(pid, curDate, isTpl))
		{
			//如果不是周末,也不是节假日,则剩余的天数-1
			left--;
			if (left == 0)
				return curDate;
		}
	}
}

uint32_t WTSBaseDataMgr::getPrevTDate(const char* pid, uint32_t uDate, int days /* = 1 */, bool isTpl /* = false */)
{
	uint32_t curDate = uDate;
	int left = days;
	while (true)
	{
		tm t;
		memset(&t, 0, sizeof(tm));
		t.tm_year = curDate / 10000 - 1900;
		t.tm_mon = (curDate % 10000) / 100 - 1;
		t.tm_mday = curDate % 100;
		//t.tm_isdst 	
		time_t ts = mktime(&t);
		ts -= 86400;

		tm* newT = localtime(&ts);
		curDate = (newT->tm_year + 1900) * 10000 + (newT->tm_mon + 1) * 100 + newT->tm_mday;
		if (newT->tm_wday != 0 && newT->tm_wday != 6 && !isHoliday(pid, curDate, isTpl))
		{
			//如果不是周末,也不是节假日,则剩余的天数-1
			left--;
			if (left == 0)
				return curDate;
		}
	}
}

bool WTSBaseDataMgr::isTradingDate(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	uint32_t wd = TimeUtils::getWeekDay(uDate);
	if (wd != 0 && wd != 6 && !isHoliday(pid, uDate, isTpl))
	{
		return true;
	}

	return false;
}

void WTSBaseDataMgr::setTradingDate(const char* pid, uint32_t uDate, bool isTpl /* = false */)
{
	std::string tplID = pid;
	if (!isTpl)
		tplID = getTplIDByPID(pid);

	auto it = m_mapTradingDay.find(tplID);
	if (it == m_mapTradingDay.end())
		return;

	TradingDayTpl* tpl = (TradingDayTpl*)&it->second;
	tpl->_cur_tdate = uDate;
}


CodeSet* WTSBaseDataMgr::getSessionComms(const char* sid)
{
	auto it = m_mapSessionCode.find(sid);
	if (it == m_mapSessionCode.end())
		return NULL;

	return (CodeSet*)&it->second;
}

const char* WTSBaseDataMgr::getTplIDByPID(const char* pid)
{
	const StringVector& ay = StrUtil::split(pid, ".");
	WTSCommodityInfo* commInfo = getCommodity(ay[0].c_str(), ay[1].c_str());
	if (commInfo == NULL)
		return "";

	return commInfo->getTradingTpl();
}