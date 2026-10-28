/*!
 * \file r11_uftctx_param_log_test.cpp
 * \brief 覆盖率迭代01 第七波 R11(WtUftCore) - UftStraContext 参数面与日志/会话转发
 *
 * 覆盖 UftStraContext.cpp:
 * - read_param 六个重载 + sync_param 六个重载 + watch_param 字符串重载 + commit_param_watcher:
 *   这些接口全部经 ShareManager 单例, 而 ShareManager 在共享模块缺失时 _inited==false,
 *   会返回默认值/nullptr(见 r11_share_manager_test.cpp), 所以它们**不需要真实 shm 就能进**。
 *   (q04 曾把这一族整族 DISABLED, 判定为"需要共享内存", 本轮实测推翻该结论。)
 * - stra_log_info/debug/error 与 log_info/log_debug/log_error 的三条日志出口;
 * - on_params_updated / on_session_begin / on_session_end / on_channel_lost / on_position
 *   在 _strategy==NULL 时的静默分支;
 * - id() / set_strategy / get_stragety / stra_get_date / stra_get_time / stra_get_secs 的时间转发。
 *
 * 缺陷: watch_param 的 5 个数值重载对 ShareManager::allocate_value 的返回值直接解引用,
 * 未初始化时是 *nullptr -> 段错误。用例整条 DISABLED_, 见 bugs/R11_wtuftcore.md BUG-R11-002。
 */
#include <boost/noncopyable.hpp>

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtUftEngine.h"
#include "WtUftCore/ShareManager.h"

#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSMarcos.h"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>

#include "q04_trader_fixture.h"

using namespace wtp;

namespace {

//////////////////////////////////////////////////////////////////////////
//只装配引擎, 不装配 trader: 本文件不触发任何下单/持仓查询接口
struct LightEnv
{
	q04::TempRoot			_tmp{ "r11ctx" };
	q04::FakeBaseDataMgr	_bd;
	WtUftEngine				_engine;
	UftStraContext			_ctx;

	explicit LightEnv(const char* name)
		: _ctx(&_engine, name)
	{
		_bd.add("SHFE", "ag", "ag2406");
		_engine.init(nullptr, &_bd, nullptr, nullptr);
		_engine.set_date_time(20240315, 93001, 33, 93001);
		_engine.set_trading_date(20240315);
	}
};

const char* kName = "r11ctx_light";

//////////////////////////////////////////////////////////////////////////
//read_param 六个重载: ShareManager 未初始化时必须原样回吐默认值
TEST(R11UftCtxParam, readParamReturnsDefaultWhenShareOffline)
{
	LightEnv env(kName);

	EXPECT_STREQ("dflt", env._ctx.read_param("rk_str", "dflt"));
	EXPECT_STREQ("", env._ctx.read_param("rk_str_empty", (const char*)""));

	EXPECT_EQ((int32_t)-11, env._ctx.read_param("rk_i32", (int32_t)-11));
	EXPECT_EQ((int64_t)-9876543210LL, env._ctx.read_param("rk_i64", (int64_t)-9876543210LL));
	EXPECT_EQ((uint32_t)4000000001U, env._ctx.read_param("rk_u32", (uint32_t)4000000001U));
	EXPECT_EQ((uint64_t)18446744073709551610ULL, env._ctx.read_param("rk_u64", (uint64_t)18446744073709551610ULL));
	EXPECT_DOUBLE_EQ(2.75, env._ctx.read_param("rk_dbl", 2.75));

	//数值重载的缺省默认值是 0
	EXPECT_EQ((int32_t)0, env._ctx.read_param("rk_i32_zero", (int32_t)0));
	EXPECT_DOUBLE_EQ(0.0, env._ctx.read_param("rk_dbl_zero", 0.0));
}

//sync_param 六个重载: 未初始化 -> allocate_value 返回 nullptr, 必须原样透传而不是崩
TEST(R11UftCtxParam, syncParamYieldsNullPointerWhenShareOffline)
{
	LightEnv env(kName);

	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_str", "init", false));
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_i32", (int32_t)1, false));
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_u32", (uint32_t)2, false));
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_i64", (int64_t)3, false));
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_u64", (uint64_t)4, false));
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_dbl", 5.5, false));
	//bForceWrite=true 走另一条实参组合, 结果同样是 nullptr
	EXPECT_EQ(nullptr, env._ctx.sync_param("sk_dbl2", 5.5, true));
}

//watch_param 字符串重载不解引用, 因此离线时安全返回 nullptr; commit_param_watcher 只是转发
TEST(R11UftCtxParam, stringWatchParamAndCommitAreNullSafe)
{
	LightEnv env(kName);

	EXPECT_EQ(nullptr, env._ctx.watch_param("wk_str", "hello"));
	//提交参数监视: ShareManager::commit_param_watcher 未初始化返回 false, 但这里返回值被丢弃
	env._ctx.commit_param_watcher();
	//提交后再读仍然只有默认值可读
	EXPECT_STREQ("again", env._ctx.read_param("wk_after", "again"));
}

//缺陷接缝: 5 个数值 watch_param 会 *nullptr -> 崩溃, 整条 DISABLED
TEST(DISABLED_R11UftCtxParam, numericWatchParamDereferencesNull)
{
	LightEnv env(kName);
	//BUG-R11-002: 下面 5 行任何一行都会段错误
	env._ctx.watch_param("wk_dbl", 1.0);
	env._ctx.watch_param("wk_u32", (uint32_t)1);
	env._ctx.watch_param("wk_i32", (int32_t)1);
	env._ctx.watch_param("wk_u64", (uint64_t)1);
	env._ctx.watch_param("wk_i64", (int64_t)1);
	FAIL() << "unreachable";
}

//////////////////////////////////////////////////////////////////////////
//三条日志出口 + 会话/参数回调在 _strategy==NULL 时的静默分支
TEST(R11UftCtxLog, logSinksAndNullStrategyCallbacksAreSilent)
{
	LightEnv env(kName);

	env._ctx.stra_log_info("r11 info message");
	env._ctx.stra_log_debug("r11 debug message");
	env._ctx.stra_log_error("r11 error message");

	//没有挂策略: 这四个回调只走 `if(_strategy)` 的假分支
	env._ctx.on_params_updated();
	env._ctx.on_session_begin(20240315);
	env._ctx.on_session_end(20240318);
	env._ctx.on_channel_lost();
	//账户持仓通知按产品注释直接吞掉(空实现)
	env._ctx.on_position("SHFE.ag2406", true, 1, 1, 2, 2, 20240315);
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position("SHFE.ag2406"));
	//on_channel_lost 只判 _strategy, 不会碰本地块指针
	env._ctx.on_channel_lost();
}

//////////////////////////////////////////////////////////////////////////
//上下文身份与时间转发
TEST(R11UftCtxTime, idAndTimeForwardingFromEngine)
{
	LightEnv env("r11ctx_time");

	//构造时按名字哈希出的 context id 在同一实例上必须稳定
	uint32_t first = env._ctx.id();
	EXPECT_EQ(first, env._ctx.id());

	EXPECT_EQ((uint32_t)20240315, env._ctx.stra_get_date());
	//stra_get_time 走 raw time
	EXPECT_EQ((uint32_t)93001, env._ctx.stra_get_time());
	EXPECT_EQ((uint32_t)33, env._ctx.stra_get_secs());

	//引擎时间推进后, 上下文跟着变(证明是转发而不是缓存)
	env._engine.set_date_time(20240318, 145959, 59, 145959);
	EXPECT_EQ((uint32_t)20240318, env._ctx.stra_get_date());
	EXPECT_EQ((uint32_t)145959, env._ctx.stra_get_time());
	EXPECT_EQ((uint32_t)59, env._ctx.stra_get_secs());
}

} // namespace
