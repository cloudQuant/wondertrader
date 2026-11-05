# R20_wtbtcore#1 / I03-NEW-082：vendored zstd 1.4.4 的空指针算术 UB 修复；无 notifier 的 Runner 回放 ASan+UBSan 验收

主线程，2026-11-05。HEAD 见 `HEAD.txt`，改前在 `before/`，改后在 `after/`，哈希见 `sources.sha256`。

## 修复（`src/WTSUtils/zstdlib/`，与上游后续版本的修法一致，最小改动）
- `zstd_compress.c` `ZSTD_reset_matchState`：清零 window 后，先把 `base`/`dictBase` 设为非空的空串，再计算 `nextSrc = base + 1`。原代码对 NULL 加 1（UBSan：applying non-zero offset 1 to null pointer）。上游 `ZSTD_window_init()` 采用同样做法；下标仍从 1 开始，压缩结果不变。
- `zstd_fast.c`、`zstd_double_fast.c`、`zstd_lazy.c`：`dictAndPrefixLength` 改写为 `(ip - prefix) + (dictEnd - dictStart)`。没有字典时，原式会对 NULL 做整数偏移（UBSan：applying zero offset to null pointer），上游同样加了括号。
- 未升级 zstd 版本，未改格式。

## 验证
- **库级**（`harness/`、`logs/`，独立进程，UBSan 遇错即停）：
  - 88 字节、级别 1–3：改前在 `zstd_compress.c:1326` 中止，exit 134（`run-before.log`）；改后 0（`run-after.log`）。
  - 扩展 harness2：级别 1–19 × 输入 0/1/88/4096/200000 字节，单次调用与复用 context 往返。改前 UBSan 中止，改后通过。
  - **输出等价**：普通编译下改前、改后全部压缩帧的摘要相同（91b4c34601778540），压缩输出逐字节不变（`run2-*-plain.log`）。
- **Runner 级**（`sanitizer/`）：Homebrew LLVM 22 构建 ASan+UBSan 临时树（`halt_on_error=1`）。
  - `N04ExecMocker.SuccessfulRunnerReplayCallsPluginAndClosesSafely`（未配置 notifier 的真实 Runner 回放）：改前 zstd 在 1326 处中止，exit 134（`red-runner.log`）；改后通过（`green-runner.log`）。
  - `R20EventNotifier.*` 6/6 通过。
- **sanitizer 全二进制逐例**：btcore+btporter 共 836 例（`btcore-btporter-asan-ubsan-summary.json`）。首轮 5 例非通过，归因如下，处理后单独复跑 5/5 通过、无 sanitizer 报告：
  - 3 例 R111ExecMocker：临时树未构建 exec_v2 假插件夹具，属环境问题，补建后通过；
  - 2 例测试代码 UB，另立 I03-NEW-102 并已修。
- **常规树**（默认 -Werror）：
  - zstd 使用方 CoverageTests/adstorage/btcore/btporter/dtcore/dtservo/m_WtBtRunner/m_QuoteFactory 共 2842 例，2835 通过、7 跳过（`regression-zstd-consumers.json`）；
  - 测试修改后 btcore/btporter/porter 共 1098 例，1090 通过、8 跳过（`regression-final-btcore-btporter-porter.json`）。

## 工具链说明（不属本仓缺陷）
- Apple clang 17 的 ASan 运行时在本机（Darwin 25）初始化阶段自旋死锁（`AsanInitInternal` → dyld 迭代 → malloc → 重入初始化），发生在任何本仓代码运行之前（`sanitizer/appleclang-asan-init-hang-sample.txt`），因此改用 Homebrew LLVM 22。
- 该临时树的配置仅用于本地证据，不提交：
  - Homebrew clang 从 PATH 取到 anaconda 的 `ld`，用 `--ld-path=/usr/bin/ld` 指定系统链接器；
  - LLVM 22 的 libc++ 不再传递包含 `<cstdlib>`，第三方 fmt 依赖这一点，故加 `-include cstdlib`；
  - `-DWT_WARNINGS_AS_ERRORS=OFF`（clang 22 的新告警）。
- 常规树仍按默认 -Werror 构建通过。

## 未覆盖
- active MQ 路径属独立路径。
- 预编依赖（Boost、系统库）未插桩。
- 其他平台未验。
