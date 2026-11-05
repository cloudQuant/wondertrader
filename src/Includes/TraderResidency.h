#pragma once
// Decision K43 (2026-11-04, BUG-I02-NEW-052 / I03-NEW-037): ITraderApi offers no
// proof that a provider has stopped calling back. Once a host has created a trader
// API from a module, it therefore never deletes that API object and never unloads
// the module; both stay resident until process exit. A manager keeps the released
// adapter objects (the SPI the provider may still call) resident the same way.
// ITraderApi::release() is still called once so the provider stops its own work.
//
// The registry is intentionally never destroyed (no static destruction at exit).
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "WTSMarcos.h"

NS_WTP_BEGIN
class ITraderApi;

class TraderResidency
{
public:
	static void keepProvider(ITraderApi* api, void* module)
	{
		std::lock_guard<std::mutex> lock(mutex());
		providers().push_back(Provider{ api, module });
	}

	static void keepObject(const std::shared_ptr<void>& object)
	{
		if (!object)
			return;
		std::lock_guard<std::mutex> lock(mutex());
		objects().push_back(object);
	}

	static std::size_t providerCount()
	{
		std::lock_guard<std::mutex> lock(mutex());
		return providers().size();
	}

	static std::size_t objectCount()
	{
		std::lock_guard<std::mutex> lock(mutex());
		return objects().size();
	}

private:
	struct Provider
	{
		ITraderApi*	api;
		void*		module;
	};

	static std::mutex& mutex()
	{
		static std::mutex* instance = new std::mutex();
		return *instance;
	}

	static std::vector<Provider>& providers()
	{
		static std::vector<Provider>* instance = new std::vector<Provider>();
		return *instance;
	}

	static std::vector<std::shared_ptr<void>>& objects()
	{
		static std::vector<std::shared_ptr<void>>* instance = new std::vector<std::shared_ptr<void>>();
		return *instance;
	}
};

NS_WTP_END
