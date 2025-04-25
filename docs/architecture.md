# WonderTrader 架构文档

## 1. 概述

WonderTrader是一个基于C++核心模块的，适应全市场全品种交易的，高效率、高可用的量化交易开发框架。其核心设计理念是将数据源、策略计算和交易执行分离，形成"M+1+N"架构，即：M个数据源、1个策略引擎和N个交易通道。

WonderTrader框架具有以下主要特点：
- 高效的C++核心
- 多引擎支持（CTA、SEL、HFT、UFT）
- 策略和执行分离
- 组合式管理
- 完整的回测支持
- 灵活的风险控制

## 2. 系统架构

### 2.1 整体架构图

```
+------------------+    +------------------+    +------------------+
| 行情数据源(Parser) |    | 基础数据管理(DataMgr)|    | 风险管理(RiskMon) |
+--------+---------+    +--------+---------+    +--------+---------+
         |                      |                       |
         v                      v                       v
+------------------------------------------------------------------+
|                       基础引擎(WtEngine)                           |
+------------------------------------------------------------------+
         |                      |                       |
         v                      v                       v
+------------------+    +------------------+    +------------------+
|  CTA引擎(同步)     |    |  SEL引擎(异步)    |    |  HFT/UFT引擎(高频)|
+--------+---------+    +--------+---------+    +--------+---------+
         |                      |                       |
         v                      v                       v
+------------------------------------------------------------------+
|                     执行管理器(WtExecMgr)                          |
+------------------------------------------------------------------+
         |                      |                       |
         v                      v                       v
+------------------+    +------------------+    +------------------+
| 交易通道1(Adapter) |    | 交易通道2(Adapter) |    | 交易通道N(Adapter) |
+------------------+    +------------------+    +------------------+
```

### 2.2 运行架构

WonderTrader的实盘运行架构如下：

- 行情数据通过UDP广播实现1对多的数据分发
- 可以同时运行多个策略组合
- 多个交易账户可以复用同一个策略组合
- 支持多级的风控机制

## 3. 核心组件

### 3.1 引擎组件

WonderTrader提供了多种不同类型的策略引擎，适用于不同的交易场景：

#### 3.1.1 WtEngine - 基础引擎

`WtEngine`是所有策略引擎的基类，提供了基础的数据访问、时间管理、风控接口等功能。主要职责包括：
- 管理当前日期、时间和交易日
- 提供基础数据查询接口
- 处理底层行情推送
- 集成风控模块

```cpp
class WtEngine : public WtPortContext, public IParserStub
{
    // 主要功能：
    // 1. 设置和获取当前时间日期
    // 2. 获取基础数据和合约信息
    // 3. 处理行情推送
    // 4. 设置风控模块
    // 5. 管理交易适配器
};
```

#### 3.1.2 WtCtaEngine - CTA策略引擎

`WtCtaEngine`是针对CTA类策略的引擎实现，适用于标的较少、计算逻辑较快的策略，例如单标的择时或中频以下的套利等。特点是事件驱动+时间驱动的混合模式。

```cpp
class WtCtaEngine : public WtEngine, public IExecuterStub
{
    // 主要功能：
    // 1. 管理CTA策略上下文
    // 2. 分发行情数据
    // 3. 处理交易信号
    // 4. A执行调度任务
};
```

#### 3.1.3 WtSelEngine - 选股策略引擎

`WtSelEngine`是针对选股类策略的引擎实现，适用于标的较多、计算逻辑复杂的策略，例如多因子选股策略、截面多空策略等。采用异步时间驱动模式。

```cpp
class WtSelEngine : public WtEngine
{
    // 主要功能：
    // 1. 管理选股策略上下文
    // 2. 执行异步计算调度
    // 3. 处理多标的信号合并
};
```

#### 3.1.4 WtHftEngine - 高频策略引擎

`WtHftEngine`是针对高频策略的引擎实现，主要针对高频或低延时策略，完全事件驱动，系统延迟在1-2微秒之间。

```cpp
class WtHftEngine : public WtEngine
{
    // 主要功能：
    // 1. 管理高频策略上下文
    // 2. 提供低延迟的行情分发
    // 3. 直接连接交易接口
};
```

#### 3.1.5 WtUftEngine - 极速策略引擎

`WtUftEngine`是针对超高频或超低延时策略的引擎实现，完全事件驱动，系统延迟在200纳秒之内。为了追求极致性能，该引擎独立于WtCore项目，不向应用层提供接口，全部在C++层面实现。

### 3.2 策略上下文

WonderTrader为每种策略类型提供了对应的上下文类，用于管理策略运行环境和提供策略接口：

#### 3.2.1 CtaStraBaseCtx - CTA策略上下文

```cpp
class CtaStraBaseCtx : public ICtaStraCtx
{
    // 主要功能：
    // 1. 提供策略回调接口(on_init, on_tick, on_bar等)
    // 2. 提供策略操作接口(开仓、平仓、获取数据等)
    // 3. 管理策略持仓和信号
    // 4. 记录策略日志和交易记录
};
```

#### 3.2.2 SelStraBaseCtx - 选股策略上下文

```cpp
class SelStraBaseCtx : public ISelStraCtx
{
    // 主要功能：
    // 1. 提供异步调度回调(on_schedule)
    // 2. 提供多标的管理接口
    // 3. 管理标的池和权重
};
```

#### 3.2.3 HftStraBaseCtx - 高频策略上下文

```cpp
class HftStraBaseCtx : public IHftStraCtx, public ITrdNotifySink
{
    // 主要功能：
    // 1. 提供高频数据回调(逐笔成交、委托队列等)
    // 2. 提供低延迟下单接口
    // 3. 处理实时订单反馈
};
```

### 3.3 数据管理组件

#### 3.3.1 WtDtMgr - 数据管理器

`WtDtMgr`负责管理策略所需的历史数据和实时数据，包括K线、Tick等。

```cpp
class WtDtMgr
{
    // 主要功能：
    // 1. 初始化和管理数据存储
    // 2. 提供K线和Tick数据访问接口
    // 3. 管理实时数据缓存
};
```

#### 3.3.2 ParserAdapter - 行情适配器

`ParserAdapter`负责对接不同的行情数据源，将它们统一转换为WonderTrader内部格式。

```cpp
class ParserAdapter : public IParserApi
{
    // 主要功能：
    // 1. 连接行情数据源
    // 2. 转换和标准化行情数据
    // 3. 分发行情数据到引擎
};
```

### 3.4 交易执行组件

#### 3.4.1 TraderAdapter - 交易适配器

`TraderAdapter`负责对接各种交易接口，执行实际的交易操作。

```cpp
class TraderAdapter : public ITraderApi
{
    // 主要功能：
    // 1. 连接交易接口
    // 2. 执行委托下单
    // 3. 查询账户和持仓
    // 4. 管理订单状态
};
```

#### 3.4.2 WtExecMgr - 执行管理器

`WtExecMgr`负责管理多个交易通道，并根据信号分配执行任务。

```cpp
class WtExecMgr
{
    // 主要功能：
    // 1. 管理多个交易适配器
    // 2. 实现交易路由规则
    // 3. 管理委托指令队列
};
```

### 3.5 风控组件

#### 3.5.1 WtRiskMonitor - 风险监控器

`WtRiskMonitor`负责监控各种风险指标，在必要时干预交易操作。

```cpp
class WtRiskMonitor
{
    // 主要功能：
    // 1. 检查交易风险
    // 2. 监控账户资金
    // 3. 控制交易频率
    // 4. 实现风控规则
};
```

#### 3.5.2 ActionPolicyMgr - 行为策略管理器

`ActionPolicyMgr`负责管理各种交易行为策略，例如下单频率控制、市场状态检查等。

```cpp
class ActionPolicyMgr
{
    // 主要功能：
    // 1. A管理交易行为策略
    // 2. 判断是否允许指定行为
    // 3. 记录行为日志
};
```

## 4. 目录结构

WonderTrader的源代码主要目录结构如下：

```
src/
├── API/                  # 第三方交易API接口
├── Common/               # 公共基础组件
├── Includes/             # 头文件和接口定义
├── Share/                # 共享工具函数和类
├── WtCore/               # 核心引擎实现
│   ├── WtEngine.h/cpp    # 基础引擎
│   ├── WtCtaEngine.h/cpp # CTA引擎
│   ├── WtHftEngine.h/cpp # 高频引擎
│   ├── WtSelEngine.h/cpp # 选股引擎
│   └── ...
├── WtBtCore/             # 回测核心引擎
├── WtDataStorage/        # 数据存储模块
├── WtExecMon/            # 执行监控模块
├── WtMsgQue/             # 消息队列模块
├── WtPorter/             # 对外接口封装
├── TraderXXX/            # 不同交易接口的实现
├── ParserXXX/            # 不同行情解析器的实现
└── WtRiskMonFact/        # 风控工厂模块
```

## 5. 数据流程

### 5.1 行情数据流程

1. 外部行情源 -> ParserXXX接口 -> ParserAdapter -> WtEngine的行情回调
2. WtEngine处理行情数据 -> 缓存到WtDtMgr -> 分发给对应的策略引擎
3. 策略引擎调用策略上下文的on_tick/on_bar回调 -> 策略计算 -> 产生信号

### 5.2 交易信号流程

1. 策略上下文产生信号 -> 传递给策略引擎
2. 策略引擎合并同一标的的信号 -> 经过风控检查
3. 通过WtExecMgr分配到不同的交易通道
4. TraderAdapter执行实际交易操作
5. 交易反馈回调到策略上下文

## 6. 策略开发流程

### 6.1 CTA策略开发

1. 继承CtaStrategy基类
2. 实现on_init, on_tick, on_bar等回调函数
3. 使用stra_xxx系列接口进行交易操作和数据查询
4. 在配置文件中注册策略，设置参数

### 6.2 选股策略开发

1. 继承SelStrategy基类
2. 实现on_init和on_schedule回调函数
3. 在on_schedule中完成选股逻辑，设置目标头寸
4. 在配置文件中设置调度周期和参数

### 6.3 高频策略开发

1. 继承HftStrategy基类
2. 实现on_tick, on_order_queue, on_transaction等回调
3. 使用stra_buy/stra_sell等接口进行下单操作
4. 处理订单状态回调，实现高频交易逻辑

## 7. 回测系统

WonderTrader提供了完善的回测支持：

1. 所有策略类型（CTA、SEL、HFT、UFT）统一在相同的回测引擎中回测
2. 回测引擎用C++实现，保证高效率
3. 支持多品种、多周期的回测
4. 详细的回测报告和绩效分析
5. 支持Python和C++两种语言开发的策略回测

## 8. 实用工具

### 8.1 wtpy - Python封装

wtpy是构建在WonderTrader核心模块之上的Python封装，提供了更友好的Python接口。

```python
# 创建CTA策略示例
from wtpy import WtEngine, CtaContext

class MyStrategy:
    def __init__(self, name):
        self.name = name
    
    def on_init(self, context: CtaContext):
        print("策略初始化")
    
    def on_tick(self, context: CtaContext, code, tick):
        print(f"收到{code}的Tick数据")
    
    def on_bar(self, context: CtaContext, code, period, bar):
        print(f"收到{code}的K线数据")
```

### 8.2 WtMonSvr - 监控服务

WtMonSvr是wtpy内置的监控服务组件，提供了以下功能：
- 远程Web界面监控
- 策略组合运行情况实时监控
- 全天24x7的自动调度服务
- 事件通知推送

## 9. 常见应用场景

### 9.1 团队内控

- 不同投研人员的策略组合运行
- C++级别的策略保密性
- 独立的绩效核算

### 9.2 多账户交易

- M+1+N执行架构
- 不同账户设置不同的手数倍率
- 独立的账户风控

### 9.3 多标的跟踪

- 高效的C++核心
- 数据伺服支持多组合共享
- 信号执行和策略计算分离

### 9.4 大计算量策略

- SEL引擎支持异步计算
- 定时触发重算
- 支持多标的调整目标仓位

### 9.5 极速交易

- HFT引擎延迟1-2微秒
- UFT引擎延迟200纳秒内
- 针对极速交易场景优化

### 9.6 算法交易

- 独立的执行器入口模块WtExecMon
- 可扩展的算法执行单元
- 高效的C++底层保障执行效果

## 10. 支持的交易接口

### 10.1 期货
- CTP
- CTPMini
- 飞马Femas
- 艾克朗科(仅组播行情)
- 易达

### 10.2 期权
- CTPOpt
- 金证期权maOpt
- QWIN二开

### 10.3 股票
- 中泰XTP
- 中泰XTPXAlgo
- 华鑫奇点
- 华锐ATP
- 宽睿OES

## 11. 总结

WonderTrader是一个功能强大、架构清晰的量化交易框架，其核心优势在于：

1. **高效的C++核心**：保证了低延迟和高性能
2. **模块化设计**：策略、数据和执行分离，便于扩展和维护
3. **多引擎支持**：适应各种交易场景和策略类型
4. **完整的生态**：从数据采集、策略开发到实盘交易全流程覆盖
5. **强大的风控**：多级风控机制保障交易安全

通过深入理解WonderTrader的架构，开发者可以更好地利用这个框架开发自己的量化交易系统。
