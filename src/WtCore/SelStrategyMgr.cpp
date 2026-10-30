/*!
* \file MfStrategyMgr.cpp
* \project	WonderTrader
*
* \author Wesley
* \date 2020/03/30
*
* \brief
*/
#include "SelStrategyMgr.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"

#include <filesystem>
namespace fs = std::filesystem;


SelStrategyMgr::SelStrategyMgr()
{
}


SelStrategyMgr::~SelStrategyMgr()
{
}

bool SelStrategyMgr::loadFactories(const char* path)
{
	if (!StdFile::exists(path))
	{
		WTSLogger::error("Directory {} of SEL strategy factory not exists", path);
		return false;
	}

	uint32_t count = 0;
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

		DllHandle hInst = DLLHelper::load_library(iter->path().string().c_str());
		if (hInst == NULL)
			continue;

		FuncCreateSelStraFact creator = (FuncCreateSelStraFact)DLLHelper::get_symbol(hInst, "createSelStrategyFact");
		FuncDeleteSelStraFact remover = (FuncDeleteSelStraFact)DLLHelper::get_symbol(hInst, "deleteSelStrategyFact");
		if (creator == NULL || remover == NULL)
		{
			DLLHelper::free_library(hInst);
			continue;
		}

		ISelStrategyFact* fact = creator();
		if (fact != NULL)
		{
			const char* factName = fact->getName();
			if (factName == NULL || factName[0] == '\0')
			{
				remover(fact);
				DLLHelper::free_library(hInst);
				continue;
			}

			std::string factoryName(factName);
			auto existing = _factories.find(factoryName);
			if (existing != _factories.end())
			{
				WTSLogger::warn("SEL strategy factory[{}] already loaded from {}, skip duplicate {}",
					factoryName, existing->second._module_path, iter->path().string());
				remover(fact);
				DLLHelper::free_library(hInst);
				continue;
			}

			StraFactInfo fInfo{};
			fInfo._module_inst = hInst;
			fInfo._module_path = iter->path().string();
			fInfo._creator = creator;
			fInfo._remover = remover;
			fInfo._fact = fact;
			_factories.emplace(factoryName, fInfo);

			WTSLogger::info("SEL strategy factory[{}] loaded", factoryName);

			count++;
		}
		else
		{
			DLLHelper::free_library(hInst);
			continue;
		}

	}

	WTSLogger::info("{} SEL strategy factories in directory[{}] loaded", count, path);

	return true;
}

SelStrategyPtr SelStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	auto it = _factories.find(factname);
	if (it == _factories.end())
		return SelStrategyPtr();

	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	SelStrategyPtr ret(new SelStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	_strategies[id] = ret;
	return ret;
}

SelStrategyPtr SelStrategyMgr::createStrategy(const char* name, const char* id)
{
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return SelStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	auto it = _factories.find(factname);
	if (it == _factories.end())
		return SelStrategyPtr();

	StraFactInfo& fInfo = (StraFactInfo&)it->second;
	SelStrategyPtr ret(new SelStraWrapper(fInfo._fact->createStrategy(unitname, id), fInfo._fact));
	_strategies[id] = ret;
	return ret;
}

SelStrategyPtr SelStrategyMgr::getStrategy(const char* id)
{
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return SelStrategyPtr();

	return it->second;
}

