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
#include <utility>
namespace fs = std::filesystem;

namespace
{
std::string selFactoryLogText(const std::string& value)
{
	constexpr std::size_t maxDisplayLength = 128;
	return value.size() <= maxDisplayLength ? value : value.substr(0, maxDisplayLength) + "...";
}

// A loader may execute a module's static initializers before returning its
// handle. Once a handle is available, retain it before entering factory code.
// The registry is intentionally retained for process lifetime: the plugin ABI
// does not provide a general way to drain background activity after an
// exception. Separate copies of this translation unit have separate registries.
struct SelFactoryAttempt
{
	explicit SelFactoryAttempt(const std::string& path) : modulePath(path) {}
	std::string modulePath;
	DllHandle handle = nullptr;
	ISelStrategyFact* factory = nullptr;
	bool quarantined = false;
	bool noCreator = false;
	bool retainedConflict = false;
	SelFactoryAttempt* next = nullptr;
};

struct SelFactoryRegistry
{
	std::mutex mutex;
	SelFactoryAttempt* head = nullptr;
};

SelFactoryRegistry& selFactoryRegistry()
{
	static SelFactoryRegistry* registry = new SelFactoryRegistry;
	return *registry;
}

class SelFactoryGuard
{
public:
	enum class AttachResult { Attached, BusyOrQuarantined, NoCreator };
	explicit SelFactoryGuard(const std::string& path) : _registry(selFactoryRegistry()), _attempt(new SelFactoryAttempt(path)) {}
	SelFactoryGuard(const SelFactoryGuard&) = delete;
	SelFactoryGuard& operator=(const SelFactoryGuard&) = delete;

	~SelFactoryGuard()
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
		for (SelFactoryAttempt* current = _registry.head; current; current = current->next)
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

	void recordFactory(ISelStrategyFact* factory)
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		_attempt->factory = factory;
	}

	// Transfer ownership to an already-published manager entry, or discard an
	// attempt that never entered a creator. No plugin function runs under lock.
	void releaseRecord()
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		SelFactoryAttempt** slot = &_registry.head;
		while (*slot != _attempt)
			slot = &(*slot)->next;
		*slot = _attempt->next;
		delete _attempt;
		_attempt = nullptr;
		_attached = false;
	}

private:
	SelFactoryRegistry& _registry;
	SelFactoryAttempt* _attempt;
	bool _attached = false;
};
}




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

	// Bound diagnostics only: filesystem operations still receive the full path.
	std::string displayPath(path);
	if (displayPath.size() > 128)
	{
		displayPath.resize(128);
		displayPath += "...";
	}
	fs::path myPath(path);
	std::error_code ec;
	const bool isDirectory = fs::is_directory(myPath, ec);
	if (ec || !isDirectory)
	{
		WTSLogger::error("Directory {} of SEL strategy factory is not accessible: {}",
			displayPath.c_str(), selFactoryLogText(ec ? ec.message() : "not a directory"));
		return false;
	}

	uint32_t count = 0;
	bool refusedFactory = false;
	fs::directory_iterator iter(myPath, ec);
	if (ec)
	{
		WTSLogger::error("Cannot enumerate SEL strategy factory directory {}: {}", displayPath.c_str(), selFactoryLogText(ec.message()));
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
		SelFactoryGuard guard(modulePath);
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
			return;
		}
		const auto attachResult = guard.attach(hInst);
		if (attachResult != SelFactoryGuard::AttachResult::Attached)
		{
			// The earlier pending/quarantined attempt keeps its own loader ref.
			DLLHelper::free_library(hInst);
			if (attachResult != SelFactoryGuard::AttachResult::NoCreator)
			{
				refusedFactory = true;
				WTSLogger::error("SEL strategy factory module is already loading or quarantined");
			}
			else
				WTSLogger::warn("SEL strategy factory module was previously skipped and retained");
			return;
		}
		// Another scan may have completed between the first check and attach.
		// While this attempt is attached, no same-handle creator can start.
		const auto secondLocalState = localModuleState();
		if (secondLocalState != LocalModule::Unseen)
		{
			guard.releaseRecord();
			DLLHelper::free_library(hInst);
			return;
		}

		// Card 4b=A: only a validated versioned SEL table may create a factory.
		// Legacy v1 creators/deleters are never called (unknown deleter ABI and
		// stale inlined headers); such a module is kept and refused instead.
		const WtStrategyPluginApiV2* api = NULL;
		std::string rejectReason;
		const auto probe = wt_stra_plugin::probeModule(
			[hInst](const char* symbol) { return DLLHelper::get_symbol(hInst, symbol); },
			WT_STRA_PLUGIN_V2_FAMILY_SEL, api, rejectReason);
		if (probe == wt_stra_plugin::ProbeResult::NotStrategyPlugin ||
			probe == wt_stra_plugin::ProbeResult::ForeignFamily)
		{
			// dlopen may already have run module initializers and started work.
			// Keep one reference: the ABI gives no way to establish quiescence.
			// Plugins of other families keep SEL's ignored-module contract.
			if (probe == wt_stra_plugin::ProbeResult::ForeignFamily)
				WTSLogger::warn("SEL strategy factory directory entry {} skipped: {}",
					selFactoryLogText(modulePath).c_str(), rejectReason.c_str());
			guard.recordNoCreator();
			return;
		}
		if (probe != wt_stra_plugin::ProbeResult::Accepted)
		{
			// An old or invalid SEL binary is a deployment error: report it.
			refusedFactory = true;
			WTSLogger::error("SEL strategy plugin {} rejected: {}",
				selFactoryLogText(modulePath).c_str(), rejectReason.c_str());
			return; // no creator ran: the module stays quarantined with its loader ref
		}

		ISelStrategyFact* fact = static_cast<ISelStrategyFact*>(api->create_factory());
		guard.recordFactory(fact);
		if (fact == NULL)
		{
			// Preserve SEL's ignored-invalid-factory return contract on rescans.
			guard.recordNoCreator();
			WTSLogger::error("SEL strategy factory creator returned null; module quarantined");
			return; // creator has run: keep its module quarantined.
		}
		const char* rawName = fact->getName();
		if (rawName == NULL || rawName[0] == '\0')
		{
			guard.recordNoCreator();
			WTSLogger::error("SEL strategy factory name is empty; module quarantined");
			return;
		}
		const std::string name(rawName);
		StraFactInfo info{};
		info._module_inst = hInst;
		info._module_path = modulePath;
		// v1 entry points are deliberately not recorded; the manager never
		// destroys SEL factories (they stay with their module until exit).
		info._creator = NULL;
		info._remover = NULL;
		info._api = api;
		info._fact = fact;
		// WTSLogger formats into a fixed buffer. Bound only the displayed name;
		// the full plugin-provided name remains the registration identity.
		std::string displayName = name.substr(0, 128);
		if (name.size() > 128)
			displayName += "...";
		WTSLogger::info("Preparing SEL strategy factory[{}]", displayName.c_str());
		bool inserted = false;
		{
			std::lock_guard<std::mutex> lock(_factories_mutex);
			inserted = _factories.emplace(name, std::move(info)).second;
			if (!inserted)
				_conflicting_factory_modules.insert(hInst);
		}
		if (!inserted)
		{
			guard.retainLocalConflict();
			WTSLogger::error("Duplicate SEL strategy factory name; instance retained");
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
			WTSLogger::error("Cannot inspect SEL factory entry in {}: {}", displayPath.c_str(), selFactoryLogText(ec.message()));
			return false;
		}
		if (!isSubdirectory)
			loadFactory(factoryPath);

		iter.increment(ec);
		if (ec)
		{
			WTSLogger::error("Cannot continue enumerating SEL strategy factory directory {}: {}", displayPath.c_str(), selFactoryLogText(ec.message()));
			return false;
		}
	}

	WTSLogger::info("{} SEL strategy factories in directory[{}] loaded", count, displayPath.c_str());

	return !refusedFactory;
}

SelStrategyPtr SelStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	if (!factname || !unitname || !id)
		return SelStrategyPtr();
	ISelStrategyFact* fact = nullptr;
	{
		std::lock_guard<std::mutex> lock(_factories_mutex);
		auto it = _factories.find(factname);
		if (it == _factories.end())
			return SelStrategyPtr();
		fact = it->second._fact;
	}
	if (!fact)
		return SelStrategyPtr();
	// A null entry reserves the ID while the external creator is running.
	const std::string strategyId(id);
	{
		std::lock_guard<std::mutex> lock(_strategies_mutex);
		if (!_strategies.emplace(strategyId, SelStrategyPtr()).second)
			return SelStrategyPtr();
	}
	auto releaseReservation = [this](const std::string* reservedId) noexcept {
		std::lock_guard<std::mutex> lock(_strategies_mutex);
		_strategies.erase(*reservedId);
	};
	std::unique_ptr<const std::string, decltype(releaseReservation)> reservation(&strategyId, releaseReservation);
	auto destroyStrategy = [fact](SelStrategy* strategy) noexcept {
		if (strategy)
			fact->deleteStrategy(strategy);
	};
	std::unique_ptr<SelStrategy, decltype(destroyStrategy)> strategy(
		fact->createStrategy(unitname, id), destroyStrategy);
	if (!strategy)
		return SelStrategyPtr();
	SelStraWrapper* rawWrapper = new SelStraWrapper(strategy.get(), fact);
	strategy.release();
	SelStrategyPtr ret(rawWrapper);
	{
		std::lock_guard<std::mutex> lock(_strategies_mutex);
		_strategies.find(strategyId)->second = ret;
		reservation.release();
	}
	return ret;
}

SelStrategyPtr SelStrategyMgr::createStrategy(const char* name, const char* id)
{
	if (!name || !id)
		return SelStrategyPtr();
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return SelStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	return createStrategy(factname, unitname, id);
}

SelStrategyPtr SelStrategyMgr::getStrategy(const char* id)
{
	if (!id)
		return SelStrategyPtr();
	std::lock_guard<std::mutex> lock(_strategies_mutex);
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return SelStrategyPtr();

	return it->second;
}

