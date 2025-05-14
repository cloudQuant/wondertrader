/**
 * @page exec_engine 执行引擎
 * 
 * @section exec_overview 执行引擎概述
 * 
 * 执行引擎(WtExecMgr)是WonderTrader中负责交易指令执行的关键组件，它连接策略生成的交易信号与实际市场订单。
 * 执行引擎通过优化算法和执行策略，确保交易指令以最优方式在市场中执行。
 * 
 * @section exec_components 主要组件
 * 
 * - **WtExecuterMgr** - 执行管理器，负责协调多个执行器的工作
 * - **WtExecuter** - 执行器基类，提供执行算法的基础架构
 * - **WtExecPolicy** - 执行策略，定义执行的细节逻辑
 * - **ExecContext** - 执行上下文，维护执行状态和环境
 * 
 * @section exec_features 核心功能
 * 
 * 1. **订单拆分** - 将大订单拆分为小订单，减少市场冲击
 * 2. **时间分配** - 根据时间分布执行订单
 * 3. **价格优化** - 通过市场行为分析优化下单价格
 * 4. **执行跟踪** - 实时跟踪订单执行情况
 * 5. **多执行器管理** - 支持多种执行算法并存
 * 
 * @section exec_algos 执行算法
 * 
 * 执行引擎支持多种执行算法，包括：
 * 
 * - **TWAP** - 时间加权平均价格算法
 * - **VWAP** - 成交量加权平均价格算法
 * - **Iceberg** - 冰山算法，隐藏大单实际规模
 * - **Sniper** - 狙击手算法，等待最优执行机会
 * - **DMA** - 直接市场访问算法
 * 
 * @section exec_interfaces 关键接口
 * 
 * - `enum_executer()` - 遍历执行器
 * - `set_positions()` - 设置持仓数据
 * - `handle_pos_change()` - 处理持仓变化
 * - `handle_tick()` - 处理行情数据
 * - `add_target_to_cache()` - 添加目标到缓存
 * - `commit_cached_targets()` - 提交缓存的目标
 * 
 * @see WtExecuterMgr
 * @see WtExecuter
 */
