# HftStrategyMgr 类优化建议

## 概述

`HftStrategyMgr` 类是 WonderTrader 高频交易系统中的策略管理器组件，负责加载策略工厂动态库、创建和管理策略实例。通过代码分析，本文档提出了一些可能的优化建议，以提高代码的性能、可维护性、稳定性和功能扩展性。

## 代码结构优化

### 1. 添加异常处理机制

**问题描述**：
当前 `loadFactories` 方法在加载动态库和创建工厂实例过程中，主要依靠返回值判断，缺乏完善的异常处理机制。

**优化建议**：
```cpp
try {
    // 现有加载代码
} catch (const std::exception& e) {
    WTSLogger::error("Error loading factory {}: {}", factoryName, e.what());
    // 适当的清理和错误处理
    return false;
}
```

### 2. 重构工厂信息结构体

**问题描述**：
`_StraFactInfo` 结构体嵌套在类定义中，可能影响代码的清晰度和可维护性。

**优化建议**：
将 `_StraFactInfo` 结构体提取为独立的类或结构体，添加更完善的生命周期管理方法。

```cpp
struct HftStrategyFactInfo {
    std::string modulePath;
    DllHandle moduleInst;
    IHftStrategyFact* factoryInstance;
    FuncCreateHftStraFact creatorFunc;
    FuncDeleteHftStraFact removerFunc;
    
    // 构造和析构函数
    // 其他辅助方法
};
```

## 性能优化

### 1. 策略查找优化

**问题描述**：
当前每次调用 `getStrategy` 都需要进行哈希表查找，对于频繁访问的策略可能影响性能。

**优化建议**：
为常用策略添加缓存机制，特别是在高频交易环境下：

```cpp
// 添加最近使用的策略缓存
HftStrategyPtr _lastUsedStrategy;
std::string _lastUsedId;

HftStrategyPtr getStrategy(const char* id) {
    // 检查是否是最近使用的策略ID
    if (_lastUsedId == id && _lastUsedStrategy)
        return _lastUsedStrategy;
    
    // 原有查找逻辑
    auto it = _strategies.find(id);
    if (it == _strategies.end())
        return HftStrategyPtr();
    
    // 更新缓存
    _lastUsedStrategy = it->second;
    _lastUsedId = id;
    return _lastUsedStrategy;
}
```

### 2. 策略工厂加载优化

**问题描述**：
当前策略工厂加载方法会遍历目录下所有文件，对每个文件尝试加载，可能导致不必要的I/O和处理开销。

**优化建议**：
添加配置文件或命名规则，只加载符合特定模式的文件，减少无效文件的处理：

```cpp
// 添加工厂文件过滤函数
bool isValidFactoryFile(const boost::filesystem::path& path) {
    // 检查文件名是否符合规则，如以"HftFact_"开头
    std::string filename = path.filename().string();
    return filename.find("HftFact_") == 0;
}

// 在loadFactories中使用
if (!isValidFactoryFile(iter->path()))
    continue;
```

## 功能扩展建议

### 1. 增加工厂版本控制

**问题描述**：
当前代码没有对策略工厂的版本进行管理，可能导致兼容性问题。

**优化建议**：
在工厂接口中添加版本信息，并在加载时进行兼容性检查：

```cpp
// 在IHftStrategyFact接口中添加
virtual uint32_t getVersion() = 0;

// 在加载时检查
uint32_t version = pFact->getVersion();
if (version < MIN_REQUIRED_VERSION) {
    WTSLogger::warn("Factory {} version {} is outdated, minimum required: {}", 
                   pFact->getName(), version, MIN_REQUIRED_VERSION);
    // 决定是否继续使用
}
```

### 2. 增加热重载功能

**问题描述**：
当前需要重启应用才能加载新的策略工厂或更新已有工厂。

**优化建议**：
添加工厂热重载功能，允许在运行时更新策略工厂：

```cpp
bool reloadFactory(const char* factname) {
    auto it = _factories.find(factname);
    if (it == _factories.end())
        return false;
    
    // 保存原路径
    std::string oldPath = it->second._module_path;
    
    // 关闭并移除原工厂
    _factories.erase(it);
    
    // 重新加载
    // 加载单个工厂的实现...
    
    return true;
}
```

## 代码健壮性优化

### 1. 增强参数验证

**问题描述**：
当前代码中参数验证不够严格，例如对空指针参数的处理。

**优化建议**：
在所有接口函数开始处添加更严格的参数验证：

```cpp
HftStrategyPtr createStrategy(const char* name, const char* id) {
    if (name == nullptr || id == nullptr || strlen(name) == 0 || strlen(id) == 0) {
        WTSLogger::error("Invalid parameters for createStrategy");
        return HftStrategyPtr();
    }
    
    // 现有创建逻辑...
}
```

### 2. 添加线程安全保障

**问题描述**：
在多线程环境下，当前实现可能存在线程安全问题。

**优化建议**：
使用互斥锁保护关键资源的访问：

```cpp
// 添加互斥锁成员
mutable std::mutex _mtxFact;
mutable std::mutex _mtxStrat;

HftStrategyPtr getStrategy(const char* id) {
    std::lock_guard<std::mutex> guard(_mtxStrat);
    // 原有查找逻辑...
}

bool loadFactories(const char* path) {
    std::lock_guard<std::mutex> guard(_mtxFact);
    // 原有加载逻辑...
}
```

## 日志和监控优化

### 1. 增强日志记录

**问题描述**：
当前的日志记录主要关注成功加载和基本错误，缺少详细的运行时信息。

**优化建议**：
增加更详细的日志记录，特别是在策略创建和运行过程中：

```cpp
HftStrategyPtr createStrategy(const char* name, const char* id) {
    WTSLogger::debug("Creating strategy {} with ID {}", name, id);
    
    // 创建逻辑...
    
    if (ret) {
        WTSLogger::info("Strategy {} with ID {} created successfully", name, id);
    } else {
        WTSLogger::error("Failed to create strategy {} with ID {}", name, id);
    }
    
    return ret;
}
```

### 2. 添加性能指标收集

**问题描述**：
缺少对策略加载和创建性能的监控。

**优化建议**：
添加性能指标收集，记录关键操作的耗时：

```cpp
bool loadFactories(const char* path) {
    uint64_t startTime = TimeUtils::getUTCNano();
    
    // 加载逻辑...
    
    uint64_t endTime = TimeUtils::getUTCNano();
    WTSLogger::info("Loading factories took {} ms", (endTime-startTime)/1000000.0);
    
    return true;
}
```

## 结论

通过实施上述优化建议，可以显著提高 `HftStrategyMgr` 类的性能、可维护性和稳定性。这些优化不仅有助于当前系统的运行，还能为未来功能扩展和性能提升奠定基础。建议根据实际项目需求和资源情况，选择性地实施这些优化措施。
