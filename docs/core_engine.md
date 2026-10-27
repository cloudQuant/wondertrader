/**
 * @page core_engine 核心引擎
 * 
 * @section core_overview 核心引擎概述
 * 
 * 核心引擎（WtCore）是WonderTrader的中央处理系统，负责各种交易逻辑的调度和执行。
 * 它连接了行情数据、策略、执行系统等多个模块，是整个系统的枢纽。
 * 
 * @section core_components 主要组件
 * 
 * - **WtEngine** - 基础引擎类，提供通用执行环境
 * - **WtCtaEngine** - CTA策略执行引擎，支持期货、股票等品种的CTA交易
 * - **WtHftEngine** - 高频交易执行引擎，支持低延迟交易策略
 * - **WtSelEngine** - 选股策略执行引擎，主要针对股票、基金等品种
 * 
 * @section core_features 核心功能
 * 
 * 1. **统一事件调度**：处理市场数据、交易信号等各类事件
 * 2. **风险控制**：内置风险控制功能，支持绑定外部风控系统
 * 3. **交易管理**：管理订单流、仓位、资金等核心交易要素
 * 4. **数据分发**：向策略分发处理后的行情数据
 * 5. **指标计算**：提供常用技术指标的计算功能
 * 
 * @section core_architecture 内部架构
 * 
 * 核心引擎采用事件驱动模型，主要由以下几个部分组成：
 * 
 * - **事件队列**：接收并管理各类事件
 * - **调度器**：按优先级调度事件处理
 * - **上下文管理**：为各类策略维护执行上下文
 * - **数据缓存**：缓存关键市场数据，提高访问效率
 * 
 * @section core_interfaces 关键接口
 * 
 * - `init()` - 引擎初始化
 * - `run()` - 启动引擎运行
 * - `push_quote()` - 推送行情数据
 * - `on_order()` - 订单回报处理
 * - `on_trade()` - 成交回报处理
 * 
 * @see WtEngine
 * @see WtCtaEngine
 * @see WtHftEngine
 * @see WtSelEngine
 */
