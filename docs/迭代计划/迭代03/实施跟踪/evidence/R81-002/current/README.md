# BUG-R81-002 最新 P01 修订后定向复验

工作区当前 `CtaStraBaseCtx.cpp` 和当前 `r81_cta_gap_test.cpp` 以实体 Boost bridge 优先重新编译并与 v8 其余对象/库独立重链，命令参数 `commands.json`，源码与二进制哈希 `hashes.json`，三个 build 日志均 exit 0。编译前后及运行结束后四个关键输入哈希一致，构建和运行目录未发现新符号链接。`results.json` 和 run0/1/2 的 XML/日志分别为：

- `ConditionRecoveryRejectsWholeMalformedSnapshotAndRestoresValidSnapshot`：混合非法 alg/action/field 的文件整体拒绝，合法文件逐字段恢复和一次触发正控。由于混合坏字段中首个 alg 导致提前拒绝，单此用例不独立证明 action/field 校验。
- `ConditionAlgorithmsActionsAndNoMatchCode`：不同条件比较与动作的正常控制。
- `ScheduleTwentiethEmitPersistsUserDataAndConditions`：条件持久化控制。

上述三个单例独立进程，各 1/1 PASS，无 disabled。另在 `/tmp/wt-i03-r81-r42` 以同样隔离方式重编当前 `r42_cta_persist_test.cpp` 和当前 CTA 产品 TU，运行单例 `R42CtaPersistTest.MalformedSnapshotsDoNotPublishAnyStateOrSubscriptions`，1/1 PASS，无 disabled；该用例分别对有效快照注入 field=99、alg=99、action=99，逐个检查拒绝诊断及既有状态不变。第二构建命令及哈希见其 `commands.json` / `hashes.json`，XML 和日志在其 run/。

本机 macOS arm64；各构建只重编两个 TU，其他对象/库复用旧 v8，未做全依赖 sanitizer、真实柜台、Windows 或整体回归；结果不等于全树验收。
