# I03-NEW-006 checked 本地时间快照修正

本机 macOS arm64。此前新增的 checked 时间使用 `getTZOffset()` 的整数小时缓存；虽然报告成功，但尼泊尔、澳大利亚半小时时区和 DST 转换日会得到错误本地日期或时刻，CTPOpt 进而发送错误的确认时间。本次仅改变 `tryGetDateTime`：对一次系统时钟采样的秒值调用 `localtime_r`（MSVC 分支为 `localtime_s`），生成真正本地日期与时间。旧 `getTZOffset`、`fasttime` 及其他旧接口的缓存与整数小时语义不变。CTPOpt 的确认代码未改，但因调用 checked 接口而使用修正后的日期时间。

新增 `tryGetDateTime(uint32_t& date, uint32_t& time, uint64_t& epochMillis)` 重载，日期、HHMMSSsss 和 Unix epoch 毫秒由同一时钟采样派生；先检查输出内存重叠、采样和本地时间分解、整数范围与毫秒乘加溢出，任何失败均不写三个输出。原两参数接口调用该重载并保持原签名；时间和日期输出别名仍在采样前拒绝。输出使用者必须检查 bool，调用成功也不代表时钟同步或 provider 就绪。

## 可复现验证

先前 checked 版头 `baseline/TimeUtils.hpp` 是此任务前的工作树文件（SHA256 `3b603ba7a58806bed3cc39c933cc9106fc28e2bcc7e6d7eda95e6d5569c070ff`），使用同一当前 CTPOpt 源和同一 fake SDK 测试编译，不是仓库最初版本。受控 `clock_gettime` 固定采样，时区通过 `TZ` 与 `tzset` 在独立测试进程设置。旧 checked 版在 Kathmandu、Adelaide、本地跨年、纽约 DST 转换和 legacy 时区计算故障隔离共 5 项退出 1；UTC 正控退出 0。旧版 Kathmandu 请求是 05:00 而正确为 05:45；跨年场景旧版请求为上一日 23:15，正确为新年 00:00。

当前源码局部 ASan/UBSan **27/27** 通过：上述时区/DST/UTC 和 CTPOpt 确认发送、时钟/本地分解失败及重试、不发送或不报告登录成功的回调路径、跨午夜单采样、无 API/未就绪、SDK 拒绝、纳秒范围、负秒、超范围和两参数别名，以及三参数 epoch 同源、失败保持、溢出及输出重叠。隔离故障通过替换副本中系统调用实现；不证明真实操作系统故障或真实 SDK 时序。`results.json` 与逐项日志记录退出值。需要本机 zoneinfo 时区数据库；并发更改进程 TZ 未测试。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-localtime-checked-red checked-baseline
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-localtime-green
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/I03-NEW-006-localtime-20261102/verify.py'
```

编译在实体复制的隔离源码目录，Homebrew LLVM 22.1.8 对产品 TU 与 fixture 使用 `-fsanitize=address,undefined`；fake SDK，动态链接用 `-undefined dynamic_lookup`，LSan 关闭，不代表整个项目或 SDK 插桩。目录内未创建符号链接。`red-build-commands.json`、`green-build-commands.json`、`hashes.sha256` 包含具体命令和源码/二进制哈希。旧时区缓存相关 R16 用例 **7/7**，24 线程首次缓存访问 TSan 控制 **1/1**；日志 `r16-run.log`、`tsan-cache.log`，并非完整并发证明。

POSIX `localtime_r` 与 MSVC `localtime_s` 的接口和失败返回分别参照 [GNU C Library 文档](https://sourceware.org/glibc/manual/2.41/html_node/Broken_002ddown-Time.html) 与 [Microsoft CRT 文档](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/localtime-s-localtime32-s-localtime64-s?view=msvc-170)。本机只验证 macOS；Linux、MSVC/Windows、运行中修改时区配置、真实 provider 和完整整合树均未验证。CTPOpt 的 `void` 回调仍忽略 confirm 返回，仅 sink 日志可见，无自动重试；其他未迁移旧时间调用者仍可能使用 99 哨兵。I03-NEW-006 未闭合。
