# 缺陷记录复核 — N06 策略工厂 / WtShareHelper / WtMsgQue

> 复核范围：对照当前实现、公开接口、Coverage CMake 装配和测试源码。所有 `DISABLED_` 用例均未执行；未经动态验证的结论标为静态发现或待契约确认。

## BUG-WSN06-001 — 策略工厂未处理空 name

- 状态: 静态参数边界风险确认；调用方契约及动态证据不足
- 严重级别: Minor（当前调用链可达性未证明）
- 模块/文件: `src/WtCtaStraFact/WtCtaStraFact.cpp`、`src/WtCtaTwoMaFact/WtCtaTwoMaFact.cpp`、`src/WtSelStraFact/WtSelStraFact.cpp`、`src/WtUftStraFact/WtUftStraFact.cpp`、`src/WtHftStraFact/WtHftStraFact.cpp` 的 `createStrategy`
- 接口依据: 各 factory 接口声明 `createStrategy(const char* name, ...)`，没有明确规定 `name == NULL` 是合法输入，也没有明确声明非空前置条件；工厂注释只规定不支持的名称返回空指针。
- 源码事实: 五处实现都在判空前调用 `strcmp(name, ...)`，直接传空指针会触发未定义行为。
- 测试事实: `n06_cta_stra_test.cpp` 与 `n06_sel_uft_hft_test.cpp` 的相关 `DISABLED_` 用例未执行；因此没有稳定崩溃的动态证据。策略管理器的正常配置路径先拆分名称，直接 factory API 的空指针可达性尚未证实。
- 结论: 源码缺少空指针处理属实，但需先定接口契约；不能按当前证据定为 Blocker 或声称配置驱动调用已打崩进程。
- 修复方向: 若接口允许非法/缺失名称进入 factory，入口判空并返回 `NULL`；若要求非空，则在接口和 manager 边界记录、验证此前置条件。

## BUG-WSN06-002 — UFT/HFT 演示策略 init 未处理空 cfg

- 状态: 静态问题确认；缺失 params 的动态结果未验证
- 严重级别: Major（活动策略配置缺少 `params` 时）
- 模块/文件: `src/WtUftStraFact/WtUftStraDemo.cpp` `init`；`src/WtHftStraFact/WtHftStraDemo.cpp` `init`
- 源码事实: 两个实现直接对 `cfg` 调用 `getCString/getUInt32/...`；与 `WtStraTwoMa/WtStraDualThrust/WtStraDtSel::init` 的显式空指针检查不同。
- 调用链依据: `WtRunner.cpp` 和 `WtUftRunner.cpp` 对已创建的活动策略直接调用 `init(cfgItem->get("params"))`，没有检查 `params` 是否存在，也没有使用 init 返回值。因此缺少 `params` 时可以把空指针传给这两个实现；调用链在源码上可达。
- 测试事实: `n06_sel_uft_hft_test.cpp` 的空 cfg 输入在禁用用例中；正常用例只测了非空对象。未运行测试，也未声称已动态复现。
- 修复方向: 在两个 init 入口判空返回 `false`；runner 同时处理 init 失败，避免无效配置继续构造/挂接策略上下文。

## BUG-WSN06-003 — 工厂符号冲突与当前 Coverage 目标装配

- 状态: 已撤回（原“当前 CoverageTests 重复链接 Blocker”结论不成立；覆盖限制另行记录）
- 严重级别: 不适用（当前目标未证实失败）
- 涉及文件: 五个工厂实现；`src/TestUnits/coverage/CMakeLists.txt` 336–398；`src/TestUnits/coverage/n06_strategies_mq_share/n06_cta_stra_test.cpp`
- 静态装配事实: `CoverageTests_ctafact` 仅编入 `WtCtaTwoMaFact.cpp`、`WtStraTwoMa.cpp` 和 `WtStraDualThrust.cpp`，没有编入 `WtCtaStraFact.cpp`，故该目标不存在两份 CTA `WtStraFact` 工厂实现或两份 CTA `createStrategyFact` 定义。生产 CMake 也把两个 CTA 插件分别构建为共享库。`CoverageTests_selfthft` 对 UFT/HFT 的 `FACT_NAME` 与同名 C 入口使用编译定义重命名，当前目标配置规避了这些链接冲突。
- 测试事实: `n06_cta_stra_test.cpp` 引入的是 TwoMa 工厂头；其 `WtStraFact` 只支持 `TwoMa`。`createStrategy("DualThrust")` 在当前目标中返回空，`hits == 1` 断言可通过，但并未覆盖 DualThrust 工厂。该目标还把 DualThrust 策略 TU 与 TwoMa 工厂提供的唯一 `FACT_NAME` 同链，因此本测试二进制中的 DualThrust `getFactName()` 不能代表独立插件身份；现有断言只检查非空，不会因此失败。
- 结论: 源码若被人为把多个工厂 TU 合并到一个二进制，确有同名类/符号冲突风险；但当前 CMake 不是该装配，未发现当前构建失败。测试源码头部“linker 选中其中一个”与 CMake 实际装配不符，是注释勘误/覆盖边界，不是错误断言或现有失败。
- 修复方向: 本项不要求产品代码修复。若后续要覆盖 `WtCtaStraFact`，单独增加专属测试目标或通过实际插件动态加载验证；这是覆盖建设，需单独立项，不能计为当前失败。

## BUG-WSN06-004 — CTA 样例策略未验证 days 与历史数据长度

- 状态: 静态参数边界问题确认；交易结果未动态验证
- 严重级别: Major（非法参数可能使计算失效，具体影响需策略验收）
- 模块/文件: `src/WtCtaTwoMaFact/WtStraTwoMa.cpp` `init/on_schedule`；`src/WtCtaStraFact/WtStraDualThrust.cpp` `init/on_schedule`
- 源码事实: 两个 init 都接受 `days == 0`。TwoMa 的 `on_schedule` 对 `short_days/long_days` 作除法且不检查零值；零值时循环为空，当前式的分子为 0，默认 IEEE 浮点下结果为 NaN（不是原记录所说的“除零得到 inf”）。其 `WTSValueArray::at` 对越界返回 `INVALID_DOUBLE`，TwoMa 未检查该哨兵，故 K 线少于所需历史长度时极值会进入计算；这不是越界内存访问，具体信号结果未验证。DualThrust 的 `maxprice(0, -2)` 在负索引转为从切片起点到倒数第二项的窗口，窗口随返回切片长度变化，不会因为 `days == 0` 自动代表零天；其业务语义是否错误取决于 days 的约定。
- 测试事实: `n06_cta_ctx_mock_test.cpp` 的 `DISABLED_ZERO_PERIOD_DAYS_DIVIDE_CLASS` 传入两个零值和 5 根 K 线，但无交易结果断言，且用例禁用。它没有覆盖数据条数不足；不能支持“产生随机信号”或“DualThrust 已验证错误窗口”的结论。
- 修复方向: 明确并校验 `days > 0`；TwoMa 在访问前校验实际切片长度满足 `max(short_days,long_days)+1`，并拒绝/跳过含 `INVALID_DOUBLE` 的计算；DualThrust 明确零值和最小窗口语义。新增启用态数值断言后再定级影响。

## BUG-WSN06-005 — WtMsgQue 创建接口丢弃 init 结果，失败返回语义未定义

- 状态: 源码事实确认；接口契约不足以认定返回值违约
- 严重级别: Minor（失败句柄可能被误认为可用）
- 模块/文件: `src/WtMsgQue/MQManager.cpp` `create_server/create_client`；`src/WtMsgQue/MQServer.cpp`、`MQClient.cpp`；`src/WtMsgQue/WtMsgQue.h`
- 源码事实: manager 忽略 `init` 的 bool 结果，仍注册实例并返回分配的非零 id。`MQServer::init` 在 bind 失败时关闭 socket 并返回 false；对应 `_sock < 0` 时 `publish` 只记录未初始化并返回。公开头文件只描述创建返回新 id，没有规定失败返回 0 或其它哨兵。因此可以确认失败对象仍有 id，但不能把“必须返回 0”当作既有接口契约。
- 测试事实: `invalid_url_server_survives_and_cleanup` 调用非法 scheme 后 publish/destroy，但既不检查返回 id，也不确认 `init` 是否失败，只能作为安全调用尝试；用例未执行。它没有证明返回语义或失败路径实际发生。
- 空 URL 子项: `MQServer::init` 在调用 `nn_bind` 前执行 `_url = url`；`MQClient::init` 在 `nn_connect` 前执行 `m_strURL = url`。传空指针违反 `std::string` 输入前置条件；相关测试禁用，无法据此声称 nanomsg 稳定段错误。
- 修复方向: 先定义公开 API 的失败返回/句柄契约，再让 manager 依据 init 结果决定是否注册及如何报告失败；若 URL 非空是前置条件，应在 C API 边界显式检查。

## BUG-WSN06-006 — lane 派工文本中的文件/符号名

- 状态: 已撤回（非产品代码缺陷；外部派工原文无法复核）
- 严重级别: 不适用
- 模块/文件: 派工简报（仓库中未提供原始简报）
- 源码事实: 当前源码由 `WtShareHelper/ShareBlocks.h/.cpp` 的 `shareblock::ShareBlocks` 与 `WtShareHelper.h/.cpp` 的 C 接口组成；在源码目录没有找到 `shareblock_helper.h` 或 `CSplitStringPool`。仓库中的 manifest 和本记录本身包含这些待核对词，因此“全仓不存在”的表述过宽。
- 结论: 仓库源码只能确认实际文件布局，不能验证仓库外派工文本原文是否写错。此项应从产品缺陷计数中分离。
- 修复方向: 若需追踪派工勘误，补充原始简报路径/版本并更新该任务文本；不涉及生产代码修复。
