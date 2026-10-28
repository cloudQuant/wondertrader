# R6 manifest — WtRtRunner 生命周期/分发层 + WtRunner 可执行体（第七波 lane R6）

- 波次: 迭代01 第七波，路 6（派生自 `reports/分派范围-20261027T113454Z.md` L41-45）
- 目标二进制:
  - `CoverageTests_porter`（已登记目录 `src/TestUnits/coverage/q06_porter/`，编 `src/WtPorter/*.cpp` + `libWtCore.a`）
  - `CoverageTests_m_WtRunner`（**本轮新建目录** `src/TestUnits/coverage/m_WtRunner/`；由
    `coverage/CMakeLists.txt` 尾部 `WT_MAIN_MODULES` 循环从磁盘派生，目录里有 .cpp 即成目标，
    未改任何 CMakeLists；需主控重新 configure）
- 生产文件（只读，一行未改）: `src/WtPorter/WtRtRunner.cpp`（1271 行，分派口径 missed=621/938）、
  `src/WtRunner/WtRunner.cpp`（638 行，missed=491/491）+ 同目录 `src/WtRunner/main.cpp`（128 行，入口所在 TU）
- 门禁（真实构建口径，未自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_porter.dir/q06_porter/q06_porter_runner_test.cpp.o | tail -1`
  → 截掉 ` -MD ... -o ... -c ...` 尾巴 → 追加 `-fsyntax-only <我的 cpp>`。
  `m_WtRunner/r6_wtrunner_main_test.cpp` 借用同一串 porter flags，**只手工补了一个宏**
  `-Dmain=wt_coverage_main_wtrunner`（生产 TU 侧由 CMake 的 `main=wt_coverage_main_${_modlc}` 提供，
  小写；其余 flags 未动）。三个文件全部 0 诊断通过（`-Wall -Wextra -Werror`）。
- 未改动任何 `CMakeLists.txt`、生产代码、他人用例；未跑构建、未跑二进制、未跑 ctest、无 git 操作。

## 一、main() 入口机制（照抄已跑通的形状）

`src/TestUnits/coverage/m_LoaderRunner/seam_loader_runner_test.cpp` 的形状：构建期把产品 TU 里的
`main` 重命名，测试按受控 argv 调用它，与 gtest 自己的 main 同链一个二进制。
本 lane 声明的是 `int wt_coverage_main_wtrunner(int, char**)`（小写，实测大写链不上）。
`m_WtRunner` 目标把 `src/WtRunner/` 下的 **两个** TU（`WtRunner.cpp` + `main.cpp`）一起编进二进制，
所以入口在 `wt_coverage_main_wtrunner`（main.cpp:51），而 `WtRunner.cpp` 的成员函数可以被用例直接调用。

## 二、用例 → 生产文件:行段（行号一律本轮 Read/Grep 结果）

### `src/TestUnits/coverage/q06_porter/r6_porter_lifecycle_test.cpp`（13 启用 + 6 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `R6PorterLifecycle.init_with_absent_logcfg_file_returns_true_and_stamps_inst_dir` | `WtRtRunner.cpp:107-127`（守卫在 `WTSTools/WTSLogger.cpp:206-214`） | 不存在的日志文件时 init 不起线程、仍 `return true`，`:124` 把安装目录写进 WtHelper |
| `R6PorterLifecycle.init_inline_content_that_fails_to_parse_also_returns_true` | `WtRtRunner.cpp:118-127` | `isFile=false` 走 `load_from_content`；解析失败后 `:124/:125` 照旧执行 |
| `R6PorterLifecycle.init_with_gen_dir_normalizes_and_keeps_inst_dir_identical` | `WtRtRunner.cpp:124-126` | 第二个实例的 init 不改变已算好的安装目录（`getBinDir` 的 static 缓存，`Share/ModuleHelper.hpp:78`） |
| `R6PorterFactories.cta_factory_absent_dir_returns_false` | `WtRtRunner.cpp:1168-1172` → `WtCore/CtaStrategyMgr.cpp:30-36` | 目录不存在的守卫返回 false |
| `R6PorterFactories.sel_factory_absent_dir_returns_false` | `WtRtRunner.cpp:1173-1177` | 同上（SEL 族） |
| `R6PorterFactories.hft_factory_absent_dir_returns_false` | `WtRtRunner.cpp:1183-1187` | 同上（HFT 族） |
| `R6PorterFactories.executer_factory_absent_dir_returns_false` | `WtRtRunner.cpp:1178-1182` | 同上（执行器族） |
| `R6PorterFactories.factory_path_pointing_at_a_file_does_not_return_cleanly` | `WtRtRunner.cpp:1168-1172` → `CtaStrategyMgr.cpp:32`（`StdFile::exists` 用 `std::filesystem::exists`，`Share/StdUtils.hpp:137-145`）与 `:39-41`（boost `directory_iterator`） | 传文件路径时不会「安静地返回 false」：现状抛异常，加了类型守卫后必须返回 false（BUG-R6-008） |
| `R6PorterLogRoute.level_to_tag_table_is_the_documented_seven_entries` | `WtRtRunner.cpp:1126-1134` | `LOG_TAGS` 与 `Includes/WTSTypes.h:128-137`（LL_ALL=100..LL_NONE=106）同步 |
| `R6PorterLogRoute.handleLogAppend_accepts_every_declared_level` | `WtRtRunner.cpp:1136-1139` + `WtCore/EventNotifier.cpp:114-117` | 七个级别全走一遍不越界；未 init 通知器时 `_mq_sid==0` 立刻返回 |
| `R6PorterExtParserQuote.unknown_parser_leaves_the_supplied_tick_untouched` | `WtRtRunner.cpp:1243-1256`（未命中 `:1254` warn 支） | 未知 parser id 时不消费 tick，也不反向触发 parser 回调 |
| `R6PorterExtExecuter.create_ext_executer_returns_true_without_engine` | `WtRtRunner.cpp:305-312` | `createExtExecuter` 不依赖 `_engine`，与 `createExtParser`（`:295-303` 必解引用 `_engine`）不对称 |
| `R6PorterExtExecuter.executer_callbacks_fire_on_the_runner_that_registered_them` | `WtRtRunner.cpp:1261-1264`、`:1267-1270` | 回调记在哪个实例上就哪个实例响（对照 `ExpExecuter.cpp:24-28` 的硬绑单例，BUG-R6-007） |
| `DISABLED_missing_config_file_derefs_null_config` | `WtRtRunner.cpp:489-494` | BUG-R6-001（`_config` 未判空） |
| `DISABLED_valid_content_without_basefiles_derefs_null_section` | `WtRtRunner.cpp:494-495` | BUG-R6-002（`cfgBF` 未判空） |
| `DISABLED_create_ext_parser_without_engine_crashes` | `WtRtRunner.cpp:295-303` | BUG-R6-003（`_engine` 未判空） |
| `DISABLED_get_raw_stdcode_without_engine_crashes` | `WtRtRunner.cpp:361-366` | 同族，已由 `bugs/q06_porters.md` BUG-Q06-002 登记，本条仅保留用例形状 |
| `DISABLED_feed_adj_factors_without_feeder_jumps_to_garbage` | `WtRtRunner.cpp:277-280` | 已由 BUG-Q06-004 登记；根因（构造列表缺 `_config/_feed_obj/_feeder_bars/_feeder_fcts`）另见 BUG-R6-009 |
| `DISABLED_run_without_engine_and_release_global_logger_stop` | `WtRtRunner.cpp:1091-1124`、`1141-1155` | BUG-R6-006（`run` 未判 `_engine` + `release` 里 `WTSLogger::stop()` 是进程级副作用） |

### `src/TestUnits/coverage/q06_porter/r6_porter_dispatch_silence_test.cpp`（12 启用 + 2 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `R6PorterSilence.cta_dispatch_is_silent_before_any_registration` | `WtRtRunner.cpp:417-427`、`:396-405`、`:384-394`、`:429-439`、`:407-415` | 六个 `ctx_on_*` 的 `if(_cb_*)` 假支全静默 |
| `R6PorterSilence.tick_dispatch_with_null_callback_never_touches_the_argument` | `WtRtRunner.cpp:441-451` | 未注册时连 `newTick->getTickStruct()` 都不求值 |
| `R6PorterSilence.hft_micro_data_dispatch_is_silent_and_skips_struct_unwrap` | `WtRtRunner.cpp:1188-1206`、`:453-487` | 9 个 `hft_on_*` 未注册即静默（含 ordque/orddtl/trans 的解包点） |
| `R6PorterSilence.parser_and_executer_slots_are_silent_until_registered` | `WtRtRunner.cpp:1207-1242`、`1261-1270` | 八个入口注册前 0 命中、注册后逐条命中（对照控制组） |
| `R6PorterSilence.registering_sel_slots_leaves_cta_and_hft_silent` | `WtRtRunner.cpp:167-176`、`:384-451` | cta/hft/sel 三族槽互不串扰 |
| `R6PorterSilence.hft_slots_do_not_fill_cta_calc_slot` | `WtRtRunner.cpp:179-200`、`:396-405` | HFT 没有 calc 槽（`registerHftCallbacks` 形参里没有 cbCalc） |
| `R6PorterSilence.cond_trigger_slot_only_occupied_by_cta_registration` | `WtRtRunner.cpp:407-415` | `_cb_cta_cond_trigger` 只有 `registerCtaCallbacks` 能填 |
| `R6PorterReregister.second_cta_registration_replaces_the_first` | `WtRtRunner.cpp:154-164` | 注册语义是「覆盖」不是「追加」 |
| `R6PorterReregister.passing_nulls_clears_every_cta_slot` | `WtRtRunner.cpp:154-164` | 传全 NULL 可撤销整族槽 |
| `R6PorterReregister.passing_nulls_clears_hft_and_parser_and_executer_slots` | `WtRtRunner.cpp:179-200`、`138-143`、`146-151` | 同上（hft/parser/executer 三族） |
| `R6PorterReregister.evt_callback_can_be_revoked` | `WtRtRunner.cpp:129-136` + `WtRtRunner.h` 的 `on_initialize_event/on_schedule_event/on_session_event` 内联 | 三个引擎事件共用 `_cb_evt`，可注册可撤销 |
| `R6PorterPassthrough.unknown_context_id_reaches_the_callback_verbatim` | `WtRtRunner.cpp:417-427`、`:384-394` | 分发层不做上下文存在性过滤，id/合约码原样透传 |
| `DISABLED_parser_id_null_builds_string_from_nullptr` | `WtRtRunner.cpp:1213-1218`、`1243-1245` | BUG-R6-004 |
| `DISABLED_ctx_on_tick_null_tickdata_derefs` | `WtRtRunner.cpp:441-451` | BUG-R6-005 |

### `src/TestUnits/coverage/m_WtRunner/r6_wtrunner_main_test.cpp`（9 启用 + 3 DISABLED_）

| 用例 | 生产位置 | 钉住的行为 |
| --- | --- | --- |
| `r6_wtrunner_entry.short_help_returns_zero_before_touching_runner` | `main.cpp:51-87`（`cppcli` 定义 L62-83，`:86-87` 早退） | `-h` 分支 return 0，且早于 `WtRunner runner;`（L100） |
| `r6_wtrunner_entry.long_help_returns_zero` | `main.cpp:80,86-87` | `--help` 长选项同支 |
| `r6_wtrunner_entry.help_wins_over_config_and_logcfg_words` | `main.cpp:68-87` | `-h` 与 `-c/-l` 同时给出时 hParam 优先，不触碰任何文件 |
| `r6_wtrunner_binroot.getBinDir_is_cwd_and_stable_across_calls` | `WtRunner.cpp:26-37` | static 缓存第二次走假支、返回同一指针；非空非单字符 |
| `r6_wtrunner_init.ctor_dtor_roundtrip_routes_declared_levels_only` | `WtRunner.cpp:41-48,51-53,614-622,624-627` | 构造/析构不建引擎；七个级别全走 `LOG_TAGS[ll-100]` 不越界 |
| `r6_wtrunner_init.init_with_missing_logcfg_returns_and_stamps_inst_dir` | `WtRunner.cpp:55-65`（`WTSLogger::init` 守卫在 `WTSLogger.cpp:206-214`） | `:59` 的 `setInstDir(getBinDir())` 无条件执行 |
| `r6_wtrunner_config.missing_file_returns_false` | `WtRunner.cpp:67-74` | 加载失败即 `return false`（这一支 WtRunner 有判空，WtRtRunner 没有 —— BUG-R6-001 的对照） |
| `r6_wtrunner_config.log_tag_table_matches_the_seven_declared_levels` | `WtRunner.cpp:614-622` | 与 WtPorter 侧同名表内容一致但各 TU 各定义一份（BUG-R6-010） |
| `r6_wtrunner_config.config_error_path_leaves_object_reusable` | `WtRunner.cpp:67-74` | 连续两次失败加载行为一致，`_config` 不被污染成可用状态 |
| `DISABLED_bare_argv_falls_through_to_null_engine_run` | `main.cpp:82-125` + `WtRunner.cpp:578-612`（`:585 _engine->run()`） | BUG-R6-011 |
| `DISABLED_missing_config_file_still_runs` | 同上 | BUG-R6-011 |
| `DISABLED_empty_object_config_derefs_null_basefiles` | `WtRunner.cpp:77-78` | BUG-R6-012（`cfgBF` 未判空，与 WtRtRunner 同构） |

## 三、启用/DISABLED 计数（grep 实测，非自估）

```
r6_porter_lifecycle_test.cpp    : 13 启用 / 6 DISABLED_
r6_porter_dispatch_silence_test.cpp: 12 启用 / 2 DISABLED_
r6_wtrunner_main_test.cpp       :  9 启用 / 3 DISABLED_
合计 porter 侧 25 启用（区间 15~25）、WtRunner 侧 9 启用（区间 6~12）
```

## 四、留给主控的三件事（不在本轮动手）

1. `CoverageTests_m_WtRunner` 的**链接缺口**：`coverage/CMakeLists.txt` 的 `WT_MAIN_MODULES` 循环只链
   `${WT_COV_SHARED_LIBS}`（不含 `WtCore`），而 `WtRunner.cpp` + `main.cpp` 这两个 TU 必然引用
   `wtp::WtCtaEngine/WtHftEngine/WtSelEngine/WtDtMgr/CtaStrategyMgr/EventNotifier/WtHelper` 等
   `libWtCore.a` 符号（对照：同文件里 `porter` 目标是单独 `if(suffix STREQUAL "porter") TARGET_LINK_LIBRARIES(... WtCore)`
   补的）。`LoaderRunner` 那族能过是因为它的 TU 不碰核心。缺陷条目 BUG-R6-013。
2. `WtRunner.cpp:411-414` 与 `WtRtRunner.cpp:1157-1166` 的 `initActionPolicy` 不对称（后者判空、前者直接
   `_act_policy.init(_config->getCString("bspolicy"))`）；`initDataMgr`/`initEngine` 也是同构不同形。BUG-R6-014。
3. 本 lane 启用用例里唯一动过**进程级状态**的两处，请主控在按用例分进程的口径下确认无残留：
   - `WtHelper::setInstDir("")` / `init()` 写安装目录（3 个用例）；
   - `getRunner().createExtExecuter("r6exe_a")` 往单例的 `_cta_engine` 执行器表里塞了一条
     （1 个用例；`release()` 已整条 DISABLED，不会 `WTSLogger::stop()`）。
