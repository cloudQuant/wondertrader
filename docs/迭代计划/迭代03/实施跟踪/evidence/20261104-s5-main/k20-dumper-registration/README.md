# 决策 K20=B（BUG-P10-006）：IDataWriter 生命周期 V1 表追加版本化 add/remove dumper

裁定（2026-11-04）：IDataWriter V1 表追加版本化 add/remove dumper；宿主共享所有权至 DRAINED；重复 ID 拒绝并保留原实例。

## ABI（`src/Includes/IDataWriter.h`，CRLF 保持）
- `WT_DATA_WRITER_LIFECYCLE_V1_ABI_MINOR` 由 0 改为 1。表末尾追加 `add_dumper(writer, id, dumper)`、`remove_dumper(writer, id)`，新增能力位 `CAP_DUMPERS` 与状态 `DUPLICATE (-2)`。
- 新增 `WT_DATA_WRITER_LIFECYCLE_V1_MINOR0_SIZE`。宿主（DataManager）校验改为 `struct_size >= MINOR0_SIZE`，旧 minor-0 写端照常加载并走旧的 `add_ext_dumper`。只有在 `struct_size` 足够且声明 `CAP_DUMPERS` 时，才使用新表项。
- 合同：登记与写入路径同步，写入路径对快照调用 dumper、不持注册锁；从任意线程（含 dumper 回调内）登记都不会死锁；`request_stop` 后登记返回 ERROR；`remove_dumper` 后正在进行的调用可以完成；宿主持有它所加 dumper 至 DRAINED（被隔离的写端则至进程退出）。

## 写端（WtDataStorage `WtDataWriter`、WtDataStorageAD `WtDataWriterAD`）
- 新增 `_dumper_mtx`、`snapshotDumpers()`、`addDumperV1/removeDumperV1`。
- 遍历 dumper 的 11 处（普通写端 7 处，AD 写端 4 处）改为遍历快照。
- 表项只接受本模块 `createWriter` 创建的写端，其他指针返回 INVALID_ARGUMENT。
- 旧内联 `IDataWriter::add_ext_dumper`（不加锁、同 ID 覆盖）保留，只供无 minor-1 表的宿主使用。

## 宿主
- `DataManager::add_ext_dumper` 改为返回 bool：写端提供 minor-1 表时经表登记，DUPLICATE 记错误并保留原实例；否则走旧路径。
- `WtDtRunner::createExtDumper`：
  - 同 ID 拒绝并保留原 dumper（改前覆盖并析构原 dumper，而写端仍可能调用它）；
  - 写端尚未就绪时先保留，`initDataMgr` 后统一登记（改前这种登记会静默丢失）；
  - 持有 `ExpDumperPtr` 至写端 DRAINED，关停未完成则保留至进程退出（I03-NEW-047 既有）。
- 两个 fixture 写端（`r18_writer_fixture.cpp`、`i03_dt_writer_lifecycle_20261104s5/fake_writer.cpp`）的表初始化补 `NULL, NULL`，模拟不提供 minor 1 的写端。

## 用例（`p05_datastorage2/s5_main_k20_dumper_registration_test.cpp`，AD 写端，4 例）
- 导出表：minor 1、`CAP_DUMPERS`、表项非空；经 `createWriter` 创建的写端走表登记得 OK，同 ID 返回 DUPLICATE；remove 两次分别得 OK 与 INVALID；外来指针返回 INVALID；`request_stop` 后登记返回 ERROR。
- 写端成员：同 ID 拒绝、原 dumper 继续收数据、remove 后不再被调用、空 ID 或空指针被拒。
- dumper 回调内登记新 dumper 不死锁：本次写入用旧快照，下一次写入起两个都收到。
- `requestStop` 后登记返回 ERROR。
- 红控：新增接口无改前对应 API，用例无法在改前源码编译，红控不适用。改前缺陷（注册与遍历不同步、同 ID 覆盖）为静态确认。
- 逐例回归：CoverageTests、adstorage、dtcore、dtservo、i03_dtporter_start、m_QuoteFactory 共 1986 例，pass 1984、skipped 2（`regression-summary.json`）。首轮回归中 3 例 dtporter 用例因“写端未就绪时 createExtDumper 返回 false”失败，据此改为“先保留、写端就绪后登记”。同轮 `ws05_CfgLoaderFile.LoadJsonFile` 因固定计数命名临时目录在并行时撞名失败，补进程号（K48 遗漏）。

## 未覆盖
- TSan 重复运行。
- 普通写端（WtDataStorage）的专门用例：仅编译与逐例回归覆盖。
- Windows、外部第三方写端。
