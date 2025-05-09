/*!
 * \file WTSHotMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 主力合约管理器实现
 *
 * 实现WTSHotMgr类的各种方法，包括主力合约、次主力合约和自定义切换规则的加载、查询和管理。
 * 主要功能包括读取切换配置文件、根据日期查询主力和次主力合约、判断合约类型、计算复权因子等。
 */
#include "WTSHotMgr.h"
#include "../WTSUtils/WTSCfgLoader.h"

#include "../Includes/WTSSwitchItem.hpp"
#include "../Includes/WTSVariant.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/CodeHelper.hpp"
#include "../Share/StdUtils.hpp"
#include "../Share/decimal.h"


/**
 * @brief 构造函数
 * 
 * 初始化主力合约管理器对象，设置自定义规则映射为空，初始化状态为未初始化
 */
WTSHotMgr::WTSHotMgr()
	: m_mapCustRules(NULL)
	, m_bInitialized(false)
{
}


/**
 * @brief 析构函数
 * 
 * 析构时不需要释放内存，因为内存释放由release()函数后续调用处理
 */
WTSHotMgr::~WTSHotMgr()
{
}

/**
 * @brief 获取标准合约代码对应的规则标签
 *
 * 根据标准合约代码查找对应的切换规则标签
 * 如果合约代码以+或-结尾，则忽略这些符号
 *
 * @param stdCode 标准合约代码
 * @return const char* 对应的规则标签，如果未找到则返回空字符串
 */
const char* WTSHotMgr::getRuleTag(const char* stdCode)
{
	if (m_mapCustRules == NULL)
		return "";

	auto len = strlen(stdCode);
	if (stdCode[len - 1] == '+' || stdCode[len - 1] == '-')
		len--;

	auto idx = StrUtil::findLast(stdCode, '.');
	if (idx == std::string::npos)
	{
		auto it = m_mapCustRules->find(std::string(stdCode, len));
		if (it == m_mapCustRules->end())
			return "";

		return it->first.c_str();
	}

	const char* tail = stdCode + idx + 1;
	auto it = m_mapCustRules->find(std::string(tail, len - idx - 1));
	if (it == m_mapCustRules->end())
		return "";

	return it->first.c_str();
}

/**
 * @brief 获取指定日期的复权因子
 *
 * 根据给定的规则标签、品种代码和日期计算复权因子
 * 如果未指定日期或找不到特定日期的记录，会选择适当的近期复权因子
 *
 * @param ruleTag 规则标签
 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
 * @param uDate 日期，格式YYYYMMDD，默认为0表示取最新数据
 * @return double 复权因子，默认为1.0
 */
double WTSHotMgr::getRuleFactor(const char* ruleTag, const char* fullPid, uint32_t uDate /* = 0 */ )
{
	if (m_mapCustRules == NULL)
		return 1.0;

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(ruleTag);
	if (prodMap == NULL)
		return 1.0;

	WTSDateHotMap* dtMap = STATIC_CONVERT(prodMap->get(fullPid), WTSDateHotMap*);
	if (dtMap == NULL)
		return 1.0;

	if(uDate == 0)
	{
		WTSSwitchItem* pItem = STATIC_CONVERT(dtMap->rbegin()->second, WTSSwitchItem*);
		return pItem->get_factor();
	}

	auto it = dtMap->lower_bound(uDate);
	if(it == dtMap->end())
	{
		//找不到，说明记录的日期都比传入的日期小，所以返回最后一条的复权因子
		WTSSwitchItem* pItem = STATIC_CONVERT(dtMap->rbegin()->second, WTSSwitchItem*);
		return pItem->get_factor();
	}
	else
	{
		//找到了，就要看切换日期是否等于传入日期
		//如果相等，说明刚好切换，那么就直接返回复权因子
		WTSSwitchItem* pItem = STATIC_CONVERT(it->second, WTSSwitchItem*);
		if (pItem->switch_date() == uDate)
		{
			return pItem->get_factor();
		}
		else
		{
			//如果切换日期大于传入日期，则要看前一个阶段
			//如果已经是第一个了，则直接返回1.0
			if (it == dtMap->begin())
			{
				return 1.0;
			}
			else
			{
				//如果不是第一个，则回退一个，再返回即可
				it--;
				WTSSwitchItem* pItem = STATIC_CONVERT(it->second, WTSSwitchItem*);
				return pItem->get_factor();
			}
		}
	}

}

#pragma region "次主力接口"
/**
 * @brief 加载主力合约切换配置文件
 *
 * 此函数调用loadCustomRules函数加载主力合约的切换规则配置，使用"HOT"作为规则标签
 * 加载后将初始化状态标记设置为已初始化
 *
 * @param filename 主力合约切换配置文件路径
 * @return bool 始终返回true，表示加载成功
 */
bool WTSHotMgr::loadHots(const char* filename)
{
	loadCustomRules("HOT", filename);
	m_bInitialized = true;
	return true;
}

/**
 * @brief 获取指定日期的前一主力合约代码
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用getPrevCustomRawCode函数获取指定日期的前一主力合约代码
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param dt 日期，格式YYYYMMDD
 * @return const char* 前一主力合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getPrevRawCode(const char* exchg, const char* pid, uint32_t dt)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return getPrevCustomRawCode("HOT", fullPid, dt);
}

/**
 * @brief 获取指定日期的当前主力合约代码
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用getCustomRawCode函数获取指定日期的当前主力合约代码
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return const char* 当前主力合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getRawCode(const char* exchg, const char* pid, uint32_t dt)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return getCustomRawCode("HOT", fullPid, dt);
}

/**
 * @brief 判断合约是否为指定日期的主力合约
 *
 * 此函数将交易所代码和原始合约代码组合成完整合约代码，
 * 然后调用isCustomHot函数判断该合约是否为指定日期的主力合约
 *
 * @param exchg 交易所代码
 * @param rawCode 原始合约代码
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return bool 如果是主力合约返回true，否则返回false
 */
bool WTSHotMgr::isHot(const char* exchg, const char* rawCode, uint32_t dt)
{
	static thread_local char fullCode[64] = { 0 };
	fmtutil::format_to(fullCode, "{}.{}", exchg, rawCode);

	return isCustomHot("HOT", fullCode, dt);
}

/**
 * @brief 分割指定时间范围内的主力合约切换区间
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用splitCustomSections函数将指定时间范围内的主力合约切换分成多个区间
 * 每个区间包含一个主力合约代码及其使用的时间段和复权因子
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param sDt 开始日期，格式YYYYMMDD
 * @param eDt 结束日期，格式YYYYMMDD
 * @param sections 输出参数，用于存储切分的主力合约区间
 * @return bool 分割成功返回true，失败返回false
 */
bool WTSHotMgr::splitHotSecions(const char* exchg, const char* pid, uint32_t sDt, uint32_t eDt, HotSections& sections)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return splitCustomSections("HOT", fullPid, sDt, eDt, sections);
}
#pragma endregion "主力接口"

#pragma region "次主力接口"
/**
 * @brief 加载次主力合约切换配置文件
 *
 * 此函数调用loadCustomRules函数加载次主力合约的切换规则配置，使用"2ND"作为规则标签
 *
 * @param filename 次主力合约切换配置文件路径
 * @return bool 加载成功返回true，失败返回false
 */
bool WTSHotMgr::loadSeconds(const char* filename)
{
	return loadCustomRules("2ND", filename);
}

/**
 * @brief 获取指定日期的前一次主力合约代码
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用getPrevCustomRawCode函数获取指定日期的前一次主力合约代码
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param dt 日期，格式YYYYMMDD
 * @return const char* 前一次主力合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getPrevSecondRawCode(const char* exchg, const char* pid, uint32_t dt)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return getPrevCustomRawCode("2ND", fullPid, dt);
}

/**
 * @brief 获取指定日期的当前次主力合约代码
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用getCustomRawCode函数获取指定日期的当前次主力合约代码
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return const char* 当前次主力合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getSecondRawCode(const char* exchg, const char* pid, uint32_t dt)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return getCustomRawCode("2ND", fullPid, dt);
}

/**
 * @brief 判断合约是否为指定日期的次主力合约
 *
 * 此函数将交易所代码和原始合约代码组合成完整合约代码，
 * 然后调用isCustomHot函数判断该合约是否为指定日期的次主力合约
 *
 * @param exchg 交易所代码
 * @param rawCode 原始合约代码
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return bool 如果是次主力合约返回true，否则返回false
 */
bool WTSHotMgr::isSecond(const char* exchg, const char* rawCode, uint32_t dt)
{
	static thread_local char fullCode[64] = { 0 };
	fmtutil::format_to(fullCode, "{}.{}", exchg, rawCode);

	return isCustomHot("2ND", fullCode, dt);
}

/**
 * @brief 分割指定时间范围内的次主力合约切换区间
 *
 * 此函数将交易所代码和品种代码组合成完整品种代码，
 * 然后调用splitCustomSections函数将指定时间范围内的次主力合约切换分成多个区间
 * 每个区间包含一个次主力合约代码及其使用的时间段和复权因子
 *
 * @param exchg 交易所代码
 * @param pid 品种代码
 * @param sDt 开始日期，格式YYYYMMDD
 * @param eDt 结束日期，格式YYYYMMDD
 * @param sections 输出参数，用于存储切分的次主力合约区间
 * @return bool 分割成功返回true，失败返回false
 */
bool WTSHotMgr::splitSecondSecions(const char* exchg, const char* pid, uint32_t sDt, uint32_t eDt, HotSections& sections)
{
	static thread_local char fullPid[64] = { 0 };
	fmtutil::format_to(fullPid, "{}.{}", exchg, pid);

	return splitCustomSections("2ND", fullPid, sDt, eDt, sections);
}

#pragma endregion "次主力接口"

#pragma region "自定义主力接口"
/**
 * @brief 加载自定义切换规则配置文件
 *
 * 根据指定的规则标签和文件路径加载切换规则配置
 * 支持JSON格式的配置文件，规则包括一系列合约切换日期、从合约、到合约等信息
 *
 * @param tag 规则标签，用于区分不同规则，如"HOT"表示主力合约，"2ND"表示次主力合约
 * @param filename 配置文件路径
 * @return bool 加载成功返回true，失败返回false
 */
bool WTSHotMgr::loadCustomRules(const char* tag, const char* filename)
{
	if (!StdFile::exists(filename))
	{
		return false;
	}

	WTSVariant* root = WTSCfgLoader::load_from_file(filename);
	if (root == NULL)
		return false;

	if (m_mapCustRules == NULL)
		m_mapCustRules = WTSCustomSwitchMap::create();

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(tag);
	if(prodMap == NULL)
	{
		prodMap = WTSProductHotMap::create();
		m_mapCustRules->add(tag, prodMap, false);
	}

	for (const std::string& exchg : root->memberNames())
	{
		WTSVariant* jExchg = root->get(exchg);

		for (const std::string& pid : jExchg->memberNames())
		{
			WTSVariant* jProduct = jExchg->get(pid);
			std::string fullPid = fmt::format("{}.{}", exchg, pid);

			WTSDateHotMap* dateMap = WTSDateHotMap::create();
			prodMap->add(fullPid.c_str(), dateMap, false);

			std::string lastCode;
			double factor = 1.0;
			for (uint32_t i = 0; i < jProduct->size(); i++)
			{
				WTSVariant* jHotItem = jProduct->get(i);
				WTSSwitchItem* pItem = WTSSwitchItem::create(
					exchg.c_str(), pid.c_str(),
					jHotItem->getCString("from"), jHotItem->getCString("to"), 
					jHotItem->getUInt32("date"));

				//计算复权因子
				double oldclose = jHotItem->getDouble("oldclose");
				double newclose = jHotItem->getDouble("newclose");
				factor *= (decimal::eq(oldclose, 0.0) ? 1.0 : (oldclose/ newclose));
				pItem->set_factor(factor);
				dateMap->add(pItem->switch_date(), pItem, false);
				lastCode = jHotItem->getCString("to");
			}

			std::string fullCode = fmt::format("{}.{}", exchg.c_str(), lastCode.c_str());
			m_mapCustCodes[tag].insert(fullCode);
		}
	}

	root->release();
	return true;
}

/**
 * @brief 获取指定日期的前一自定义规则合约代码
 *
 * 根据规则标签、完整品种代码和日期，查找并返回当前使用的合约前一个合约代码
 * 如果指定日期的当前合约是该品种的第一个合约，则无前一合约，返回空字符串
 *
 * @param tag 规则标签，如"HOT"表示主力合约，"2ND"表示次主力合约
 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return const char* 前一合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getPrevCustomRawCode(const char* tag, const char* fullPid, uint32_t dt /* = 0 */)
{
	if (m_mapCustRules == NULL)
		return "";

	if (dt == 0)
		dt = TimeUtils::getCurDate();

	if (m_mapCustRules == NULL)
		return "";

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(tag);
	if (prodMap == NULL)
		return "";

	WTSDateHotMap* dtMap = STATIC_CONVERT(prodMap->get(fullPid), WTSDateHotMap*);
	if (dtMap == NULL)
		return "";

	WTSDateHotMap::ConstIterator cit = dtMap->lower_bound(dt);
	if (cit != dtMap->end())
	{
		if (dt < cit->first)
			cit--;

		if (cit == dtMap->end() || cit == dtMap->begin())
			return "";

		cit--;

		WTSSwitchItem* pItem = STATIC_CONVERT(cit->second, WTSSwitchItem*);
		return pItem->to();
	}
	else
	{
		cit--;

		if (cit == dtMap->end() || cit == dtMap->begin())
			return "";

		cit--;

		WTSSwitchItem* pItem = STATIC_CONVERT(cit->second, WTSSwitchItem*);
		return pItem->to();
	}

	return "";
}

/**
 * @brief 获取指定日期的当前自定义规则合约代码
 *
 * 根据规则标签、完整品种代码和日期，查找并返回当前使用的合约代码
 * 使用二分查找定位指定日期对应的合约，如果找不到则返回最后一个合约
 *
 * @param tag 规则标签，如"HOT"表示主力合约，"2ND"表示次主力合约
 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return const char* 当前合约代码，如果不存在则返回空字符串
 */
const char* WTSHotMgr::getCustomRawCode(const char* tag, const char* fullPid, uint32_t dt /* = 0 */)
{
	if (m_mapCustRules == NULL)
		return "";

	if (dt == 0)
		dt = TimeUtils::getCurDate();

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(tag);
	if (prodMap == NULL)
		return "";

	WTSDateHotMap* dtMap = STATIC_CONVERT(prodMap->get(fullPid), WTSDateHotMap*);
	if (dtMap == NULL)
		return "";

	WTSDateHotMap::ConstIterator cit = dtMap->lower_bound(dt);
	if (cit != dtMap->end())
	{
		if (dt < cit->first)
			cit--;

		if (cit == dtMap->end())
			return "";

		WTSSwitchItem* pItem = STATIC_CONVERT(cit->second, WTSSwitchItem*);
		return pItem->to();
	}
	else
	{
		WTSSwitchItem* pItem = STATIC_CONVERT(dtMap->last(), WTSSwitchItem*);
		return pItem->to();
	}

	return "";
}

/**
 * @brief 判断合约是否为指定日期的自定义规则合约
 *
 * 根据规则标签、完整合约代码和日期，判断给定合约是否在指定日期为当前使用的合约
 * 如果未指定日期，则直接在当前使用合约集合中查找；否则查找指定日期的切换规则并比对
 *
 * @param tag 规则标签，如"HOT"表示主力合约，"2ND"表示次主力合约
 * @param fullCode 完整合约代码，格式为“交易所.合约代码”
 * @param dt 日期，格式YYYYMMDD，默认为0表示当前日期
 * @return bool 如果是自定义规则当前合约返回true，否则返回false
 */
bool WTSHotMgr::isCustomHot(const char* tag, const char* fullCode, uint32_t dt /* = 0 */)
{
	if (m_mapCustRules == NULL)
		return false;

	const auto& curHotCodes = m_mapCustCodes[tag];
	if (curHotCodes.empty())
		return false;

	if (dt == 0)
	{
		auto it = curHotCodes.find(fullCode);
		if (it == curHotCodes.end())
			return false;
		else
			return true;
	}

	auto idx = StrUtil::findFirst(fullCode, '.');
	const char* rawCode = fullCode + idx + 1;
	std::string fullPid(fullCode, idx);
	fullPid += ".";
	fullPid += CodeHelper::rawMonthCodeToRawCommID(rawCode);

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(tag);
	if (prodMap == NULL)
		return "";

	WTSDateHotMap* dtMap = STATIC_CONVERT(prodMap->get(fullPid), WTSDateHotMap*);
	if (dtMap == NULL)
		return "";

	WTSDateHotMap::ConstIterator cit = dtMap->lower_bound(dt);
	if (cit != dtMap->end())
	{
		WTSSwitchItem* pItem = STATIC_CONVERT(cit->second, WTSSwitchItem*);
		//因为登记的换月日期是开始生效的交易日，如果是下午盘后确定主力的话
		//那么dt就会是第二天，所以，dt必须大于等于切换日期
		if (pItem->switch_date() > dt)
			cit--;

		pItem = STATIC_CONVERT(cit->second, WTSSwitchItem*);
		if (strcmp(pItem->to(), rawCode) == 0)
			return true;
	}
	else if (dtMap->size() > 0)
	{
		WTSSwitchItem* pItem = (WTSSwitchItem*)dtMap->last();
		if (strcmp(pItem->to(), rawCode) == 0)
			return true;
	}

	return false;
}

/**
 * @brief 分割指定时间范围内的自定义规则合约切换区间
 *
 * 根据规则标签、完整品种代码和时间范围，将指定时间范围内的合约切换分成多个区间
 * 每个区间包含一个合约代码、区间起始日期、区间结束日期和复权因子
 *
 * @param tag 规则标签，如"HOT"表示主力合约，"2ND"表示次主力合约
 * @param fullPid 完整品种代码，格式为“交易所.品种代码”
 * @param sDt 开始日期，格式YYYYMMDD
 * @param eDt 结束日期，格式YYYYMMDD
 * @param sections 输出参数，用于存储分割的合约切换区间
 * @return bool 分割成功返回true，失败返回false
 */
bool WTSHotMgr::splitCustomSections(const char* tag, const char* fullPid, uint32_t sDt, uint32_t eDt, HotSections& sections)
{
	if (m_mapCustRules == NULL)
		return false;

	WTSProductHotMap* prodMap = (WTSProductHotMap*)m_mapCustRules->get(tag);
	if (prodMap == NULL)
		return false;

	WTSDateHotMap* dtMap = STATIC_CONVERT(prodMap->get(fullPid), WTSDateHotMap*);
	if (dtMap == NULL)
		return false;

	uint32_t leftDate = sDt;
	uint32_t lastDate = 0;
	const char* curHot = "";
	auto cit = dtMap->begin();
	double prevFactor = 1.0;
	for (; cit != dtMap->end(); cit++)
	{
		uint32_t curDate = cit->first;
		WTSSwitchItem* hotItem = (WTSSwitchItem*)cit->second;

		if (curDate > eDt)
		{
			sections.emplace_back(HotSection(hotItem->from(), leftDate, eDt, prevFactor));
		}
		else if (leftDate < curDate)
		{
			//如果开始日期小于当前切换的日期,则添加一段
			if (strlen(hotItem->from()) > 0)//这里from为空,主要是第一条规则,如果真的遇到这种情况,也没有太好的办法,只能不要这一段数据了,一般情况下是够的
			{
				sections.emplace_back(HotSection(hotItem->from(), leftDate, TimeUtils::getNextDate(curDate, -1), prevFactor));
			}

			leftDate = curDate;
		}

		lastDate = curDate;
		prevFactor = hotItem->get_factor();
		curHot = hotItem->to();
	}

	if (leftDate >= lastDate && lastDate != 0)
	{
		sections.emplace_back(HotSection(curHot, leftDate, eDt, prevFactor));
	}

	return true;
}
#pragma endregion "自定义主力接口"

/**
 * @brief 释放内存资源
 *
 * 释放管理器所持有的自定义切换规则映射对象
 * 注释的代码是早期版本中的主力合约和次主力合约映射对象释放逻辑，现已不再使用
 */
void WTSHotMgr::release()
{
	//if (m_pExchgHotMap)
	//{
	//	m_pExchgHotMap->release();
	//	m_pExchgHotMap = NULL;
	//}

	//if (m_pExchgScndMap)
	//{
	//	m_pExchgScndMap->release();
	//	m_pExchgScndMap = NULL;
	//}

	if(m_mapCustRules)
	{
		m_mapCustRules->release();
		m_mapCustRules = NULL;
	}
}