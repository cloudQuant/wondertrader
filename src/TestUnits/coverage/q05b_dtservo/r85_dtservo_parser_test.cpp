/*!
 * \file r85_dtservo_parser_test.cpp
 * \brief 迭代01 第十六波 R85 - WtDtServo ParserAdapter 无 createParser 符号的插件拒绝
 *
 * 覆盖（只读生产代码）:
 *  - ParserAdapter.cpp:120-125 插件可 dlopen、但取不到 createParser 入口 ->
 *    fatal 日志 + 返回 false（此前 R78 只覆盖了成功装载，R85 补这条残留守卫）
 *
 * 插件用构建产物 libWtDataStorageAD.dylib 改名为 libNoParser.dylib 后软链进
 * 临时模块根的 parsers/ 子目录；只验证符号缺失分支，不 init/run 任何真实模块。
 */
#include <gtest/gtest.h>

#include <WtDtServo/ParserAdapter.h>
#include <WtDtServo/WtHelper.h>

#include <WTSTools/WTSBaseDataMgr.h>

#include <Includes/WTSVariant.hpp>

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

fs::path makeTempDir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string("r85_") + tag + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	fs::path p = fs::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

}	// namespace

TEST(R85DtParserInit, module_without_create_symbol_is_rejected)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";

	const fs::path dir = makeTempDir("dtparser");
	std::error_code ec;
	fs::create_directories(dir / "parsers", ec);
	ASSERT_FALSE(ec);
	const std::string real = bin + "libWtDataStorageAD.dylib";
	if (!fs::exists(real, ec))
		GTEST_SKIP() << "libWtDataStorageAD.dylib not found in this build";
	// Copy the real data-storage module under a parser-like name. Loading must
	// still fail because the copied module does not export createParser.
	fs::copy_file(real, dir / "parsers" / "libNoParser.dylib",
		fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	WtHelper::set_module_dir((dir.string() + "/").c_str());

	WTSBaseDataMgr bd;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "NoParser");

	{
		ParserAdapter adapter(&bd, nullptr);
		EXPECT_FALSE(adapter.init("r85dtnoentry", cfg));
		EXPECT_FALSE(adapter.run());
		adapter.release();
	}

	cfg->release();
	WtHelper::set_module_dir("");
	fs::remove_all(dir, ec);
}
