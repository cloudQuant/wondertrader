# R202-001：全天收盘后的次日行情与同步回调重入

2026-11-02，macOS arm64。此目录仅添加隔离验证夹具；**没有修改产品 ticker，也没有完成本条修复或验收**。`nextday.inc` 合并到既有 R202 真实引擎/真实 ticker fixture，数据读取器及策略回调为测试替身；不访问真实行情或交易。三个 ticker 的当前源码与旧隔离快照逐字节一致，脚本将当前 `WTSSessionInfo.hpp` 与 `TimeUtils.hpp` 复制入实体快照后重编三个 ticker 和测试 TU。其他引擎对象、静态库及依赖使用 `/tmp/wt-i03-r202-001-midnight` 的旧隔离构建，因此并非当前完整树的统一构建，不能作为整条验收证据。输入哈希及实际结果在 `evidence-20261102/`。

## 已知红控和正控

- 全天 `23:59:59.999` 先产生旧日 `2400` 收盘；随后新日 `00:00` 与 `00:01` 两个 tick 均进入测试策略，但三个引擎的交易日仍为旧日、未产生新日 `00:01` 分钟结束。`R202NextDay.Cta/Sel/Hft` 各以断言失败 exit 1；不表示通过。
- 同步重入场景：数据读取器的 `2400` 结束回调内部直接投递新日 `00:00` tick，回调返回后三个引擎分别把分钟状态写成新日 `2400` 且交易日停留旧日。`R202Reentry.Cta/Sel/Hft` 各 exit 1；这是受控 fake 同步回调，不证明每个真实 provider 都这样重入。原分钟标签的 `2400` 用于旧日收盘；不能将该状态当作新日正常时刻。
- 既有三个全天 `2400` 收盘与三个普通收盘控制各 1/1，无 skip。它们仅验证收盘边界，不抵消上述红控，也不验证后续整日行为。
- 局部 TSan 构建仅插桩三个 ticker TU 和测试 TU，CTA/SEL/HFT 三进程分别报告 `on_tick` 的 4 字节写与 ticker worker 读的竞争，并由 TSan 终止（Python returncode -6）；引擎对象、GTest、依赖未全面插桩，不可称整链通过。`TSAN.*.log` 是原始报告，未静音或屏蔽。

首次 TSan 编译因 LLVM 22 需提前包含 `stdlib.h` 失败，随后链接器不接受 `-no_warn_duplicate_libraries`；初始失败日志保留。最终 TSan 测试仅在该隔离链接中移除不兼容的链接器 warning flags，未关闭 sanitizer。

## 复现

需要保留此前 `/tmp/wt-i03-r202-001-midnight/{snapshot,build}` 实体隔离构建，缺失时先按照 `docs/迭代计划/迭代03/实施跟踪/evidence/R202-001-midnight-20261102/README.md` 构建；脚本使用此前构建的 compile_commands 和 Ninja 链接命令，输出目录必须不存在，不建立符号链接。脚本退出 0 仅表明**观测到预期的红控失败及正控通过**，不能将其当作产品验证通过。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r202_nextday/run.py /tmp/wt-i03-r202-nextday-new
R202_TSAN=1 /Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r202_nextday/run.py /tmp/wt-i03-r202-nextday-tsan-new
```

## 修复边界

当前时间字段、分钟位置和 worker 之间缺少完整一致的状态同步，外部结束回调在现有分钟锁内同步执行。只在自然日期变化时清零位置既不能处理夜盘跨自然日仍属同一交易日，也不能解决 worker 与 `on_tick` 竞争、结束回调重入和事件顺序；直接在外部回调期间持普通状态锁会产生重入死锁，仅改原子字段也不能确保组合状态一致。安全修复需确定并实施同一 ticker 内事件串行化、重入时同步返回及顺序、回调对象的保活与停止/排空语义，并覆盖旧日 `2400`、新日首 tick、worker 竞争、夜盘自然日变化和多日回放；若改变同步回调或旧 ABI 须先落实宿主合同。本次未尝试通过改变交易日定义或只调整测试来消除失败。真实 provider、跨平台及完整回归均未验证。

两个 probe 在观察结果前均按现有 fake 的 100ms session-end 睡眠再等待 200ms；该等待不是回调完成的确定性屏障。在极端调度或慢机器上可能造成超时/波动，需以记录的逐进程退出码和原始日志为准，后续修复应增加完成屏障而非放大延时。
