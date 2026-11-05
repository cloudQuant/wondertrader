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
#include <system_error>
namespace fs = std::filesystem;


SelStrategyMgr::SelStrategyMgr()
{
}


SelStrategyMgr::~SelStrategyMgr()
{
}

bool SelStrategyMgr::loadFactories(const char* path)
{
	if (path == NULL || path[0] == '\0')
	{
		WTSLogger::error("Directory of SEL strategy factory is empty");
		return false;
	}

	fs::path myPath(path);
	std::error_code ec;
	const bool isDirectory = fs::is_directory(myPath, ec);
	if (ec || !isDirectory)
	{
		WTSLogger::error("Directory {} of SEL strategy factory is not accessible: {}",
			path, ec ? ec.message() : "not a directory");
		return false;
	}

	uint32_t count = 0;
	fs::directory_iterator iter(myPath, ec);
	if (ec)
	{
		WTSLogger::error("Cannot enumerate SEL strategy factory directory {}: {}", path, ec.message());
		return false;
	}
	const fs::directory_iterator endIter;
	auto loadFactory = [this, &count](const fs::path& factoryPath)
	{

#ifdef _WIN32
		if (factoryPath.extension() != ".dll")
			return;
#elif defined(__APPLE__)
		if (factoryPath.extension() != ".dylib")
			return;
#else //_UNIX
		if (factoryPath.extension() != ".so")
			return;
#endif

		DllHandle hInst = DLLHelper::load_library(factoryPath.string().c_str());
		if (hInst == NULL)
			return;

		FuncCreateSelStraFact creator = (FuncCreateSelStraFact)DLLHelper::get_symbol(hInst, "createSelStrategyFact");
		FuncDeleteSelStraFact remover = (FuncDeleteSelStraFact)DLLHelper::get_symbol(hInst, "deleteSelStrategyFact");
		if (creator == NULL || remover == NULL)
		{
			DLLHelper::free_library(hInst);
			return;
		}

		ISelStrategyFact* fact = creator();
		if (fact != NULL)
		{
			const char* factName = fact->getName();
			if (factName == NULL || factName[0] == '\0')
			{
				remover(fact);
				DLLHelper::free_library(hInst);
				return;
			}

			std::string factoryName(factName);
			auto existing = _factories.find(factoryName);
			if (existing != _factories.end())
			{
				WTSLogger::warn("SEL strategy factory[{}] already loaded from {}, skip duplicate {}",
					factoryName, existing->second._module_path, factoryPath.string());
				remover(fact);
				DLLHelper::free_library(hInst);
				return;
			}

			StraFactInfo fInfo{};
			fInfo._module_inst = hInst;
			fInfo._module_path = factoryPath.string();
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
		}
	};

	while (iter != endIter)
	{
		const fs::path factoryPath = iter->path();
		ec.clear();
		const bool isSubdirectory = fs::is_directory(factoryPath, ec);
		if (ec)
		{
			WTSLogger::error("Cannot inspect SEL factory entry in {}: {}", path, ec.message());
			return false;
		}
		if (!isSubdirectory)
			loadFactory(factoryPath);

		iter.increment(ec);
		if (ec)
		{
			WTSLogger::error("Cannot continue enumerating SEL strategy factory directory {}: {}", path, ec.message());
			return false;
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

