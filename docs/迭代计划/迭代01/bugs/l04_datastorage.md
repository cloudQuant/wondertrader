# L04（WtDataStorage / WtDataStorageAD）缺陷记录

> 本迭代只记录缺陷，不修改生产代码。状态一律「待审核」。

## BUG-L04-001

- 状态: 待审核
- 严重级别: Blocker
- 模块/文件: `src/WtDataStorageAD/WtDataReaderAD.cpp:220-227`（`WtDataReaderAD::read_bars_to_buffer`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_ad_lmdb_store_test.cpp`（`BarsCommittedAndReadableBack` 用逐条 `item.data()` 的正确拷贝方式验证期望语义）
- 输入/前置条件: 任意含 K 线记录的 LMDB 库（`*_kline_*.db`），调用 `read_bars_to_buffer(exchg, code, period)`
- 期望行为: 把回调收到的每条 value（一个 `WTSBarStruct`）依次拷进 `buffer`，得到 `N * sizeof(WTSBarStruct)` 字节的连续 bar 数组
- 实际行为: `memcpy((void*)buffer.data(), ayVals.data(), sizeof(WTSBarStruct)*ayVals.size())` 的源是 `std::vector<std::string>` 的对象数组首地址（`ayVals.data()`），即把 `std::string` 对象头（24 字节/项，含堆指针）当作 bar 数据拷贝
- 复现步骤: 写入 20 根 bar → 触发该函数 → 读到的 "bar" 字段全部是 string 对象头；且当 `N*88 > N*24`（N≥1 即成立）时越界读 vector 内部缓冲区之后的内存
- 影响范围: 该函数的调用方（回测/导出链路）拿到完全错误的数据；同时是 ASAN 构建下的必然 heap-buffer-overflow
- 备注: 同一处 `query.get_range(lKey, lKey, cb)` 把上界也传成了 `lKey`（`rKey` 声明后未使用），即使修好 memcpy 也只能取到 1 条记录。建议改为 `for (auto& item : ayVals) memcpy(...)` 并把上界换成 `rKey`。不阻塞本迭代（测试未依赖该函数输出）。

## BUG-L04-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataReader.cpp:50-52, 89`（自由函数 `proc_block_data`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_proc_block_data_test.cpp`（仅覆盖长度合法的路径；越界路径未写成测试以免整轮用例被 ASAN 打断）
- 输入/前置条件: `content` 长度小于 `sizeof(BlockHeader)`（12 字节），例如被截断 / 0 字节的 `.dsb` 文件
- 期望行为: 校验长度后返回 `false`，不触碰缓冲区之外的内存
- 实际行为: 直接 `BlockHeader* header = (BlockHeader*)content.data()` 读取 `_version`；老版本未压缩分支还会执行 `content.size() - BLOCK_HEADER_SIZE`，长度不足时 `size_t` 下溢为巨大值传给 `buffer.append`
- 复现步骤: 造一个 4 字节的 `.dsb` 文件 → `WtBtDtReader::read_raw_bars` → `proc_block_data` → 越界读 / 巨量 append
- 影响范围: 所有 `read_raw_*` / `WtRdmDtReader` / `WtDataWriter` 的数据文件解析入口，损坏文件可导致崩溃或 OOM
- 备注: 建议入口处 `if (content.size() < BLOCK_HEADER_SIZE) return false;`（压缩分支则校验 `< BLOCK_HEADERV2_SIZE`）。

## BUG-L04-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtBtDtReader.cpp:151, 182, 212, 242, 272`
- 相关测试: 同上（`CorruptedBlockReturnsFalse` 覆盖长度字段不一致的分支）
- 输入/前置条件: `.dsb` 文件存在但为空或长度不足 12 字节
- 期望行为: 读取侧对 `StdFile::read_file_content` 的结果做最小长度校验，直接返回 `false`
- 实际行为: 未校验即调用 `proc_block_data`，把非法长度继续下传（与 BUG-L04-002 叠加）
- 复现步骤: 在 `his/<period>/<exchg>/<code>.dsb` 放一个空文件后调用 `read_raw_bars`
- 影响范围: 回测数据读取全部 5 个 `read_raw_*` 接口
- 备注: 与 BUG-L04-002 同源，建议一并修复。

## BUG-L04-004

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorageAD/LMDBKeys.h:92-93, 134-135`
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_lmdb_key_layout_test.cpp`（`MaxLengthFieldsFitExactly` 验证 15/31 字符边界内安全；越界场景未写测试以免破坏套件）
- 输入/前置条件: `LMDBBarKey(exchg, code, t)` / `LMDBHftKey(...)` 传入 `exchg` 长度 ≥ `MAX_EXCHANGE_LENGTH`(16) 或 `code` 长度 ≥ `MAX_INSTRUMENT_LENGTH`(32)
- 期望行为: 使用 `wt_strcpy`/`snprintf` 截断并返回错误，或静态断言长度
- 实际行为: 裸 `strcpy(_exchg, exchg)`、`strcpy(_code, code)`，越界写入 packed 结构体后续字段（含 `_date`/`_time`/`_bartime`），栈上键对象被破坏
- 复现步骤: 以异常上游数据（如把 `交易所.合约` 整串误传为 exchg）构造键对象
- 影响范围: WtDataStorageAD 全部读写路径（Writer/Reader/RdmReader/BtDtReader 的键构造点）
- 备注: 建议统一改用 `wt_strcpy`，并考虑加 `static_assert` + 显式截断。

## BUG-L04-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtDataStorageAD/WtDataReaderAD.cpp:280-302`（`update_cache_from_lmdb`）
- 相关测试: `l04_ad_lmdb_store_test.cpp`（`GetUppersSkipsBarEqualToLastBarTime` 复刻其取数/去重语义）
- 输入/前置条件: LMDB 中某条 value 长度 < `sizeof(WTSBarStruct)`；或回调一次都没被调用（`barsList._bars` 为空）
- 期望行为: 对 `item.size()` 做校验；日志行不依赖容器非空
- 实际行为: 直接 `*(WTSBarStruct*)item.data()`（越界读）；结尾日志 `barsList._bars.back()` 在空 vector 上是 UB
- 复现步骤: 手工向 k 线库写入一条 8 字节的 value，或对一个空 bar 列表调用该函数
- 影响范围: 分钟线/日线增量更新路径（`on_minute_end`、`readKlineSlice`）
- 备注: 非阻塞。

## BUG-L04-006

- 状态: 待审核
- 严重级别: Minor（归属 WTSUtils，但直接影响本车道 AD 存储）
- 模块/文件: `src/WTSUtils/WtLMDB.hpp:57-84`（`WtLMDB::open`）、`src/Includes/WTSStruct.h:204`（`WTSTickStruct::operator=(const WTSTickStructOld&)`）
- 相关测试: `l04_ad_lmdb_store_test.cpp`（显式传 8MB mapsize 并在 open 后写入 20 条记录）、`l04_proc_block_data_test.cpp`（`OldTicksUpgraded`）
- 期望行为: `mdb_env_set_mapsize` 必须在 `mdb_env_open` 之前调用并检查返回值；老→新 tick 拷贝应保证字符串以 0 结尾且不越过源缓冲区
- 实际行为: open 之后才 set_mapsize 且返回值被丢弃；`strncpy(exchg, tick.exchg, MAX_EXCHANGE_LENGTH)` 从 `char[10]` 源读取 16 字节（源无 NUL 时越界读，且目标可能未结尾）
- 复现步骤: 任意调用 `WtLMDB::open` 传 >1MB mapsize；老版本 tick 块经 `proc_block_data` 升级
- 影响范围: AD 存储的容量配置；历史 tick 升级路径
- 备注: 建议 `set_mapsize` 前置 + `memcpy(dst, src, sizeof(dst)-1)` 式安全拷贝。本车道未修改任何源码。

## 跳过（不测）说明

- `WtDataWriter`/`WtDataWriterAD` 的 `proc_loop`/`check_loop` 线程与 `BoostMappingFile` 共享内存块：依赖 `IHotMgr`/`IBaseDataMgr`/`IDataWriterSink` 全套宿主环境与后台线程（含 `sleep_for(10s/300s)` 的清理循环），单测内起线程会引入不确定性。
- `WtDataReader::init`/`onMinuteEnd`、`WtRdmDtReader` 的行情订阅与复权因子加载：依赖 CTP Loader / 真实 `hisdatadir` 与网络。
- `WtBtDtReaderAD`、`WtRdmDtReaderAD` 的 `get_k_db`/`get_t_db`：需要先有 WtDataWriterAD 落库（其写入依赖共享内存 RT 块），本车道改为直接对 `LMDBKeys + WtLMDB` 组合做等价的区间语义测试。
