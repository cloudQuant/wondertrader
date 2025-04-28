# WtDataReader.cpp 详细注释文档

本文档提供了 WtDataReader.cpp 文件中各个函数和方法的详细中文注释，帮助新手理解代码功能和实现逻辑。

## 文件头部

```cpp
/*!
 * \file WtDataReader.cpp
 * \brief 数据读取模块实现文件
 * \author Wesley
 * \date 2020/03/30
 * 
 * \details
 * 从不同的数据存储引擎中读取K线、订单簿、成交明细等数据的具体实现
 */
```

## 辅助函数

### pipe_reader_log 函数

```cpp
/**
 * @brief 数据读取器日志输出函数模板
 * @details 通过数据读取器接收器输出日志信息，支持格式化输出
 * @tparam Args 可变参数模板类型
 * @param sink 数据读取器接收器指针
 * @param ll 日志级别
 * @param format 格式化字符串
 * @param args 可变参数列表
 */
template<typename... Args>
inline void pipe_reader_log(IDataReaderSink* sink, WTSLogLevel ll, const char* format, const Args&... args);
```

### 导出C接口

```cpp
/**
 * @brief 导出C接口
 * @details 提供创建和删除数据读取器的C接口函数，用于动态库加载
 */
extern "C"
{
    /**
     * @brief 创建数据读取器实例
     * @return 数据读取器接口指针
     */
    EXPORT_FLAG IDataReader* createDataReader();

    /**
     * @brief 删除数据读取器实例
     * @param reader 数据读取器接口指针
     */
    EXPORT_FLAG void deleteDataReader(IDataReader* reader);
};
```

### proc_block_data 函数

```cpp
/**
 * @brief 处理数据块内容
 * @details 处理数据块的压缩和版本转换，包括解压缩和旧版本数据结构的转换
 * @param content 数据块内容，传入传出参数
 * @param isBar 是否为K线数据，true为K线数据，false为Tick数据
 * @param bKeepHead 是否保留数据块头部，默认为true
 * @return 处理是否成功
 */
bool proc_block_data(std::string& content, bool isBar, bool bKeepHead /* = true */);
```

## WtDataReader 类方法

### 构造函数和析构函数

```cpp
/**
 * @brief 构造函数
 * @details 初始化数据读取器对象，设置初始参数
 */
WtDataReader::WtDataReader();

/**
 * @brief 析构函数
 * @details 清理数据读取器对象的资源
 */
WtDataReader::~WtDataReader();
```

### init 方法

```cpp
/**
 * @brief 初始化数据读取器
 * @details 根据配置初始化数据读取器，设置数据目录和加载除权因子
 * @param cfg 配置项指针，包含路径和复权设置
 * @param sink 数据读取器接收器指针，用于回调和日志输出
 * @param loader 历史数据加载器指针，默认为NULL
 */
void WtDataReader::init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader /* = NULL */);
```

### loadStkAdjFactorsFromLoader 方法

```cpp
/**
 * @brief 从数据加载器中加载股票复权因子
 * @details 通过历史数据加载器接口加载所有股票的复权因子
 * @return 是否成功加载复权因子
 */
bool WtDataReader::loadStkAdjFactorsFromLoader();
```

### loadStkAdjFactorsFromFile 方法

```cpp
/**
 * @brief 从文件中加载股票复权因子
 * @details 从指定的JSON格式文件中加载股票复权因子
 * @param adjfile 复权因子文件路径
 * @return 是否成功加载复权因子
 */
bool WtDataReader::loadStkAdjFactorsFromFile(const char* adjfile);
```

### readTickSlice 方法

```cpp
/**
 * @brief 读取Tick数据切片
 * @details 根据标准化合约代码、数量和结束时间读取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 要读取的Tick数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return Tick数据切片指针，如果读取失败则返回NULL
 */
WTSTickSlice* WtDataReader::readTickSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */);
```

### readOrdQueSlice 方法

```cpp
/**
 * @brief 读取委托队列数据切片
 * @details 根据标准化合约代码、数量和结束时间读取委托队列数据切片
 * @param stdCode 标准化合约代码
 * @param count 要读取的委托队列数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 委托队列数据切片指针，如果读取失败则返回NULL
 */
WTSOrdQueSlice* WtDataReader::readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */);
```

### readOrdDtlSlice 方法

```cpp
/**
 * @brief 读取委托明细数据切片
 * @details 根据标准化合约代码、数量和结束时间读取委托明细数据切片
 * @param stdCode 标准化合约代码
 * @param count 要读取的委托明细数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 委托明细数据切片指针，如果读取失败则返回NULL
 */
WTSOrdDtlSlice* WtDataReader::readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */);
```

### readTransSlice 方法

```cpp
/**
 * @brief 读取成交数据切片
 * @details 根据标准化合约代码、数量和结束时间读取成交数据切片
 * @param stdCode 标准化合约代码
 * @param count 要读取的成交数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 成交数据切片指针，如果读取失败则返回NULL
 */
WTSTransSlice* WtDataReader::readTransSlice(const char* stdCode, uint32_t count, uint64_t etime /* = 0 */);
```

### cacheFinalBarsFromLoader 方法

```cpp
/**
 * @brief 从数据加载器中缓存最终K线数据
 * @details 通过历史数据加载器接口加载并缓存最终K线数据
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功缓存数据
 */
bool WtDataReader::cacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheIntegratedBars 方法

```cpp
/**
 * @brief 缓存集成的K线数据
 * @details 将不同周期的K线数据整合并缓存，支持复权处理
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功缓存数据
 */
bool WtDataReader::cacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheAdjustedStkBars 方法

```cpp
/**
 * @brief 缓存复权后的股票K线数据
 * @details 对股票K线数据进行复权处理并缓存
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功缓存数据
 */
bool WtDataReader::cacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheHisBarsFromFile 方法

```cpp
/**
 * @brief 从文件中缓存历史K线数据
 * @details 从历史数据文件中读取K线数据并缓存
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功缓存数据
 */
bool WtDataReader::cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### readKlineSlice 方法

```cpp
/**
 * @brief 读取K线数据切片
 * @details 根据标准化合约代码、周期、数量和结束时间读取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param count 要读取的K线数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return K线数据切片指针，如果读取失败则返回NULL
 */
WTSKlineSlice* WtDataReader::readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime /* = 0 */);
```

### 获取数据块方法

```cpp
/**
 * @brief 获取实时Tick数据块
 * @details 根据交易所和合约代码获取实时Tick数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时Tick数据块对指针，如果不存在则返回NULL
 */
WtDataReader::TickBlockPair* WtDataReader::getRTTickBlock(const char* exchg, const char* code);

/**
 * @brief 获取实时委托明细数据块
 * @details 根据交易所和合约代码获取实时委托明细数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时委托明细数据块对指针，如果不存在则返回NULL
 */
WtDataReader::OrdDtlBlockPair* WtDataReader::getRTOrdDtlBlock(const char* exchg, const char* code);

/**
 * @brief 获取实时委托队列数据块
 * @details 根据交易所和合约代码获取实时委托队列数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时委托队列数据块对指针，如果不存在则返回NULL
 */
WtDataReader::OrdQueBlockPair* WtDataReader::getRTOrdQueBlock(const char* exchg, const char* code);

/**
 * @brief 获取实时成交数据块
 * @details 根据交易所和合约代码获取实时成交数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时成交数据块对指针，如果不存在则返回NULL
 */
WtDataReader::TransBlockPair* WtDataReader::getRTTransBlock(const char* exchg, const char* code);

/**
 * @brief 获取实时K线数据块
 * @details 根据交易所、合约代码和周期获取实时K线数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param period K线周期
 * @return 实时K线数据块对指针，如果不存在则返回NULL
 */
WtDataReader::RTKlineBlockPair* WtDataReader::getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period);
```

### onMinuteEnd 方法

```cpp
/**
 * @brief 分钟结束回调
 * @details 在每分钟结束时调用，用于更新实时数据和触发回调
 * @param uDate 日期，格式YYYYMMDD
 * @param uTime 时间，格式HHMMSS或HHMM
 * @param endTDate 结束交易日，默认为0
 */
void WtDataReader::onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate /* = 0 */);
```

### getAdjFactorByDate 方法

```cpp
/**
 * @brief 根据日期获取复权因子
 * @details 根据标准化合约代码和日期获取对应的复权因子
 * @param stdCode 标准化合约代码
 * @param date 日期，默认为0表示当前日期
 * @return 复权因子，如果不存在则返回1.0
 */
double WtDataReader::getAdjFactorByDate(const char* stdCode, uint32_t date /* = 0 */);
```

### getAdjFactors 方法

```cpp
/**
 * @brief 获取复权因子列表
 * @details 根据合约代码、交易所和品种ID获取复权因子列表
 * @param code 合约代码
 * @param exchg 交易所代码
 * @param pid 品种ID
 * @return 复权因子列表的常量引用
 */
const WtDataReader::AdjFactorList& WtDataReader::getAdjFactors(const char* code, const char* exchg, const char* pid);
```

## 函数实现详细说明

### proc_block_data 函数

这个函数用于处理数据块内容，包括解压缩和版本转换。主要步骤如下：

1. 检查数据块头部，判断是否需要解压缩或版本转换
2. 如果数据已经是最新版本且未压缩，则直接返回
3. 如果数据被压缩，则进行解压缩
4. 如果数据是旧版本格式，则进行版本转换
5. 根据需要保留或删除头部信息
6. 返回处理结果

### init 方法

初始化数据读取器的方法，主要步骤如下：

1. 调用父类的初始化方法
2. 获取基础数据管理器和主力合约管理器
3. 解析配置，设置实时数据目录和历史数据目录
4. 设置复权标志
5. 尝试从数据加载器中加载除权因子
6. 如果从加载器加载失败且配置了除权因子文件，则从文件加载

### loadStkAdjFactorsFromLoader 方法

从数据加载器中加载股票复权因子的方法，主要步骤如下：

1. 检查加载器是否存在
2. 使用加载器加载所有复权因子，并通过回调函数处理
3. 遍历所有复权因子并添加到列表中
4. 添加一个基准复权因子（日期设为1990年，因子为1）
5. 按日期升序排序复权因子列表
6. 输出加载结果日志

### readTickSlice 方法

读取Tick数据切片的方法，主要步骤如下：

1. 从标准化合约代码提取交易所、品种等信息
2. 处理时间参数，计算结束时间
3. 计算交易日期，判断是否是当天的数据
4. 处理合约代码，如果是期货合约需要考虑主力合约转换
5. 创建用于时间比较的Tick结构体
6. 根据是否是当天数据，从实时数据块或历史数据文件中读取
7. 使用二分查找找到结束时间对应的Tick位置
8. 计算起始和结束位置，创建并返回数据切片

### onMinuteEnd 方法

分钟结束回调方法，主要步骤如下：

1. 获取当前时间和日期
2. 遍历所有缓存的K线数据
3. 对于每个K线数据，检查是否需要更新
4. 如果需要更新，则从实时数据块中读取最新数据
5. 更新K线数据并触发回调
6. 更新最后更新时间

### getAdjFactorByDate 方法

根据日期获取复权因子的方法，主要步骤如下：

1. 从标准化合约代码提取信息
2. 检查是否是股票，如果不是则直接返回1.0
3. 获取复权因子列表
4. 使用二分查找找到对应日期的复权因子
5. 如果找不到，则返回最后一条复权因子
6. 如果找到了，但日期大于目标日期，则使用前一条复权因子
7. 返回复权因子值
