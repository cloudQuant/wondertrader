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
 * @see WtDataStorage
 * @see WtLMDB
 * @see WtDtHelper
 */
