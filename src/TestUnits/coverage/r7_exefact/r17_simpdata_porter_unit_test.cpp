// R17 lane：WtExecMon 可达残余 — WtSimpDataMgr 拒绝分支 + WtExecPorter C 入口
// 目标文件: src/WtExecMon/WtSimpDataMgr.cpp (missed 125，本文件只打可达片)
//           src/WtExecMon/WtExecPorter.cpp (missed 25：get_version/write_log/init/config/set/commit)
// 边界: initStore 成功路径要 dlopen 真实存储插件 + runner 基础数据（BUG-R7-018 已判不可达）；
//       run_exec 起线程、release_exec 停未初始化 logger —— 一律 DISABLED，只写触发条件。
// 只引用 WtExecMon 符号（extern "C" 入口名与 WtPorter/WtBtPorter 的同名冲突不在本二进制发生：
// CoverageTests_exefact 生产目录 = WtExeFact,WtExecMon）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSTypes.h"
#include "Includes/WTSDataDef.hpp"
#include "WtExecMon/WtSimpDataMgr.h"
#include "WtExecMon/WtExecPorter.h"

using namespace wtp;

namespace
{
const char* kCode = "SHFE.rb.2405";
}

//////////////////////////////////////////////////////////////////////////
// init：cfg 无 store 段 -> initStore(NULL) L45-46 -> false；_reader 保持空
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, simpdata_init_without_store_section_returns_false)
{
	WtSimpDataMgr mgr;
	WTSVariant* cfg = WTSVariant::createObject();   // 空对象，get("store") -> NULL
	EXPECT_FALSE(mgr.init(cfg, NULL));              // L84-88 + L45-46
	cfg->release();
	EXPECT_EQ(mgr.reader(), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// initStore：模块名指向不存在的 dylib -> load_library NULL -> L54-58 拒绝
// （不走 module 为空的默认分支：那会 dlopen 真实 WtDataStorage 插件，属进程级副作用）
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, simpdata_store_module_load_failure_returns_false)
{
	WtSimpDataMgr mgr;
	WTSVariant* cfg = WTSVariant::createObject();
	WTSVariant* store = WTSVariant::createObject();
	store->append("module", "r17_no_such_storage_module");
	cfg->append("store", store);                    // bAutoRetain=true，随父 release

	// L48-52 走 else 分支 wrap_module，L54-58 load 失败写错误日志后 return false
	EXPECT_FALSE(mgr.init(cfg, NULL));
	cfg->release();
	EXPECT_EQ(mgr.reader(), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// _reader 为空的只读接口一律安全返回 NULL；空实现回调与 reader_log 直通 log_raw L120-123
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, simpdata_reader_null_paths_and_noop_callbacks)
{
	WtSimpDataMgr mgr;
	EXPECT_EQ(mgr.get_tick_slice(kCode, 10, 0), nullptr);            // L179-181
	EXPECT_EQ(mgr.get_kline_slice(kCode, KP_Minute1, 1, 5, 0), nullptr); // L187-190
	EXPECT_EQ(mgr.grab_last_tick(kCode), nullptr);                   // L165-167（rt map 未建）

	mgr.on_bar(kCode, KP_Minute1, NULL);          // L125-128 空实现
	mgr.on_all_bar_updated(0);                    // L90-93 空实现
	mgr.handle_push_quote(kCode, NULL);           // L132-133 判空早退
	mgr.reader_log(LL_WARN, "r17 reader_log passthrough");           // L120-123
	EXPECT_EQ(mgr.grab_last_tick(kCode), nullptr); // 以上调用不应建立任何缓存
}

//////////////////////////////////////////////////////////////////////////
// WtExecPorter::get_version L102-118：UNIX + WT_VERSION + Build@ 且二次调用取同一缓存串
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, porter_get_version_build_string)
{
	WtString v = get_version();
	ASSERT_NE(v, nullptr);
	EXPECT_NE(strstr(v, "UNIX"), nullptr);
	EXPECT_NE(strstr(v, "Build@"), nullptr);
	EXPECT_EQ(v, get_version());                  // static _ver 缓存：指针同一
}

//////////////////////////////////////////////////////////////////////////
// WtExecPorter::write_log L127-139：catName 为空走 log_raw，非空走 log_raw_by_cat
// WTSLogger 未 init 时两路都退化为 stdout 打印（WTSLogger.cpp 的 !m_bInited 分支），无文件副作用
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, porter_write_log_both_raw_paths_without_logger_init)
{
	EXPECT_NO_THROW(write_log((unsigned int)LL_WARN, "r17 raw line", ""));
	EXPECT_NO_THROW(write_log((unsigned int)LL_INFO, "r17 cat line", "EXECTRL"));
	// 版本号仍可读：write_log 不应破坏进程状态
	EXPECT_NE(get_version(), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// init_exec/config_exec（L48-60/L68-75）对缺失文件容忍：
// WtExecRunner::init 缺日志文件返回 true、config 缺文件返回 false（R7 已在 runner 层实测），
// 这里只验证 porter 包装层不崩、且 inited 静态位让二次 init 走早退分支
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, porter_init_and_config_tolerate_missing_files)
{
	EXPECT_NO_THROW({
		init_exec("r17_no_such_logcfg.json", true);
		init_exec("r17_no_such_logcfg.json", true);   // 第二次命中 if(inited) return
		config_exec("r17_no_such_cfgexec.json");     // 绝不传 ""：那会读 CWD 的 cfgexec.json
	});
}

//////////////////////////////////////////////////////////////////////////
// set_position/commit_positions（L147-161）在未 config 状态只写 runner 内存缓存
//（对应 R7 实测过的 WtExecRunner.cpp:181-194/407-416 空跑路径）
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, porter_set_and_commit_positions_before_config)
{
	EXPECT_NO_THROW({
		set_position(kCode, 3.0);
		commit_positions();
	});
	// 再次提交空增量仍无副作用
	EXPECT_NO_THROW(commit_positions());
}

//////////////////////////////////////////////////////////////////////////
// DISABLED：WtSimpDataMgr::init 不判 cfg 空 -> L87 在 initStore 判空之前对 NULL 调 get()
// 触发条件：mgr.init(NULL, NULL)。initStore 有 L45 判空而 init 没有，属同一族的漏判。BUG-R17-006
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, DISABLED_simpdata_init_null_cfg_derefs)
{
	WtSimpDataMgr mgr;
	mgr.init(NULL, NULL);
	EXPECT_FALSE(true);   // 占位；实际在上一行 SEGV
}

//////////////////////////////////////////////////////////////////////////
// DISABLED：handle_push_quote 真实 tick 在 initStore 失败后解引用未初始化的 _s_info
// 触发条件：未成功 initStore 的 mgr 收到非空 tick —— 构造函数 L28-34 没初始化
// _s_info/_cur_date/_cur_act_time 等（_cur_* 部分已由 BUG-N05-001 登记），L143 判的是垃圾值、
// L153 直接 _s_info->timeToMinutes()。BUG-R17-005
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, DISABLED_simpdata_push_quote_without_session_info)
{
	WtSimpDataMgr mgr;
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	snprintf(ts.code, sizeof(ts.code), "%s", kCode);
	ts.action_date = 20240101;
	ts.action_time = 93000000;
	WTSTickData* tick = WTSTickData::create(ts);
	mgr.handle_push_quote(kCode, tick);           // L143/L153 崩溃点
	tick->release();
	EXPECT_TRUE(false);
}

//////////////////////////////////////////////////////////////////////////
// DISABLED：get_basedata_mgr/get_hot_mgr 未判 _runner 空（L95-103 直通成员调用）
// 触发条件：任何未 init 的 mgr 被当作 IDataReaderSink 回调（读线程在 runner 前回调即命中）。BUG-R17-007
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, DISABLED_simpdata_basedata_mgr_null_runner)
{
	WtSimpDataMgr mgr;
	EXPECT_EQ(mgr.get_basedata_mgr(), nullptr);   // 实际：NULL->get_bd_mgr() 崩溃
	EXPECT_EQ(mgr.get_hot_mgr(), nullptr);
}

//////////////////////////////////////////////////////////////////////////
// DISABLED：run_exec -> WtExecRunner::run() 起收发/共享内存线程（硬规则 2 禁线程）。
// 触发条件：启用即违反用例边界；与 R7 对 WtExecRunner 完整 config 链的阻塞判定一致（BUG-R7-018）。
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, DISABLED_porter_run_exec_spawns_threads)
{
	EXPECT_NO_THROW(run_exec());
}

//////////////////////////////////////////////////////////////////////////
// DISABLED：release_exec -> runner.release() 停未初始化 logger
// 触发条件：本用例若排在必启用 init_exec 用例之前即崩（同 R7
// DISABLED_release_stops_uninited_logger，WtExecRunner.cpp:401-404）。
//////////////////////////////////////////////////////////////////////////
TEST(r17_mon, DISABLED_porter_release_exec_stops_uninited_logger)
{
	EXPECT_NO_THROW(release_exec());
}
