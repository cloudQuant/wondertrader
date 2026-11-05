# P10-007：六类实时块首次插入与 checker 的锁交错

2026-11-02，macOS arm64。这里是局部并发证据，**不是整体验收**。保留产品源码及既有测试不变，`run.sh` 将当前 `src` 实体复制至新目录，先校验关键文件复制前后哈希，再仅在副本 writer 的 5 个 `emplace` 之后及各 snapshot 拷贝后插入测试屏障。分钟线的同一插入点按 period 区分 min1/min5。副本原有 `WT_DATA_WRITER_TEST_HOOKS` 开启 real checker 首轮唤醒与 map `try_lock` 竞争观察；正常产品构建不含本夹具额外屏障。

六个测试分别从真实 `writeTick`（tick、min1、min5）、`writeTransaction`、`writeOrderDetail`、`writeOrderQueue` 入队。测试挂住真实 task worker 首次持 map 锁插入，让真实 `check_loop()` 的 snapshot `try_lock` 失败后放行，并在对应快照持有 `shared_ptr` 时观察所有权；校验首插和 sink 回调均发生在同一 task worker，checker 是另一线程，release/join 后 weak 引用到期，并对落盘的首条数据读取和比对。六项使用独立进程和临时目录，不涉及真实交易或共享内存。控制线程等待信号有期限，工作线程的门闩等待本身没有硬超时；整个进程由 runner 限制 20 秒，因此锁等待卡住时也不会无限等待。

`evidence-20261102/results.txt` 及 12 份独立日志记录普通 **6/6**、TSan **6/6**，每项退出 0。writer 和测试 TU 用 LLVM 22 插桩，复用预编译的 WTSTools/WTSUtils、Boost、GTest、iconv 等未插桩依赖；这不证明全部依赖内部无竞争。无共享库整树构建、Linux/Windows、真实供应商或插件验证。该六项验证首次插入与 snapshot 交错，并检查释放后文件和 pair 生命周期；没有在六个场景分别驱动过期关闭，不能替代之前过期交错测试或完整生命周期验收。P10-008 的外部回调重入、P10-006 dumper 所有权及 R23 停机/释放等问题不由此解决。

## 复现

输出目录必须不存在，运行环境需本机 LLVM 22、Homebrew 实体 Boost/fmt/spdlog/GTest，以及 `/tmp/wt-i03-r41-drain/libs` 中现有预编 `libWTSTools.a`、`libWTSUtils.a`：

```sh
bash src/TestUnits/fixtures/p10_first_insert/run.sh /tmp/wt-i03-p10-firstinsert-new
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/p10_first_insert/run_tests.py /tmp/wt-i03-p10-firstinsert-new
```

脚本未创建符号链接，保存 `source-before.sha256`、`source-after.sha256`、每项日志和 `results.txt`。相关源在复制前后哈希一致，完整源码及其余依赖未作统一构建。此前构建中有 bash 空数组与 tick 临时值编译错误，已修正并保留于 `/tmp/wt-i03-p10-firstinsert-build.log` 和 `/tmp/wt-i03-p10-firstinsert-build-r2.log`，不作为通过记录。
