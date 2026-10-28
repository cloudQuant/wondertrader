# L04 交付清单 — WtDataStorage / WtDataStorageAD

- 车道: L04
- 模块: `src/WtDataStorage/`、`src/WtDataStorageAD/`
- 状态: 已交付（语法门禁全绿）
- 生产代码改动: 无（`src/**` 仅新增 `src/TestUnits/coverage/l04_datastorage/`，未动 `CMakeLists.txt`，`coverage/CMakeLists.txt` 用 GLOB_RECURSE 自动纳入新目录）

## 新增测试文件（4 个，共 1468 行）

| 文件 | 行数 | 主题 |
|---|---|---|
| `src/TestUnits/coverage/l04_datastorage/l04_lmdb_key_layout_test.cpp` | 270 | `WtDataStorageAD/LMDBKeys.h` 键编码 |
| `src/TestUnits/coverage/l04_datastorage/l04_block_header_layout_test.cpp` | 354 | `WtDataStorage/DataDefine.h` 块头/柔性数组 + 落盘 round-trip |
| `src/TestUnits/coverage/l04_datastorage/l04_proc_block_data_test.cpp` | 429 | `proc_block_data` + `WtBtDtReader` 文件驱动链路 |
| `src/TestUnits/coverage/l04_datastorage/l04_ad_lmdb_store_test.cpp` | 415 | `DataDefineAD.h` 缓存块布局 + LMDB 区间读写语义 |

共 25 个 TEST/TEST_F、约 150 条断言。所有落盘写入 `std::filesystem::temp_directory_path()/l04_{blk,btdt,ad}_<pid>_<seq>`，`TearDown` 用 `remove_all` 清理，不读 `dist/` 真实数据，不依赖上次运行残留（序号 + pid 保证唯一）。

## 覆盖的类 / 函数 / 符号

WtDataStorage
- `DataDefine.h`：`BlockHeader::is_old_version`/`is_compressed`（4 个版本 + 0x00/0x05/0xFFFF 未知版本）、`BlockHeaderV2` 同上、`RTBlockHeader`/`RTDayBlockHeader`/`RTKlineBlock`/`RTTickBlock`/`RTTickCache`/`TickCacheItem`/`HisKlineBlockV2`/`HisKlineBlockOld` 的 packed 尺寸与柔性数组偏移、`BLK_FLAG`/`FLAG_SIZE`、`BlockType` 枚举取值
- `WtDataReader.cpp:50` 自由函数 `proc_block_data`（外部链接，来自 libWtDataStorage）：RAW_V2 剥头/留头、`BLOCK_VERSION_RAW` 老 bar → 新 bar、老 tick → 新 tick、`BLOCK_VERSION_CMP_V2` 解压、`BLOCK_VERSION_CMP` 解压 + 结构体升级、压缩长度不一致的失败分支（内容不被改写）
- `WtBtDtReader`：`init(NULL/ cfg)`、`read_raw_bars`、`read_raw_ticks`、`read_raw_order_details`、`read_raw_order_queues`、`read_raw_transactions`，含文件不存在、损坏块失败、`his/<PERIOD_NAME>/`、`his/ticks|orders|queue|trans/<exchg>/<date>/` 的真实路径拼装
- `Includes/WTSStruct.h` 顺带：`WTSBarStruct::operator=(const WTSBarStructOld&)` 字段映射

WtDataStorageAD
- `LMDBKeys.h`：`reverseEndian(uint16_t)`/`reverseEndian(uint32_t)`（已知向量、对合性、内存大端等价）、`LMDBHftKey`/`LMDBBarKey` packed 布局与字段偏移、填充字节清零、`etime/1000000000`+`%1000000000` 拆分回环、**字典序 == 数值序**（LMDB range scan 的正确性前提）、`(0, 0xffffffff)` 哨兵区间、多交易所/多合约前缀隔离、`MAX_EXCHANGE_LENGTH-1`/`MAX_INSTRUMENT_LENGTH-1` 边界
- `DataDefineAD.h`：`BlockHeader`/`RTBlockHeader`/`RTTickCache`/`TickCacheItem`/`RTBarCache`/`BarCacheItem` 尺寸与 `_items[]` 偏移、`BT_RT_Cache`/`BLOCK_VERSION_RAW`、bar/tick 缓存块写盘读回校验
- `WtDataReaderAD.cpp` 的取数语义（等价复刻，源码未改动）：`read_bars_to_buffer` 的 `get_range` 全量、`cacheBarsFromStorage` 的 `get_lowers(count)` + 左边界过滤、`update_cache_from_lmdb` 的 `get_uppers` + `curBarTime == lastBarTime` 覆盖式去重、`get_t_db` 的按日 HFT 键区间 + 精确 `get`
- `Share/TimeUtils.hpp`：`timeToMinBar`/`minBarToDate`/`minBarToTime` 与 AD 侧 `(uDate-19900000)*10000+uTime` 的一致性
- `WTSUtils/WtLMDB.hpp`：`WtLMDB::open`（写/只读两种）、`WtLMDBQuery` 事务 `put`/`commit`、`get_range`/`get_lowers`/`get_uppers`/`get`（临时目录下真实 LMDB 环境）

## 跳过目标及原因

- `WtDataWriter`（2701 行）/`WtDataWriterAD`（1073 行）主体：`init` 需要 `IBaseDataMgr`/`IHotMgr`/`IDataWriterSink` 全套宿主，`writeTick` 走后台 `proc_loop` 线程 + `BoostMappingFile` 共享内存 RT 块，`check_loop` 内含 `sleep_for(10s)`/`sleep_for(300s)` 清理循环 —— 单测内起线程/共享内存段会带来不确定与残留，跳过。
- `WtDataReader::init`/`onMinuteEnd`、`WtRdmDtReader`（2654 行）：依赖 CTP Loader / 真实历史目录 / 复权因子外部数据源。
- `WtBtDtReaderAD`、`WtRdmDtReaderAD` 的 `get_k_db`/`get_t_db` 与 `WtDataWriterAD::pipeTo*`：需先有 Writer 落库（依赖上一项的共享内存块），改为直接用 `LMDBKeys + WtLMDB/WtLMDBQuery` 复刻其区间与去重语义（见 `l04_ad_lmdb_store_test.cpp`）。
- LMDB / 共享内存的多进程语义（`update_slave`、`WtShareHelper` 主从）：按要求不真实起进程，跳过。

## 疑似缺陷（不修源码）

`docs/迭代计划/迭代01/bugs/l04_datastorage.md`，6 条，全部状态「待审核」：
- BUG-L04-001（Blocker）`WtDataReaderAD::read_bars_to_buffer` 从 `vector<std::string>` 对象数组 memcpy + 上界误传 `lKey`
- BUG-L04-002（Major）`proc_block_data` 未校验最小长度，截断文件越界读 / `size()` 下溢
- BUG-L04-003（Major）`WtBtDtReader` 五个 `read_raw_*` 未校验文件长度
- BUG-L04-004（Major）`LMDBKeys.h` 裸 `strcpy` 未限长
- BUG-L04-005（Minor）`update_cache_from_lmdb` value 长度未校验 + 空 vector `back()`
- BUG-L04-006（Minor）`WtLMDB::open` 在 `mdb_env_open` 之后才 `set_mapsize`；老→新 tick `strncpy` 越界读

越界/UB 路径（BUG-L04-002/003/004/006 的触发用例）**故意未写成测试**，避免 ASAN 覆盖构建下整轮用例被中止；已在 bug 文档中说明复现步骤。

## 语法门禁

命令（仓库根执行，对 4 个文件逐个运行）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/l04_datastorage/<file>.cpp
```

结果：**4/4 通过，0 error 0 warning**（最后一次全量复跑无任何输出）。过程中修掉 3 处自身问题：`KP_MIN` → `KP_Minute1`、`BarCacheItem::open` → `_bar.open`、lambda 未用参数 `ayKeys` 注释掉。未运行 cmake/ninja。

## 剩余（可选后续）

预算内未做的 2 个候选文件：
1. `WtDataWriter` 的 `resizeRTBlock`/`getKlineBlock` 系列：需先造一个可注入的 `BoostMappingFile` 替身，属结构性改造（要动生产代码，本迭代禁止）。
2. `WtRdmDtReader::indexBarFromCacheByRange/ByCount`（约 200 行纯二分索引）：可在下一迭代以「自建 cache + 复刻 key」方式补测，预计 1 个 300 行文件。
