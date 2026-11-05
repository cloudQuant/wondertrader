# R14-010：UFT Runner 启动与关闭交错

2026-11-02，macOS arm64。本目录是局部修复及定向证据，未连接真实行情或交易；不是 provider 停止排空或迭代整体验收。

`run()` 原先在读取配置、启动引擎、parser、trader 和共享监视器时未持有整体启动租约。受控屏障暂停在引擎调用前，外部 `shutdown()` 已返回 true；另一测试让两个 `run(true)` 均进入该位置。此处不据此宣称已实测悬空引用或真实 provider 回调。

候选复用现有 parser-start 互斥锁及状态字段保护完整 runner 启动，不增加对象字段；原无参私有 `startParsers()` 和其直接启动的等待关闭行为保留，新增私有 overload。启动期 `shutdown()` 设置请求并返回 false，不持锁等待回调；`run()` 离开租约后尝试处理待关闭请求。若其他 shutdown 已在途、provider 不静止或关闭仍返回 false，调用者必须持续持有对象并在控制线程重试。每个对象最多尝试一次 `run()` 启动；重复调用在进入引擎前返回，运行接口仍为 void。通过增加完整启动前置准入，不再接受在一次失败启动后无条件重试；若需可重启语义，须另行定义并实现。

现有 `r95/r220` 夹具的两个 run 正控和三个直接 parser-start 停止/重入控制保留。新增七项检查并发/重复 run、启动中外部关闭、启动线程测试钩子内重入关闭、配置失败及关闭重试、启动异常的租约释放和异常时待关闭请求。测试 hook 仅插入复制的源码中；它模拟启动线程重入，**并不证明真实 provider 的回调行为**。每项均以独立进程并加 30 秒硬超时；无预编库全面插桩、无硬故障、无 Windows/Linux 或完整工程构建。

## 定向结果

- 旧版：七个新增用例中六个红控退出 1，配置失败控制退出 0；原五个控制均退出 0。记录分别为 `evidence-20261102/baseline/results.txt` 及日志。
- 最终候选：普通、局部 ASan/UBSan 和局部 TSan 分别 **12/12**，每项退出 0；原始逐项日志和最终输入哈希见 `evidence-20261102/candidate/`。
- 编译仅覆盖复制的 `WtUftRunner.cpp` 与两个 test TU，预编 `WtUftCore`、`WTSTools`、`WTSUtils`、GTest、Boost、系统库等未全插桩。ASan 使用 `detect_leaks=0`，不能声称 LSan 或整链 TSan 通过。析构之前调用者必须维持对象存活，真实插件 callback quiescence、生命周期和跨平台未验。
- 初次构建曾因旧 bash 空数组、测试变量重名及新版 Clang 的 fmt 头预包含次序失败；修正测试脚本后重新构建。曾在两个测试进程还在运行时修改测试文件，导致当轮脚本异常退出；该轮不计结果。最终本次源码和脚本在运行前后 SHA256 一致。

## 复跑

```sh
bash src/TestUnits/fixtures/r14_runner_startup_overlap/run.sh /tmp/wt-i03-r14-review-new
```

输出目录必须不存在。脚本使用先前实体隔离快照 `/tmp/wt-i03-r14-010-20261102/snapshot/src` 和其构建目录中的预编库，覆盖当前工作树的 runner 源码及头文件，在复制中加一个屏障；核对相关 r95/r220 旧夹具与快照一致，不创建符号链接。若先前 `/tmp` 已清理，需按 `docs/迭代计划/迭代03/实施跟踪/evidence/R14-010-20261102/README.md` 重建其隔离依赖。脚本最后按 `results.txt` 的实际逐项退出码确定候选成功；原始日志需保留以识别 sanitizer、超时等不同失败。

旧版重跑使用 `R14_BASELINE=/tmp/wt-i03-r14-pre-change-20261102`，该目录可能被清理；`evidence-20261102/runner-candidate.patch` 是本轮基线到候选的精确差异，可在临时副本中反向应用以重建基线，严禁对共享工作树反向应用。
