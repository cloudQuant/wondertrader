# I03-NEW-008：HFT 成交与手续费接入探针

本目录只增加测试，没有修改产品。它使用真实 `TraderAdapter::onPushTrade`、`HftStraContext::on_trade` 和 `HftStraBaseCtx`，交易 API、基础数据与行情管理为进程内替身，不连接柜台。当前会计仍使用商品对象的旧费率，本测试的合约费率直接设置到 `WTSContractInfo`，**不表示 v2 费用表已经接线**。

## 当前观测

macOS arm64，LLVM 22。五个用例各自进程运行：原 `R71HftTest.TradeAccountingAndDynProfitExtremes` 控制 exit 0；四项按目标行为编写的红控分别 exit 1：

1. 成交合约按手费率为 7、其所属品种按手费率为 1；成交记录仍写 1.00，未使用实际合约费率。
2. 委托回报明确 `WOT_CLOSE`，对应品种平仓费率为 3、平今为 9；HFT 本地根据仓位开仓日推断为平今，记录 9.00。测试的输入遵循 `WTSTradeInfo::getOffsetFlag()` 将 `WOT_CLOSE` 映射到 close 的当前约定，不能替代真实供应商的 offset 合同。
3. 未配置费率与显式配置全零，均保留成交，但两份 `trades.csv` 完全相同且费用均为 0.00；其中缺费场景确认 adapter 已增加仓位。该观察限于现有交易 CSV，不证明所有外部审计渠道都无错误信号。
4. 两个 HFT context 注册到同一 adapter，其中一个实际提交订单；该订单的成交同样写入另一个 context 的交易记录。此用例故意使用当前 runner 的普通 sink 路径，不声称已使用尚未接入成交生产路径的 owner 路由 helper。

控制及红控原始输出在 `evidence-20261102/`。编译和测试 TU、五个当前产品 TU 以局部 UBSan 插桩；其余 WtCore/WtDtCore/WTSTools/WTSUtils 静态库与 GoogleTest 使用旧隔离构建，未全部插桩。脚本复制五个产品源码到输出目录、对关键源码/头记录并校验前后 SHA-256；未冻结所有依赖头，也不是完整当前树构建，不能推断真实 provider、并发、断电或整项验收。首次尝试因测试漏传 `getPosition` 的 `bValidOnly` 参数编译失败；补足参数后重编才得到本结果。

```sh
bash src/TestUnits/fixtures/i03_hft_fee_pipeline/run.sh \
  /tmp/wt-i03-r42-binding-20261102/build /tmp/wt-i03-hft-fee-pipeline-review
```

需要本机现有隔离构建、Homebrew LLVM 22、Boost 1.88、GoogleTest 和 nanomsg；输出目录必须不存在。脚本执行五个用例并预期 exit 1（红控仍失败）。可用 `I03_HFT_FEE_FILTER=R71HftTest.<用例名>` 单独运行；具体源码与构建库、二进制哈希见 `evidence-20261102/source.sha256` 和 `outputs.sha256`。动态库、其他静态依赖和编译期间不在哈希清单的文件未证明一致。

## 接入设计及可并行边界（待主代理确定）

用户已裁定：合约级优先、品种级回退，缺费报错，合法零有效，连续码按实际成交合约。可分解为以下依赖关系，不能将任一子项通过等同于完整链闭合：

1. **费用解析及快照**：`Share/FeeSchedule.hpp` 的未接线 v2 使用 `version`、`contracts`、`products` 分表；加载完整验证后发布只读快照，按事件持有固定版本并区分 contract/product/missing/invalid。旧版非版本文件需显式迁移/校验缺项，避免将缺项作为零费率。金额倍数、负费率舍入和 minfee 规则需与真实数据合同明确。旧消费者必须拒绝 v2，先完成消费者能力检测，再切换 CTPLoader 导出。该工作可由费用模块独占，不能只更新共享商品对象。
2. **成交身份与 owner 路由**：`TraderAdapter`、`ITrdNotifySink` 的版本化扩展及注册处共同拥有这一改造。现有 callback 只携带 localid、标准码、买卖、数量和价格；trade ID、物理 raw exchange/code、offset、交易日、account/owner 等没有传给 HFT。应以可发现的版本化接口/尺寸约定提供独立事件，不更改旧 sink 的既有 vtable；对一笔事件只选择一个已验证 owner、只调用一次。代码转换前复制必要字段和元数据，不保存 borrowed 指针。来源、交易所、物理合约与品种的关系必须通过实际成交关联的 `WTSContractInfo` 验证，不能在成交时根据当日 HOT 重算。provider 未提供稳定身份、offset 或合约元数据时不得伪造；当前 UNKNOWN 合约甚至在 `onPushTrade` 起始处直接 return，需在解析前保存可恢复原始事件。
3. **账本与异常状态**：`HftStraContext` 和 `HftStraBaseCtx` 负责在保留策略代码/持仓视图的同时，将事件中物理身份及明确开平传入 checked 费用计算；不应以 `_pos_map` 开仓日期代替 provider 明确 offset。净额回报若需拆成开/平，必须依据可靠的期初/今昨明细并验证总量及保序，无法确定则标记不可计算。策略回调当前先于本地账务且可能重入，adapter 持仓更早更新；不能在缺费时简单 return 或丢弃已发生的成交。
4. **可恢复性与准入**：与 NEW-008 的 owner/订单身份链共用一致的可靠事件与账户准入设计。将已发生成交与缺费状态、费率快照版本、账本落账状态关联并提供机器可观察查询；缺费不得写成合法零，待补费处理需使用已验证稳定身份幂等。持久化失败时保留内存中的原始事件、停止新增自动动作并报告；不能仅凭 void 回调或单个文件推断硬断电零丢失。事务边界、日志与订单身份跨文件原子性、恢复对账和旧插件处理须共同设计。

接线后须在真实 host/fake provider 的确定性屏障上验证：两合约不同费率、合约缺项的品种回退、有效零与缺费、错配元数据、明确三种 offset 和净额拆分；不同 owner 的同一合约、旧/新 sink 去重、provider 同步重入、乱序与重复成交；持久化前后失败/重启恢复、缺费补记幂等、恢复状态可观察，以及保留已成交的数量/仓位/通知。跨进程、真实 SDK/供应商和平台仍需另行验收。
