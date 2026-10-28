# 迭代01 覆盖率清单 — lane L02（WTSTools / WtShareHelper / WtMsgQue）

- 工作流: L02
- 分支状态: 已完成 3 个测试文件，语法门禁全绿
- 产出目录: `src/TestUnits/coverage/l02_wtstools_mq/`
- 缺陷记录: `docs/迭代计划/迭代01/bugs/l02_wtstools_mq.md`（13 条，状态全部「待审核」，未改任何生产代码）

## 1. 新增文件

| 文件 | 行数 | 主题 |
| --- | --- | --- |
| `src/TestUnits/coverage/l02_wtstools_mq/l02_variant_collection_test.cpp` | 664 | WTSObject/WTSPoolObject 引用计数、WTSVariant 类型容器、WTSArray/WTSHashMap/WTSQueue |
| `src/TestUnits/coverage/l02_wtstools_mq/l02_session_contract_test.cpp` | 572 | WTSSessionInfo 时段计算、WTSCommodityInfo、WTSContractInfo |
| `src/TestUnits/coverage/l02_wtstools_mq/l02_csv_shareblock_test.cpp` | 355 | CsvReader 读写、shareblock 结构体布局（内存内可测部分） |

注：单文件行数略超简报建议的 150–400 区间（引用计数配对断言密集），未再拆分以免重复跑门禁。
`src/TestUnits/coverage/CMakeLists.txt` 用 `GLOB_RECURSE` 收集 `*.cpp`，新文件会被自动纳入 `CoverageTests`，无需改构建脚本（本 lane 未修改任何 CMakeLists）。

## 2. 覆盖的类 / 函数

### WTSObject（src/Includes/WTSObject.hpp）
- `retain/retainCount/isSingleRefs/release`（含 refs==0 早退分支、归零即 `delete this`）
- `WTSPoolObject<T>::allocate()` + 覆写 `release()` 归还对象池路径

### WTSVariant（src/Includes/WTSVariant.hpp）
- `createObject/createArray`、私有 `create(标量)` 工厂（经 `append` 间接触发，覆盖 int32/uint32/int64/uint64/double/bool/string 全部分支）
- `append` 全部 14 个重载的正/负路径（容器类型不匹配返回 false、传 NULL 返回 false）
- `get(const char*)/get(std::string)/get(uint32_t)`、`has`、`size`、`memberNames`、`type/isArray/isObject`
- `asInt32/asUInt32/asInt64/asUInt64/asDouble/asString/asCString` 的 switch 各分支（含 default）
- `getInt32...getBoolean` 带缺省值路径
- `release()` 覆写（VT_Array/VT_Object/标量三个分支）与父子引用计数配对
- 越界下标 `get(idx)` 返回 NULL

### WTSCollection（src/Includes/WTSCollection.hpp）
- `WTSArray`: `create/size/empty/resize/at/at<T>/operator[]/grab/idxOf/append(obj,bool)/append(array)/set/clear/release/begin/end/rbegin/rend/sort`
- `WTSHashMap<std::string>`: `create/size/get/grab/add/remove/find/begin/end/clear/release`
- `WTSQueue`: `create/push/pop/front/back/size/empty/swap/begin/end/clear/release`

### WTSSessionInfo（src/Includes/WTSSessionInfo.hpp）
- `create/id/name/getOffsetMins/setOffsetMins`
- `addTradingSection/setAuctionTime/addAuctionTime/getTradingSections/getAuctionSections/getSectionCount`
- `offsetTime`（左/右对齐、跨 1440 圈回、0000→2400）、`originalTime`
- `getOffsetDate`（同日/次日/前一日/跨月，全部显式传参，不取真实时间）
- `timeToMinutes`（段首/段中/段尾/集合竞价/午休/盘前/盘后 + `autoAdjust` 对齐分支）
- `minuteToTime`（含 `bHeadFirst`、超范围回退 `getCloseTime`）
- `timeToSeconds`（含 `seconds == stopSecs` 时 `offset--`）、`secondsToTime`（含越界返回 INVALID_UINT32）
- `getOpenTime/getCloseTime/getAuctionStartTime/getTradingMins/getTradingSeconds/getSecMinList`（含缓存复用与空模板兜底 1440）
- `isInTradingTime(bStrict)/isFirstOfSection/isLastOfSection/isInAuctionTime`（含 (0,0) 段跳过）
- 空模板分支：所有换算接口返回 `INVALID_UINT32`
- 夜盘偏移（-420 分钟）双段场景端到端换算

### WTSCommodityInfo / WTSContractInfo（src/Includes/WTSContractInfo.hpp）
- `create` 全字段读写、`getFullPid/getFullCode/getFullAltCode`、`setAltCode`
- `setVolScale/setPriceTick/setLotsTick/setMinLots/setCategory/setCoverMode/setPriceMode/setTradingMode/setMarginRate/setMarginRatios`
- `isFuture/isStock/isOption/canShort/isT1`、`addCode/getCodes`
- `calcFee`：`m_nFeeAlg == -1/0/1` 三个分支 × offset 0/1/2/未知，合约无费率时委托品种
- `setFeeRates/setCommInfo/getCommInfo/getLongMarginRatio/getShortMarginRatio`（flag 0/1、品种费率为 0 的回退）
- `setVolumeLimits/setDates/getOpenDate/getExpireDate/setTotalIndex/getTotalIndex/addHotCode/isFlat/getHotCodes`
- `setExtData<INDEX>/getExtData<T,INDEX>`（含未使用槽位为 NULL）

### CsvReader（src/WTSTools/CsvHelper.h / .cpp）
- `CsvReader(splitter)`、`load_from_file`（缺文件返回 false、UTF-8 BOM 分支外的表头处理）
- 表头特殊字符 `< > " '` 剔除 + `toLowerCase` + 四类 `trim` + 空字段 `break`
- `next_row`（首行为表头、空行跳过、EOF 返回 false）
- `col_count`、`get_int32/get_uint32/get_int64/get_uint64/get_double/get_string` 的按列与按字段两套重载（含未知字段 → `INT_MAX` → `check_cell` 拦截、非数字文本 → 0）
- 自定义分隔符 `;`、`\t`
- 临时文件写在 `std::filesystem::temp_directory_path()/l02_<pid>_<seq>/`，TearDown `remove_all`

### shareblock（src/WtShareHelper/ShareBlocks.h，仅内存内部分）
- `KeyInfo`/`SecInfo`/`ShmBlock`/`CmdInfo`/`_CmdBlock<N>` 的 `#pragma pack(1)` 布局、构造函数 memset 归零
- `SecInfo::get<T>(offset)` 偏移读写（含 `_data` 尾部 1024-8 边界）
- `SMVT_SIZES` 尺寸表、`FLAG_SIZE/MAX_SEC_CNT/MAX_KEY_CNT/MAX_CMD_SIZE`、`BLK_FLAG`

## 3. 跳过目标及原因

| 目标 | 原因 |
| --- | --- |
| `src/WtShareHelper/ShareBlocks.cpp` 全部成员（`init_master/init_slave/update_slave/allocate_*/set_*/get_*/commit_section/delete_section/get_sections/get_keys/init_cmder/add_cmd/get_cmd`） | 依赖 `BoostMappingFile` 映射文件 + 跨进程共享内存段（master/slave 生命周期、`_updatetime` 版本协商），单进程单测无法构造对端，且会污染 `/dev/shm` 与本机文件；仅覆盖其纯结构体布局 |
| `src/WtShareHelper/WtShareHelper.cpp`（`EXPORT_FLAG` C 接口） | 是 `ShareBlocks` 的转发壳，同样需要共享内存后端 |
| `src/WtMsgQue/MQServer.*`、`MQClient.*`、`MQManager.*`、`WtMsgQue.cpp` | 全部走 nanomsg `NN_REP/NN_SUB` socket（`tcp://` 端口绑定 + 后台线程 `sleep_for` 轮询 + `SpinLock` 多线程收发），属端口/多进程/时序依赖；简报里提到的 `MQPacker`/`MQVerifier` 经全仓检索**在本仓库不存在**（无任何符号命中），故无可测对象 |
| `WTSVariant::to_json` | 该接口不存在（`to_json/emit_members` 在 `src/**` 无任何命中），序列化由 `WTSUtils/JsonFileParser` 承担，属 ws05 lane |
| `WTSCfgLoader` | 属 `src/WTSUtils/`，已由 ws05 lane 的 `test_ws05_cfgloader.cpp` 覆盖，避免重复计账 |
| `WTSObjectCache` | 该类型在仓库中不存在（无符号命中）；缓存语义由 `WTSCollection.hpp` 的 `WTSArray/WTSHashMap/WTSQueue` 承载，已覆盖 |
| `WTSBaseDataMgr`/`WTSHotMgr`/`WTSDataFactory` 的 load*、`updateKlineData/extractKlineData` | 需要真实 `configs/*.csv` 数据文件或 AD 共享内存片段 + `WTSKlineSlice/WTSTickSlice` 构造，属集成层；`WTSDataFactory::getPrevMinute` 等静态纯函数需要包含 1647 行的 `WTSDataDef.hpp`，本 lane 预算内未展开 |
| `WTSLogger`（`initLogger/log_raw/print_message` 家族） | 会创建 sink、线程池与日志目录（`spdlog::init_thread_pool` + `flush_every`），依赖真实时间与文件写入，非确定性 |
| `WTSSessionInfo::getOffsetDate()` 的默认参数分支 | `uDate==0` 时走 `TimeUtils::getDateTime` 读系统时钟，测试全部显式传参 |

## 4. 剩余工作（预算耗尽前未展开，按投产比排序）

1. `WTSDataFactory` 的 `updateMin1Data/extractMin5Data/getPrevMinute`（纯内存，需先包一层 `WTSDataDef.hpp`，预计 +1 文件）
2. `WTSHotMgr::splitHotSecions/splitSecondSecions` 的纯计算部分（`_HotSections` 拼装可用内存内构造的规则表）
3. `WTSCollection.hpp` 的 `WTSMap<T>`、`WTSStack` 两个容器（本轮因 `WTSQueue::pop` 泄漏缺陷占用预算而未展开）
4. `WTSBaseDataMgr` 的 `calcTradingDate/getBoundaryTime/isHoliday`（需要先造一份最小 session/commodity/holiday 内存表）

## 5. 语法 + 告警门禁

命令（仓库根执行，逐文件）：

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/l02_wtstools_mq/<file>.cpp
```

| 文件 | 结果 |
| --- | --- |
| `l02_variant_collection_test.cpp` | 0 error / 0 warning |
| `l02_session_contract_test.cpp` | 0 error / 0 warning |
| `l02_csv_shareblock_test.cpp` | 0 error / 0 warning |

未执行 cmake/ninja 全量构建（按分工由主控完成）。

## 6. 已知风险 / 需要主控留意

- 本 lane 全部断言只读取「已被生产代码显式初始化」的字段，未初始化字段（BUG-L02-002）刻意规避，`-Wuninitialized` 类告警不会因测试触发。
- `WTSQueue::pop()` 泄漏（BUG-L02-005）在测试里做了手工 release 补偿，若后续修复，需同步删除补偿行，否则该用例会因引用计数提前归零而失败。
- `WTSArray::append(obj,false)` + `clear()` 的过度释放（BUG-L02-004）同理，测试用一次额外 `retain()` 抵消；修复后需回归。
- 三个文件均无网络/真实时钟/固定绝对路径依赖，临时目录带 `l02_` 前缀并在 TearDown 清理。
- `src/TestUnits/coverage/ws01_share_core/ws01_test_util.h` 仅作阅读参考，本 lane 未复用也未修改。
