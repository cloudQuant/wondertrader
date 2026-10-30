#include "Includes/ITraderApi.h"
#include "Includes/WTSVariant.hpp"

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace
{
std::mutex g_log_mutex;

std::string logPath()
{
	const char* path = std::getenv("WT_UFT_LIFECYCLE_LOG");
	return path == nullptr ? std::string() : std::string(path);
}

void record(const std::string& event)
{
	const std::string path = logPath();
	if (path.empty())
		return;
	std::lock_guard<std::mutex> lock(g_log_mutex);
	std::ofstream out(path, std::ios::app);
	if (out)
		out << event << '\n';
}

bool envEnabled(const char* name)
{
	const char* value = std::getenv(name);
	return value != nullptr && std::string(value) == "1";
}

class LifecycleTrader : public wtp::ITraderApi
{
public:
	~LifecycleTrader() override
	{
		record("api.destroy refs=" + std::to_string(_params ? _params->retainCount() : 0));
	}

	bool init(wtp::WTSVariant* params) override
	{
		_params = params;
		const std::string mode = params == nullptr ? std::string() : params->getString("lifecycle_mode");
		record("api.init refs=" + std::to_string(_params ? _params->retainCount() : 0));
		if (mode == "init_throw")
		{
			record("api.init.throw");
			throw std::runtime_error("fake lifecycle init throw");
		}
		if (mode == "init_false")
		{
			record("api.init.false");
			return false;
		}
		return true;
	}

	void release() override
	{
		record("api.release refs=" + std::to_string(_params ? _params->retainCount() : 0));
		if (envEnabled("WT_UFT_LIFECYCLE_RELEASE_THROW"))
		{
			record("api.release.throw");
			throw std::runtime_error("fake lifecycle release throw");
		}
	}

private:
	wtp::WTSVariant* _params{nullptr};
};
}

#if defined(_WIN32)
#define UFT_LIFECYCLE_EXPORT __declspec(dllexport)
#else
#define UFT_LIFECYCLE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" UFT_LIFECYCLE_EXPORT wtp::ITraderApi* createTrader()
{
	record("factory.create");
	const char* mode = std::getenv("WT_UFT_LIFECYCLE_CREATE_MODE");
	if (mode != nullptr && std::string(mode) == "throw")
	{
		record("factory.create.throw");
		throw std::runtime_error("fake lifecycle factory throw");
	}
	if (mode != nullptr && std::string(mode) == "null")
	{
		record("factory.create.null");
		return nullptr;
	}
	return new LifecycleTrader();
}

extern "C" UFT_LIFECYCLE_EXPORT void deleteTrader(wtp::ITraderApi*& trader)
{
	record("factory.delete.begin");
	if (std::getenv("WT_UFT_LIFECYCLE_DELETE_THROW") != nullptr &&
		std::string(std::getenv("WT_UFT_LIFECYCLE_DELETE_THROW")) == "1")
	{
		record("factory.delete.throw");
		throw std::runtime_error("fake lifecycle delete throw");
	}
	delete trader;
	trader = nullptr;
	record("factory.delete.end");
}
