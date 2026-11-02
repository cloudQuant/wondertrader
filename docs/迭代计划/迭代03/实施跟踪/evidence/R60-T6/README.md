# BUG-R60-002 / BUG-T6-001 本机定向复验

当前 TraderMocker.cpp、r60_tradermocker_async_test.cpp、r113_mocker_partial_cancel_test.cpp 使用实体 Boost bridge 优先单独重编并重链至独立 /tmp 可执行文件；完整命令和返回值见 `commands.json`，输入和输出哈希见 `sha256.json`。其他目标库复用迭代02 v8，非完整当前树重建。

逐进程、隔离目录仅运行如下 4 个指定单例，各 1/1 PASS、无跳过，见 `results.json` 与 `run0..3` 的 XML/日志：

- `R60MockerTest.cancel_unknown_order_before_any_insert_reports_error`
- `R60MockerTest.cancel_open_order_and_unknown_cancel_reports_error`
- `R113MockerPartialCancel.TradedVolumeAccumulatesAcrossPushAndQuerySnapshots`
- `R113MockerPartialCancel.R113Mocker_PartialFillThenCancelPushesCanceled`

构建期间 ModuleHelper.hpp 哈希由 `d9c297a85a4a0d2f222c20b201d578dc705a91abee9b80be2d2df538cb645a74` 变化为 `50f18e5b9efadbebeef1cb9c6ff46f5d2488ee68ad2ef207d843c0297c78688f`；三个本次重编 TU 的编译依赖 `.d` 未引用该头。其他列出的输入在构建期间未变，运行后 `drift.json` 相对构建结束无漂移。其余复用对象和依赖未据此证明与当前工作树一致。

测试为本机模拟数据/UDP，未运行 ASan、修复前动态红控、真实市场、其他平台及整体回归；不能外推生产或整树验收。
