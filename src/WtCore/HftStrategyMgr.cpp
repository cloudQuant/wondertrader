/*!
 * \file HftStrategyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "HftStrategyMgr.h"

#include <filesystem>
#include <system_error>
#include <utility>
namespace fs = std::filesystem;

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"


namespace
{
std::string hftFactoryLogText(const std::string& value)
{
	constexpr std::size_t maxDisplayLength = 128;
	if (value.size() <= maxDisplayLength)
		return value;
	return value.substr(0, maxDisplayLength) + "...";
}

// A loader may execute a module's static initializers before returning its
// handle. Once a handle is available, retain it before entering factory code.
// The registry is intentionally retained for process lifetime: the plugin ABI
// does not provide a general way to drain background activity after an
// exception. Separate copies of this translation unit have separate registries.
struct HftFactoryAttempt
{
	explicit HftFactoryAttempt(const std::string& path) : modulePath(path) {}
	std::string modulePath;
	DllHandle handle = nullptr;
	IHftStrategyFact* factory = nullptr;
	bool quarantined = false;
	bool noCreator = false;
	bool retainedConflict = false;
	HftFactoryAttempt* next = nullptr;
};

struct HftFactoryRegistry
{
	std::mutex mutex;
	HftFactoryAttempt* head = nullptr;
};

HftFactoryRegistry& hftFactoryRegistry()
{
	static HftFactoryRegistry* registry = new HftFactoryRegistry;
	return *registry;
}

class HftFactoryGuard
{
public:
	enum class AttachResult { Attached, BusyOrQuarantined, NoCreator };
	explicit HftFactoryGuard(const std::string& path) : _registry(hftFactoryRegistry()), _attempt(new HftFactoryAttempt(path)) {}
	HftFactoryGuard(const HftFactoryGuard&) = delete;
	HftFactoryGuard& operator=(const HftFactoryGuard&) = delete;

	~HftFactoryGuard()
	{
		if (!_attempt)
			return;
		if (!_attached)
		{
			delete _attempt;
			return;
		}
		std::lock_guard<std::mutex> lock(_registry.mutex);
		if (!_attempt->retainedConflict)
			_attempt->quarantined = true;
	}

	AttachResult attach(DllHandle handle)
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		for (HftFactoryAttempt* current = _registry.head; current; current = current->next)
			if (current->handle == handle && !current->retainedConflict)
				return current->noCreator ? AttachResult::NoCreator : AttachResult::BusyOrQuarantined;
		_attempt->handle = handle;
		_attempt->next = _registry.head;
		_registry.head = _attempt;
		_attached = true;
		return AttachResult::Attached;
	}

	void recordNoCreator()
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		_attempt->noCreator = true;
	}

	void retainLocalConflict()
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		_attempt->retainedConflict = true;
	}

	void recordFactory(IHftStrategyFact* factory)
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		_attempt->factory = factory;
	}

	// Transfer ownership to an already-published manager entry, or discard an
	// attempt that never entered a creator. No plugin function runs under lock.
	void releaseRecord()
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		HftFactoryAttempt** slot = &_registry.head;
		while (*slot != _attempt)
			slot = &(*slot)->next;
		*slot = _attempt->next;
		delete _attempt;
		_attempt = nullptr;
		_attached = false;
	}

private:
	HftFactoryRegistry& _registry;
	HftFactoryAttempt* _attempt;
	bool _attached = false;
};
}


HftStrategyMgr::HftStrategyMgr()
{
}


HftStrategyMgr::~HftStrategyMgr()
{
}

bool HftStrategyMgr::loadFactories(const char* path)
{
	if (path == NULL || path[0] == '\0')
	{
		WTSLogger::error("Directory of HFT strategy factory is empty");
		return false;
	}

	fs::path myPath(path);
	// Keep the filesystem path intact; bound only text sent to WTSLogger's
	// fixed-size formatting buffer.
	const std::string displayPath = hftFactoryLogText(myPath.string());
	std::error_code ec;
	const bool isDirectory = fs::is_directory(myPath, ec);
	if (ec || !isDirectory)
	{
		WTSLogger::error("Directory {} of HFT strategy factory is not accessible: {}",
			displayPath, hftFactoryLogText(ec ? ec.message() : "not a directory"));
		return false;
	}

	uint32_t count = 0;
	bool refusedFactory = false;
	fs::directory_iterator iter(myPath, ec);
	if (ec)
	{
		WTSLogger::error("Cannot enumerate HFT strategy factory directory {}: {}", displayPath, hftFactoryLogText(ec.message()));
		return false;
	}
	const fs::directory_iterator endIter;
	auto loadFactory = [this, &count, &refusedFactory](const fs::path& factoryPath)
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

		const std::string modulePath = factoryPath.string();
		// All allocations required for the quarantine record precede dlopen.
		HftFactoryGuard guard(modulePath);
		DllHandle hInst = DLLHelper::load_library(modulePath.c_str());
		if (hInst == NULL)
			return;
		enum class LocalModule { Unseen, Registered, Conflicting };
		auto localModuleState = [this, hInst]() {
			std::lock_guard<std::mutex> lock(_factories_mutex);
			for (const auto& entry : _factories)
				if (entry.second._module_inst == hInst)
					return LocalModule::Registered;
			return _conflicting_factory_modules.count(hInst) ? LocalModule::Conflicting : LocalModule::Unseen;
		};
		const auto firstLocalState = localModuleState();
		if (firstLocalState != LocalModule::Unseen)
		{
			// Balance only this scan's loader reference; the manager's original
			// reference still protects its factory and any strategy wrappers.
			DLLHelper::free_library(hInst);
			if (firstLocalState == LocalModule::Conflicting)
				refusedFactory = true;
			return;
		}
		const auto attachResult = guard.attach(hInst);
		if (attachResult != HftFactoryGuard::AttachResult::Attached)
		{
			// The earlier pending/quarantined attempt keeps its own loader ref.
			DLLHelper::free_library(hInst);
			if (attachResult != HftFactoryGuard::AttachResult::NoCreator)
			{
				refusedFactory = true;
				WTSLogger::error("HFT strategy factory module is already loading or quarantined");
			}
			return;
		}
		// Another scan may have completed between the first check and attach.
		// While this attempt is attached, no same-handle creator can start.
		const auto secondLocalState = localModuleState();
		if (secondLocalState != LocalModule::Unseen)
		{
			guard.releaseRecord();
			DLLHelper::free_library(hInst);
			if (secondLocalState == LocalModule::Conflicting)
				refusedFactory = true;
			return;
		}

		FuncCreateHftStraFact creator = (FuncCreateHftStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
		if (creator == NULL)
		{
			// dlopen may already have run module initializers and started work.
			// Keep one reference: the ABI gives no way to establish quiescence.
			guard.recordNoCreator();
			return;
		}

		IHftStrategyFact* fact = creator();
		guard.recordFactory(fact);
		if (fact == NULL)
		{
			refusedFactory = true;
			WTSLogger::error("HFT strategy factory creator returned null; module quarantined");
			return; // creator has run: keep its module quarantined.
		}
		const char* rawName = fact->getName();
		if (rawName == NULL || rawName[0] == '\0')
		{
			refusedFactory = true;
			WTSLogger::error("HFT strategy factory name is empty; module quarantined");
			return;
		}
		const std::string name(rawName);
		// Prepare all potentially throwing fields before publishing a factory.
		std::string entryPath(modulePath);
		FuncDeleteHftStraFact remover = (FuncDeleteHftStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
		// Not active until publication: on any preparation failure the guard
		// retains the untrusted factory and module without calling its remover.
		auto owner = std::make_shared<FactoryOwner>(fact, remover, hInst);
		const std::string displayName = hftFactoryLogText(name);
		WTSLogger::info("Preparing HFT strategy factory[{}]", displayName.c_str());
		bool inserted = false;
		{
			std::lock_guard<std::mutex> lock(_factories_mutex);
			auto result = _factories.try_emplace(name);
			inserted = result.second;
			if (inserted)
			{
				// Node-based value has an owning destructor and is never copied.
				StraFactInfo& info = result.first->second;
				info._module_path.swap(entryPath);
				info._module_inst = hInst;
				info._creator = creator;
				info._remover = remover;
				info._fact = fact;
				info._owner = owner;
				owner->_active = true;
			}
			else
				_conflicting_factory_modules.insert(hInst);
		}
		if (!inserted)
		{
			guard.retainLocalConflict();
			refusedFactory = true;
			WTSLogger::error("Duplicate HFT strategy factory name; instance retained");
			return;
		}
		guard.releaseRecord();
		count++;
	};

	while (iter != endIter)
	{
		const fs::path factoryPath = iter->path();
		ec.clear();
		const bool isSubdirectory = fs::is_directory(factoryPath, ec);
		if (ec)
		{
			WTSLogger::error("Cannot inspect HFT factory entry in {}: {}", displayPath, hftFactoryLogText(ec.message()));
			return false;
		}
		if (!isSubdirectory)
			loadFactory(factoryPath);

		iter.increment(ec);
		if (ec)
		{
			WTSLogger::error("Cannot continue enumerating HFT strategy factory directory {}: {}", displayPath, hftFactoryLogText(ec.message()));
			return false;
		}
	}

	WTSLogger::info("{} HFT strategy factories in directory[{}] loaded", count, displayPath);

	return !refusedFactory;
}

HftStrategyPtr HftStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	IHftStrategyFact* fact = nullptr;
	std::shared_ptr<FactoryOwner> owner;
	{
		std::lock_guard<std::mutex> lock(_factories_mutex);
		auto it = _factories.find(factname);
		if (it == _factories.end())
			return HftStrategyPtr();
		fact = it->second._fact;
		owner = it->second._owner;
	}
	if (fact == nullptr)
		return HftStrategyPtr();
	// If allocating the wrapper fails, return the newly-created strategy to its
	// still-live factory. After construction, shared_ptr invokes its deleter
	// even when allocation of the control block fails.
	auto destroyStrategy = [fact](HftStrategy* strategy) noexcept {
		if (strategy)
			fact->deleteStrategy(strategy);
	};
	std::unique_ptr<HftStrategy, decltype(destroyStrategy)> strategy(
		fact->createStrategy(unitname, id), destroyStrategy);
	auto destroyWrapper = [owner](HftStraWrapper* wrapper) mutable noexcept {
		// Empty the control block's capture before deletion: weak_ptrs may keep
		// that control block alive after the final strong wrapper disappears.
		auto keepFactoryAlive = std::move(owner);
		delete wrapper;
	};
	HftStraWrapper* rawWrapper = new HftStraWrapper(strategy.get(), fact);
	strategy.release();
	HftStrategyPtr ret(rawWrapper, std::move(destroyWrapper));
	_strategies[id] = ret;
	return ret;
}

HftStrategyPtr HftStrategyMgr::createStrategy(const char* name, const char* id)
{
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return HftStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	return createStrategy(factname, unitname, id);
}

HftStrategyPtr HftStrategyMgr::getStrategy(const char* id)
{
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return HftStrategyPtr();

	return it->second;
}
