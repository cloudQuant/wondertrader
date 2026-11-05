# R13-004：实际选股引擎触发隔离探针

本目录只包含测试，不修改产品代码。以真实 `WtSelEngine::init/addContext/on_minute_end` 和 `SelStraBaseCtx` 派生测试策略构造每日任务；通过先构造不注册的上下文，使任务自增 ID 与实际注册 context ID 不相等。回调、基础数据、行情/柜台使用本机 fake，不访问真实交易。

可复现：

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r13_real_schedule/run.py /tmp/wt-i03-r13-real-schedule-new
```

脚本使用 `/tmp/wt-i03-r202-001-midnight` 的旧隔离源码/构建，确认 `WtSelEngine.cpp` 与当前工作树逐字节相同，再复制到新的实体目录分别编译引擎测试版本；其他头、对象和静态库仍为旧快照，不能称当前完整树验证。`evidence-20261102` 是本轮原始日志和结果。每个场景独立进程，最长 20 秒；本轮未运行 sanitizer：

- `original`：未改引擎源，任务到达后局部 joinable 线程析构使进程中止，返回 -6；不是通过。
- `joined`：**只在隔离测试副本**中临时 join 线程，便于观察线程析构之后的行为；注册策略未得到调度，断言失败 exit 1。不能将测试用 join 看成产品候选：ticker 可在持非递归锁时调用此方法，策略若同步重入该锁或请求释放 ticker 可能死锁。
- `key_fixed`：隔离副本再临时按 `_tasks` 注册时的 key 查询 context，且保留测试用 join；注册策略被调用、对照未调用，exit 0。这仅隔离证明 task ID 和 context ID 查询不匹配，不证明同步 join、callback 重入、并发、异常处理或关闭生命周期正确；禁止把此测试改动当产品补丁。

实时 ticker 在 `_mtx` 内调用 `on_minute_end`，当前 `release()` 又直接 stop/join ticker。完整实现需要定义 callback 顺序与重入、实际任务执行线程的生存与排空、关闭及 callback 内销毁的 owner 合同；直接同步回调或临时 join 会把原 ticker 锁暴露给策略回调，直接 detach 则缺少排空保障。本探针没有测试公开 `create_sel_context`、多 context 并发或真实 provider，未修复 R13-004。

另以原版真实引擎验证 `_day=10` 的月任务在 2025-01-10 09:31 触发：该日非假期，仍没有调用策略，断言 exit 1（`monthly.log`）。头文件定义月任务 `_day` 为 1..31，但当前分支将日期 `% 1000000`（得到 YYMMDD）与之比较，普通日期无法相等。测试只覆盖这一普通日，未验证节假日顺延或改变产品代码；直接修正日期判断会暴露前述 joinable 线程终止缺陷，不能单独视为完整安全修复。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。`run.py` 依赖 `/tmp/wt-r202-001-midnight` 旧隔离快照，并在复制的 `WtSelEngine.cpp` 中按固定文本注入测试专用 join；当前调度实现已改写（任务在其 context 上执行并由 release 排空），锚点 `assert text.count(target)==1` 失败。其 `original`（-6）/`joined`（exit 1）是旧缺陷的已知红，`key_fixed` 是“禁止当产品补丁”的隔离改动。
- 由以下 coverage 用例取代（F3 均为 pass）：`CoverageTests_wtcore::S5SelScheduleThread.FiredTaskRunsOnceOnItsContextAndIsJoined`（任务按注册 context 触发一次且线程被 join：对应 task ID/context ID 失配与局部线程析构中止两项）、`CoverageTests_wtcore::S5SelScheduleThread.TasksOnSeveralDaysAreAllJoinedByRelease`、`CoverageTests_wtcore::R211SelEngine.ExpiredTaskTerminatesBeforeSchedule`（BUG-R192-002/003，迭代03 启用）、`CoverageTests_wtcore::S3K27SelPeriodFire.*`（6 例，日/周/月/年/分钟周期触发）。
