/*!
 * \file r143_uftcore_ctx_fallback_test.cpp
 * \brief 第二十二波 R143 - UftStraContext 本地块 mmap 失败后的兜底清空
 *
 * 覆盖（只读生产代码）:
 *  - UftStraContext.cpp:1209-1213  position.membin map 失败 -> file.reset/block=NULL
 *  - UftStraContext.cpp:1257-1261  order.membin    map 失败 -> 同上
 *  - UftStraContext.cpp:1305-1309  trade.membin    map 失败 -> 同上
 *  - UftStraContext.cpp:1353-1357  round.membin    map 失败 -> 同上
 *
 * 手法:
 *  - load_local_data 先用 StdFile::exists 判断块文件是否存在；存在就不调用 create_new_file，
 *    直接 BoostMappingFile::map。把四个块路径预建成*目录*，exists 为真而 mmap 必失败（ENODEV），
 *    从而稳定进入 else 兜底，不需要悬空软链（R133 的悬空链接路径会挂住）。
 *  - on_channel_ready 前 _tradingday==0 的早退由既有 q04 用例覆盖，这里直接给交易日。
 *  - 输出目录只落在系统临时目录；名字带 pid+序号，结束 remove_all。
 *  - 不加载插件、不起线程、不读写共享内存。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftEngine.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

using namespace wtp;

namespace {

std::string nextName()
{
	static std::atomic<uint32_t> seq(0);
	return "r143ctx_" + std::to_string((long)::getpid()) + "_" + std::to_string(++seq);
}

} // namespace

TEST(R143UftCtxFallback, directory_block_paths_take_map_failure_else)
{
	q04::TempRoot tmp("r143ctx");
	WtHelper::setGenerateDir((tmp.dir().string() + "/gen/").c_str());

	const std::string name = nextName();
	// getOutputDir() 的目录串是函数内静态；以它为准，避免依赖本用例是否先于 q04 执行
	const std::string folder = std::string(WtHelper::getOutputDir()) + name + "/";

	std::error_code ec;
	std::filesystem::create_directories(folder, ec);
	ASSERT_FALSE(ec) << folder;

	//四个块路径都是目录：StdFile::exists 命中，BoostMappingFile::map 失败
	const char* blockFiles[] = { "position.membin", "order.membin", "trade.membin", "round.membin" };
	for (const char* one : blockFiles)
	{
		std::filesystem::create_directories(folder + one, ec);
		ASSERT_FALSE(ec) << one;
		ASSERT_TRUE(std::filesystem::is_directory(folder + one));
	}

	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	WtUftEngine engine;
	engine.init(nullptr, &bd, nullptr, nullptr);

	{
		UftStraContext ctx(&engine, name.c_str());
		//四个块都走 map 失败分支：不崩、持仓为空
		ctx.on_channel_ready(20240315);
		EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_position("SHFE.ag2406"));
	}

	std::filesystem::remove_all(folder, ec);
}
