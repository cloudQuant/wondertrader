/*!
 * \file r102_datamgr_writer_test.cpp
 * \brief WtDtCore/DataManager 写手成功装载与空查询转发（迭代01 第十八波 R102）
 *
 * 与既有用例的分工（只读，不修改）：
 *  - q05/r18/r44/r68 已覆盖模块加载失败、createWriter 符号缺失（DISABLED，BUG-R18-002）、
 *    _writer==NULL 的全部守卫、add_caster 扇出、canSessionReceive 两条、日志转发；
 *  - 本文件补「真实 AD writer 装载成功」的尾段与转发：
 *      DataManager.cpp:78-106（dlopen + 双符号 + _writer->init）
 *      :51-57 isSessionProceeded、:115-121 add_ext_dumper、:127-134 release
 *      :143-149 writeTick、:157-163 writeOrderQueue、:171-177 writeOrderDetail、
 *      :185-191 writeTransaction、:200-206 getCurTick、:213-217 transHisData
 *      :72-76 默认模块名（WtDataStorage -> 临时目录里的同名软链）
 *
 * 只用 AD writer（WtDataWriterAD::init 只建 mmap 缓存文件，不建线程），
 * 且只给它 NULL 载荷，避免 _async_task 残值触发异步线程（R4-BUG-01）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/WtHelper.h>
#include <Includes/WTSVariant.hpp>
#include <WTSTools/WTSBaseDataMgr.h>
#include <WTSUtils/WTSCfgLoader.h>

#include "r102_dtcore_util.h"

namespace fs = std::filesystem;

namespace {

std::string writer_cfg(const std::string& storeWithSlash, bool withModule)
{
	std::string s = "{\"path\":\"" + storeWithSlash + "\",\"groupsize\":10,"
		"\"disabletick\":true,\"disablemin1\":true,\"disablemin5\":true,\"disableday\":true";
	if (withModule)
		s += ",\"module\":\"WtDataStorageAD\"";
	s += "}";
	return s;
}

class R102DataManagerWriterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = r102dt::make_tmp_dir("dm");
		_bin = r102dt::bin_dir();
		WtHelper::set_module_dir(_bin.c_str());
	}

	void TearDown() override
	{
		WtHelper::set_module_dir("");
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	WTSVariant* load(const char* name, const std::string& body)
	{
		r102dt::write_file(_dir / name, body);
		return WTSCfgLoader::load_from_file((_dir / name).string().c_str());
	}

	fs::path		_dir;
	std::string		_bin;
};

TEST_F(R102DataManagerWriterTest, InitLoadsAdWriterAndForwardsCalls)
{
	if (!r102dt::ad_writer_ready(_bin))
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createWriter 不可用";

	const std::string store = r102dt::with_slash(_dir / "store1");
	WTSVariant* cfg = load("ad_writer.json", writer_cfg(store, true));
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	// DataManager.cpp:78-106：load_library 成功、createWriter/deleteWriter 双符号、
	// _writer->init 成功（AD writer 只 mmap 四个缓存文件）
	ASSERT_TRUE(dm.init(cfg, &bdMgr, nullptr));
	cfg->release();

	EXPECT_EQ(dm.getBDMgr(), static_cast<IBaseDataMgr*>(&bdMgr));

	// :51-57 _writer 非空 -> 转发 isSessionProceeded（IDataWriter 默认 true）
	EXPECT_TRUE(dm.isSessionProceeded("SD0930"));

	// :115-121 dumper 注册（IDataWriter::add_ext_dumper 只写 map）
	dm.add_ext_dumper("r102", nullptr);

	// :143-149 / :157-163 / :171-177 / :185-191 四个写入口的转发。
	// AD writer 对 NULL tick 立即返回 false；三个 order 接口走基类默认 false。
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	EXPECT_FALSE(dm.writeOrderQueue(nullptr));
	EXPECT_FALSE(dm.writeOrderDetail(nullptr));
	EXPECT_FALSE(dm.writeTransaction(nullptr));

	// :200-206 getCurTick：AD 实现要求 _bd_mgr 非空；空基础库 -> 查不到合约 -> NULL
	EXPECT_EQ(dm.getCurTick("IC2204", "CFFEX"), nullptr);

	// :213-217 transHisData 转发（IDataWriter 默认空实现）
	dm.transHisData("SD0930");

	// :127-134 release：_writer->release() + _remover(_writer)
	dm.release();
}

TEST_F(R102DataManagerWriterTest, DefaultModuleNameUsesAliasedAdWriter)
{
	if (!r102dt::ad_writer_ready(_bin))
		GTEST_SKIP() << "libWtDataStorageAD.dylib / createWriter 不可用";

	// 默认模块名是 "WtDataStorage"；在临时模块目录里放同名软链指向 AD writer，
	// 驱动 DataManager.cpp:72-76 的 module.empty() 分支，同时不碰真实文件写手线程。
	const fs::path mods = _dir / "mods";
	fs::create_directories(mods);
	const fs::path alias = mods / "libWtDataStorage.dylib";
	std::error_code ec;
	fs::create_symlink(fs::path(_bin) / "libWtDataStorageAD.dylib", alias, ec);
	if (ec)
	{
		ec.clear();
		fs::copy_file(fs::path(_bin) / "libWtDataStorageAD.dylib", alias,
			fs::copy_options::overwrite_existing, ec);
	}
	ASSERT_FALSE(ec) << "无法建立默认模块名软链";

	WtHelper::set_module_dir(r102dt::with_slash(mods).c_str());

	const std::string store = r102dt::with_slash(_dir / "store2");
	WTSVariant* cfg = load("default_writer.json", writer_cfg(store, false));
	ASSERT_NE(cfg, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	ASSERT_TRUE(dm.init(cfg, &bdMgr, nullptr));
	cfg->release();

	EXPECT_TRUE(dm.isSessionProceeded("SD0930"));
	dm.release();

	WtHelper::set_module_dir(_bin.c_str());
}

TEST_F(R102DataManagerWriterTest, InitMissingModuleKeepsWriterNull)
{
	// 模块名不存在：load_library 失败 -> :100-103 return false。
	// 之后所有委托都必须停在 _writer==NULL 守卫，release 也不得解引用。
	const std::string store = r102dt::with_slash(_dir / "store3");
	WTSVariant* bad = load("missing_writer.json",
		"{\"module\":\"r102_no_such_writer\",\"path\":\"" + store + "\"}");
	ASSERT_NE(bad, nullptr);

	WTSBaseDataMgr bdMgr;
	DataManager dm;
	EXPECT_FALSE(dm.init(bad, &bdMgr, nullptr));
	bad->release();

	EXPECT_FALSE(dm.isSessionProceeded("SD0930"));
	dm.add_ext_dumper("r102", nullptr);
	EXPECT_FALSE(dm.writeTick(nullptr, 0));
	EXPECT_FALSE(dm.writeOrderQueue(nullptr));
	EXPECT_FALSE(dm.writeOrderDetail(nullptr));
	EXPECT_FALSE(dm.writeTransaction(nullptr));
	EXPECT_EQ(dm.getCurTick("IC2204", "CFFEX"), nullptr);
	dm.transHisData("SD0930");
	dm.release();
}

}	// namespace
