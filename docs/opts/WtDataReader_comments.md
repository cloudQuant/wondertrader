# WtDataReader 类注释文档

本文档记录了 WtDataReader 类的 Doxygen 风格注释，包括类、结构体、方法和成员变量的详细说明。

## 文件头注释

```cpp
/*!
 * \file WtDataReader.h
 * \brief 数据读取模块
 * \author Wesley
 * \date 2020/03/30
 * 
 * \details
 * 从不同的数据存储引擎中读取K线、订单簿、成交明细等数据
 */
```

## 类注释

```cpp
/**
 * @brief 数据读取器
 * @details 从不同的数据存储引擎中读取K线、订单簿、成交明细等数据
 * @ingroup WtDataStorage
 */
class WtDataReader : public IDataReader
```

## 内部结构体注释

### RTKlineBlockPair 结构体

```cpp
/**
 * @brief 实时K线数据块对
 * @details 存储实时K线数据的数据块指针和内存映射文件
 */
typedef struct _RTKBlockPair
{
    RTKlineBlock*   _block;     ///< 实时K线数据块指针
    BoostMFPtr      _file;      ///< 内存映射文件指针
    uint64_t        _last_time; ///< 最后更新时间

    /**
     * @brief 构造函数
     */
    _RTKBlockPair()
    {
        _block = NULL;
        _file = NULL;
        _last_time = 0;
    }
} RTKlineBlockPair;
```

### TickBlockPair 结构体

```cpp
/**
 * @brief 实时Tick数据块对
 * @details 存储实时Tick数据的数据块指针和内存映射文件
 */
typedef struct _TBlockPair
{
    RTTickBlock*    _block;     ///< 实时Tick数据块指针
    BoostMFPtr      _file;      ///< 内存映射文件指针
    uint64_t        _last_time; ///< 最后更新时间

    /**
     * @brief 构造函数
     */
    _TBlockPair()
    {
        _block = NULL;
        _file = NULL;
        _last_time = 0;
    }
} TickBlockPair;
```

### TransBlockPair 结构体

```cpp
/**
 * @brief 实时成交数据块对
 * @details 存储实时成交数据的数据块指针和内存映射文件
 */
typedef struct _TransBlockPair
{
    RTTransBlock*   _block;     ///< 实时成交数据块指针
    BoostMFPtr      _file;      ///< 内存映射文件指针
    uint64_t        _last_time; ///< 最后更新时间

    /**
     * @brief 构造函数
     */
    _TransBlockPair()
    {
        _block = NULL;
        _file = NULL;
        _last_time = 0;
    }
} TransBlockPair;
```

### OrdDtlBlockPair 结构体

```cpp
/**
 * @brief 实时委托明细数据块对
 * @details 存储实时委托明细数据的数据块指针和内存映射文件
 */
typedef struct _OdrDtlBlockPair
{
    RTOrdDtlBlock*  _block;     ///< 实时委托明细数据块指针
    BoostMFPtr      _file;      ///< 内存映射文件指针
    uint64_t        _last_time; ///< 最后更新时间

    /**
     * @brief 构造函数
     */
    _OdrDtlBlockPair()
    {
        _block = NULL;
        _file = NULL;
        _last_time = 0;
    }
} OrdDtlBlockPair;
```

### OrdQueBlockPair 结构体

```cpp
/**
 * @brief 实时委托队列数据块对
 * @details 存储实时委托队列数据的数据块指针和内存映射文件
 */
typedef struct _OdrQueBlockPair
{
    RTOrdQueBlock*  _block;     ///< 实时委托队列数据块指针
    BoostMFPtr      _file;      ///< 内存映射文件指针
    uint64_t        _last_time; ///< 最后更新时间

    /**
     * @brief 构造函数
     */
    _OdrQueBlockPair()
    {
        _block = NULL;
        _file = NULL;
        _last_time = 0;
    }
} OrdQueBlockPair;
```

### HisTBlockPair 结构体

```cpp
/**
 * @brief 历史Tick数据块对
 * @details 存储历史Tick数据的数据块指针和缓冲区
 */
typedef struct _HisTBlockPair
{
    HisTickBlock*   _block;     ///< 历史Tick数据块指针
    uint64_t        _date;      ///< 日期
    std::string     _buffer;    ///< 数据缓冲区

    /**
     * @brief 构造函数
     */
    _HisTBlockPair()
    {
        _block = NULL;
        _date = 0;
        _buffer.clear();
    }
} HisTickBlockPair;
```

### HisTransBlockPair 结构体

```cpp
/**
 * @brief 历史成交数据块对
 * @details 存储历史成交数据的数据块指针和缓冲区
 */
typedef struct _HisTransBlockPair
{
    HisTransBlock*  _block;     ///< 历史成交数据块指针
    uint64_t        _date;      ///< 日期
    std::string     _buffer;    ///< 数据缓冲区

    /**
     * @brief 构造函数
     */
    _HisTransBlockPair()
    {
        _block = NULL;
        _date = 0;
        _buffer.clear();
    }
} HisTransBlockPair;
```

### HisOrdDtlBlockPair 结构体

```cpp
/**
 * @brief 历史委托明细数据块对
 * @details 存储历史委托明细数据的数据块指针和缓冲区
 */
typedef struct _HisOrdDtlBlockPair
{
    HisOrdDtlBlock* _block;     ///< 历史委托明细数据块指针
    uint64_t        _date;      ///< 日期
    std::string     _buffer;    ///< 数据缓冲区

    /**
     * @brief 构造函数
     */
    _HisOrdDtlBlockPair()
    {
        _block = NULL;
        _date = 0;
        _buffer.clear();
    }
} HisOrdDtlBlockPair;
```

### HisOrdQueBlockPair 结构体

```cpp
/**
 * @brief 历史委托队列数据块对
 * @details 存储历史委托队列数据的数据块指针和缓冲区
 */
typedef struct _HisOrdQueBlockPair
{
    HisOrdQueBlock* _block;     ///< 历史委托队列数据块指针
    uint64_t        _date;      ///< 日期
    std::string     _buffer;    ///< 数据缓冲区

    /**
     * @brief 构造函数
     */
    _HisOrdQueBlockPair()
    {
        _block = NULL;
        _date = 0;
        _buffer.clear();
    }
} HisOrdQueBlockPair;
```

### BarsList 结构体

```cpp
/**
 * @brief K线列表结构体
 * @details 存储K线数据及相关信息的结构体
 */
typedef struct _BarsList
{
    std::string     _exchg;         ///< 交易所代码
    std::string     _code;          ///< 合约代码
    WTSKlinePeriod  _period;        ///< K线周期
    uint32_t        _rt_cursor;     ///< 实时数据游标
    std::string     _raw_code;      ///< 原始代码

    std::vector<WTSBarStruct>   _bars;  ///< K线数据数组
    double          _factor;        ///< 复权因子

    /**
     * @brief 构造函数
     */
    _BarsList() :_rt_cursor(UINT_MAX), _factor(DBL_MAX){}
} BarsList;
```

### AdjFactor 结构体

```cpp
/**
 * @brief 除权因子结构体
 * @details 存储股票除权信息的结构体
 */
typedef struct _AdjFactor
{
    uint32_t    _date;      ///< 日期
    double      _factor;    ///< 复权因子
} AdjFactor;
```

## 私有方法注释

### getRTKilneBlock 方法

```cpp
/**
 * @brief 获取实时K线数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @param period K线周期
 * @return 实时K线数据块对指针
 */
RTKlineBlockPair* getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period);
```

### getRTTickBlock 方法

```cpp
/**
 * @brief 获取实时Tick数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时Tick数据块对指针
 */
TickBlockPair* getRTTickBlock(const char* exchg, const char* code);
```

### getRTOrdQueBlock 方法

```cpp
/**
 * @brief 获取实时委托队列数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时委托队列数据块对指针
 */
OrdQueBlockPair* getRTOrdQueBlock(const char* exchg, const char* code);
```

### getRTOrdDtlBlock 方法

```cpp
/**
 * @brief 获取实时委托明细数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时委托明细数据块对指针
 */
OrdDtlBlockPair* getRTOrdDtlBlock(const char* exchg, const char* code);
```

### getRTTransBlock 方法

```cpp
/**
 * @brief 获取实时成交数据块
 * @param exchg 交易所代码
 * @param code 合约代码
 * @return 实时成交数据块对指针
 */
TransBlockPair* getRTTransBlock(const char* exchg, const char* code);
```

### cacheIntegratedBars 方法

```cpp
/**
 * @brief 缓存集成的K线数据
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功
 */
bool cacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheAdjustedStkBars 方法

```cpp
/**
 * @brief 缓存复权后的股票K线数据
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功
 */
bool cacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheHisBarsFromFile 方法

```cpp
/**
 * @brief 将历史数据放入缓存
 * @details 从文件中读取历史K线数据并放入缓存
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功
 */
bool cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### cacheFinalBarsFromLoader 方法

```cpp
/**
 * @brief 从数据加载器中缓存最终K线数据
 * @param codeInfo 合约信息
 * @param key 缓存键
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @return 是否成功
 */
bool cacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
```

### loadStkAdjFactorsFromFile 方法

```cpp
/**
 * @brief 从文件中加载股票复权因子
 * @param adjfile 复权因子文件路径
 * @return 是否成功
 */
bool loadStkAdjFactorsFromFile(const char* adjfile);
```

### loadStkAdjFactorsFromLoader 方法

```cpp
/**
 * @brief 从数据加载器中加载股票复权因子
 * @return 是否成功
 */
bool loadStkAdjFactorsFromLoader();
```

### getAdjFactors 方法

```cpp
/**
 * @brief 获取复权因子列表
 * @param code 合约代码
 * @param exchg 交易所代码
 * @param pid 品种ID
 * @return 复权因子列表的常量引用
 */
const AdjFactorList& getAdjFactors(const char* code, const char* exchg, const char* pid);
```

## 公共方法注释

### init 方法

```cpp
/**
 * @brief 初始化数据读取器
 * @param cfg 配置项
 * @param sink 数据读取器回调接口
 * @param loader 历史数据加载器，默认为NULL
 */
virtual void init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override;
```

### onMinuteEnd 方法

```cpp
/**
 * @brief 分钟结束回调
 * @param uDate 日期，格式YYYYMMDD
 * @param uTime 时间，格式HHMMSS或HHMM
 * @param endTDate 结束交易日，默认为0
 */
virtual void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override;
```

### readTickSlice 方法

```cpp
/**
 * @brief 读取Tick数据切片
 * @param stdCode 标准化合约代码
 * @param count 读取数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return Tick数据切片指针
 */
virtual WTSTickSlice* readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
```

### readOrdDtlSlice 方法

```cpp
/**
 * @brief 读取委托明细数据切片
 * @param stdCode 标准化合约代码
 * @param count 读取数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 委托明细数据切片指针
 */
virtual WTSOrdDtlSlice* readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
```

### readOrdQueSlice 方法

```cpp
/**
 * @brief 读取委托队列数据切片
 * @param stdCode 标准化合约代码
 * @param count 读取数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 委托队列数据切片指针
 */
virtual WTSOrdQueSlice* readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
```

### readTransSlice 方法

```cpp
/**
 * @brief 读取成交数据切片
 * @param stdCode 标准化合约代码
 * @param count 读取数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return 成交数据切片指针
 */
virtual WTSTransSlice* readTransSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
```

### readKlineSlice 方法

```cpp
/**
 * @brief 读取K线数据切片
 * @param stdCode 标准化合约代码
 * @param period K线周期
 * @param count 读取数量
 * @param etime 结束时间，默认为0表示当前时间
 * @return K线数据切片指针
 */
virtual WTSKlineSlice* readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;
```

### getAdjFactorByDate 方法

```cpp
/**
 * @brief 根据日期获取复权因子
 * @param stdCode 标准化合约代码
 * @param date 日期，默认为0表示当前日期
 * @return 复权因子
 */
virtual double getAdjFactorByDate(const char* stdCode, uint32_t date = 0) override;
```

### getAdjustingFlag 方法

```cpp
/**
 * @brief 获取复权标志
 * @return 复权标志，采用位运算表示，1|表示成交量复权，2表示成交额复权，4表示总持复权
 */
virtual uint32_t getAdjustingFlag() override { return _adjust_flag; }
```

## 成员变量注释

```cpp
std::string     _rt_dir;            ///< 实时数据目录
std::string     _his_dir;           ///< 历史数据目录
IBaseDataMgr*   _base_data_mgr;     ///< 基础数据管理器
IHotMgr*        _hot_mgr;           ///< 主力合约管理器
uint32_t        _adjust_flag;       ///< 复权标记
uint64_t        _last_time;         ///< 最后更新时间
```
