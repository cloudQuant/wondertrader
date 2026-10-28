/*!
 * \file r183_uftcore_remaining_test.cpp
 * \brief R183 · WtUftCore 剩余公开面（工厂跳过、时钟/上下文只读路径、内联默认值）
 *
 * 覆盖的生产行段：
 *  - WtUftCore/UftStrategyMgr.cpp:82-86  dlopen 成功但无 createStrategyFact 时 free+continue
 *  - WtUftCore/WtUftEngine.cpp:182/213-242 之前已由 R173 记录为死代码，本文件不重复主张
 *  - WtUftCore/UftStraContext.cpp:933-956 stra_get_bars/stra_get_ticks 的 NULL 返回路径
 *  - WtUftCore/WtUftEngine.h:71-73       get_raw_time/get_secs/get_trading_date
 *  - WtUftCore/TraderAdapter.h:66-71/94-96/110/137
 *  - WtUftCore/UftDataDefs.h:65-67
 *  - WtUftCore/UftStrategyMgr.h:32-44/50 UftStraWrapper 的 NULL 自持路径
 *
 * 边界：只构造纯内存对象；临时目录仅写系统临时目录并清理；不建共享内存、
 *      不起线程、不装载策略/交易插件（dylib 是既有 libWtShareHelper 的符号链接）。
 */
#include <gtest/gtest.h>

// TraderAdapter.h 未自行 include boost/noncopyable，先显式给出（与 q04 既有夹具一致）
#include <boost/noncopyable.hpp>

#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftDataDefs.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/UftStrategyMgr.h"
#include "WtUftCore/WtUftEngine.h"

#include <mach-o/dyld.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

fs::path make_tmp_dir(const char* tag)
{
	fs::path p = fs::temp_directory_path()
		/ (std::string("r183_uft_") + tag + "_" + std::to_string((long)::getpid()));
	std::error_code ec;
	fs::remove_all(p, ec);
	fs::create_directories(p, ec);
	return p;
}

fs::path share_helper_lib()
{
	char exe[4096] = { 0 };
	uint32_t size = sizeof(exe);
	if (_NSGetExecutablePath(exe, &size) != 0)
		return fs::path();

	// <build>/bin/TestUnits/CoverageTests_uftcore -> <build>/bin/libWtShareHelper.dylib
	const fs::path exePath = fs::canonical(exe);
	return exePath.parent_path().parent_path() / "libWtShareHelper.dylib";
}

}	// namespace

/*
 * 生产: UftStrategyMgr.cpp:82-86
 * 目录里只有一个能 dlopen、但不导出 createStrategyFact 的 dylib：
 * loadFactories 必须 dlclose 并 continue，整体仍返回 true；随后按未知工厂名
 * createStrategy 返回空指针。临时目录在用例末尾删除。
 */
TEST(R183UftFactory, DylibWithoutFactoryEntryIsSkipped)
{
	const fs::path lib = share_helper_lib();
	ASSERT_FALSE(lib.empty());
	ASSERT_TRUE(fs::exists(lib)) << lib;

	const fs::path dir = make_tmp_dir("fact");
	std::error_code ec;
	fs::create_symlink(lib, dir / "libnofactory.dylib", ec);
	ASSERT_FALSE(ec) << ec.message();

	UftStrategyMgr mgr;
	EXPECT_TRUE(mgr.loadFactories(dir.string().c_str()));
	EXPECT_EQ(nullptr, mgr.createStrategy("nofact.unit", "r183"));

	fs::remove_all(dir, ec);
}

/*
 * 生产: WtUftEngine.h:71-73；WtUftEngine.cpp:81-110；
 *       UftStraContext.cpp:850-863/933-956（NULL 切片路径）
 * 构造引擎后只设置时钟；上下文在无数据管理器时取 bars/ticks 必须安全返回 NULL，
 * 三个时钟 getter 与引擎当前值一致。不调用 get_last_tick/get_order_*（会解引用
 * 未初始化的 _data_mgr，见 bugs/R183.md 交叉引用 BUG-R173-001）。
 */
TEST(R183UftEngine, ClockAndReadOnlySlicePaths)
{
	WtUftEngine engine;
	engine.set_date_time(20240315, 931, 654321, 932);

	EXPECT_EQ(20240315U, engine.get_date());
	EXPECT_EQ(931U, engine.get_min_time());
	EXPECT_EQ(932U, engine.get_raw_time());
	EXPECT_EQ(654321U, engine.get_secs());

	UftStraContext ctx(&engine, "r183_ctx");
	EXPECT_EQ(nullptr, ctx.stra_get_bars("SHFE.rb2510", "m1", 10));
	EXPECT_EQ(nullptr, ctx.stra_get_ticks("SHFE.rb2510", 10));

	EXPECT_EQ(20240315U, ctx.stra_get_date());
	EXPECT_EQ(932U, ctx.stra_get_time());
	EXPECT_EQ(654321U, ctx.stra_get_secs());
}

/*
 * 生产: TraderAdapter.h:66-71/94-96/110/137；UftDataDefs.h:65-67；
 *       UftStrategyMgr.h:32-44/50
 * PosItem 多空合计、RiskParams 默认清零、adapter 初始状态/空未成交流水、
 * DetailStruct 零初始化，以及 UftStraWrapper 在 _stra/_fact 均为 NULL 时
 * self() 返回 NULL 且析构不解引用工厂。
 */
TEST(R183UftTypes, InlineDefaultsAndNullWrapperAreSafe)
{
	TraderAdapter::PosItem item;
	item.l_newvol = 1.0;
	item.l_prevol = 2.0;
	item.s_newvol = 3.0;
	item.s_prevol = 4.0;
	EXPECT_DOUBLE_EQ(3.0, item.total_pos(true));
	EXPECT_DOUBLE_EQ(7.0, item.total_pos(false));

	TraderAdapter::RiskParams risk;
	EXPECT_EQ(0U, risk._cancel_times_boundary);
	EXPECT_EQ(0U, risk._order_times_boundary);

	TraderAdapter adapter;
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty("SHFE.rb2510"));

	uft::DetailStruct detail;
	EXPECT_EQ(0U, detail._direct);
	EXPECT_DOUBLE_EQ(0.0, detail._volume);

	{
		UftStraWrapper wrapper(nullptr, nullptr);
		EXPECT_EQ(nullptr, wrapper.self());
	}
}
