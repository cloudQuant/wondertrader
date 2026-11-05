# I03-NEW-008：WtCore 旧费用加载器的配置门禁

这是局部前置修复，不是费用链验收。`WtEngine::load_fees` 在配置路径非空时，先将 `fee_config_valid` 置为 false，只有读取、解析与原有校验/应用完成才置 true；不存在的文件、不支持的扩展名、空内容、非对象以及解析失败不再被标为有效。CTA/SEL/HFT 的现有 `run()` 和两种现有 runner 初始化路径都会查询此状态；本夹具仅动态执行了真实 CTA 的初始化和 `run()` 门禁。无配置的空路径和 null 参数仍保留可选配置行为。

明确拒绝 `version` / `contracts` / `products` 顶层 envelope，并记录不支持的诊断。原实现对格式正确的 v2 已因 `version` 不符合商品键语法而拒绝；本轮没有实现 v2 账务，也没有先切换导出。拒绝配置时原先活动费率快照保留，本改动只阻止该状态下经现有门禁启动；保护**已经运行中的引擎**、错误时 `calc_fee` 的机器可观察状态、实际成交与账务异常传播均未完成。旧费用文件仍可能缺字段或缺商品而被解释为零费用；`FeeSchedule` 候选尚未接入。

本机 macOS arm64，LLVM 22；本目录的测试 TU 和当前 `WtEngine.cpp` 以 UBSan 插桩重编，链接 `/tmp/ctpm-i03-build` 既有静态库与 GoogleTest（未保证这些依赖和当前工作树一致或插桩）。六个原 R130 用例和四个门禁用例共 **10/10**，`evidence-20261102/final.log`；其中覆盖配置的文件缺失、空、错误扩展名、非法 JSON/YAML、非对象、版本 envelope、原费率快照未变、CTA 启动时拒绝以及恢复旧配置。未测试真实 provider、完整 runner 构建、并发重载、其他平台、全量回归或 ASan。

首次运行成功编译后因未设置 `DYLD_LIBRARY_PATH` 加载 nanomsg 失败；修正脚本后重编通过。旧代码的第一次红控还对 null 文件名触发旧版空指针异常，之后将 null 测试独立，单独运行其余红控：对空文件、非法 JSON/YAML、非对象、缺失路径、错误扩展名等，旧代码仍将 `fee_config_valid` 保持 true，退出 1，见 `evidence-20261102/baseline-red.log`。该红控不能证明版本 envelope 是本轮新发现：旧版的正常 v2 拒绝对照本来就通过。源码及依赖哈希见 `evidence-20261102/inputs.sha256`。

在仓库根目录执行，输出路径必须不存在。需要现存 `/tmp/ctpm-i03-build` 配置与库、Homebrew LLVM/Boost/GTest/nanomsg：

```sh
bash src/TestUnits/fixtures/i03_fee_engine_gate/run.sh /tmp/ctpm-i03-build /tmp/wt-i03-fee-engine-gate-review
```

若复现红控，先核对 `git show` 的版本与 `inputs.sha256` 中 baseline 一致，再使用新的输出路径：

```sh
git show ccaad010:src/WtCore/WtEngine.cpp > /tmp/wt-i03-fee-engine-gate-old.cpp
I03_FEE_ENGINE_SOURCE=/tmp/wt-i03-fee-engine-gate-old.cpp I03_FEE_TEST_FILTER=I03FeeEngineGate.ConfiguredUnreadableOrInvalidFilesRejectStartup \
  bash src/TestUnits/fixtures/i03_fee_engine_gate/run.sh /tmp/ctpm-i03-build /tmp/wt-i03-fee-engine-gate-old-run
```

红控预期 exit 1；脚本本身不将已知失败计为通过。CTPLoader 导出尚未迁移，实盘消费端在确认具体成交合约之后才可调用 checked 合约/品种查找；缺费时需保留真实成交并标记可追踪的账户异常。回测也需在任何账本副作用前传播缺费，不能把当前日期重算的连续码当作已确认的成交身份。

## 2026-11-05 最终验收更新（卡13d 合同）

卡13d（I03-NEW-008）之后 `WtEngine::load_fees` 已接入版本化费用表：`{"version":2,"contracts":{…},"products":{…}}` 由 `wt_fee::Schedule` 严格解析，空表也是合法 v2 文件。上文“明确拒绝 version/contracts/products envelope”的描述属于卡13d 之前的阶段，已过期。夹具相应调整为仍然非法的样本，断言强度不变：

- `RejectsUnsupportedEnvelopeWithoutChangingFees`：改用 `version: 3`（不支持的版本），仍断言整文件拒绝、原费率快照与商品费率不变、CTA `run()` 门禁在访问数据管理器前返回、恢复旧配置后重新有效。
- `InvalidFileAtInitIsRejectedByCtaRun`：样本为语法错误、`version: 3`、以及缺少必填字段的 v2 品种条目（与 coverage 用例 `CoverageTests_wtcore::S5LiveFeeSchedule.InvalidVersionedTemplateBlocksStart` 同一合同）。

合法 v2 的接受与账务由 coverage 用例 `S5LiveFeeSchedule.*`（`n02_wtcore_engines/s5_live_fee_schedule_test.cpp`）覆盖。运行方式不变：`run.sh <带 build_x64/Debug/libs 与 TestUnits/coverage/cov_boost_include 的 coverage 构建> <新输出目录>`，须从仓库外的源码副本运行（脚本会 cd 到所在源码树根）。10/10 通过的重跑记录见 `docs/迭代计划/迭代03/实施跟踪/evidence/20261105-final-acceptance/fixtures-fixtures-agent/README.md`。
