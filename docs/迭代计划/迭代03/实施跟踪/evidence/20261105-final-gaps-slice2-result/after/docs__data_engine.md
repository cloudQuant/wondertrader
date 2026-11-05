/**
 * @page data_engine 数据引擎
 * 
 * @section data_overview 数据引擎概述
 * 
 * 数据引擎是WonderTrader中负责数据处理、存储和管理的核心组件。它处理从多个数据源收集的行情数据，
 * 并提供统一的数据访问接口，包括历史数据和实时行情。
 * 
 * @section data_components 主要组件
 * 
 * - **WtDataStorage** - 数据存储器，负责行情数据的存储和管理
 * - **WtLMDB** - 基于LMDB的高性能数据库封装，提供快速的数据读写能力
 * - **WtDtHelper** - 数据辅助工具，提供数据转换和处理能力
 * - **IDataReader/IDataWriter** - 数据读写接口，实现了数据的输入输出
 * 
 * @section data_features 核心功能
 * 
 * 1. **数据存储** - 高效存储行情和历史数据
 * 2. **数据检索** - 快速检索和访问历史数据
 * 3. **数据转换** - 支持多种数据格式的转换
 * 4. **数据整合** - 整合来自不同来源的数据
 * 5. **行情缓存** - 提供实时行情的内存缓存
 * 
 * @section data_storage 数据存储格式
 * 
 * 数据引擎支持多种数据存储格式：
 * 
 * - **Tick数据** - 逐笔成交数据
 * - **K线数据** - 包括分钟、小时、日线等多周期K线
 * - **交易数据** - 订单、成交等交易相关数据
 * - **高级指标** - 预计算的技术指标数据
 * 
 * @section data_interfaces 关键接口
 * 
 * - `open()` - 打开数据库
 * - `get()` - 获取数据
 * - `put()` - 写入数据
 * - `get_range()` - 获取范围数据
 * - `load_raw_bars()` - 加载原始K线数据
 * - `load_raw_ticks()` - 加载原始Tick数据
 * 
 * @section data_lmdb_minute_limit WtDataStorageAD 分钟K线时间上限（2032-12-31）
 * 
 * WtDataStorageAD 的 LMDB K线键（`LMDBBarKey`，V1 磁盘格式）用 32 位无符号整数 `_bartime`
 * 存放分钟线时间，编码为 `(日期 - 19900000) * 10000 + HHMM`，最大值为 4294967295。
 * 因此 V1 分钟线（m1/m5）能表示的最后一个交易日是 **2032-12-31**
 * （20321231 23:59 编码为 4212312359）；2033-01-01 起的分钟时间（20330101 00:00 为 4301010000）
 * 无法用 V1 键表示。按迭代03 用户裁定 K33=A，本版本不接入 V2 分钟键，规则如下：
 * 
 * - **写端**：`WtDataWriterAD` 遇到超过 32 位的分钟时间，拒绝写入 LMDB 并记 ERROR，
 *   不再截断成回绕键；外部 dumper 仍收到完整的 64 位 `WTSBarStruct`。
 * - **读端**：`WtDataReaderAD`、`WtRdmDtReaderAD` 的分钟线查询截止时间、区间端点和
 *   `onMinuteEnd` 都用 64 位计算；超出 32 位键范围时记 ERROR 并拒绝该次查询，不发
 *   `on_all_bar_updated`，不会返回回绕后指向更早年份的K线。
 * - **日线不受影响**：日线键按交易日（YYYYMMDD）存放，2033 年及以后可正常读写。
 * - **WtDataStorage（非 AD）** 的 dsb/dmb 文件使用 64 位时间字段，不受此上限约束。
 * 
 * 需要在 2033 年后继续存放 AD 分钟线时，必须先实现并验收版本化的 V2 分钟键与离线迁移
 * （键编解码草案见 `LMDBBarKeyV2.hpp`/`LMDBBarMigration.hpp`，当前未接入读写路径）。
 * 
 * @see WtDataStorage
 * @see WtLMDB
 * @see WtDtHelper
 */
