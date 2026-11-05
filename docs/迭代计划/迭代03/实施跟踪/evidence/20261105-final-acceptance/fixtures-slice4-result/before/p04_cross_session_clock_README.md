# P04-011：混合 session 自然时钟与历史 Tick 桶

本次只新增定向夹具，**没有修改产品源，也没有关闭 P04-011**。运行 macOS arm64 的真实 `HisDataReplayer`、本机 `WtDataStorage` 和临时 DSB 文件，数据为合成行情，无真实交易。构建依赖 `/tmp/wt-i03-r58-20261102-build` 和 `/tmp/wt-i03-r58-20261102-src` 的预编对象、静态库与动态模块；非整树重编、非 sanitizer、未验证 Linux/Windows 或真实 provider，临时依赖可能被系统清理。

## 可复现结果

1. `green.log`：18/18，其中 17 个既有跨 session、夜盘、无数据范围、24h 周末/偏移和同刻四流排序测试；新增 `pureTickContinuousAndFutureDispatchOnlyWithNoDataDay` 组合 24h CRY 与 SHFE 21–23 夜盘，Jan 9 自然日无事件，Jan 10 夜盘按周末映射到 Jan 13 交易日文件。5 笔真实 tick 按自然时间逐一派发，最后一笔为范围末秒的 `23:59:59.999`，回调读取的时钟与 tick 时间相同，重播完成一次。该正控只检查默认查询返回的每条历史 tick 严格早于当前时钟，不证明一定返回了全部应见数据；不覆盖显式 etime 防前视。
2. `known-red-slice.log`：`pureTickContinuousAndFutureNoDataDayAndFinalMillisecond` 使用同一构造，五笔和时钟顺序正确，但第 3 笔 CRY（Jan 8 23:59:59.999）在 `get_tick_slice(code,100,0)` 中取得空值，尽管 Jan 8 00:05 的第一笔应按严格早于 cutoff 被包含。该回调内断言 `get_trading_date()==20200109` 成立，当前 `get_tick_slice` 用全局 `_cur_tdate` 调 `checkTicks`，而 CRY 24h 文件位于 Jan 8 桶；因此在此受控旧快照中观察到实际桶不匹配。**这是预期失败的产品红控，exit 1；不是通过。**
3. `known-red-negative-offset.log`：另一人工配置中，一个连续 24h 品种与 `offset=-300, 00:00–02:00` 品种混合，两天都各自有事件；回放第 2 天数据前 fail-closed，诊断 `Session-begin boundary 202001090000 ... does not map to trading date 20200109`。该自定义配置、交易日映射及边界的合法业务合同尚未裁定，不能将它单独认定为产品应支持的输入或把中止改为成功；记录 exit 1 以便后续核查。

现有显式 `etime` 历史查询可以指定未来截止，且旧测试在初始化期间依赖此类行为；本任务没有改变其语义，也不声称它满足无未来可见性。默认查询此前返回严格早于 cutoff 的 tick，本探针不改变规则。

**依赖漂移**：`HisDataReplayer.cpp/.h`、原 `p07_tick_loader_test.cpp` 与 R58 隔离快照字节一致，并在编译前核对；但 `WTSSessionInfo.hpp`、`WTSBaseDataMgr.cpp` 和 `TimeUtils.hpp` 的快照均与当前工作区不同，散列见 `evidence-20261102/hashes.txt`。因此红控和正控只证明该隔离构建的行为，不能声称在当前整合树独立复现。没有对先前自动审核中止的配置加载子任务重新构建或重试。

不能直接把 `checkTicks` 的参数替换为按品种计算出的交易日后就声称修复：现有 `WTSTickSlice` 借用 `_ticks_cache` 内 vector，`checkTicks` 在日期切换时可清空/替换 vector，改变日期选择会改变失效时点；需先明确并验证已借出 slice 的生命周期、跨品种/跨日缓存所有权及显式查询兼容，再测试正确桶与无未来数据。期间不可丢弃或覆盖用户持有的数据，不用简单时钟 clamp 来掩盖漏数。

## 复现

输出目录须不存在，Python 使用用户 Anaconda base：

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/p04_cross_session_clock/run.py /tmp/wt-p04-cross-build-new
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/p04_cross_session_clock/verify.py /tmp/wt-p04-cross-build-new/fixture /tmp/wt-p04-cross-results-new
```

`run.py` 编译包含原测试和新夹具的 TU，链接既有隔离目标对象，默认只运行单独新增正控。`verify.py` 在三个独立进程中运行 18 项正控、slice 红控、负 offset 红控；它要求红控 exit 1，脚本 exit 0 仅表示观察到预期的失败，不能把两项红控计作通过。全部运行使用独立 `/tmp` 目录。第一次 17 用例的 shell 包装误将 zsh 只读变量 `status` 赋值而 exit 1，之后用 bash 捕获退出码重跑为上述 18/18，原始日志在 `/tmp/wt-i03-p04-cross-initial/run.log`。
