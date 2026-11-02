#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <type_traits>

#include <filesystem>
#include <dlfcn.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSVariant.hpp"
#include "Share/DLLHelper.hpp"
#include "WtMsgQue/WtMsgQue.h"

#define private public
#include "WtUftCore/EventNotifier.h"
#undef private
#include "WtUftCore/WtHelper.h"

using namespace wtp;

static_assert(std::is_same<FuncCreateMQServer, decltype(&::create_server)>::value,
	"EventNotifier create_server ABI must match the WtMsgQue export");

namespace {

std::string binaryDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binaryDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();

	std::string dir = std::filesystem::path(info.dli_fname).parent_path().parent_path().string();
	if (!dir.empty() && dir.back() != '/')
		dir.push_back('/');
	return dir;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

class VariantGuard
{
public:
	explicit VariantGuard(WTSVariant* value) : _value(value) {}
	~VariantGuard() { if (_value != nullptr) _value->release(); }
	VariantGuard(const VariantGuard&) = delete;
	VariantGuard& operator=(const VariantGuard&) = delete;
	WTSVariant* get() const { return _value; }

private:
	WTSVariant* _value;
};

VariantGuard makeConfig(const std::string& url)
{
	WTSVariant* cfg = WTSVariant::createObject();
	if (cfg != nullptr)
	{
		cfg->append("active", true);
		cfg->append("url", url.c_str());
	}
	return VariantGuard(cfg);
}

std::string nextValidUrl()
{
	static std::atomic<uint32_t> seq { 0 };
	return "inproc://r143_mq_failure_" + std::to_string(static_cast<long>(::getpid()))
		+ "_" + std::to_string(++seq);
}

} // namespace

TEST(R143UftNotifierInit, PropagatesBindFailureWithoutInitializingAndCanRetry)
{
	const std::string bin = binaryDir();
	ASSERT_FALSE(bin.empty());
	const std::string modulePath = bin + DLLHelper::wrap_module("WtMsgQue", "lib");
	ASSERT_TRUE(std::filesystem::exists(modulePath)) << modulePath;
	InstDirGuard instdir(bin);

	const std::string badUrl = "r143-unsupported://forced-bind-failure";
	DllHandle inspector = DLLHelper::load_library(modulePath.c_str());
	ASSERT_NE(nullptr, inspector);
	const auto createServer = reinterpret_cast<FuncCreateMQServer>(
		DLLHelper::get_symbol(inspector, "create_server"));
	const auto destroyServer = reinterpret_cast<FuncDestroyMQServer>(
		DLLHelper::get_symbol(inspector, "destroy_server"));
	ASSERT_NE(nullptr, createServer);
	ASSERT_NE(nullptr, destroyServer);
	const WtUInt32 failedServerId = createServer(badUrl.c_str(), false);
	EXPECT_EQ(0u, failedServerId)
		<< "WtMsgQue must propagate MQServer::init(nn_bind) failure as server ID 0";
	if (failedServerId != 0)
		destroyServer(failedServerId);
	DLLHelper::free_library(inspector);

	EventNotifier notifier;
	VariantGuard failedCfg = makeConfig(badUrl);
	ASSERT_NE(nullptr, failedCfg.get());
	EXPECT_FALSE(notifier.init(failedCfg.get()));
	EXPECT_EQ(nullptr, notifier._dll_handle);
	EXPECT_EQ(0u, notifier._mq_sid);
	EXPECT_EQ(nullptr, notifier._creator);
	EXPECT_EQ(nullptr, notifier._remover);
	EXPECT_EQ(nullptr, notifier._publisher);
	EXPECT_EQ(nullptr, notifier._register);
	EXPECT_EQ(nullptr, notifier._worker);
	EXPECT_EQ(nullptr, notifier._work_guard);

	VariantGuard validCfg = makeConfig(nextValidUrl());
	ASSERT_NE(nullptr, validCfg.get());
	ASSERT_TRUE(notifier.init(validCfg.get()));
	EXPECT_NE(nullptr, notifier._dll_handle);
	EXPECT_NE(0u, notifier._mq_sid);
	ASSERT_NE(nullptr, notifier._worker);
	EXPECT_TRUE(notifier._worker->joinable());
}
