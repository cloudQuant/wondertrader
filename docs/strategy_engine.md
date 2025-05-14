/**
 * @page strategy_engine 策略引擎
 * 
 * @section strat_overview 策略引擎概述
 * 
 * 策略引擎是WonderTrader的核心组件之一，负责管理和执行各类交易策略。它为策略提供运行环境，
 * 实现了策略与底层引擎的解耦，使策略开发人员能够专注于交易逻辑而非底层细节。
 * 
 * @section strat_types 策略类型
 * 
 * WonderTrader支持多种类型的策略：
 * 
 * - **CTA策略** - 商品交易顾问策略，适用于期货、外汇等趋势交易
 * - **HFT策略** - 高频交易策略，专注于低延迟和高频执行
 * - **SEL策略** - 选股策略，主要用于股票、ETF等证券品种的选择和组合
 * 
 * @section strat_components 主要组件
 * 
 * - **SelStrategyMgr** - 选股策略管理器
 * - **HftStrategyMgr** - 高频策略管理器
 * - **CtaStrategyMgr** - CTA策略管理器
 * - **ISelStrategy** - 选股策略接口
 * - **IHftStrategy** - 高频策略接口
 * - **ICtaStrategy** - CTA策略接口
 * 
 * @section strat_features 核心功能
 * 
 * 1. **策略加载** - 动态加载并初始化策略
 * 2. **上下文管理** - 为策略维护专用的执行上下文
 * 3. **信号处理** - 处理策略生成的交易信号
 * 4. **数据推送** - 向策略推送市场数据
 * 5. **绩效统计** - 统计策略运行绩效
 * 
 * @section strat_dev 策略开发
 * 
 * 开发WonderTrader策略的基本步骤：
 * 
 * 1. 继承相应的策略基类（ICtaStrategy、IHftStrategy或ISelStrategy）
 * 2. 实现必要的接口方法（on_init、on_tick、on_bar等）
 * 3. 在策略中使用上下文提供的API进行交易操作
 * 4. 编译策略为动态库
 * 5. 通过配置文件加载策略
 * 
 * @section strat_interfaces 关键接口
 * 
 * **CTA策略接口**：
 * - `on_init()` - 策略初始化
 * - `on_tick()` - 处理Tick数据
 * - `on_bar()` - 处理K线数据
 * - `on_calculate()` - 执行策略计算
 * 
 * **HFT策略接口**：
 * - `on_init()` - 策略初始化
 * - `on_tick()` - 处理Tick数据
 * - `on_order_queue()` - 处理委托队列
 * - `on_order_detail()` - 处理委托明细
 * - `on_transaction()` - 处理逐笔成交
 * 
 * **SEL策略接口**：
 * - `on_init()` - 策略初始化
 * - `on_schedule()` - 定时调度
 * - `on_tick()` - 处理Tick数据
 * - `on_bar()` - 处理K线数据
 * 
 * @see ICtaStrategy
 * @see IHftStrategy
 * @see ISelStrategy
 */
