# K36=A：RT HFT 读端最后一个已发布槽可见（BUG-R100-004）

- 产品：`src/WtDataStorage/WtRdmDtReader.cpp` 委托队列/委托明细/逐笔成交 RT 路径由 `[0, _size-1)` 改为卡13 `wt_rtpub::visibleRecords`（已发布且落在本进程映射内）；三个 tick 路径同样以 visibleRecords 约束（原用 `_size`，不受映射长度约束）。与 `WtDataReader` 统一。改前/改后副本 `before/`、`after/`，哈希 `sources.sha256`。
- 用例：`src/TestUnits/coverage/q03_storage3/s5_main_k36_rdm_rt_visible_test.cpp`（三条已发布记录，范围终点等于最后一条时间时三类集合均返回 3 条）。
- 红控：以改前读端重编 `CoverageTests`（`build-k36-red.log`），`red.log/red.xml` 失败（Which is: 2 vs 3）；恢复改后文件（哈希一致）后全目标重编（`build-k36-green.log`），`green.log/green.xml` 通过。
- 未覆盖：任务清单要求的跨进程 writer/reader 并发发布证明（独立 OS 进程、确定性屏障、回绕/扩容重映射）尚未实施；`RTBlockHeader::_size` 的跨进程发布顺序依赖卡13 发布协议（另行验收）。整体回归 NOT_RUN。
