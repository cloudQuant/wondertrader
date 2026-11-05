# I03-NEW-008 / 卡13d：实际导出文件与回测费用消费

## 当前合同与用例（2026-11-05 最终验收更新）

卡13d=A：合约级费率优先、品种级回退；缺费是错误，显式零费率有效；费用按实际成交合约计算。回测 `HisDataReplayer` 用 `wt_fee::Schedule` 严格解析费用文件（无版本文件只解释为品种表，每个条目必须有 open/close/closetoday/byvolume），费用计算需要已登记的物理合约元数据。CTPLoader 导出尚未迁移（I03-NEW-008 仍阻塞于导出端），仍输出无版本、`交易所.合约` 为键的对象。

`exported-fees.json`、`exported-commodities.json`、`exported-contracts.json` 是 `t26_fees_atomic` normal-valid 场景的同一次实际回调输出（`exported-contracts.json` 于 2026-11-05 在最终冻结树上取自该场景，前两份与当时字节一致）。四个用例使用真实 `HisDataReplayer::init()`、`prepare_fee_context()` 与 `calc_fee()`，各自独立进程，全部应通过：

1. `UnmigratedExportedContractKeysAreAnObservableMissingFee`：未迁移的导出文件可以初始化，但其合约键不是合约规则，`SHFE.au.2512` 的费用必须是可观察的错误（`NaN`、上下文准备失败且不改写输出），不再是旧实现的静默 0。
2. `VersionedContractVolumeFeeIsUsedForThePhysicalContract`：同一组导出费率写成 v2 `contracts` 条目后，按物理合约 `SHFE.au2512` 计费（按手 -0.25，2 手 = -0.5）；导出的 `SHFE.rb2510` 显式零费率是合法的 0。
3. `ContractAmountFeeRequiresPhysicalContractMetadata`：合约级按金额费率在缺少合约元数据时初始化失败；加载导出的合约后按合约计费（价格×数量×合约乘数×费率）。
4. `LegacyProductAmountFeeStillWorks`：完整的旧品种键按金额费率经品种回退计费。

导出的 commodity 中两个 session 为空字符串，脚本只在测试副本中替换为 `SD0930`，以排除商品未加载因素；这不等于真实 CTP 生产配置。

```sh
python3 -I src/TestUnits/fixtures/i03_fee_consumer_pipeline/run.py <新输出目录> <coverage 构建目录>
```

须从仓库外的源码副本运行；构建目录必须包含 `CoverageTests_btcore` 的对象。脚本用该构建记录的 `l06_replayer_runtime_test.cpp` 编译命令（去掉依赖文件选项，不向构建目录写入）编译本 TU，用 `CoverageTests_btcore` 的链接命令另行链接，并先核对 HisDataReplayer.cpp/.h、CodeHelper.hpp、FeeSchedule.hpp 与构建所用源码一致。普通编译，未执行 sanitizer、真实 provider 或热切换。

原 2026-11-02 版本是“现存缺陷反例”（期望 [1,1,0]）：卡13d 实现后旧反例的前提已变化——旧断言 `calc_fee != 0` 在缺费返回 NaN 后会空泛通过，旧品种正控的不完整条目被严格解析拒绝——因此按合同改写为上述正向用例，不再作为“已知红”。下文保留原记录。

## 原记录（2026-11-02，已过期）


本目录没有修复产品代码。`exported-fees.json` 和 `exported-commodities.json` 是 `t26_fees_atomic` 的 normal-valid 实际回调输出；它的 `SHFE.au2512` 按手开仓费率为 -0.25，`SHFE.rb2510` 费率为 0。三个用例使用真实 `HisDataReplayer::init()`、`loadFees()` 和 `calc_fee()`，不运行策略或真实交易。

1. 按手合约键：真实导出费用文件可使回放器初始化，但查询 `SHFE.au.2512` 返回 0，断言非零失败（exit 1）。测试不使用旧实现对负数的四舍五入结果作为预期。
2. 按额合约键：仅将合约键构造成按金额收费，初始化按键直接查找商品，失败（exit 1）。
3. 旧品种键：同一金额费率使用 `SHFE.au`，初始化并算出非零费用（exit 0）。

生成的 commodity 文件来自受控 CTP 模拟，两个 session 是空字符串；脚本只在测试副本中将其替换为 `SD0930`，以排除商品未加载因素。第一次运行时没有可用 session，导致旧品种正控也失败，日志 `evidence-20261102/initial-control-missing-session.log` 保留，不计通过。该控制验证不等于真实 CTP 生产配置。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python \
  src/TestUnits/fixtures/i03_fee_consumer_pipeline/run.py /tmp/wt-i03-fee-consumer-pipeline-new
```

输出路径须不存在。脚本复用 `/tmp/wt-i03-l06-fees-20261102` 隔离目标的编译/链接命令与对象，核对 HisDataReplayer.cpp/.h 和 CodeHelper 与当前源码一致；WTSSessionInfo.hpp 及其他依赖存在快照漂移，当前完整工作树未经重编。普通编译，未执行 sanitizer、真实 provider、热切换或最终回归。两个 exit 1 是预期的现存缺陷，脚本 exit 0 仅说明红绿观测符合预期，不能称为产品通过。

直接修改旧 double `calc_fee` 无法区分有效零费用与缺失，CTA/SEL/HFT/UFT 的调用中还存在先改仓位后计算费用的顺序。主力连续码没有事件确认的物理合约时，不能只用当前交易日重算身份；安全接入必须先定义经过确认的物理身份、checked 返回状态，以及调用者在任何账本与策略回调副作用之前的失败传播和回放结果状态。`WtBtRunner`/公开 Porter 的旧 `void` 入口也不提供机器可观察的失败结果。该方案尚未实现。
