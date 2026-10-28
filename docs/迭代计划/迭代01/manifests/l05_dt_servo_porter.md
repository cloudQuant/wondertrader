# Lane L05 交付清单 — WtDtCore / WtDtHelper / WtDtServo / WtDtPorter

- 工作流: L05（测试覆盖率迭代01）
- 范围: `src/WtDtCore/`、`src/WtDtHelper/`、`src/WtDtServo/`、`src/WtDtPorter/`
- 生产代码改动: 无（四个模块及其 CMakeLists 全部只读）

## 1. 新增测试文件（`src/TestUnits/coverage/l05_dt_servo_porter/`）

| 文件 | 行数级别 | 说明 |
| --- | --- | --- |
| `l05_dt_helper_bar_test.cpp` | ~330 | WtDtHelper 的 K 线存储/读取/CSV 导出与 CSV→二进制 |
| `l05_dt_helper_tick_test.cpp` | ~330 | WtDtHelper 的 Tick/委托明细/委托队列/逐笔成交 + dump_ticks |
| `l05_dt_core_util_test.cpp` | ~190 | WtDtCore 无 socket 依赖的工具（WtHelper/StatHelper/IDataCaster） |
| `l05_dt_servo_capi_test.cpp` | ~150 | WtDtServo 导出 C API 的「未初始化」错误返回路径 |
| `l05_dt_porter_capi_test.cpp` | ~170 | WtDtPorter 导出 C API + ExpDumper→WtDtRunner 委托路径 |

全部落盘写到 `std::filesystem::temp_directory_path()/l05_helper_bar_*` / `l05_helper_tick_*`（带 pid+序号，TearDown `remove_all`），不起端口、不 fork 进程、不加载插件 DLL。

## 2. 覆盖的类 / 函数

### WtDtHelper（`src/WtDtHelper/WtDtHelper.cpp`，主要产出）
- `store_bars` / `store_ticks` / `store_order_details` / `store_order_queues` / `store_transactions`：count=0、非法 period、正常写入 + 建目录失败路径
- `read_dsb_bars` / `read_dsb_ticks` / `read_dsb_order_details` / `read_dsb_order_queues` / `read_dsb_transactions`：压缩块写入→读取往返（覆盖 `proc_block_data` 的 `BLOCK_VERSION_CMP_V2` 解压分支）、头部校验失败、文件缺失、`cbLogger` 为 NULL
- `read_dmb_bars` / `read_dmb_ticks`：`RTKlineBlock` / `RTTickBlock` 的 `_size==0`、非法头部、正常两块
- `dump_bars` / `dump_ticks`：目录不存在、非 `.dsb` 跳过、子目录跳过、块类型不匹配跳过、头部过短、CSV 表头与数据行内容断言
- `trans_csv_bars`：目录不存在、`d`/`m1`/`m5`/未知 period、`strToDate`（`2020/01/16`、`20200117`、`2020-03-02 00:00:00`）、`strToTime`（`09:30:00`→`930`）、非 csv 与子目录跳过、`empty.csv` 读取失败分支
- 内部函数 `proc_block_data` / `strToTime` / `strToDate` 均由上述导出函数间接覆盖（未直接引用非导出符号，避免 dylib 链接风险）

### WtDtCore
- `WtHelper::get_cwd`（首次 + 缓存分支）、`WtHelper::set_module_dir` / `get_module_dir`
- `StatHelper::one` / `updateStatInfo`（含 `UINT64_MAX` 溢出分支、零增量分支）/ `getStatInfo` / `StatInfo` 构造
- `IDataCaster` 的 4 个 `broadcast` 重载（覆写 + 默认空实现两条路径）

### WtDtServo
- `WtDtServo.cpp`：`get_version`、`get_bars_by_range`、`get_bars_by_date`、`get_bars_by_count`、`get_sbars_by_date`、`get_ticks_by_range`、`get_ticks_by_count`、`get_ticks_by_date`、`clear_cache`（未初始化 → 返回 0、不触发回调）
- `WtDtRunner.cpp`：上述函数体内的 `_is_inited` 守卫分支 + `get_bars_by_date` 非分钟周期报错分支 + period 解析（m1/m5/d）
- `WtDataManager::clear_cache` 的「DataReader 未初始化，clearing canceled」分支

### WtDtPorter
- `WtDtPorter.cpp`：`write_log`（分类/无分类）、`parser_push_quote`（适配器不存在）、`create_ext_dumper`、`register_extended_dumper`、`register_extended_hftdata_dumper`、`register_parser_callbacks`
- `WtDtRunner.cpp`：`on_ext_parser_quote` 未找到适配器分支、`dumpHisBars/dumpHisTicks/dumpHisOrdQue/dumpHisOrdDtl/dumpHisTrans` 的「未注册回调 → false」与「已注册 → 委托回调 → true」两条分支、`registerExtDumper`/`registerExtHftDataDumper` setter
- `ExpDumper.h`：5 个 `dumpHis*` 委托 + 构造/析构

## 3. 因需要外部服务 / 真实对端而跳过的接口（排除清单候选）

WtDtCore
- `UDPCaster`（`UDPCaster.cpp/.h` 全部：`init/start/stop/addBRecver/addMRecver/do_receive/do_broadcast/handle_send_broad/handle_send_multi` 4 个 broadcast 重载）— 需要 boost::asio UDP 广播/组播套接字与收发端
- `ShmCaster`（`ShmCaster.cpp/.h`：`init/broadcast×4/_DataQueue`）— 依赖 boost::interprocess 共享内存段与命名队列
- `DataManager`（`init/release/writeTick/writeOrderQueue/writeOrderDetail/writeTransaction/transHisData/isSessionProceeded/getCurTick/getSessionComms/getTradingDate/canSessionReceive/broadcast*/outputLog`）— 需要 dlopen 数据写入插件（createWriter/deleteWriter）+ 存储后端
- `IndexWorkerMgr`（`loadFactories/createWorker/init/handle_quote`）— 需要加载指标插件 DLL
- `StateMonitor`（`initialize/run/stop`）— 需要 sessioncfg/statecfg JSON + 后台线程与定时器
- `ParserAdapter`（WtDtCore 版全部：`init/initExt/run/release/handleQuote/handleSymbolList/handleOrderQueue/handleOrderDetail/handleTransaction/handleParserLog/getBaseDataMgr` 与 `ParserAdapterMgr::*`）— 依赖具体 parser 插件与 DataManager；且 WtDtCore/WtDtServo 各有一份同名 `ParserAdapter/ParserAdapterMgr`，同二进制内链接有歧义风险

WtDtServo
- `initialize`（需要 wtdt 存储配置 + `WtDataStorage` 后端 + parsers 配置）、`start`
- `WtDataManager::init/initStore/get_kline_slice_* / get_tick_slice_* / get_order_*_slice / update_bars / subscribe_bar / get_exright_factor`（需要真实历史存储目录与合约表）
- `subscribe_tick` / `subscribe_bar`（`sub_tick/sub_bar`，未初始化时行为未文档化，需 runner 全量初始化）
- `WtDtRunner::proc_tick / trigger_tick / trigger_bar / initParsers / parser_*`（需要解析器对端）
- `WtDtServo::ParserAdapter` 与 `ParserAdapterMgr`（同上歧义风险）
- `WtDtServo/WtHelper.{h,cpp}` 的 `WtHelper` 类与 `WtDtCore/WtHelper.h` 同名（ODR/符号冲突风险），本 lane 只测了 WtDtCore 版本

WtDtPorter
- `initialize` / `start`（需要 datakit.json + 日志配置 + parser/dumper 插件；与 WtDtServo 同名 C 符号，测试中未调用）
- `create_ext_parser`（内部 `new ExpParser` + `ParserAdapter::initExt`，会驱动插件回调链，未初始化 runner 下有解引用风险）
- `ExpParser::init/connect/disconnect/release/subscribe/unsubscribe/register_spi`（依赖 `IParserSpi` 注册与 runner 状态）
- `ExpDumper.cpp` 的 `IHisDataDumper` 工厂导出（`createDumper/deleteDumper`，由 dlopen 侧调用）

## 4. 语法 + 告警门禁

命令（仓库根执行，逐文件）：`clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY -include src/Share/fmt_enum_compat.h -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include <file>`

结果：**5/5 文件 0 error 0 warning**（`GATE_FAIL=0`）。
唯一需要的测试侧抑制：`l05_dt_core_util_test.cpp` 包含 `WtDtCore/StatHelper.hpp` 时用 `#pragma clang diagnostic ignored "-Wunused-but-set-variable"` 局部屏蔽生产代码告警（见 BUG-L05-004），并自行补齐 `BoostRWMutex/BoostWriteLock/BoostReadLock` 别名（生产代码一行未改）。

## 5. 待审核缺陷

`docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md`：BUG-L05-001 ~ BUG-L05-006（1 Blocker / 3 Major / 2 Minor）。均只记录、未修源码；相关测试按现状行为写断言（如 `StoreBarsSwallowsFileCreationFailure`），未放宽断言。

## 6. 剩余工作（预算超支说明）

工具调用已超出 30 次预算，以下项未做，留给主控或后续补：
- `WtDtHelper.cpp` 的 `resample_bars`（需 `WTSSessionInfo::create` + 会话 JSON 模板，纯内存可行，约 200 行；本可再产出一个 `l05_dt_helper_resample_test.cpp`）
- `WtDtCore/ParserAdapter.cpp::handleQuote/handleParserLog` 的适配器桩测（需先解决与 WtDtServo 同名符号的链接策略）
- Servo/Porter 已初始化态（真实存储目录 + 合约表）的正向用例，依赖 fixture 数据，属于集成覆盖范畴
