# ExecMocker 优化建议文档

本文档记录了在阅读和注释 ExecMocker.h 和 ExecMocker.cpp 文件过程中发现的潜在优化点和改进建议。这些建议旨在提高代码质量、性能和可维护性，但不修改原始代码以确保现有功能正常工作。

## 1. 资源管理优化

### 1.1 内存管理和对象生命周期

**问题描述**：
在 `ExecMocker` 类中，`_last_tick` 成员变量在析构函数中被释放，但在 `handle_tick` 方法中的处理方式可能存在优化空间，每次接收到新的 tick 数据时都会释放旧数据并保留新数据。

**优化建议**：
- 考虑使用智能指针（如 `std::shared_ptr`）管理 `WTSTickData` 对象，以避免手动的引用计数管理。
- 在 `handle_tick` 方法中，可以先检查新旧 tick 数据是否为同一对象，如果是则不进行冗余的释放和保留操作。

```cpp
// 示例改进
void ExecMocker::handle_tick(const char* stdCode, WTSTickData* curTick, uint32_t pxType)
{
    if (_last_tick != curTick)  // 避免处理同一对象
    {
        if (_last_tick)
        {
            _last_tick->release();
            _last_tick = nullptr;
        }
        
        if (curTick)
        {
            _last_tick = curTick;
            _last_tick->retain();
        }
    }
    
    // 其他处理逻辑...
}
```

### 1.2 动态库管理

**问题描述**：
在 `init` 方法中，加载执行器工厂动态库时，如果 `createExecFact` 函数指针获取失败，会释放动态库句柄，但在其他情况下，动态库句柄的释放依赖于 `_factory` 结构体的析构函数。

**优化建议**：
- 统一动态库句柄的管理，考虑使用 RAII 原则或智能指针封装。
- 增加对其他函数指针的检查，确保所有必要的函数都能正确获取。

```cpp
// 示例改进
bool ExecMocker::init(WTSVariant* cfg)
{
    // ...
    std::unique_ptr<DllHandleWrapper> dllWrapper = std::make_unique<DllHandleWrapper>(module);
    if (!dllWrapper->isValid())
        return false;
        
    auto creator = dllWrapper->getSymbol<FuncCreateExeFact>("createExecFact");
    if (!creator)
        return false;
        
    auto remover = dllWrapper->getSymbol<FuncDeleteExeFact>("deleteExecFact");
    if (!remover)
        return false;
    
    // 其他初始化逻辑...
    
    // 成功后将所有权转移给_factory
    _factory._dllWrapper = std::move(dllWrapper);
    // ...
}
```

## 2. 错误处理和日志优化

### 2.1 增强错误检查和日志

**问题描述**：
当前代码中的错误处理主要依赖于返回 `false` 或记录日志，缺乏详细的错误原因描述和统一的错误处理机制。

**优化建议**：
- 增加更详细的错误日志，包括具体的失败原因和上下文信息。
- 考虑使用异常处理机制或更丰富的错误码，提供更具描述性的错误信息。
- 为关键操作增加断言或参数验证，提前捕获潜在问题。

```cpp
// 示例改进
bool ExecMocker::init(WTSVariant* cfg)
{
    if (!cfg)
    {
        WTSLogger::error("ExecMocker::init failed: configuration is null");
        return false;
    }
    
    const char* module = cfg->getCString("module");
    if (!module || strlen(module) == 0)
    {
        WTSLogger::error("ExecMocker::init failed: module path is empty");
        return false;
    }
    
    // 其他初始化和验证逻辑...
}
```

### 2.2 日志结构优化

**问题描述**：
当前的交易日志使用 CSV 格式直接写入，缺乏结构化的日志管理和配置选项。

**优化建议**：
- 将交易日志逻辑封装为专门的日志记录类，支持不同格式（如 CSV、JSON）和输出目标（如文件、数据库）。
- 增加日志级别和过滤机制，允许用户配置详细程度。
- 考虑使用已有的结构化日志库，提供更丰富的日志功能。

```cpp
// 示例改进
class TradeLogger
{
public:
    enum class Format { CSV, JSON };
    
    TradeLogger(const std::string& id, Format format = Format::CSV);
    
    void logTrade(const TradeRecord& record);
    void logOrder(const OrderRecord& record);
    void save(const std::string& path);
    
private:
    std::string _id;
    Format _format;
    std::vector<TradeRecord> _trades;
    std::vector<OrderRecord> _orders;
};
```

## 3. 模块设计和职责划分

### 3.1 功能职责划分

**问题描述**：
`ExecMocker` 类同时实现多个接口（`ExecuteContext`、`IDataSink`、`IMatchSink`），承担了多种功能职责，如数据接收、订单管理、持仓维护、日志记录等，这使得类变得庞大且职责不够单一。

**优化建议**：
- 考虑将不同职责拆分为更小的组件，如数据接收器、订单管理器、持仓管理器、日志记录器等。
- 使用组合而非多重继承，减少接口耦合。
- 用依赖注入模式传递组件实例，提高可测试性和灵活性。

```cpp
// 示例改进
class ExecutionContext { /* ... */ };
class DataHandler { /* ... */ };
class OrderManager { /* ... */ };
class PositionManager { /* ... */ };

class ExecMocker
{
public:
    ExecMocker(
        std::shared_ptr<HisDataReplayer> replayer,
        std::shared_ptr<OrderManager> orderMgr,
        std::shared_ptr<PositionManager> posMgr
    );
    
    // ...
    
private:
    std::shared_ptr<HisDataReplayer> _replayer;
    std::shared_ptr<OrderManager> _orderMgr;
    std::shared_ptr<PositionManager> _posMgr;
    // ...
};
```

### 3.2 配置管理优化

**问题描述**：
当前配置管理直接依赖 `WTSVariant` 类，配置项通过字符串键直接访问，缺乏类型安全和验证机制。

**优化建议**：
- 创建专门的配置类，封装配置项的访问和验证逻辑。
- 使用强类型枚举或常量定义配置键，避免字符串硬编码。
- 增加配置项的默认值和类型转换逻辑。

```cpp
// 示例改进
class ExecMockerConfig
{
public:
    ExecMockerConfig(WTSVariant* cfg);
    
    const std::string& getModulePath() const { return _modulePath; }
    const std::string& getCode() const { return _code; }
    double getVolUnit() const { return _volUnit; }
    VolMode getVolMode() const { return _volMode; }
    
    enum class VolMode { Alternate = 0, AlwaysSell = -1, AlwaysBuy = 1 };
    
private:
    std::string _modulePath;
    std::string _code;
    std::string _period;
    double _volUnit = 1.0;
    VolMode _volMode = VolMode::Alternate;
};
```

## 4. 性能优化

### 4.1 时间处理优化

**问题描述**：
代码中多次进行时间戳的转换和计算，尤其是在 `handle_order` 和 `handle_trade` 方法中，存在冗余操作。

**优化建议**：
- 封装常用的时间转换操作为辅助方法，减少冗余代码。
- 考虑缓存时间转换结果，尤其是不变的转换结果，如信号时间的 Unix 时间戳。
- 使用更高效的时间处理库，如 `std::chrono`。

```cpp
// 示例改进
// 添加辅助方法
uint64_t ExecMocker::convertToUnixTime(uint32_t date, uint32_t time)
{
    // 封装时间转换逻辑
    return TimeUtils::makeTime(date, time);
}

// 缓存转换结果
void ExecMocker::updateSignalTime(uint64_t signalTime)
{
    _sig_time = signalTime;
    _sig_time_unix = convertToUnixTime((uint32_t)(_sig_time / 10000), _sig_time % 10000 * 100000);
}
```

### 4.2 数据结构优化

**问题描述**：
当前订单和交易记录的管理使用基本数据类型和字符串流，缺乏高效的数据结构支持。

**优化建议**：
- 使用专门的数据结构（如哈希表、二叉树等）管理订单和持仓信息，提高查询效率。
- 预分配内存空间，减少动态内存分配次数。
- 考虑使用内存池或对象池技术，减少频繁创建和销毁对象的开销。

```cpp
// 示例改进
// 使用哈希表存储订单信息
using OrderID = uint32_t;
std::unordered_map<OrderID, OrderInfo> _orders;

// 预分配交易日志缓冲区
_trade_logs.reserve(10 * 1024 * 1024);  // 预分配 10MB 空间
```

## 5. 接口扩展和兼容性

### 5.1 扩展性优化

**问题描述**：
当前实现中，撮合机制和执行单元工厂都是通过直接调用特定接口实现的，难以扩展或替换为其他实现。

**优化建议**：
- 使用抽象工厂模式和策略模式，将撮合引擎和执行单元的创建和使用解耦。
- 引入接口或抽象基类，允许不同的撮合引擎和执行单元实现。
- 支持通过配置选择不同的撮合模式或执行策略。

```cpp
// 示例改进
class IMatchEngine
{
public:
    virtual ~IMatchEngine() {}
    virtual void init(WTSVariant* cfg) = 0;
    virtual void regisSink(IMatchSink* sink) = 0;
    virtual OrderIDs buy(const char* stdCode, double price, double qty, uint64_t ordTime) = 0;
    // ...
};

class SimpleMatchEngine : public IMatchEngine { /* ... */ };
class AdvancedMatchEngine : public IMatchEngine { /* ... */ };

// 工厂方法
std::unique_ptr<IMatchEngine> createMatchEngine(const std::string& type);
```

### 5.2 配置灵活性

**问题描述**：
当前的配置结构相对固定，难以适应不同的回测场景和需求。

**优化建议**：
- 增加更多配置选项，支持更灵活的行为定制，如撮合模式、滑点模型、延迟模拟等。
- 支持分层配置，允许全局配置和局部配置覆盖。
- 增加动态配置更新机制，允许在回测过程中调整参数。

```cpp
// 示例改进
struct ExecMockerConfig
{
    // 基本配置
    std::string module;
    std::string code;
    std::string period;
    
    // 撮合配置
    struct {
        std::string mode = "simple";  // simple, realistic, strict
        double slippage = 0.0;
        uint32_t delay_ms = 0;
    } matching;
    
    // 执行配置
    struct {
        std::string name;
        std::string id;
        WTSVariant* params = nullptr;
    } executer;
    
    // ...
};
```

## 6. 测试和可维护性

### 6.1 测试友好性

**问题描述**：
当前的实现难以进行单元测试，因为许多关键操作依赖于外部系统和全局状态。

**优化建议**：
- 将关键操作封装为可测试的小型函数。
- 使用依赖注入模式，允许在测试中替换外部依赖。
- 增加 mock 对象和测试接口，方便测试不同场景。

```cpp
// 示例改进
// 依赖注入
ExecMocker::ExecMocker(
    IDataReplayer* replayer, 
    IMatchEngine* matcher,
    ILogger* logger
)
    : _replayer(replayer)
    , _matcher(matcher)
    , _logger(logger)
{
    // ...
}

// 测试用例示例
TEST(ExecMockerTest, HandleOrderCancellation)
{
    MockReplayer replayer;
    MockMatchEngine matcher;
    MockLogger logger;
    
    EXPECT_CALL(matcher, cancel(1001))
        .WillOnce(Return(100.0));
        
    ExecMocker mocker(&replayer, &matcher, &logger);
    bool result = mocker.cancel(1001);
    
    EXPECT_TRUE(result);
    EXPECT_EQ(mocker.getUndoneQty("SHFE.rb.2101"), -100.0);
}
```

### 6.2 文档完善

**问题描述**：
虽然已经添加了 Doxygen 风格的注释，但一些复杂的逻辑和算法缺乏详细的文档说明。

**优化建议**：
- 为复杂算法和业务逻辑添加更详细的文档，包括工作原理、边界条件和使用示例。
- 创建类和模块级别的设计文档，说明各组件之间的交互关系。
- 增加配置参数的详细文档，包括取值范围、默认值和影响。

## 结论

ExecMocker 组件作为回测系统中的执行模拟器，在系统中扮演着重要的角色。通过实施上述优化建议，可以进一步提高其可靠性、性能和可维护性，使其更好地支持各种回测场景和需求。

这些建议仅供参考，具体实施时需要考虑整个系统的架构和需求，确保不会破坏现有功能和兼容性。根据项目的优先级和资源情况，可以选择性地实施部分优化建议。
