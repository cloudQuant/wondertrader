#include "WtFilterMgr.h"
#include "EventNotifier.h"

#include "../Share/CodeHelper.hpp"
#include "../Includes/WTSVariant.hpp"
#include "../WTSUtils/WTSCfgLoader.h"
#include "../WTSTools/WTSLogger.h"
#include "../WTSUtils/yamlcpp/yaml.h"

#include <exception>

USING_NS_WTP;

void WtFilterMgr::load_filters(const char* fileName)
{
	if (_filter_file.empty() && (strlen(fileName) == 0))
		return;

	if(strlen(fileName) > 0)
		_filter_file = fileName;

	if (!StdFile::exists(_filter_file.c_str()))
	{
		WTSLogger::debug("Filters configuration file {} not exists", _filter_file);
		return;
	}

	auto lastModTime = fs::last_write_time(fs::path(_filter_file));
	

	bool isReload = !_is_first;
	if (isReload)
	{
		if (lastModTime <= _filter_timestamp)
			return;

		WTSLogger::info("Filters configuration file {} modified, will be reloaded", _filter_file);
	}

	WTSVariant* cfg = NULL;
	try
	{
		cfg = WTSCfgLoader::load_from_file(_filter_file.c_str());
	}
	catch (const YAML::Exception& e)
	{
		WTSLogger::error("Failed to parse filters configuration file {}: {}", _filter_file, e.what());
		return;
	}
	catch (const std::exception& e)
	{
		WTSLogger::error("Failed to load filters configuration file {}: {}", _filter_file, e.what());
		return;
	}

	if (cfg == NULL)
	{
		WTSLogger::error("Failed to load filters configuration file {}", _filter_file);
		return;
	}

	if (cfg->type() != WTSVariant::VT_Object)
	{
		WTSLogger::error("Filters configuration file {} must contain an object", _filter_file);
		cfg->release();
		return;
	}

	// Validate the structural containers before building replacement tables. Scalar
	// leaf conversions stay as provided by WTSVariant so existing YAML configs keep
	// their string-scalar behavior.
	WTSVariant* filterStra = cfg->get("strategy_filters");
	WTSVariant* filterCodes = cfg->get("code_filters");
	WTSVariant* filterExecuters = cfg->get("executer_filters");
	auto isObjectSection = [&](WTSVariant* section, const char* name)
	{
		if (section == NULL || section->type() == WTSVariant::VT_Object)
			return true;

		WTSLogger::error("Filter section {} in {} must be an object", name, _filter_file);
		return false;
	};
	auto filterItemsAreObjects = [&](WTSVariant* section, const char* name)
	{
		if (section == NULL)
			return true;

		for (const std::string& key : section->memberNames())
		{
			WTSVariant* item = section->get(key.c_str());
			if (item == NULL || item->type() != WTSVariant::VT_Object)
			{
				WTSLogger::error("Filter item {}.{} in {} must be an object", name, key, _filter_file);
				return false;
			}
		}
		return true;
	};
	auto executerItemsAreScalars = [&](WTSVariant* section)
	{
		if (section == NULL)
			return true;

		for (const std::string& key : section->memberNames())
		{
			WTSVariant* item = section->get(key.c_str());
			if (item == NULL || item->type() == WTSVariant::VT_Object || item->type() == WTSVariant::VT_Array)
			{
				WTSLogger::error("Executer filter {} in {} must be a scalar", key, _filter_file);
				return false;
			}
		}
		return true;
	};
	if (!isObjectSection(filterStra, "strategy_filters") ||
		!isObjectSection(filterCodes, "code_filters") ||
		!isObjectSection(filterExecuters, "executer_filters") ||
		!filterItemsAreObjects(filterStra, "strategy_filters") ||
		!filterItemsAreObjects(filterCodes, "code_filters") ||
		!executerItemsAreScalars(filterExecuters))
	{
		cfg->release();
		return;
	}

	FilterMap straFilters;
	FilterMap codeFilters;
	ExecuterFilters execFilters;

	//读策略过滤器
	if (filterStra)
	{
		auto keys = filterStra->memberNames();
		for (const std::string& key : keys)
		{
			WTSVariant* cfgItem = filterStra->get(key.c_str());
			const char* action = cfgItem->getCString("action");
			FilterAction fAct = FA_None;
			if (wt_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (wt_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;

			if (fAct == FA_None)
			{
				WTSLogger::error("Action {} of strategy filter {} not recognized", action, key);
				continue;
			}

			FilterItem& fItem = straFilters[key];
			fItem._key = key;
			fItem._action = fAct;
			fItem._target = cfgItem->getDouble("target");

			WTSLogger::info("Strategy filter {} loaded", key);
		}
	}

	//读代码过滤器
	if (filterCodes)
	{
		auto codes = filterCodes->memberNames();
		for (const std::string& stdCode : codes)
		{

			WTSVariant* cfgItem = filterCodes->get(stdCode.c_str());
			const char* action = cfgItem->getCString("action");
			FilterAction fAct = FA_None;
			if (wt_stricmp(action, "ignore") == 0)
				fAct = FA_Ignore;
			else if (wt_stricmp(action, "redirect") == 0)
				fAct = FA_Redirect;

			if (fAct == FA_None)
			{
				WTSLogger::error("Action {} of code filter {} not recognized", action, stdCode);
				continue;
			}

			FilterItem& fItem = codeFilters[stdCode];
			fItem._key = stdCode;
			fItem._action = fAct;
			fItem._target = cfgItem->getDouble("target");

			WTSLogger::info("Code filter {} loaded", stdCode);
		}
	}

	//读通道过滤器
	if (filterExecuters)
	{
		auto executer_ids = filterExecuters->memberNames();
		for (const std::string& execid : executer_ids)
		{
			bool bDisabled = filterExecuters->getBoolean(execid.c_str());
			WTSLogger::info("Executer {} is {}", execid, bDisabled?"disabled":"enabled");
			execFilters[execid] = bDisabled;
		}
	}

	_stra_filters.swap(straFilters);
	_code_filters.swap(codeFilters);
	_exec_filters.swap(execFilters);
	_filter_timestamp = lastModTime;
	cfg->release();
	_is_first = false;
	if (isReload && _notifier)
		_notifier->notify_event("Filter file has been reloaded");
}

bool WtFilterMgr::is_filtered_by_executer(const char* execid)
{
	auto it = _exec_filters.find(execid);
	if (it == _exec_filters.end())
		return false;

	return it->second;
}

const char* FLTACT_NAMEs[] =
{
	"Ignore",
	"Redirect"
};

bool WtFilterMgr::is_filtered_by_strategy(const char* straName, double& targetPos, bool isDiff /* = false */)
{
	auto it = _stra_filters.find(straName);
	if (it != _stra_filters.end())
	{
		const FilterItem& fItem = it->second;
		if(isDiff)
		{
			//如果过滤器触发，并且是增量头寸，则直接过滤掉
			WTSLogger::info("[Filters] Strategy filter {} triggered, the change of position ignored directly", straName);
			return true;
		}

		WTSLogger::info("[Filters] Strategy filter {} triggered, action: {}", straName, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if (fItem._action == FA_Redirect)
		{
			//只有不是增量的时候,才有效
			targetPos = fItem._target;
		}

		return false;
	}

	return false;
}

bool WtFilterMgr::is_filtered_by_code(const char* stdCode, double& targetPos)
{
	CodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, NULL);
	auto cit = _code_filters.find(stdCode);
	if (cit != _code_filters.end())
	{
		const FilterItem& fItem = cit->second;
		WTSLogger::info("[Filters] Code filter {} triggered, action: {}", stdCode, fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	cit = _code_filters.find(cInfo.stdCommID());
	if (cit != _code_filters.end())
	{
		const FilterItem& fItem = cit->second;
		WTSLogger::info("[Filters] CommID filter {} triggered, action: {}", cInfo.stdCommID(), fItem._action <= FA_Redirect ? FLTACT_NAMEs[fItem._action] : "Unknown");
		if (fItem._action == FA_Ignore)
		{
			return true;
		}
		else if (fItem._action == FA_Redirect)
		{
			targetPos = fItem._target;
		}

		return false;
	}

	return false;
}



