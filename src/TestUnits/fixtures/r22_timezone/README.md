# BUG-R22-002 关联：`TimeUtils::getTZOffset` 首次并发缓存竞态

`getTZOffset` 在每个翻译单元缓存首次计算结果。旧实现以普通静态整数判空并写入，多个线程首次访问时存在数据竞争，并使用 `localtime`/`gmtime` 的共享静态返回区；当前实现以原子快读加互斥保护首次计算，改用可重入的 `gmtime_r`/`localtime_r`。转换失败返回哨兵 `99` 且保留重试机会，不伪造 UTC。

```sh
bash src/TestUnits/fixtures/r22_timezone/run.sh <新输出目录>
```

须从仓库外的源码副本运行。`run.sh` 先调用 `build.sh <输出>/build`，用 Homebrew LLVM 22 构建：当前头文件的 TSan 与 ASan/UBSan 探针、`baseline/TimeUtils.hpp`（改前原件，哈希见 `baseline/SHA256SUMS`，与 docs 证据 `R22-timezone-race-20261102/TimeUtils.baseline.hpp` 相同）的 TSan 探针、以及既有 `r16_timeutils` 用例。然后逐例独立进程运行（30 s 硬超时），写 `results.txt`，任何一行 `MISMATCH` 即 exit 1：

| 用例 | 期望 |
|---|---|
| `baseline-tsan-race`（改前头，24 线程同步首次调用） | 红控：非零退出且 TSan 报 data race |
| `current-tsan-race-0..2` | exit 0、无 TSan 报告（`clock_calls=1`，只计算一次） |
| `normal-*`（当前头）与 `baseline-normal-*`（改前头）×10 个时区/时间点 | exit 0、无 sanitizer 报告；按旧算法求整数小时偏移并核对 `fasttime` 日期时间 |
| `cache` | 环境与时间变化后沿用首次缓存 |
| `failure-1..4` | `time`/UTC 分解/本地分解/`mktime` 受控失败返回 99，下次重试成功并缓存 |
| `r16-existing`（`r16_timeutils.fasttime*`、`tz_offset_is_cached_and_in_whole_hours`，TZ=America/New_York） | 全部通过 |

探针以宏把 `time`/`gmtime_r`/`localtime_r`/`mktime` 替换为确定性时钟与可注入失败的包装，并人为延长首次计算以暴露竞态；分解函数仍调用系统实现，线程与 C 运行时未全量插桩。与旧语义一致，印度、纽芬兰的半小时偏移截断为整数小时，冬夏切换不会更新已缓存结果——本夹具不认证这些地区的正确当地时间。`99` 哨兵仍可能被未检查的旧调用者当作小时偏移，失败传播不是闭合的；Windows `gmtime_s`/`localtime_s` 分支、Linux 与真实时区变更并发未验证。

2026-11-05 最终验收更新：原 `build.sh` 从 `docs/` 证据目录复制改前头文件、套件内没有运行入口（`run.py` 只在 docs 证据中），在只含 `src/`、`dist/` 的冻结树上无法运行。现把改前头文件放入 `baseline/`，新增 `run.sh` 复现 docs `run.py` 的全部场景并对每一项做期望判定。重跑记录见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-acceptance/fixtures-fixtures-agent/README.md`；2026-11-02 原始日志仍在 `docs/迭代计划/迭代03/实施跟踪/evidence/R22-timezone-race-20261102/`。
