# WtExeFact优化建议

本文档记录了对`WtExeFact`类进行代码审查后的优化建议。这些建议旨在提高代码的可读性、可维护性、性能和安全性，但不直接修改原始代码。

## 1. 代码结构优化

### 1.1 设计模式应用

- **工厂方法优化**：当前的工厂类使用了if-else链进行执行单元的创建，可以考虑使用映射表（map）替代if-else链，提高扩展性和可维护性：
  ```cpp
  std::unordered_map<std::string, std::function<ExecuteUnit*()>> _exeUnitCreators;
  
  // 在构造函数中初始化
  _exeUnitCreators["WtTWapExeUnit"] = []() { return new WtTWapExeUnit(); };
  _exeUnitCreators["WtMinImpactExeUnit"] = []() { return new WtMinImpactExeUnit(); };
  // ...
  
  // 创建方法简化为
  ExecuteUnit* WtExeFact::createExeUnit(const char* name) {
    auto it = _exeUnitCreators.find(name);
    return (it != _exeUnitCreators.end()) ? it->second() : nullptr;
  }
  ```

### 1.2 类层次结构优化

- **统一创建接口**：考虑将`createExeUnit`、`createDiffExeUnit`和`createArbiExeUnit`合并为一个统一的创建方法，使用参数区分不同类型，简化接口设计。
- **注册机制**：实现一个可扩展的执行单元注册机制，允许在运行时动态注册新的执行单元类型。

### 1.3 命名空间使用

- 推荐将`WtExeFact`类及相关执行单元类放入专门的命名空间中，例如`WT::ExeUnits`，避免全局命名空间污染。

## 2. 功能优化

### 2.1 执行单元枚举

- `enumExeUnit`方法中只枚举了两种执行单元，但实际上工厂支持五种普通执行单元和一种差量执行单元。应确保枚举方法返回所有支持的执行单元：
  ```cpp
  void WtExeFact::enumExeUnit(FuncEnumUnitCallback cb) {
      cb(FACT_NAME, "WtTWapExeUnit", false);
      cb(FACT_NAME, "WtMinImpactExeUnit", false);
      cb(FACT_NAME, "WtStockMinImpactExeUnit", false);
      cb(FACT_NAME, "WtVWapExeUnit", false);
      cb(FACT_NAME, "WtStockVWapExeUnit", false);
      cb(FACT_NAME, "WtDiffMinImpactExeUnit", true); // 差量执行单元
  }
  ```

### 2.2 配置和初始化

- 增加基于配置文件的执行单元初始化机制，允许通过配置文件动态加载和配置执行单元。
- 提供执行单元的批量创建方法，以支持策略组合场景。

### 2.3 套利执行单元

- 当前套利执行单元（`createArbiExeUnit`）始终返回NULL，建议实现至少一种套利执行单元或者增加明确的警告信息。

## 3. 性能优化

### 3.1 内存管理

- 考虑使用智能指针管理创建的执行单元，防止内存泄漏：
  ```cpp
  std::shared_ptr<ExecuteUnit> createExeUnit(const char* name);
  ```

### 3.2 字符串比较

- `strcmp`函数的使用可能影响性能，特别是在高频调用的场景。考虑使用`std::string`或者`std::string_view`提高字符串比较效率。

### 3.3 对象池

- 对于频繁创建和销毁的执行单元，可以实现对象池来减少内存分配和释放的开销。

## 4. 可维护性和可读性优化

### 4.1 错误处理

- 增强错误处理机制，例如在创建方法中返回详细的错误信息而不仅仅是NULL。
- 考虑使用异常来处理创建失败的情况，使错误更容易被跟踪和处理。

### 4.2 日志和调试支持

- 添加日志记录功能，记录执行单元的创建、删除和使用情况，便于调试和问题追踪。
- 实现执行单元的统计和监控功能，以便于运行时分析性能。

### 4.3 文档和注释

- 为每种执行单元类型添加详细的使用说明和示例代码。
- 将执行单元的参数和配置选项文档化，便于用户正确配置和使用。

## 5. 安全性优化

### 5.1 线程安全

- 确保工厂类的方法在多线程环境下是安全的，特别是创建和删除执行单元的操作。
- 考虑实现线程安全的单例模式，确保工厂实例的唯一性和安全访问。

### 5.2 输入验证

- 在创建执行单元前验证输入参数，包括检查名称是否为NULL或者空字符串。
- 为删除方法增加额外的安全检查，防止重复删除或者删除未创建的执行单元。

## 6. 测试建议

### 6.1 单元测试

- 为WtExeFact类编写全面的单元测试，覆盖所有创建和删除方法。
- 测试边界情况，如创建不存在的执行单元、重复创建和删除等。

### 6.2 集成测试

- 测试WtExeFact与其他组件的交互，确保系统级别的功能正常工作。
- 验证所有执行单元在实际交易环境中的行为是否符合预期。

## 结论

`WtExeFact`类作为执行单元工厂，负责创建和管理不同类型的交易执行单元。通过以上优化建议，可以进一步提升其代码质量、可维护性、性能和安全性。这些建议应根据实际项目需求和资源情况有选择地实施。
