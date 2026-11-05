# 迭代03 BUG-I02-NEW-016：initExt 外部 ITraderApi 登录责任（决策卡 10 = A）

用户裁定（2026-11-03）：`initExt` 传入的外部 `ITraderApi` 自认证并自行登录；adapter 不持有其凭据，`WTE_Connect` 只报告连接状态，登录结果由外部 API 经 `onLoginResult()` 推进。`init()` 配置路径保持原行为：`WTE_Connect(0)` 时用配置中的 `user`/`pass` 调用 `login()`。

测试使用真实 `src/WtCore/TraderAdapter.cpp`、进程内 fake `ITraderApi`（仿 HftLatencyTool 的自认证 TestTrader）和仓内测试专用插件 `src/TestUnits/fixtures/r59_credentials_trader.cpp`（由 `run.sh` 编译为 `libR59CredentialsTrader.dylib`，装入临时 cwd 的 `traders/`）。**不调用任何真实柜台 SDK，不联网，不登录真实账户，不下单。**

## 产品改动

- `TraderAdapter::handleEvent(WTE_Connect, 0)` 增加显式空 `_cfg` 分支：
  - `_cfg == NULL` 且存在未释放的外部 API（`!_owns_trader_api`）：只记 INFO“外部 API 自行登录”，不调用 `login()`；
  - `_cfg == NULL` 且无活跃外部 API（`initExt` 失败后、`release()` 之后的迟到事件）：记 WARN 并忽略；
  - `_cfg != NULL`（`init()` 配置路径）：保持原有 `login(user, pass, WT_PRODUCT)`。
- `TraderAdapter.h` 中 `initExt` 注释写明登录责任合同。文件保持原 CRLF（.cpp 带 BOM）。
- `ec != 0` 的连接失败事件、`WTE_Close`、`onLoginResult` 均未改。

## 用例（`initext_login_test.cpp`，每个用例独立进程）

| 用例 | 路径 | 断言要点 |
|---|---|---|
| `config_init_connect_logs_in_with_cfg_credentials` | init 配置 | 插件经模块边界收到 `cfg_user/cfg_pass`；`WTE_Connect(-2)` 不再登录；重连 `WTE_Connect(0)` 再次登录；`onLoginResult(true)`→`AS_LOGINED` |
| `init_ext_connect_reports_state_without_adapter_login` | 成功 initExt | `init(NULL)`；run 同步 `WTE_Connect(0)` 及重复事件后 `login` 0 次、状态 `AS_LOGINING`、不查持仓；外部 `onLoginResult(true)`→`AS_LOGINED`、查持仓 1 次 |
| `init_ext_self_login_success_flows_from_api_callback` | 成功 initExt | connect 内外部 API 自报登录成功→`AS_LOGINED`，`login` 0 次 |
| `init_ext_self_login_failure_flows_from_api_callback` | 成功 initExt | 外部自报登录失败→`AS_LOGINFAILED`，`login` 0 次，不查持仓 |
| `init_ext_connect_failure_event_does_not_login` | 成功 initExt | `WTE_Connect(-7)` 不登录，状态保持 `AS_LOGINING` |
| `init_ext_rejects_null_api_and_second_api` | initExt 失败 | 空 API 返回 false 且 run 失败；已初始化后再 initExt 第二个 API 返回 false、不调用其 init |
| `init_ext_api_init_failure_releases_and_never_logs_in` | initExt 失败 | API init 失败→false、release 1 次、run 失败；防御性注入 `WTE_Connect(0)` 不崩溃不登录（正常情况下失败后 SPI 未注册，事件不可达） |
| `late_connect_after_init_ext_release_does_not_login` | release 后 | 迟到 `WTE_Connect(0)` 不登录、不崩溃 |

另在覆盖率套件 `R59Misc.init_ext_connect_event_reports_state_without_login`（`src/TestUnits/coverage/p11_wtcore_traderadapter/r59_trader_misc_test.cpp`）加入同一合同的正控。

### `connect_event_uses_cfg_credentials` 的 oracle 变更

`2f4631fd` 的原始版本先 `initExt(&api2_)`，再让 `init(module="")` 失败遗留 `_cfg`，然后 `handleEvent(WTE_Connect,0)` 并断言**外部 API 收到 1 次 login**。该 oracle 依赖失败 init 的半初始化状态，且与裁定 A（外部 API 自行登录、adapter 不调用 login）相反。迭代02 已把该用例改为成功加载插件的配置路径（本次未改断言，只在注释中限定其仅覆盖 init 配置路径并记录废弃原因）；initExt 路径的新 oracle 为 **login 0 次**，由上表和 R59 新用例覆盖。

## 运行

依赖库（Debug，非覆盖率，默认 `-Werror`）：

```sh
cmake -S src -B /tmp/wt-i03-initext-20261104/build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=OFF \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_CXX_FLAGS='-include cstdlib'
cmake --build /tmp/wt-i03-initext-20261104/build --target WtCore WtDtCore WTSTools WTSUtils -- -j2
```

`run.sh <库构建目录> <新输出目录> <base|fix> <plain|asan> [gtest过滤]`：

```sh
F=src/TestUnits/fixtures/i03_initext_login_20261104
L=/tmp/wt-i03-initext-20261104/build
$F/run.sh $L /tmp/wt-i03-initext-20261104/run-fix-plain fix plain
$F/run.sh $L /tmp/wt-i03-initext-20261104/run-base-plain base plain
$F/run.sh $L /tmp/wt-i03-initext-20261104-asan/run-fix-asan fix asan
$F/run.sh $L /tmp/wt-i03-initext-20261104-asan/run-base-asan base asan
```

- `base`：本次修改前工作树快照 `baseline/`（含卡 7 改动，`baseline.sha256` 校验）；`fix`：当前源码。
- LLVM 22.1.8 以 `-Werror -Wall -Wextra` 编译 TraderAdapter 与测试；链接前从 `libWtCore.a` 副本删除 TraderAdapter/UnresolvedReportJournal 对象，避免惰性抽取混用实现。
- 每个用例独立进程、`perl alarm 120`，输出 `<用例>.log`/`<用例>.xml`；`manifest.txt` 记录 HEAD、源/二进制哈希和各步 exit。

## 2026-11-04 本机结果（macOS arm64）

| 变体/模式 | 结果 |
|---|---|
| fix plain | 8/8 PASS，overall exit 0 |
| fix ASan/UBSan | 8/8 PASS，exit 0，无 sanitizer 报告 |
| base plain | 配置路径、连接失败事件、空/重复 API 3 项 PASS；其余 5 项 SIGSEGV(139) |
| base ASan/UBSan | 同 5 项 exit 134：`TraderAdapter.cpp:2520` `member call on null pointer of type 'wtp::WTSVariant'`（空 `_cfg`，4 项）与 `'wtp::ITraderApi'`（initExt 失败后，1 项） |

## 直接相关既有用例与卡 7 回归

- 覆盖率构建 `/tmp/wt-i03-initext-20261104/cov`（Debug，`WT_ENABLE_COVERAGE=ON`）。默认 `-Werror` 构建失败于他人在改的 `src/TestUnits/coverage/n02_wtcore_engines/unresolved_report_journal_test.cpp:289/314/329`（`-Wrange-loop-construct`，日志 `/tmp/wt-i03-initext-20261104/cov-build-werror.log`），与本条无关，故以 `-DWT_WARNINGS_AS_ERRORS=OFF` 重配后构建 exit 0；产品 TraderAdapter 已在本夹具中以 `-Werror` 单独通过，依赖库默认 `-Werror` 构建 exit 0（含 `WTSDataFactory.cpp`）。
- 在临时 cwd 逐组运行，日志/XML/profraw 在 `/tmp/wt-i03-initext-20261104/related/`：
  - `R59*-R59Exec.*` 20/20 PASS（含新 `R59Misc.init_ext_connect_event_reports_state_without_login` 与 `connect_event_uses_cfg_credentials`）；
  - `N01TraderAdapter*` 33/33 PASS；
  - `P11*` 29/30，唯一失败 `P11SyncReport.order_report_rebuilds_table_and_undone`（p11_sync_report_test.cpp:331）；
  - `R3TraderTest.*` 16/17，唯一失败 `R3TraderTest.cancel_localid_synchronous_api_failure_is_not_cached`（r3_trader_risklimit.cpp:665）。
  - 两项失败与卡 7 README 记载的既有失败一致（卡 7 已用其改前快照对照确认），测试不经过 `WTE_Connect`，与本次改动无关。
- 卡 7 夹具 `i03_trader_ratelimit_20261103/run.sh`（本次依赖库、当前源码）：fix plain 24/24 exit 0（`/tmp/wt-i03-initext-20261104/card7-fix-plain/`），fix ASan/UBSan 24/24 exit 0、无 sanitizer 报告（`/tmp/wt-i03-initext-20261104-asan/card7-fix-asan/`）。

## 限制

- fake 外部 API 只模拟“connect 后自报 WTE_Connect/登录结果”的时序；真实外部 `ITraderApi`（含 HftLatencyTool TestTrader 之外的实现）的自认证行为、Windows/MSVC 未验证。
- 依赖库为本机非 sanitizer 构建；sanitizer 只覆盖本夹具编译的 TU 与插件。
- 未执行完整回归。
