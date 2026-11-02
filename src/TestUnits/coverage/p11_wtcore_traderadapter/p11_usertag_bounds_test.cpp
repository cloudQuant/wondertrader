#include "p11_trader_fixture.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& path)
		: old_dir_(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(path.c_str());
	}

	~InstDirGuard()
	{
		WtHelper::setInstDir(old_dir_.c_str());
	}

private:
	std::string old_dir_;
};

struct VariantDeleter
{
	void operator()(wtp::WTSVariant* variant) const
	{
		if (variant != nullptr)
			variant->release();
	}
};

} // namespace

TEST_F(P11TraderTest, init_rejects_overlong_ids_and_keeps_empty_id_accepted)
{
	id_.assign(49, 'i');

	const fs::path module_root = tmp_ / "usertag_bounds_module";
	const fs::path trader_dir = module_root / "traders";
	std::error_code ec;
	fs::create_directories(trader_dir, ec);
	ASSERT_FALSE(ec);

	const fs::path plugin_path(R59_CREDENTIALS_PLUGIN_PATH);
	ASSERT_TRUE(fs::is_regular_file(plugin_path, ec)) << plugin_path.string();
	ASSERT_FALSE(ec);
	fs::copy_file(plugin_path, trader_dir / plugin_path.filename(), fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	std::string inst_dir = module_root.string() + fs::path::preferred_separator;
	InstDirGuard inst_dir_guard(inst_dir);

	const fs::path login_record = tmp_ / "usertag_bounds_login.txt";
	std::unique_ptr<wtp::WTSVariant, VariantDeleter> cfg(wtp::WTSVariant::createObject());
	ASSERT_NE(nullptr, cfg);
	ASSERT_TRUE(cfg->append("module", "R59CredentialsTrader"));
	ASSERT_TRUE(cfg->append("savedata", false));
	ASSERT_TRUE(cfg->append("r59_login_record", login_record.string().c_str()));

	wtp::TraderAdapter trader;
	EXPECT_FALSE(trader.init(id_.c_str(), cfg.get(), &bd_, nullptr));
	EXPECT_STREQ("", trader.id());
	EXPECT_EQ(nullptr, trader.getBaseDataMgr());
	EXPECT_FALSE(fs::exists(fs::path(WtHelper::getBaseDir()) / "traders" / id_));
	EXPECT_DOUBLE_EQ(0.0, trader.getUndoneQty(p11fix::kStdCode));

	ASSERT_TRUE(trader.init("", cfg.get(), &bd_, nullptr));
	EXPECT_STREQ("", trader.id());
}

TEST_F(P11TraderTest, init_ext_rejects_long_ids_before_touching_api_or_files)
{
	id_.assign(49, 'e');
	wtp::TraderAdapter trader;

	EXPECT_FALSE(trader.initExt(id_.c_str(), &api_, &bd_, nullptr));
	EXPECT_EQ(0u, api_.n_init_);
	EXPECT_EQ(0u, api_.n_release_);
	EXPECT_STREQ("", trader.id());
	EXPECT_EQ(nullptr, trader.getBaseDataMgr());
	EXPECT_FALSE(fs::exists(fs::path(WtHelper::getBaseDir()) / "traders" / id_));

	EXPECT_FALSE(trader.initExt(nullptr, &api_, &bd_, nullptr));
	EXPECT_EQ(0u, api_.n_init_);
}

TEST_F(P11TraderTest, longest_representable_id_produces_a_terminated_user_tag)
{
	id_.assign(48, 'm');
	wtp::TraderAdapter trader;
	ASSERT_TRUE(trader.initExt(id_.c_str(), &api_, &bd_, nullptr));

	const uint32_t local_id = trader.openLong(p11fix::kStdCode, 7000.0, 1.0, 0, ct_);
	ASSERT_NE(UINT_MAX, local_id);

	const std::string expected = "otp." + id_ + "." + std::to_string(local_id);
	EXPECT_EQ(expected, api_.last_usertag_);
	EXPECT_LE(api_.last_usertag_.size(), sizeof(((wtp::WTSEntrust*)nullptr)->m_strUserTag) - 1);

	const std::string max_id_tag = "otp." + id_ + "." +
		std::to_string((std::numeric_limits<uint32_t>::max)());
	EXPECT_EQ(63u, max_id_tag.size());
}
