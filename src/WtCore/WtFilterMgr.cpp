/**
 * @file WtFilterMgr.cpp
 * @author wondertrader
 * @brief 信号过滤管理器实现
 * @details 实现了信号过滤管理器类，用于过滤和处理交易信号
 */

#include "WtFilterMgr.h"
#include "EventNotifier.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"

#include <boost/filesystem.hpp>

USING_NS_WTP;

/**
 * @brief 加载信号过滤器
 * @details 从指定文件加载信号过滤器的配置，包括策略过滤器、合约代码过滤器和执行器过滤器
 * @param fileName 过滤器配置文件名，默认为空字符串
 */
void WtFilterMgr::load_filters(const char* fileName)
{
	// 如果没有设置过滤器文件且也没有提供新的文件名，直接返回
	if (_filter_file.empty() && (strlen(fileName) == 0))
		return;

	// 如果提供了新的文件名，则更新过滤器文件路径
	if(strlen(fileName) > 0)
		_filter_file = fileName;

	// 检查过滤器文件是否存在
	if (!StdFile::exists(_filter_file.c_str()))
	{
		WTSLogger::debug("Filters configuration file {} not exists", _filter_file);
		return;
	}

	// 获取文件的最后修改时间
	uint64_t lastModTime = boost::filesystem::last_write_time(boost::filesystem::path(_filter_file));
	// 如果文件没有变化，直接返回
	if (lastModTime <= _filter_timestamp)
		return;

	// 如果文件变化了，输出日志并通知
	if (_filter_timestamp != 0)
	{
		WTSLogger::info("Filters configuration file {} modified, will be reloaded", _filter_file);
		if (_notifier)
			_notifier->notify_event("Filter file has been reloaded");
	}

	// 加载过滤器配置文件
	WTSVariant* cfg = WTSCfgLoader::load_from_file(_filter_file.c_str());

	// 更新文件时间戳
	_filter_timestamp = lastModTime;

	// 清空所有过滤器
	_stra_filters.clear();
	_code_filters.clear();
	_exec_filters.clear();

	// 读取策略过滤器配置
	WTSVariant* filterStra = cfg->get("strategy_filters");
	if (filterStra)
	{
		// 遍历所有策略过滤器项
		auto keys = filterStra->memberNames();
		for (const std::string& key : keys)
		{
			// 获取当前策略过滤器的配置
			WTSVariant* cfgItem = filterStra->get(key.c_str());
			const char* action = cfgItem->getCString("action");
			
			// 解析过滤器动作
			FilterAction fAct = FA_None;
			if (wt_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (wt_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;

			// 如果动作无效，跳过当前项
			if (fAct == FA_None)
			{
				WTSLogger::error("Action {} of strategy filter {} not recognized", action, key);
				continue;
			}

			// 创建并设置过滤器项
			FilterItem& fItem = _stra_filters[key];
			fItem._key = key;
			fItem._action = fAct;
			fItem._target = cfgItem->getDouble("target");

			WTSLogger::info("Strategy filter {} loaded", key);
		}
	}

	// 读取合约代码过滤器配置
	WTSVariant* filterCodes = cfg->get("code_filters");
	if (filterCodes)
	{
		// 遍历所有合约代码过滤器项
		auto codes = filterCodes->memberNames();
		for (const std::string& stdCode : codes)
		{
			// 获取当前合约代码过滤器的配置
			WTSVariant* cfgItem = filterCodes->get(stdCode.c_str());
			const char* action = cfgItem->getCString("action");
			
			// 解析过滤器动作
			FilterAction fAct = FA_None;
			if (wt_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (wt_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;

			// 如果动作无效，跳过当前项
			if (fAct == FA_None)
			{
				WTSLogger::error("Action {} of code filter {} not recognized", action, stdCode);
				continue;
			}

			// 创建并设置过滤器项
			FilterItem& fItem = _code_filters[stdCode];
			fItem._key = stdCode;
			fItem._action = fAct;
			fItem._target = cfgItem->getDouble("target");

			WTSLogger::info("Code filter {} loaded", stdCode);
		}
	}

	// 读取执行器过滤器配置
	WTSVariant* filterExecuters = cfg->get("executer_filters");
	if (filterExecuters)
	{
		// 遍历所有执行器过滤器项
		auto executer_ids = filterExecuters->memberNames();
		for (const std::string& execid : executer_ids)
		{
			// 获取当前执行器是否禁用
			bool bDisabled = filterExecuters->getBoolean(execid.c_str());
			WTSLogger::info("Executer {} is %s", execid, bDisabled?"disabled":"enabled");
			_exec_filters[execid] = bDisabled;
		}
	}

	// 释放配置对象
	cfg->release();
}

/**
 * @brief 检查是否被执行器过滤器过滤掉了
 * @details 检查指定的执行器ID是否被过滤器过滤掉了
 * @param execid 执行器ID
 * @return 是否被过滤掉了，如果过滤掉了返回true，否则返回false
 */
bool WtFilterMgr::is_filtered_by_executer(const char* execid)
{
	// 在执行器过滤器映射中查找指定的执行器ID
	auto it = _exec_filters.find(execid);
	// 如果没有找到，表示没有过滤该执行器
	if (it == _exec_filters.end())
		return false;

	// 返回过滤器设置的值，true表示过滤掉，false表示不过滤
	return it->second;
}

/**
 * @brief 过滤器动作名称数组
 * @details 用于将过滤器动作类型转换为可读的字符串
 */
const char* FLTACT_NAMEs[] =
{
	"Ignore",    ///< 忽略动作的名称
	"Redirect"   ///< 重定向动作的名称
};

/**
 * @brief 检查是否因为策略被过滤掉了
 * @details 检查策略信号是否被过滤器过滤掉了。如果过滤器动作是忽略，则返回true，
 *          如果是重定向仓位，则返回false，同时修改目标仓位为过滤器设定的值
 * @param straName 策略名称
 * @param targetPos 目标仓位，传入参数，会被修改
 * @param isDiff 是否是增量仓位，默认为false
 * @return 是否过滤掉了，如果过滤掉了返回true，否则返回false
 */
bool WtFilterMgr::is_filtered_by_strategy(const char* straName, double& targetPos, bool isDiff /* = false */)
{
	// 在策略过滤器映射中查找指定的策略名称
	auto it = _stra_filters.find(straName);
	if (it != _stra_filters.end())
	{
		// 获取过滤器项
		const FilterItem& fItem = it->second;
		
		// 如果是增量仓位，则直接过滤掉
		if(isDiff)
		{
			// 如果过滤器触发，并且是增量头寿，则直接过滤掉
			WTSLogger::info("[Filters] Strategy filter {} triggered, the change of position ignored directly", straName);
			return true;
		}

		// 输出过滤器触发的日志
		WTSLogger::info("[Filters] Strategy filter {} triggered, action: {}", straName, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		
		// 如果动作是忽略，返回true表示过滤掉
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		// 如果动作是重定向，则修改目标仓位
		else if (fItem._action == FA_Redirect)
		{
			// 只有不是增量的时候，才有效
			targetPos = fItem._target;
		}

		return false;
	}

	// 如果没有找到过滤器，表示不过滤
	return false;
}

/**
 * @brief 检查是否因为合约代码被过滤掉了
 * @details 检查合约代码是否被过滤器过滤掉了。先检查完整合约代码，如果没有找到，
 *          再检查品种代码。如果过滤器动作是忽略，则返回true，如果是重定向仓位，
 *          则返回false，同时修改目标仓位为过滤器设定的值
 * @param stdCode 标准化合约代码
 * @param targetPos 目标仓位，传入参数，会被修改
 * @return 是否过滤掉了，如果过滤掉了返回true，否则返回false
 */
bool WtFilterMgr::is_filtered_by_code(const char* stdCode, double& targetPos)
{
	// 从标准化合约代码中提取代码信息
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	
	// 先在合约代码过滤器映射中查找完整合约代码
	auto cit = _code_filters.find(stdCode);
	if (cit != _code_filters.end())
	{
		// 获取过滤器项
		const FilterItem& fItem = cit->second;
		
		// 输出过滤器触发的日志
		WTSLogger::info("[Filters] Code filter {} triggered, action: {}", stdCode, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		
		// 如果动作是忽略，返回true表示过滤掉
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		// 如果动作是重定向，则修改目标仓位
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	// 如果没有找到完整合约代码，则在品种代码过滤器映射中查找
	cit = _code_filters.find(cInfo.stdCommID());
	if (cit != _code_filters.end())
	{
		// 获取过滤器项
		const FilterItem& fItem = cit->second;
		
		// 输出过滤器触发的日志
		WTSLogger::info("[Filters] CommID filter {} triggered, action: {}", cInfo.stdCommID(), fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		
		// 如果动作是忽略，返回true表示过滤掉
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		// 如果动作是重定向，则修改目标仓位
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	// 如果没有找到任何过滤器，表示不过滤
	return false;
}



