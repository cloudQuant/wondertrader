# R22-002 关联：时区缓存首次并发访问

`TimeUtils::getTZOffset` 在每个翻译单元缓存首次计算结果，旧实现以普通静态整数判空并写入；多个线程首次访问时存在竞态。旧实现还使用 `localtime`/`gmtime` 的共享静态返回区。当前修改使用原子快读与互斥保护首次计算，调用平台可重入的 UTC/本地时间分解函数。沿用原分钟转小时顺序及首次成功后缓存的行为；不改变全局时区或运行中的 DST 切换策略。

转换失败时返回原未初始化哨兵 `99` 并保留重试机会，不伪造 UTC。`time()` 或 `mktime()` 返回 `-1` 时按失败处理；`-1` 同时也可能表示有效的 1969 年时间，属于此接口无法消歧的边界。现有 `getCurDate`、`getDateTime` 等调用者没有检查哨兵，仍可能把 `99` 当作小时偏移，失败传播不是闭合的；该子项不关闭 R22-002。

## 重建与运行

本机 macOS arm64，Homebrew LLVM 22.1.8，构建测试和当前头文件使用实体目录，未创建符号链接。隔离旧版头文件 `TimeUtils.baseline.hpp` 是本次修改前工作区原件，非单独版本的完整源码树；SHA-256 在 `source-before.sha256`、`source-after.sha256` 和本文件夹中可复核。`build.log` 记录每条构建命令：

```sh
src/TestUnits/fixtures/r22_timezone/build.sh /tmp/wt-i03-timezone-race
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python 'docs/迭代计划/迭代03/实施跟踪/evidence/R22-timezone-race-20261102/run.py'
TZ=America/New_York /tmp/wt-i03-timezone-race/r16-existing '--gtest_filter=r16_timeutils.fasttime*:r16_timeutils.tz_offset_is_cached_and_in_whole_hours'
```

新旧 TSan 均在独立进程用 24 线程同步首次调用；旧版退出 `-6`，`baseline-tsan-race.log` 明确报告 `offset` 的写/读 data race；新版 3 次均退出 0，结果中的 `clock_calls=1` 表明只计算一次。测试中将 `time` 替换为确定性时钟，并人为延长首次计算，以暴露缓存竞态；使用的 `gmtime_r`、`localtime_r`、`mktime` 仍调用系统实现。线程及 C 运行时依赖未全量插桩。

新旧版本各 10 个定向单线程场景均通过：UTC、上海、纽约冬/夏、印度、纽芬兰、基里巴斯、夏威夷和两个跨年时间。参考值按旧版算法从系统时间分解计算，另用 `gmtime_r` 检查得到的整数小时偏移应用到 `fasttime` 后的日期时间字段。新版在环境与时间变化后的缓存控制、时钟/UTC 分解/本地分解/`mktime` 四类受控失败后重试均通过，见 `results.json` 和各日志；与旧语义一致，印度和纽芬兰的半小时偏移被截断为整数小时，冬夏变更不会更新已缓存结果，不能将本测试理解为这些地区的正确当地时间认证。旧算法先转换到 `uint32_t` 再赋回有符号变量，对西半球结果依赖实现定义的 unsigned-to-signed 转换；本机结果与旧版一致，跨平台可移植性待单独处理。

现有 `r16_timeutils` 直接关联的 7 个缓存和 `fasttime` 用例在纽约时区全部通过，见 `r16-existing.log`。仅头文件探针和独立测试 TU 构建，并非完整项目构建；Windows/MSVC 的 `gmtime_s`/`localtime_s` 分支、Linux、真实时区变更并发、失败传播以及其他调用者未验证。
