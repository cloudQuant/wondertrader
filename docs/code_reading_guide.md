# WonderTrader 源代码阅读指南

本文档为WonderTrader新手提供一个系统性的源代码阅读路径，按照从基础到进阶的顺序排列关键文件，帮助您循序渐进地理解这个量化交易框架的设计与实现。

## 阅读准备

在开始阅读源代码之前，建议先熟悉以下内容：
- C++编程基础
- 量化交易基本概念
- 金融市场交易规则

## 阅读路径

### 第一阶段：基础接口和数据结构

这一阶段主要了解WonderTrader的基础接口定义和核心数据结构，为后续深入理解实现细节打下基础。

1. **`/src/Includes/WTSDataDef.hpp`**
   - 核心数据结构定义，包括Tick、Bar等行情数据结构
   - 建立对WonderTrader数据体系的基本认识

2. **`/src/Includes/WTSStruct.h`**
   - 基础交易结构体定义
   - 了解委托、成交、持仓等基本概念

3. **`/src/Includes/IDataReader.h`**
   - 数据读取接口定义
   - 了解框架如何访问历史和实时数据

4. **`/src/Includes/IBaseDataMgr.h`**
   - 基础数据管理接口
   - 了解合约、品种、交易所等静态数据的管理方式

5. **`/src/Includes/IHotMgr.h`**
   - 主力合约管理接口
   - 了解期货主力合约的管理机制

6. **`/src/Includes/WTSMarcos.h`**
   - 通用宏定义
   - 了解代码中常用的宏和工具函数

### 第二阶段：策略接口与上下文

这一阶段主要了解WonderTrader提供的各类策略接口和上下文，理解策略开发的基本框架。

7. **`/src/Includes/ICtaStraCtx.h`**
   - CTA策略上下文接口
   - 了解CTA策略的开发接口和回调函数

8. **`/src/Includes/ISelStraCtx.h`**
   - 选股策略上下文接口
   - 了解选股策略的开发接口和回调函数

9. **`/src/Includes/IHftStraCtx.h`**
   - 高频策略上下文接口
   - 了解高频策略的开发接口和回调函数

10. **`/src/Includes/FasterDefs.h`**
    - 性能优化相关定义
    - 了解框架中使用的高性能数据结构

### 第三阶段：核心引擎层

这一阶段深入理解WonderTrader的核心引擎实现，这是框架的核心部分。

11. **`/src/WtCore/WtEngine.h`**
    - 基础引擎类定义
    - 了解引擎的基础结构和功能

12. **`/src/WtCore/WtEngine.cpp`**
    - 基础引擎实现
    - 理解引擎的工作流程和关键方法实现

13. **`/src/WtCore/WtCtaEngine.h`**
    - CTA引擎头文件
    - 了解CTA引擎的特定功能和接口

14. **`/src/WtCore/WtCtaEngine.cpp`**
    - CTA引擎实现
    - 理解CTA策略的管理和执行流程

15. **`/src/WtCore/CtaStraBaseCtx.h`**
    - CTA策略基础上下文头文件
    - 了解CTA策略上下文的具体结构

16. **`/src/WtCore/CtaStraBaseCtx.cpp`**
    - CTA策略基础上下文实现
    - 理解CTA策略的执行机制和接口实现

17. **`/src/WtCore/WtSelEngine.h`**
    - 选股引擎头文件
    - 了解选股引擎的接口和功能

18. **`/src/WtCore/WtSelEngine.cpp`**
    - 选股引擎实现
    - 理解选股策略的管理和执行流程

19. **`/src/WtCore/SelStraBaseCtx.h`**
    - 选股策略基础上下文头文件
    - 了解选股策略上下文的结构

20. **`/src/WtCore/SelStraBaseCtx.cpp`**
    - 选股策略基础上下文实现
    - 理解选股策略的执行机制

21. **`/src/WtCore/WtHftEngine.h`**
    - 高频引擎头文件
    - 了解高频引擎的接口和功能

22. **`/src/WtCore/WtHftEngine.cpp`**
    - 高频引擎实现
    - 理解高频策略的管理和执行流程

23. **`/src/WtCore/HftStraBaseCtx.h`**
    - 高频策略基础上下文头文件
    - 了解高频策略上下文的结构

24. **`/src/WtCore/HftStraBaseCtx.cpp`**
    - 高频策略基础上下文实现
    - 理解高频策略的执行机制

### 第四阶段：数据管理与行情处理

这一阶段学习WonderTrader的数据管理和行情处理模块，了解数据如何流动和存储。

25. **`/src/WtCore/WtDtMgr.h`**
    - 数据管理器头文件
    - 了解数据管理器的接口和功能

26. **`/src/WtCore/WtDtMgr.cpp`**
    - 数据管理器实现
    - 理解历史和实时数据的管理流程

27. **`/src/WtCore/ParserAdapter.h`**
    - 行情解析适配器头文件
    - 了解行情数据的标准化处理

28. **`/src/WtCore/ParserAdapter.cpp`**
    - 行情解析适配器实现
    - 理解不同行情源的统一接入机制

29. **`/src/WtDataStorage/WtDataReader.h`**
    - 数据读取器头文件
    - 了解数据存储和读取接口

30. **`/src/WtDataStorage/WtDataReader.cpp`**
    - 数据读取器实现
    - 理解数据的读取和缓存机制

31. **`/src/WtDataStorage/WtDataWriter.h`**
    - 数据写入器头文件
    - 了解数据写入接口

32. **`/src/WtDataStorage/WtDataWriter.cpp`**
    - 数据写入器实现
    - 理解数据的落地和存储机制

### 第五阶段：交易执行与管理

这一阶段学习WonderTrader的交易执行和管理模块，了解交易指令如何发送和管理。

33. **`/src/WtCore/TraderAdapter.h`**
    - 交易适配器头文件
    - 了解交易接口的统一封装

34. **`/src/WtCore/TraderAdapter.cpp`**
    - 交易适配器实现
    - 理解交易指令的执行流程

35. **`/src/WtCore/WtExecMgr.h`**
    - 执行管理器头文件
    - 了解执行单元的管理接口

36. **`/src/WtCore/WtExecMgr.cpp`**
    - 执行管理器实现
    - 理解执行单元的管理流程

37. **`/src/WtExecMon/WtExecPorter.h src/WtExecMon/WtExecRunner.h src/WtExecMon/WtSimpDataMgr.h`**
    - 执行监控策略头文件
    - 了解执行监控的接口

38. **`/src/WtExecMon/WtExecPorter.cpp src/WtExecMon/WtExecRunner.cpp src/WtExecMon/WtSimpDataMgr.cpp`**
    - 执行监控策略实现
    - 理解执行单元的监控和管理机制

### 第六阶段：风险控制模块

这一阶段学习WonderTrader的风险控制模块，了解风险管理机制。

39. **`/src/WtCore/WtFilterMgr.h`**
    - 过滤管理器头文件
    - 了解信号过滤的接口

40. **`/src/WtCore/WtFilterMgr.cpp`**
    - 过滤管理器实现
    - 理解信号过滤的实现机制

41. **`/src/WtCore/ActionPolicyMgr.h`**
    - 行为策略管理器头文件
    - 了解行为控制的接口

42. **`/src/WtCore/ActionPolicyMgr.cpp`**
    - 行为策略管理器实现
    - 理解行为控制的实现机制

43. **`/src/WtRiskMonFact/WtRiskMonFact.h`**
    - 风控工厂头文件
    - 了解风控模块的工厂类

44. **`/src/WtRiskMonFact/WtRiskMonFact.cpp`**
    - 风控工厂实现
    - 理解风控模块的创建机制

### 第七阶段：回测系统

这一阶段学习WonderTrader的回测系统，了解如何进行策略回测。

45. **`/src/WtBtCore/CtaMocker.h`**
    - CTA回测模拟器头文件
    - 了解CTA策略回测的接口

46. **`/src/WtBtCore/CtaMocker.cpp`**
    - CTA回测模拟器实现
    - 理解CTA策略回测的机制

47. **`/src/WtBtCore/SelMocker.h`**
    - 选股回测模拟器头文件
    - 了解选股策略回测的接口

48. **`/src/WtBtCore/SelMocker.cpp`**
    - 选股回测模拟器实现
    - 理解选股策略回测的机制

49. **`/src/WtBtCore/HftMocker.h`**
    - 高频回测模拟器头文件
    - 了解高频策略回测的接口

50. **`/src/WtBtCore/HftMocker.cpp`**
    - 高频回测模拟器实现
    - 理解高频策略回测的机制

51. **`/src/WtBtCore/WtBtEngine.h`**
    - 回测引擎头文件
    - 了解回测引擎的接口

52. **`/src/WtBtCore/WtBtEngine.cpp`**
    - 回测引擎实现
    - 理解回测引擎的工作流程

### 第八阶段：对外接口

这一阶段学习WonderTrader的对外接口模块，了解框架如何与其他系统交互。

53. **`/src/WtPorter/WtPorter.h`**
    - 对外接口封装头文件
    - 了解对外接口的定义

54. **`/src/WtPorter/WtPorter.cpp`**
    - 对外接口封装实现
    - 理解对外接口的实现机制

55. **`/src/WtPorter/WtRtRunner.h`**
    - 实时运行器头文件
    - 了解实时模式的运行接口

56. **`/src/WtPorter/WtRtRunner.cpp`**
    - 实时运行器实现
    - 理解实时模式的运行机制

57. **`/src/WtPorter/WtBtRunner.h`**
    - 回测运行器头文件
    - 了解回测模式的运行接口

58. **`/src/WtPorter/WtBtRunner.cpp`**
    - 回测运行器实现
    - 理解回测模式的运行机制

### 第九阶段：特定交易接口

这一阶段可以选择性地学习特定交易接口的实现，了解WonderTrader如何对接不同的交易系统。

59. **`/src/TraderCTP/TraderCTP.h`**
    - CTP交易接口头文件
    - 了解CTP交易接口的实现

60. **`/src/TraderCTP/TraderCTP.cpp`**
    - CTP交易接口实现
    - 理解CTP交易的细节

61. **`/src/ParserCTP/ParserCTP.h`**
    - CTP行情解析器头文件
    - 了解CTP行情数据的解析

62. **`/src/ParserCTP/ParserCTP.cpp`**
    - CTP行情解析器实现
    - 理解CTP行情的处理细节

其他类似的交易接口也可按需研究，如XTP、FEMAS等。

## 阅读建议

1. **循序渐进**：按照上述顺序阅读，先建立整体概念，再深入细节。

2. **结合示例**：阅读代码时，结合示例项目和测试案例，帮助理解实际用法。

3. **动手实践**：尝试修改和扩展简单功能，加深理解。

4. **问题记录**：阅读过程中记录问题，通过社区或文档解决。

5. **画图辅助**：可以绘制类图和时序图，帮助理解组件间的关系和流程。

6. **注重接口**：先理解接口，再研究实现，这样更容易把握整体架构。

7. **分阶段记录**：每完成一个阶段，记录自己的理解和疑问，以便后续解决。

通过系统性地阅读这些源代码文件，您将能够全面理解WonderTrader的架构设计和实现细节，为后续的使用和扩展打下坚实基础。
