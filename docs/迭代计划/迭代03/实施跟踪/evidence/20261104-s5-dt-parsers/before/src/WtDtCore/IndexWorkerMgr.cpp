/*!
 * \file IndexWorkerMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "IndexWorkerMgr.h"
#include "DataManager.h"

#include "../Includes/WTSVariant.hpp"
#include "../Includes/WTSDataDef.hpp"
#include "../Includes/WTSContractInfo.hpp"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"

#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>
namespace fs = std::filesystem;

namespace
{
	class TickReleaseGuard
	{
	public:
		explicit TickReleaseGuard(WTSTickData* tick) : _tick(tick) {}
		~TickReleaseGuard()
		{
			if (_tick != nullptr)
				_tick->release();
		}

		TickReleaseGuard(const TickReleaseGuard&) = delete;
		TickReleaseGuard& operator=(const TickReleaseGuard&) = delete;

	private:
		WTSTickData* _tick;
	};
}


IndexWorkerMgr::IndexWorkerMgr()
{
}


IndexWorkerMgr::~IndexWorkerMgr()
{
}

bool IndexWorkerMgr::loadFactories(const char* path)
{
	// A candidate being loaded, or retained after a failure, blocks new scans.
	// This also prevents an unbounded number of unquiesced candidates per manager.
	if (_loading_factories || _conflicting_module != nullptr || _conflicting_factory != nullptr)
	{
		WTSLogger::error("Index worker factory load is in progress or quarantined");
		return false;
	}
	_loading_factories = true;
	struct LoadingGuard
	{
		bool& loading;
		~LoadingGuard() { loading = false; }
	} loadingGuard{_loading_factories};
	if (path == nullptr || path[0] == '\0')
	{
		WTSLogger::error("Directory of index worker factory is empty");
		return false;
	}

	fs::path myPath(path);
	std::error_code ec;
	if (!fs::exists(myPath, ec) || ec)
	{
		WTSLogger::error("Directory {} of index worker factory is unavailable: {}", path,
			ec ? ec.message() : "path does not exist");
		return false;
	}
	if (!fs::is_directory(myPath, ec) || ec)
	{
		WTSLogger::error("Path {} of index worker factory is not a readable directory: {}", path,
			ec ? ec.message() : "path is not a directory");
		return false;
	}

	// Finish the fallible filesystem scan before loading anything, so a mid-scan error
	// does not leave a partially loaded set of factories behind.
	std::vector<fs::path> modulePaths;
	fs::directory_iterator endIter;
	fs::directory_iterator iter(myPath, ec);
	if (ec)
	{
		WTSLogger::error("Failed to iterate index worker directory {}: {}", path, ec.message());
		return false;
	}
	while (iter != endIter)
	{
		const fs::path entryPath = iter->path();
		ec.clear();
		const bool isDirectory = fs::is_directory(entryPath, ec);
		if (ec)
		{
			WTSLogger::error("Failed to inspect index worker path {}: {}", entryPath.string(), ec.message());
			return false;
		}
		if (!isDirectory)
		{

#ifdef _WIN32
			if (entryPath.extension() == ".dll")
#elif defined(__APPLE__)
			if (entryPath.extension() == ".dylib")
#else //_UNIX
			if (entryPath.extension() == ".so")
#endif
			{
				modulePaths.push_back(entryPath);
			}
		}
		iter.increment(ec);
		if (ec)
		{
			WTSLogger::error("Failed while iterating index worker directory {}: {}", path, ec.message());
			return false;
		}
	}

	uint32_t count = 0;
	for (const fs::path& modulePath : modulePaths)
	{
		DllHandle hInst = DLLHelper::load_library(modulePath.string().c_str());
		if (hInst == NULL)
			continue;

		// Loading a module already owned by this manager adds a loader reference.
		// Balance only that new reference and keep the original factory/worker
		// ownership untouched; invoking creator again would overwrite its owner.
		bool alreadyLoaded = false;
		for (const auto& entry : _factories)
		{
			if (entry.second._module_inst == hInst)
			{
				alreadyLoaded = true;
				break;
			}
		}
		if (alreadyLoaded)
		{
			DLLHelper::free_library(hInst);
			continue;
		}

		FuncCreateIndexFact creator = (FuncCreateIndexFact)DLLHelper::get_symbol(hInst, "create_index_factory");
		if (creator == NULL)
		{
			DLLHelper::free_library(hInst);
			continue;
		}

		// Reserve the loader reference before calling plugin code. A synchronous
		// re-entry must not start another scan while this candidate is unknown.
		// If the creator or get_name throws, the plugin may already have started
		// work, so keep the reference pinned rather than unloading active code.
		_conflicting_module = hInst;
		IIndexWorkerFact* fact = nullptr;
		try
		{
			fact = creator();
		}
		catch (...)
		{
			WTSLogger::error("Index worker factory creator threw; module quarantined");
			return false;
		}
		if(fact != NULL)
		{
			_conflicting_factory = fact;
			std::string factName;
			try
			{
				const char* name = fact->get_name();
				if (name == nullptr || name[0] == '\0')
				{
					WTSLogger::error("Index worker factory has no name; candidate quarantined");
					return false;
				}
				factName = name;
			}
			catch (...)
			{
				WTSLogger::error("Reading index worker factory name failed; candidate quarantined");
				return false;
			}
			if (_factories.find(factName) != _factories.end())
			{
				WTSLogger::error("Duplicate index worker factory [{}]; candidate quarantined", factName);
				return false;
			}
			IdxFactInfo fInfo{};
			fInfo._module_inst = hInst;
			fInfo._module_path = modulePath.string();
			fInfo._creator = creator;
			fInfo._remover = (FuncDeleteIndexFact)DLLHelper::get_symbol(hInst, "delete_index_factory");
			fInfo._fact = fact;
			_factories.emplace(factName, std::move(fInfo));
			_conflicting_module = nullptr;
			_conflicting_factory = nullptr;

			WTSLogger::info("Index worker factory [{}] loaded", factName);

			count++;
		}
		else
		{
			_conflicting_module = nullptr;
			DLLHelper::free_library(hInst);
			continue;
		}
		
	}

	WTSLogger::info("{} index worker factories in directory [{}] loaded", count, path);

	return true;
}

IndexWorkerPtr IndexWorkerMgr::createWorker(const char* name, const char* id)
{
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return IndexWorkerPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	auto it = _factories.find(factname);
	if (it == _factories.end())
	{
		WTSLogger::error("Creating index worker with name {} failed, factory not exists", name);
		return IndexWorkerPtr();
	}

	IdxFactInfo& fInfo = (IdxFactInfo&)it->second;
	IIndexWorker* worker = fInfo._fact->create_worker(unitname, id);
	if (worker == NULL)
	{
		WTSLogger::error("Creating index worker with name {} failed, worker not exists", name);
		return IndexWorkerPtr();
	}

	IndexWorkerPtr ret(new IdxWorkerWrapper(worker, fInfo._fact));
	_workers[id] = ret;
	return ret;
}

bool IndexWorkerMgr::init(WTSVariant* config, IHotMgr* hotMgr, IBaseDataMgr* bdMgr, DataManager* dataMgr)
{
	_hot_mgr = hotMgr;
	_bd_mgr = bdMgr;
	_data_mgr = dataMgr;

	uint32_t poolsize = config->getUInt32("poolsize");
	if (poolsize > 0)
	{
		_pool.reset(new boost::threadpool::pool(poolsize));
	}

	WTSVariant* cfgIdx = config->get("indice");
	if (cfgIdx == NULL || !cfgIdx->isArray())
	{
		return false;
	}

	auto cnt = cfgIdx->size();
	for (std::size_t i = 0; i < cnt; i++)
	{
		WTSVariant* cfgItem = cfgIdx->get(i);
		if (!cfgItem->getBoolean("active"))
			continue;

		const char* id = cfgItem->getCString("id");
		const char* name = cfgItem->getCString("name");

		IndexWorkerPtr worker = createWorker(name, id);
		if (worker == NULL)
			continue;

		worker->self()->set_factory(this);

		if (!worker->self()->init(cfgItem))
			continue;
	}

	return true;
}

void IndexWorkerMgr::handle_quote(WTSTickData* newTick)
{
	if (newTick == NULL)
		return;

	const char* fullCode = newTick->getContractInfo()->getFullCode();
	auto it = _subbed.find(fullCode);
	if (it == _subbed.end())
		return;

	if (_pool)
	{
		newTick->retain();
		std::shared_ptr<WTSTickData> retainedTick(newTick, [](WTSTickData* tick) {
			if (tick != nullptr)
				tick->release();
		});
		_pool->schedule([this, retainedTick]() {

			for (auto& v : _workers)
			{
				auto& worker = v.second;
				worker->self()->handle_quote(retainedTick.get());
			}
		});
	}
	else
	{
		newTick->retain();
		TickReleaseGuard retainedTick(newTick);
		for (auto& v : _workers)
		{
			auto& worker = v.second;
			worker->self()->handle_quote(newTick);
		}
	}
}

void IndexWorkerMgr::push_tick(WTSTickData* newTick)
{
	_data_mgr->writeTick(newTick, 1);
}

WTSTickData* IndexWorkerMgr::sub_ticks(const char* fullCode)
{
	_subbed.insert(fullCode);

	auto ay = StrUtil::split(fullCode, ".");
	return _data_mgr->getCurTick(ay[1].c_str(), ay[0].c_str());
}

WTSTickData* IndexWorkerMgr::get_tick(const char* code, const char* exchg)
{
	return _data_mgr->getCurTick(code, exchg);
}

void IndexWorkerMgr::output_log(WTSLogLevel ll, const char* message)
{
	WTSLogger::log_raw(ll, message);
}
