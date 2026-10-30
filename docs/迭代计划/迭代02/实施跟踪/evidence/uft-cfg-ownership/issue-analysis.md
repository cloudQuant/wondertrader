# BUG-I02-NEW-007：TraderAdapter 配置引用未释放

状态：`TEST_PASS`（UFT TraderAdapter 生命周期定向测试通过；仅为本地限定验收）

## 根因与修复

初始缺陷是 `init()` retain `_cfg` 后，module 加载和 API 初始化的失败分支没有回滚；`release()` 与析构也不释放该引用，且 `_cfg` 留非空阻止重试。初始源码定位见上一轮记录的 `TraderAdapter.cpp` 构造、析构、`init()` 和 `release()` 路径；相关代码现已修复。

当前实现以 `beginInitialization()` 拒绝生命周期不安全的重入，以 `ScopeExit` 回滚 init 失败；`cleanupApiAndConfig()` 先关闭本地 SPI callback admission、调用 API release、排空已准入回调、删除由 adapter 持有的 plugin API 并卸载库，最后才释放并清空 `_cfg`。成功路径的重复 release 是幂等的，析构通过同一清理路径兜底。实现位置：`src/WtUftCore/TraderAdapter.cpp:180-198`、`:606-736`、`:739-904`、`:915-961`。

API release 或 plugin delete 抛异常时，清理按 fail-stop 处理：资源和 `_cfg` 保留，adapter 不允许重试初始化；析构无法确认清理完成时 fail-stop。这样不把“plugin 可能仍引用配置”的状态误当成已释放。回调自身调用 `release()` 时不能等待自身退出；若没有别的 teardown 控制线程继续清理，必须等回调返回后由控制线程再次调用 `release()`。详见 `TraderAdapter.h` 的 release 生命周期注释及 lifecycle 回归。

## 本地验收证据

- `/tmp/wt-uft-cfg-v2-build.log` 和 `/tmp/wt-uft-combined-objects.log` 已归档为 `build-v2.log`、`combined-objects.log`：包含 `TraderAdapter.cpp`、UFT lifecycle 测试对象、相关 UFT 对象及真实动态 fake plugin 的构建输出。
- 最新 `/tmp/wt-uft-cfg-v3-run.log` 已归档为 `run-v3.log`：`UftTraderAdapterLifecycle` 23/23 PASS，0 skipped。
- 回归覆盖正常失败回滚后引用计数回到基线并重试、成功 release/析构仅释放一次、factory/API init false 或 throw 的真实动态 fake plugin 顺序，以及 plugin release/delete 失败时保留依赖并 fail-stop。callback reentry、in-flight drain、重复/并发 release 也在同一组 23 个测试内。
- 可复现构建与测试命令见 `reproduce.sh`；它同时构建 `CoverageTests_uftcore` 和 `TraderMocker`。Ninja 生成的完整直接链接命令（含 UFT lifecycle 测试对象和实际库）见 `link-command.txt`。测试目标输出为 `build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_uftcore`，TraderMocker 输出为 `build-cov/build_x64/Debug/bin/libTraderMocker.dylib`。测试用 `dladdr` 从可执行文件定位其上两级的 `build_x64/Debug/bin`，从而找到 TraderMocker；fake lifecycle plugin 的绝对路径由 CMake 编译定义注入。

## 尚未验收的边界

- 本地 callback admission/lease 只能排空已进入 adapter 的回调。外部 provider 必须保证 `ITraderApi::release()` 返回后不再发起晚到 SPI，也没有尚未进入 adapter lease 的待派发调用；真实 provider 的这一保证未验证。
- 仅本地 UFT lifecycle 23 项通过。旧 UFT 测试全集、完整 runner/策略到真实交易 API 链路、其他平台及生产/CI 均未验。
- cleanup 异常和 callback 内 release 都不是普通成功清理：前者保留资源并 fail-stop；后者在回调退出后需要控制线程重试。不得将这两类情形记作配置引用已清理。
