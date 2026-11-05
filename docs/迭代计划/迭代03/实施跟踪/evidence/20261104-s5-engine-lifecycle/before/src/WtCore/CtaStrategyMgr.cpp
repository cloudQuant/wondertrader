/*!
 * \file CtaStrategyMgr.cpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 
 */
#include "CtaStrategyMgr.h"

#include "../Share/StrUtil.hpp"
#include "../Share/StdUtils.hpp"

#include "../WTSTools/WTSLogger.h"

#include <filesystem>
#include <system_error>
#include <utility>
namespace fs = std::filesystem;

namespace
{
std::string ctaPathForLog(const char* path)
{
	constexpr std::size_t limit = 128;
	std::size_t length = 0;
	while (length < limit && path[length] != '\0')
		++length;
	std::string display(path, length);
	if (length == limit && path[length] != '\0')
		display += "...";
	return display;
}

// A loader may execute a module's static initializers before returning its
// handle. Once a handle is available, retain it before entering factory code.
// The registry is intentionally retained for process lifetime: the plugin ABI
// does not provide a general way to drain background activity after an
// exception. Separate copies of this translation unit have separate registries.
struct CtaFactoryAttempt
{
	explicit CtaFactoryAttempt(const std::string& path) : modulePath(path) {}
	std::string modulePath;
	DllHandle handle = nullptr;
	ICtaStrategyFact* factory = nullptr;
	bool quarantined = false;
	bool noCreator = false;
	bool retainedConflict = false;
	CtaFactoryAttempt* next = nullptr;
};

struct CtaFactoryRegistry
{
	std::mutex mutex;
	CtaFactoryAttempt* head = nullptr;
};

CtaFactoryRegistry& ctaFactoryRegistry()
{
	static CtaFactoryRegistry* registry = new CtaFactoryRegistry;
	return *registry;
}

class CtaFactoryGuard
{
public:
	enum class AttachResult { Attached, BusyOrQuarantined, NoCreator };
	explicit CtaFactoryGuard(const std::string& path) : _registry(ctaFactoryRegistry()), _attempt(new CtaFactoryAttempt(path)) {}
	CtaFactoryGuard(const CtaFactoryGuard&) = delete;
	CtaFactoryGuard& operator=(const CtaFactoryGuard&) = delete;

	~CtaFactoryGuard()
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
		for (CtaFactoryAttempt* current = _registry.head; current; current = current->next)
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

	void recordFactory(ICtaStrategyFact* factory)
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		_attempt->factory = factory;
	}

	// Transfer ownership to an already-published manager entry, or discard an
	// attempt that never entered a creator. No plugin function runs under lock.
	void releaseRecord()
	{
		std::lock_guard<std::mutex> lock(_registry.mutex);
		CtaFactoryAttempt** slot = &_registry.head;
		while (*slot != _attempt)
			slot = &(*slot)->next;
		*slot = _attempt->next;
		delete _attempt;
		_attempt = nullptr;
		_attached = false;
	}

private:
	CtaFactoryRegistry& _registry;
	CtaFactoryAttempt* _attempt;
	bool _attached = false;
};
}


CtaStrategyMgr::CtaStrategyMgr()
{
}


CtaStrategyMgr::~CtaStrategyMgr()
{
}

bool CtaStrategyMgr::loadFactories(const char* path)
{
	if (path == NULL || path[0] == '\0')
	{
		WTSLogger::error("Directory of CTA strategy factory is empty");
		return false;
	}

	// WTSLogger uses a fixed formatting buffer. Keep the full path for all
	// filesystem and loader operations; bound only its diagnostic rendering.
	const std::string displayPath = ctaPathForLog(path);
	fs::path myPath(path);
	std::error_code ec;
	const bool isDirectory = fs::is_directory(myPath, ec);
	if (ec || !isDirectory)
	{
		WTSLogger::error("Directory {} of CTA strategy factory is not accessible: {}",
			displayPath.c_str(), ec ? ec.message() : "not a directory");
		return false;
	}

	uint32_t count = 0;
	bool refusedFactory = false;
	fs::directory_iterator iter(myPath, ec);
	if (ec)
	{
		WTSLogger::error("Cannot enumerate CTA strategy factory directory {}: {}", displayPath.c_str(), ec.message());
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
		CtaFactoryGuard guard(modulePath);
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
		if (attachResult != CtaFactoryGuard::AttachResult::Attached)
		{
			// The earlier pending/quarantined attempt keeps its own loader ref.
			DLLHelper::free_library(hInst);
			if (attachResult != CtaFactoryGuard::AttachResult::NoCreator)
			{
				refusedFactory = true;
				WTSLogger::error("CTA strategy factory module is already loading or quarantined");
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

		FuncCreateStraFact creator = (FuncCreateStraFact)DLLHelper::get_symbol(hInst, "createStrategyFact");
		if (creator == NULL)
		{
			// dlopen may already have run module initializers and started work.
			// Keep one reference: the ABI gives no way to establish quiescence.
			guard.recordNoCreator();
			return;
		}

		ICtaStrategyFact* fact = creator();
		guard.recordFactory(fact);
		if (fact == NULL)
		{
			refusedFactory = true;
			WTSLogger::error("CTA strategy factory creator returned null; module quarantined");
			return; // creator has run: keep its module quarantined.
		}
		const char* rawName = fact->getName();
		if (rawName == NULL || rawName[0] == '\0')
		{
			refusedFactory = true;
			WTSLogger::error("CTA strategy factory name is empty; module quarantined");
			return;
		}
		const std::string name(rawName);
		StraFactInfo info{};
		info._module_inst = hInst;
		info._module_path = modulePath;
		info._creator = creator;
		info._remover = (FuncDeleteStraFact)DLLHelper::get_symbol(hInst, "deleteStrategyFact");
		info._fact = fact;
		// WTSLogger formats into a fixed buffer. Bound only the displayed name;
		// the full plugin-provided name remains the registration identity.
		std::string displayName = name.substr(0, 128);
		if (name.size() > 128)
			displayName += "...";
		WTSLogger::info("Preparing CTA strategy factory[{}]", displayName.c_str());
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
			refusedFactory = true;
			WTSLogger::error("Duplicate CTA strategy factory name; instance retained");
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
			WTSLogger::error("Cannot inspect CTA factory entry in {}: {}", displayPath.c_str(), ec.message());
			return false;
		}
		if (!isSubdirectory)
			loadFactory(factoryPath);

		iter.increment(ec);
		if (ec)
		{
			WTSLogger::error("Cannot continue enumerating CTA strategy factory directory {}: {}", displayPath.c_str(), ec.message());
			return false;
		}
	}

	WTSLogger::info("{} CTA strategy factories in directory[{}] loaded", count, displayPath.c_str());

	return !refusedFactory;
}

CtaStrategyPtr CtaStrategyMgr::createStrategy(const char* factname, const char* unitname, const char* id)
{
	ICtaStrategyFact* fact = nullptr;
	{
		std::lock_guard<std::mutex> lock(_factories_mutex);
		auto it = _factories.find(factname);
		if (it == _factories.end())
			return CtaStrategyPtr();
		fact = it->second._fact;
	}
	CtaStrategyPtr ret(new CtaStraWrapper(fact->createStrategy(unitname, id), fact));
	_strategies[id] = ret;
	return ret;
}

CtaStrategyPtr CtaStrategyMgr::createStrategy(const char* name, const char* id)
{
	StringVector ay = StrUtil::split(name, ".");
	if (ay.size() < 2)
		return CtaStrategyPtr();

	const char* factname = ay[0].c_str();
	const char* unitname = ay[1].c_str();

	ICtaStrategyFact* fact = nullptr;
	{
		std::lock_guard<std::mutex> lock(_factories_mutex);
		auto it = _factories.find(factname);
		if (it == _factories.end())
			return CtaStrategyPtr();
		fact = it->second._fact;
	}
	CtaStrategyPtr ret(new CtaStraWrapper(fact->createStrategy(unitname, id), fact));
	_strategies[id] = ret;
	return ret;
}

CtaStrategyPtr CtaStrategyMgr::getStrategy(const char* id)
{
	auto it = _strategies.find(id);
	if (it == _strategies.end())
		return CtaStrategyPtr();

	return it->second;
}

