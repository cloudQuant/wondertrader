# I03-NEW-006：checked 时间快照及 CTPOpt 确认子项

`TimeUtils::tryGetDateTime(date,time)` 新增独立 checked 接口：从一次实时时钟采样派生日期及 `HHMMSSsss`，检查时钟、时区转换、范围、输出别名和 `fasttime`；失败保持两个输出不变。旧接口及未迁移调用方保持原样。首次时区偏移未缓存时，`getTZOffset()` 仍会额外调用一次 `time()` 进行偏移初始化；“同源”仅指最终日期与时间来自同一次 `clock_gettime`（Windows 使用既有共享时钟读取），不是说初始化过程中仅进行一次系统时钟读取。保留原整数小时/DST 缓存语义，不解决半小时时区或运行中时区变化。

`TraderCTPOpt::confirm()` 仅在 API 和状态有效、取得成功快照后构造并发送请求；故障时记录日志并返回 -1，不调用 SDK、不分配请求 ID。成功时保持 `YYYYMMDD` 与 `HH:MM:SS` 格式，去掉对临时字符串的固定长度 `memcpy`。现有 `OnRspQrySettlementInfoConfirm` 为 void 并忽略 `confirm()` 返回值；该路径故障时保留 `WS_CONFIRM_QRYED`、不产生登录成功，sink 存在时可见错误日志，但没有机器可读失败回调，也没有自动重试。下面的“重试”是测试主动再次调用并不证明真实 SDK 自动再次回调。

## 定向测试

macOS arm64，Homebrew LLVM 22.1.8，相关产品 TU 和测试 TU 局部 ASan/UBSan，`detect_leaks=0`；测试只用进程内 fake SDK，不连接真实交易。其他 C++ 库/SDK 依赖未全量插桩，链接使用 `-undefined dynamic_lookup`，不能外推至完整产品或真实 provider。旧版本为改动前的 `baseline/TraderCTPOpt.cpp` 及测试用平台函数注入后的 `baseline/TimeUtils.instrumented.hpp`；不是干净仓库全树。为直接检查 private `confirm()` 返回值，隔离副本中仅把该成员声明改为 public；产品访问控制未改变。用真实 `OnRspQrySettlementInfoConfirm` 回调的测试不依赖此暴露。

`results.json` 和逐场景日志记录退出值：

- 五类时钟/时区转换故障（时钟、`time`、`gmtime_r`、`localtime_r`、`mktime`）分别测直接确认及真实回调链，旧版各错误地发送 SDK 请求，共 10 个预期红控 exit 1；新版均拒绝发送、记录错误，恢复后可再次调用，共 10/10 通过。
- 跨午夜模拟时钟在连续读取间跳秒：旧版生成 `20231231 00:00:00`，预期红控 exit 1；新版只采样一次，生成 `20231231 23:59:59`，exit 0。正常发送、无 API、未就绪、SDK 返回失败四项旧新正控均通过。
- 新 checked 接口在五类故障、无效纳秒、负秒、不可表示年份及输出别名的 9 个场景均保持原输出，后续可恢复的 8 个场景验证恢复后正常快照。新版合计 24/24 场景退出 0；`ASan/UBSan` 仅覆盖当前测试配置，LSan 未运行。
- 新接口加入后，原 R16 相关 7 个日期/缓存用例重编 7/7，24 线程时区缓存 TSan 再测通过，各见 `r16-existing.log` 和 `tsan-cache.log`。

重建和重跑（`build.py` 在隔离副本中注入受控系统函数、保留实体依赖，不创建符号链接）：

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-ctpopt-time-red baseline
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-ctpopt-time-green
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/run.py 'docs/迭代计划/迭代03/实施跟踪/evidence/I03-NEW-006-ctpopt'
```

实际编译命令、退出码和源码/二进制哈希见 `*-build-commands.json`、`hashes.sha256`。`clock_gettime`、`time` 等故障由隔离副本替换为测试函数，不代表真实系统故障发生时的完整宿主行为。原失败传播审计涉及的 StateMonitor、引擎、ID、回放、存储等调用方均未迁移；Windows/MSVC、Linux、真实 SDK、整合构建和全量回归未验证。I03-NEW-006 仍未闭合。
