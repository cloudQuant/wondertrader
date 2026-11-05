# BUG-T25-005 查询 gate 与关闭：本机定向诊断

2026-11-02，macOS arm64。本次从 `ccaad010` 导出实体 `src` 快照；`TraderCTP.cpp`、`.h` 和 `t25_ctp_api_stub.hpp` 与当前工作区一致。测试源码在当前工作区增加线程安全的 fake sink 日志读写及等待日志（详见 `test-sync.diff`），复制到隔离快照后仅在该快照末尾追加 `probe.cpp`。没有修改产品；快照、测试、二进制和预编库哈希见 `sha256.txt`。构建脚本 `build.sh`、`build.log` 和 `build-after.log` 使用实体 Boost、fmt、spdlog 和 GoogleTest 路径，重编产品及测试 TU；复用 `/tmp/ctpm-i03-build/build_x64/Debug/libs` 的 WTSTools/WTSUtils 静态库，不证明依赖闭包一致。快照与临时构建目录的符号链接检查 `symlinks.log` 为空。

```sh
mkdir -p /tmp/wt-i03-t25-005-20261102/srcsnap /tmp/wt-i03-t25-005-20261102/obj /tmp/wt-i03-t25-005-20261102/run
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-t25-005-20261102/srcsnap
# 首轮在快照的测试 TU 末尾附加本目录 probe.cpp，再运行 build.sh 与 run.sh。
# 修正 fake sink 后把当前工作区测试 TU 复制到相同快照，再附加 probe.cpp，运行：
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/T25-005-20261102/build.sh'
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/T25-005-20261102/run-after.sh'
bash 'docs/迭代计划/迭代03/实施跟踪/evidence/T25-005-20261102/repeat.sh'
```

所有测试只使用本地 StubApi，不连接真实柜台。每例独立进程，脚本以 8 秒 alarm 硬限时。

- 指定现有 `query_confirm_sync_failure_clears_gate_and_worker_releases` 首次退出 1：日志断言未观察到。源码中 query 失败分支先将 atomic `m_bInQuery=false`，再调用 sink 写入日志；原测试以 gate 变化为条件后读取未加锁 vector，既缺日志完成同步，也存在并发访问可能。此次只修测试 sink：`handleTraderLog` 与 `hasLog` 共用 mutex，等待 gate 及原始日志出现，保留原断言。重编后 1/1 PASS、exit 0，另独立重复 5/5 通过。不能将首次失败单独归因于产品漏日志，也不能把测试变更视为产品关闭修复。
- `i03_no_final_query_response_disconnect_bounded`：fake 查询发出后明确输出 `QUERY_IN_FLIGHT`，不发送终帧，再调用 `disconnect()`；8 秒被 SIGALRM 终止，exit 142，无 XML，证明此本地条件下未有界返回；不启用旧 DISABLED 用例。
- `i03_error_final_disconnect_then_late_error_callback`：fake 错误终帧释放 gate 后断开，限 2 秒内完成，Release 计数为 1；对象与 fake 均仍存活时再手动注入错误终帧，Release 仍为 1，1/1 PASS、exit 0。此人工调用不证明真实 SDK 会否在 Release 后继续回调，也不覆盖对象析构后的迟到回调。

各原始输出与 XML 在本目录和 `after/`，首次失败没有被覆盖；`after/repeat-results.txt` 保存重复退出码。未运行 sanitizer、真实 provider、其他平台或整体回归。

## 生命周期只读分析与下一步

- `disconnect()` 通过 `m_mtxQuery` 将 `release()` 入普通查询队列，再 join worker；worker 每轮先检查 `m_bInQuery`，只有响应末帧或部分同步失败分支清 gate。无终帧时队列中的 release 无法执行，因此本机反例阻塞。`queryAccount` 等普通查询 lambda 没有检查同步返回，provider 即时拒绝时也可能留 gate；不把仅 `queryConfirm` 的修复外推其他请求。
- 当前直接 `release()` 会设停止标志并调用 `m_pUserAPI->Release()`，但本身不 join worker；host 的 WtCore/WtUftCore adapter 有直接调用 release 的路径。简单在 disconnect 时清 gate、异线程直接 Release、或仅修改停止标志，可能与正在执行的 API 调用、SPI 回调和宿主释放发生交错，也不能证明无回调后对象安全回收；worker 或回调内发起关闭时还可能 self-join/重入。
- 候选须将关闭请求与查询/交易 admission 在线性化点收口，停止后不接纳新调用；区分已经完成的请求、等待中的回报与未执行任务，并明确其失败/取消报告。外部 owner 再等待本地 worker 与在途 API 调用退出，不能由 worker 同步 join 自身；重复关闭须幂等。供应商 `Release`/SPI 排空、阻塞上界及能否在请求进行时并发 release 的版本合同目前未证实，`void` 关闭 API 在超时后如何可观察报告和保留对象/模块亦需明确。只要无法确认 quiescence，就不得销毁可能被回调访问的对象或声称关闭有界完成。
- 测试应补硬限时无终帧、正常/错误终帧、同步 query 失败、请求与停止交错、回调内关闭、迟到与重复回调及 repeated release；对真实 provider 需精确 SDK 文件或独立证据。本项保持阻塞。
