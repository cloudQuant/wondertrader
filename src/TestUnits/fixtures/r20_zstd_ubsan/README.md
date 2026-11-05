# R20：vendored Zstandard 空指针算术 UB 回归检查

## 当前用途（2026-11-05 最终验收更新）

2026-11-05 主线程已按上游修法最小修复 vendored zstd 1.4.4（`zstd_compress.c` 的 `ZSTD_reset_matchState` 先给 window 设非空基址；`zstd_fast.c`/`zstd_double_fast.c`/`zstd_lazy.c` 的 `dictAndPrefixLength` 加括号），见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261105-s5-main-r20-zstd/README.md`（R20_wtbtcore#1 / I03-NEW-082）。原探针期望“UBSan 在 zstd_compress.c:1326:46 中止”的已知红前提已不成立，本目录改为该修复的回归检查：

```sh
bash src/TestUnits/fixtures/r20_zstd_ubsan/run.sh <新输出目录>
```

须从仓库外的源码副本运行。脚本只用本源码树的 `src/WTSUtils/zstdlib/*.c`、`WTSCmpHelper.hpp` 与 `probe.cpp`，分别以 Homebrew LLVM 22 ASan+UBSan（`-fno-sanitize-recover=undefined`，遇报即停）和普通方式编译，不依赖任何预编库。三个模式都必须普通与 sanitizer 两种进程均 exit 0、sanitizer 日志无 `runtime error`/`AddressSanitizer` 报告且往返通过：

- `direct`：88 字节 `WTSBarStruct`、级别 1 直接 `ZSTD_compress`/`ZSTD_decompress`（原红控场景）；
- `wrapper`：同一数据经 `WTSCmpHelper::compress_data`/`uncompress_data`；
- `sweep`：级别 1–19（fast/double_fast/lazy/optimal 各策略）× 输入 0/1/88/4096/200000 字节，单次调用与复用 `ZSTD_CCtx` 各一次，逐字节往返。

红控：仓库内不保存改前 zstd 源码；改前副本在上述 docs 证据 `before/`。2026-11-05 用这四个改前文件覆盖到临时副本后运行本脚本，`direct`/`wrapper`/`sweep` 的 sanitizer 进程均被 UBSan 中止（记录见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-acceptance/fixtures-fixtures-agent/README.md`），因此本检查能区分修复前后。脚本本身不运行红控。

未覆盖：其他平台、Apple clang sanitizer（本机初始化死锁）、依赖库插桩、真实 Runner 回放（由 R20 证据与 coverage 回归覆盖）。

## 原记录（2026-11-02，已过期）


2026-11-02，macOS arm64。此探针不修改生产代码或第三方库，不连接真实行情或交易，不创建符号链接。它复用 `docs/迭代计划/迭代03/实施跟踪/evidence/R20-1-20261102/README.md` 中检查点 `ccaad010` 的两个本机隔离构建：一个用 LLVM 22 ASan/UBSan 编译，另一个普通编译。此处仅核对包装头与报错源文件同快照的 SHA256 相同，不能由此宣称当前完整工作树已统一构建。

原无 notifier 的 Runner 测试从 CSV 创建一根 bar，栈经过 `HisDataReplayer::cacheRawBarsFromCSV`、`WTSCmpHelper::compress_data` 和 `ZSTD_compress`。静态源码在非空 bars 检查后以 `bars.data()` 和 `sizeof(WTSBarStruct) * count` 调用包装；fixture 提供一行 CSV。结合本机结构体大小，可推断该场景预期压缩 88 字节，但没有在原 Runner 的调用点实测记录指针与长度，不能将静态推断冒充实测。

本探针另行构建一个有效的非空 `WTSBarStruct`，源长度 **88**、输出缓冲 **151**（`ZSTD_compressBound`）、级别 **1**，对同一数据分别直接调用 vendored `ZSTD_compress` 和通过 `WTSCmpHelper` 调用，正常编译两种方式均成功压缩并完整解压比较。ASan/UBSan 两种方式均以退出 **134** 中止，均在 `zstd_compress.c:1326:46` 报对空指针应用非零偏移。`results.txt` 中 134 是预期失败观察，不是通过的 sanitizer 验证。

运行库报告 `ZSTD_versionNumber() == 10404`；同版本头声明为 **1.4.4**。报错源码 `ZSTD_reset_matchState` 先将 `ms->window` 全部清零，再执行 `ms->window.nextSrc = ms->window.base + 1`，属于库内针对零化指针的算术；本探针直接调用绕过本仓包装，且源缓冲有效，说明原本的假设“非空输入由包装传成了空指针”不能解释此 UBSan 位置。这不证明其他压缩输入或包装错误处理均无缺陷，也不证明原 Runner 除此之外无其他问题。

复现（输出目录必须不存在）：

```sh
bash src/TestUnits/fixtures/r20_zstd_ubsan/run.sh /tmp/wt-i03-zstd-ubsan-new
```

脚本编译两个独立进程探针，复用 `/tmp/wt-i03-r20-20261102/build*/build_x64/Debug/libs/libWTSUtils.a`，分别记录正常和 sanitizer 日志；探针用到旧构建的 zstd，其余系统库未必插桩，`ASAN_OPTIONS=detect_leaks=0`。先前构建的其他文件可能被系统清理；如缺失须按原 R20 证据重建。`evidence-20261102/` 保留本次结果、栈及输入库哈希；日志中路径指向当时的实体快照。原 Runner 的 sanitizer 回放仍未完成，R20 不应关闭。

安全处理需要单独评估 vendored zstd 的升级或维护方案及其文件格式和平台兼容性，再对真实 Runner 的当前树重新编译运行。此任务未更改或屏蔽依赖的 UB，也未以普通通过抵消 sanitizer 失败；没有验证跨平台、外部 provider、完整树或完整回归。
