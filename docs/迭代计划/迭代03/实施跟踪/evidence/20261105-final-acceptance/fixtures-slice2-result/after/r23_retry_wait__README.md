# R23-001：清理失败后的关闭唤醒

2026-11-02，macOS arm64。该项只处理常规 `WtDataWriter` 收盘 worker 的清理失败等待，不代表整个关闭协议验收。失败后非停止状态仍以 300 秒为周期等待；当 `_proc_stopping` 变为 true，已有 `_proc_cond` 唤醒等待并立即继续剩余最多五次尝试，之后继续处理已接受的后续队列任务。`_proc_stopping` 与等待谓词共用 `_proc_mtx`，队列任务通知不能让未满足的谓词提前返回。失败日志只陈述错误，避免在关闭或最后一次尝试时错误承诺等待时间或重试。

`build.sh` 先复制当前源码并核对指定关键文件的哈希，在隔离副本中插入两个专属 hook：移除目录之前可控地抛出异常，等待之前报告执行位置；产品源没有这些新 hook。baseline 仅将该副本的条件变量等待还原成旧版 `sleep_for(300s)`，日志仍使用新版内容，因此是**等待行为红控**而非整个旧源字节快照。测试经公开 `transHisData("CMD_CLEAR_CACHE")` 进入实际处理线程。锁屏障确认 worker 已进入等待后，先入队第二个清理任务并发送队列通知，200ms 有界检查观察没有提前重试；外部 `release` 后，首任务经历五次失败并跳过重试延迟，第二任务的第六次清理成功，线程在五秒内 join。正常路径另测无失败时启动、处理及关闭。测试进程有外部硬超时；故障为受控异常，不证明真实文件系统故障。

局部结果（日志在 `evidence-20261102/`）：

- 恢复旧等待行为的 baseline 在外部关闭后等待超时，按预期 exit 5；不是通过的生产测试。
- 新等待的正常构建、局部 ASan/UBSan、局部 TSan 各有关闭唤醒和正常关闭 2/2 通过；关闭测试同时检查队列通知不触发提前重试及两个已接纳任务被处理。
- **实际 300 秒墙钟重试对照**：最终源码独立进程在 300006ms 后重试并成功，exit 0；日志及退出码保存在 `evidence-20261102/normal-retry.log` 和 `normal-retry.exit`。此前仅日志文案不同的版本也在 300006ms 后通过，其记录单独保留。

复现（输出目录必须不存在，依赖 `/tmp/wt-i03-r41-drain/libs` 已有 WTSTools/WTSUtils 静态库）：

```sh
bash src/TestUnits/fixtures/r23_retry_wait/build.sh /tmp/wt-i03-r23-retry-new
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/r23_retry_wait/run_tests.py /tmp/wt-i03-r23-retry-new
# 单独运行，实际需要至少 300 秒：
/tmp/wt-i03-r23-retry-new/normal-probe normal-retry /tmp/wt-i03-r23-retry-data-new
```

writer 和探针编译了相应 sanitizer；预编译静态库、Boost 等依赖未全部插桩。未验证其他平台、真实磁盘异常、完整工作树及完整回归。`release()` 只有在全部活动 operation 结束后才设置 `_proc_stopping`，阻塞的操作或 sink 仍可能使关闭无界；callback 内自毁、DataManager 提前销毁、真实 provider drain 和其他原条目要求不由此解决。没有运行真实交易或创建符号链接。

## 2026-11-05 最终验收复核（slice2-result）

- 在最终冻结树 F3 上 `build.sh` 失败：插桩脚本的第二个锚点仍是 `std::string path = fmtutil::format("{}rt/min1/", _base_dir);`，而 I03-NEW-039（2026-11-05）已把该清理路径的定长缓冲 `fmtutil::format` 改为 `fmt::format`，`assert` 不成立。只把锚点文本改为当前写法；hook 位置、baseline 还原等待行为的方式均未变。
- 夹具运行器在镜像中把 `git rev-parse`、conda python 与 `/tmp/wt-i03-r41-drain/libs` 换成运行器提供的仓库根、系统 python 与 `--build` 的静态库；300 秒墙钟重试长测仍按上文单独运行，最终验收未跑。
