# Journal 首次建档：局部恢复取证与进程退出验证

2026-11-02，本机 macOS arm64，范围仅 `UnresolvedReportJournal.cpp` 和专属测试；无生产接入、真实交易、符号链接或提交。之前工作区已有的 marker 失败修复及首次建档预检保留，参见 `../I03-journal-marker-failure-20261102/README.md`。

## 变化和红控

预检之后、创建锁之后仍会再次探测 marker；旧代码在这一阶段发现 marker 时返回 `RecoveryRequired`，但 `recoveryInspection()` 返回 `NotOpen`。`red.log` 使用测试专用编译钩子在预检后创建普通文件来复现，旧逻辑该测试失败、exit 1；同一红控中途退出测试通过。现再次探测时发现 marker 会捕获已有安全 marker 的字节并标记人工恢复；探测错误返回 `StorageFailed`。不删除任何 journal 组件，也不覆盖已有文件。测试专用的阶段钩子仅在显式定义 `WT_JOURNAL_BOOTSTRAP_TEST_HOOKS` 时编译，生产构建不包含钩子。

## 验证

`final.log` / `final.xml`：本类 20 个相关测试通过，包含旧恢复回归、预检后 marker/guard 出现，以及 fork 子进程在 lock、WAL、guard 创建后、文件及目录同步后、bootstrap marker/prepare/primary/commit 写入后共 8 个阶段于代码内直接 `_exit`。这些中途退出均会在重开时返回 `RecoveryRequired`，重试建档返回 `AlreadyExists`；还验证创建前退出 `NotFound` 和 `createNew` 成功后进程退出可正常重开。guard 在预检后出现时不被覆盖，但会遗留新建的 lock 和 WAL；该负例明确显示未做到多文件原子创建。

测试使用 LLVM clang++ 22.1.8、`-std=c++17 -g -O1 -DWT_JOURNAL_BOOTSTRAP_TEST_HOOKS -fsanitize=address,undefined -fno-omit-frame-pointer`，仅链接该类及专属测试和预编译 GoogleTest；运行设置 `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`。系统库及预编译 GoogleTest 未插桩，子进程直接 `_exit` 不运行进程退出清理，因此不据此证明子进程泄漏检查。另无钩子生产形式执行 `clang++ -std=c++17 -Wall -Wextra -Isrc -c src/WtCore/UnresolvedReportJournal.cpp` 成功，`git diff --check` 成功。`hashes.txt` 为最终测试源和头文件摘要。

## 未完成的要求和约束

现有布局将 lock、WAL、guard 分别放在调用者既有目录的独立路径；并无统一原子发布点。单个 `O_EXCL` 不能消除预检到多文件创建的竞态。上述恢复是失败后保留证据并拒绝使用，**不是版本化多文件原子建档，不是自动安全清理，也不是 L01-014 验收**。可行方向需要共同确定受信父目录和独占版本目录、在版本目录中准备完整文件并同步，通过平台支持的不替换原子发布机制暴露单一版本入口，再定义旧调用者/旧布局读取、版本识别、所有权及未知孤儿的人工恢复；在解决旧读取器可绕过版本入口及部署迁移之前，不能宣称兼容或原子。这里没有实现该协议或进行跨平台认证。

没有模拟断电、文件系统损坏、真实 ACL 故障、Linux/Windows、旧版本进程混跑或非合作进程持续篡改，不能从进程 `_exit` 推断硬断电持久性。账户级暂停、身份认证、外部对账和 TraderAdapter 回报接入仍另待完成。
