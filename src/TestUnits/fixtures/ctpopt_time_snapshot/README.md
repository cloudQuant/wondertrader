# CTPOpt 结算确认的 checked 时间快照（I03-NEW-006 CTPOpt 子项）

本机 fake 夹具：真实 `TraderCTPOpt::confirm()` + `t_TraderCTPOpt` 的 SDK 替身，时间相关 libc 调用（`clock_gettime`/`time`/`gmtime_r`/`localtime_r`/`mktime`）在私有快照的 `TimeUtils.hpp` 中改名为可注入故障的 `wt_i03_*`。不连接柜台、不下单、不结算，无符号链接。

```sh
python3 src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-ctpopt-time-green          # 当前代码
python3 src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-ctpopt-time-red baseline    # 改前（需 docs 证据基线）
python3 src/TestUnits/fixtures/ctpopt_time_snapshot/run.py <新证据目录>
```

- `build.py <out> [baseline|checked-baseline|reuse]`：把 `src/TraderCTPOpt`、`Includes`、`Share`、`WTSTools`、`WTSUtils`、`FasterLibs`、`API/CTPOpt3.5.8` 与 `src/TraderCTP/VendorTdLifecycle.hpp` 复制到 `<out>/src` 私有快照，在快照中改名时间调用、把 `confirm()` 改为 public，并以 LLVM 22 ASan/UBSan 构建 `<out>/confirm-probe`。仓库源码不改。
- `run.py <evidence>`：依次运行 `/tmp/wt-i03-ctpopt-time-red` 与 `/tmp/wt-i03-ctpopt-time-green` 下的探针，每个模式独立进程，记录退出码与日志到 `<evidence>/results.json`；green 模式期望全部 exit 0。
- red（`baseline`）变体使用 `docs/迭代计划/迭代03/实施跟踪/evidence/I03-NEW-006-ctpopt/baseline/` 中的改前 `TraderCTPOpt.cpp` 与插桩 `TimeUtils`，该基线不在套件内；夹具运行器只运行 green，red 记为 not-applicable。

## 2026-11-05 更新

最终冻结树 F3 上 `build.py` 编译失败：当前 `TraderCTPOpt.h` 新增 `#include "../TraderCTP/VendorTdLifecycle.hpp"`（卡9 厂商生命周期辅助），私有快照缺该头。`build.py` 现额外复制该头文件（仅此一个文件）；探针、模式表与期望未改。夹具运行器重跑：green 27 个模式全部 exit 0。

限制：fake SDK 与注入的时间故障不证明真实 CTPOpt SDK 时序、真实时区数据库行为、Windows 或完整回归。
