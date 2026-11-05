/*!
 * \file UftStrategyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "UftStrategyMgr.h"

#include <filesystem>
namespace fs = std::filesystem;

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"


UftStrategyMgr::UftStrategyMgr()
{
}


UftStrategyMgr::~UftStrategyMgr()
{
}

UftStrategyMgr::FactoryOwner::~FactoryOwner()
{
	// Card 9a: only a plugin promising synchronous teardown is asked to destroy
	// its factory; otherwise the factory stays quarantined until process exit.
	if (_fact != NULL && wt_stra_plugin::hasSyncTeardown(_api))
	{
		try
		{
			_api->destroy_factory(static_cast<void*>(_fact));
		}
		catch (...)
		{
			// No exception may cross the C table; keep the module mapped.
		}
	}
}

bool UftStrategyMgr::loadFactories(const char* path)
{
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of UFT strategy factory not exists", path);
		return false;
	}

	uint32_t count = 0;
	bool refused = false;
	fs::path myPath(path);
	fs::directory_iterator endIter;
	for (fs::directory_iterator iter(myPath); iter != endIter; iter++)
	{
		if (fs::is_directory(iter->path()))
			continue;

#ifdef _WIN32
		if (iter->path().extension() != ".dll")
			continue;
#elif defined(__APPLE__)
		if (iter->path().extension() != ".dylib")
			continue;
#else //_UNIX
		if (iter->path().extension() != ".so")
			continue;
#endif

		const std::string modulePath = iter->path().string();
		if (_refused_modules.count(modulePath) != 0)
		{
			// A refused module stays resident and refused; no plugin entry runs again.
			refused = true;
			continue;
		}
		if (_skipped_modules.count(modulePath) != 0)
			continue;
		bool published = false;
		for (const auto& entry : _factories)
			if (entry.second._module_path == modulePath)
				published = true;
		if (published)
			continue;	// same-manager rescan: no second factory

		DllHandle hInst = DLLHelper::load_library(modulePath.c_str());
		if (hInst == NULL)
			continue;

		// Card 4b=A: only a validated versioned UFT table may create a factory;
		// legacy createStrategyFact (shared by other families) is never called.
		// Card 9a: once loaded, a module is never unloaded by this manager.
		const WtStrategyPluginApiV2* api = NULL;
		std::string why;
		const auto probe = wt_stra_plugin::probeModule(
			[hInst](const char* symbol) { return DLLHelper::get_symbol(hInst, symbol); },
			WT_STRA_PLUGIN_V2_FAMILY_UFT, api, why);
		if (probe == wt_stra_plugin::ProbeResult::NotStrategyPlugin ||
			probe == wt_stra_plugin::ProbeResult::ForeignFamily)
		{
			if (probe == wt_stra_plugin::ProbeResult::ForeignFamily)
				WTSLogger::warn("UFT strategy factory directory entry {} skipped: {}", modulePath, why);
			_skipped_modules.insert(modulePath);
			continue;
		}
		if (probe != wt_stra_plugin::ProbeResult::Accepted)
		{
			WTSLogger::error("UFT strategy plugin {} rejected: {}", modulePath, why);
			_refused_modules.insert(modulePath);
			refused = true;
			continue;
		}

		IUftStrategyFact* pFact = NULL;
		try
		{
			pFact = static_cast<IUftStrategyFact*>(api->create_factory());
		}
		catch (...)
		{
			pFact = NULL;
		}
		if (pFact == NULL)
		{
			WTSLogger::error("UFT strategy factory creator of {} returned null; module quarantined", modulePath);
			_refused_modules.insert(modulePath);
			refused = true;
			continue;
		}
		const char* rawName = pFact->getName();
		if (rawName == NULL || rawName[0] == '\0' || _factories.find(rawName) != _factories.end())
		{
			// The created factory is retained (never destroyed) with its module.
			WTSLogger::error("UFT strategy factory of {} has an empty or duplicate name; instance quarantined", modulePath);
			_refused_modules.insert(modulePath);
			refused = true;
			continue;
		}

		std::shared_ptr<FactoryOwner> owner = std::make_shared<FactoryOwner>(pFact, api, hInst);
		StraFactInfo& fInfo = _factories[rawName];
		fInfo._module_inst = hInst;
		fInfo._module_path = modulePath;
		fInfo._fact = pFact;
		fInfo._owner = owner;
		WTSLogger::info("UFT strategy factory[{}] loaded{}", rawName,
			wt_stra_plugin::hasSyncTeardown(api) ? "" : " (no synchronous teardown: quarantined until exit)");

		count++;
	}

	WTSLogger::info("{} UFT strategy factories in directory[{}] loaded", count, path);

	return !refused;
}

UftStrategyPtr UftStrategyMgr::wrapStrategy(StraFactInfo& fInfo, const char* unitname, const char* id)
{
	// Each wrapper keeps the factory owner alive through its deleter, so the
	// factory is destroyed (if allowed) only after its last strategy.
	std::shared_ptr<FactoryOwner> owner = fInfo._owner;
	UftStrategyPtr ret(new UftStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact),
		[owner](UftStraWrapper* wrapper) { delete wrapper; });
	_strategies[id] = ret;
	return ret;
}

UftStrategyPtr UftStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return UftStrategyPtr();

	return wrapStrategy(it->second, unitname, id);
}

UftStrategyPtr UftStrategyMgr::createStrategy(const char* name, const char* id)
{
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return UftStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	auto it = _factories.find(factname);
	if (it == _factories.end())
		return UftStrategyPtr();

	return wrapStrategy(it->second, unitname, id);
}

UftStrategyPtr UftStrategyMgr::getStrategy(const char* id)
{
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return UftStrategyPtr();

	return it->second;
}
