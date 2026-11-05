# WtUftCore ParserAdapter 生命周期夹具（BUG-R78-001，卡 9）

运行：`run.sh <coverage-build> <fixture-build> <before-dir|""> [thread]`。脚本逐用例独立进程运行 `uft_parser_cur`；只有给出 before-dir 时才构建基线红控 `uft_parser_base`。

## 最终验收（2026-11-05）

决策 K18=否（2026-11-04，卡9a）：UFT `initExt` 即使带 remover，也不把“remover 把引用置空”视为回调已停止的证明。这类外部 API 与无 remover 的一样，隔离到进程退出，回调准入保持关闭，remover 永不调用（`src/WtUftCore/ParserAdapter.cpp` 中的 `_legacy_quarantined`）。

2026-11-04 s5 版本的两例仍按旧语义断言：
- `ReleaseInsideCallbackIsDeferred` 期望 remover 调用 1 次、`hasApi()=false`；
- `ConcurrentCallbacksNeverOverlapTeardown` 循环等待 `hasApi()` 变为 false，因而超时。

两例已改为当前合同：
- 回调内的 release 只请求停止（`isTeardownPending()`）；
- 安全点的 release 进入常驻隔离：`isResidentQuarantined()`、`hasApi()`、remover 0 次、外部对象未删除；
- 隔离后回调不再被接纳；并发下拆除不与已接纳的回调重叠。

测试替身在其 owner 与 provider 线程结束后由测试自行回收。
