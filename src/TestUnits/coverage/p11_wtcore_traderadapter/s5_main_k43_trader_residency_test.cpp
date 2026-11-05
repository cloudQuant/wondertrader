// 迭代03 第五会话主线程：决策 K43（2026-11-04 用户裁定，BUG-I02-NEW-052 / I03-NEW-037）
// ITraderApi 无法证明 provider 回调已停止：模块创建的 trader API 不删除、模块不卸载，常驻到进程退出；
// 管理器释放的模块适配器对象同样常驻。改前 release() 无条件 deleteTrader + free_library。
#include "r59_trader_fixture.h"

#include <dlfcn.h>
#include <filesystem>
#include <memory>

#include "Includes/TraderResidency.h"

namespace {

namespace k43fs = std::filesystem;

class K43InstDir
{
public:
	explicit K43InstDir(const std::string& dir) : _old(WtHelper::getInstDir()) { WtHelper::setInstDir(dir.c_str()); }
	~K43InstDir() { WtHelper::setInstDir(_old.c_str()); }
private:
	std::string _old;
};

class S5MainK43TraderResidency : public R59TraderTest
{
protected:
	k43fs::path install(const char* tag)
	{
		const k43fs::path root = tmp_ / tag;
		const k43fs::path traders = root / "traders";
		std::error_code ec;
		k43fs::create_directories(traders, ec);
		const k43fs::path plugin(R59_CREDENTIALS_PLUGIN_PATH);
		const k43fs::path installed = traders / plugin.filename();
		k43fs::copy_file(plugin, installed, k43fs::copy_options::overwrite_existing, ec);
		EXPECT_FALSE(ec) << ec.message();
		return installed;
	}

	WTSVariant* cfg(const k43fs::path& record)
	{
		WTSVariant* c = WTSVariant::createObject();
		c->append("module", "R59CredentialsTrader");
		c->append("savedata", false);
		c->append("user", "k43_user");
		c->append("pass", "k43_pass");
		c->append("r59_login_record", record.string().c_str());
		return c;
	}

	static bool loaded(const k43fs::path& module)
	{
		void* handle = ::dlopen(module.string().c_str(), RTLD_NOW | RTLD_NOLOAD);
		if (handle == NULL)
			return false;
		::dlclose(handle);	// 只撤销本次探测增加的引用
		return true;
	}
};

} // namespace

TEST_F(S5MainK43TraderResidency, ReleasedModuleProviderAndModuleStayResident)
{
	const k43fs::path installed = install("k43_direct");
	K43InstDir inst((installed.parent_path().parent_path()).string() + k43fs::path::preferred_separator);
	const std::size_t before = TraderResidency::providerCount();
	{
		TraderAdapter trader;
		WTSVariant* c = cfg(tmp_ / "k43_direct_login.txt");
		ASSERT_TRUE(trader.init(id_.c_str(), c, &bd_, nullptr));
		c->release();
		ASSERT_TRUE(trader.run());	// registerSpi + connect: the provider now has a callback channel
		ASSERT_TRUE(loaded(installed));
		trader.release();
	}
	EXPECT_EQ(before + 1, TraderResidency::providerCount()) << "the module-created API must be kept, not deleted";
	EXPECT_TRUE(loaded(installed)) << "the trader module must stay loaded after release";
}

TEST_F(S5MainK43TraderResidency, ProviderWithoutCallbackChannelIsStillDeletedAndUnloaded)
{
	const k43fs::path installed = install("k43_noreg");
	K43InstDir inst((installed.parent_path().parent_path()).string() + k43fs::path::preferred_separator);
	const std::size_t before = TraderResidency::providerCount();
	{
		TraderAdapter trader;
		WTSVariant* c = cfg(tmp_ / "k43_noreg_login.txt");
		ASSERT_TRUE(trader.init(id_.c_str(), c, &bd_, nullptr));
		c->release();
		trader.release();	// never ran: no SPI was registered, so no callback can arrive
	}
	EXPECT_EQ(before, TraderResidency::providerCount());
	EXPECT_FALSE(loaded(installed)) << "a provider that never had a callback channel is unloaded as before";
}

TEST_F(S5MainK43TraderResidency, ManagerKeepsReleasedModuleAdapterObjects)
{
	const k43fs::path installed = install("k43_mgr");
	K43InstDir inst((installed.parent_path().parent_path()).string() + k43fs::path::preferred_separator);
	std::weak_ptr<TraderAdapter> watch;
	{
		TraderAdapterMgr mgr;
		TraderAdapterPtr adapter(new TraderAdapter());
		WTSVariant* c = cfg(tmp_ / "k43_mgr_login.txt");
		ASSERT_TRUE(adapter->init("k43_mgr", c, &bd_, nullptr));
		c->release();
		ASSERT_TRUE(adapter->run());
		ASSERT_TRUE(mgr.addAdapter("k43_mgr", adapter));
		watch = adapter;
		adapter.reset();
		mgr.release();
	}
	EXPECT_FALSE(watch.expired()) << "a released module adapter must stay resident until process exit";
	EXPECT_TRUE(loaded(installed));
}
