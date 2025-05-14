/**
 * @mainpage WonderTrader项目文档
 * 
 * @section intro_sec 项目介绍
 * 
 * WonderTrader是一个高性能量化交易平台，专为中国金融市场设计，支持期货、股票等多种金融工具。
 * 该平台提供了完整的量化交易解决方案，包括数据管理、策略开发、回测分析、实盘交易等功能。
 * 
 * @section architecture 系统架构
 * 
 * WonderTrader采用模块化设计，主要包含以下核心组件：
 * 
 * - **核心引擎** - 提供交易核心功能和事件处理
 * - **执行引擎** - 负责交易指令的执行和跟踪
 * - **数据引擎** - 处理行情数据和历史数据
 * - **策略引擎** - 支持策略开发和执行
 * - **回测引擎** - 支持策略回测和性能分析
 * 
 * @image html architecture.png "WonderTrader系统架构图"
 * 
 * @section modules 核心模块
 * 
 * @subsection core_engine 核心引擎
 * 
 * 核心引擎是整个系统的中枢，负责：
 * - 事件管理和调度
 * - 订单流转和管理
 * - 风控规则执行
 * - 数据分发和同步
 * 
 * 相关组件：WtCore、WtEngine
 * 
 * @subsection exec_engine 执行引擎
 * 
 * 执行引擎负责交易指令的执行和状态管理：
 * - 订单拆分和优化
 * - 执行算法支持
 * - 订单状态跟踪
 * - 多通道执行管理
 * 
 * 相关组件：WtExecMgr、WtExecuter
 * 
 * @subsection data_engine 数据引擎
 * 
 * 数据引擎负责处理各类市场数据：
 * - 实时行情处理
 * - 历史数据管理
 * - 数据存储和缓存
 * - 数据适配和转换
 * 
 * 相关组件：WtDataStorage、WtDtHelper
 * 
 * @subsection strat_engine 策略引擎
 * 
 * 策略引擎支持多种类型的交易策略：
 * - CTA策略
 * - HFT高频策略
 * - SEL选股策略
 * 
 * 相关组件：WtStraFact、WtStrategies
 * 
 * @section usage 使用指南
 * 
 * @subsection build_guide 编译指南
 * 
 * WonderTrader支持Windows和Linux平台，编译步骤如下：
 * 
 * **Linux平台**：
 * ```bash
 * cd src
 * cmake .
 * make
 * ```
 * 
 * @subsection deployment 部署指南
 * 
 * 系统部署架构分为以下模式：
 * - 单机部署
 * - 分布式部署
 * - 混合部署
 * 
 * @section api_reference API参考
 * 
 * 详细API文档请参考各模块的接口说明：
 * - [核心引擎API](modules.html)
 * - [策略开发接口](annotated.html)
 * - [数据访问接口](files.html)
 * 
 */
