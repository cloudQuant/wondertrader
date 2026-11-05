# BUG-L03-002：商品对象零初始化 + 显式就绪状态 + 基础数据准入（裁定卡4：4a=A）

2026-11-03，macOS arm64。基础 HEAD `ccaad010e9169f610aded4d8dc0f5647f70a5cff`，工作区含其他迭代未提交修改。**只是本项定向红绿，不是整项验收或整体回归。**

## 起点

修改前工作区（`baseline/WTSContractInfo.hpp`，`87be53d7…`）已有构造置零与仅检查数值的 `hasValidConfiguration()`，`WTSBaseDataMgr.cpp`（`baseline/`，`ac17459b…`）已有整文件预校验（缺 `pricetick`/`volscale` 拒绝整份导入）。剩余缺口：零值交易模式等于 `TM_Both`，未配置对象 `canShort()==true`；无显式 ready 状态；预校验失败无诊断日志；回测滑点按 price tick 计算未防零。`baseline/WTSContractInfo.HEAD.hpp` 为 HEAD 版本。

## 修改

`src/Includes/WTSContractInfo.hpp`
- 新增状态字节 `m_uConfigState`（位：交易模式已显式设置 / 已标记就绪），放在 `m_nFeeAlg` 与 `m_dMarginRate` 之间原有的 4 字节对齐填充中，构造置 0。
- `setTradingMode()` 同时置“交易模式已设置”位；`getTradingMode()` 在未设置时返回 `TM_None`，`canShort()`/`isT1()` 基于有效模式，故未配置对象 fail-closed（不能做空、非 T+1、不能交易）。
- 新增 `isTradingModeConfigured()`、`markReady()`、`isReady()`；`hasValidConfiguration()` 增加“交易模式已显式设置”条件。`markReady()` 只有字段全部有效且已绑定 session 才置位；`isReady()` 每次复核，后续非法 setter 会撤销就绪。
- 未新增虚函数，未移动任何既有成员。

`src/WTSTools/WTSBaseDataMgr.cpp`
- `commodityConfigValid()` 输出原因；新增 `stagedCommodityReady()`（暂存对象 parse + session + `markReady()`）。商品文件与合约 `rules` 预校验失败时 `WTSLogger::error` 给出商品/合约、文件与原因，并保持整份导入不发布。
- 两处发布点（`loadCommodities`、`rules` 派生）在登记前调用 `markReady()`，失败不登记并返回失败；`product` 关联合约要求所指商品 `isReady()`。
- 缺 `trademode` 仍沿用加载器既有默认 `TM_Both`（由加载器显式设置，因此就绪）。`dist/common/commodities.json` 70/70、`stk_comms.json` 4/4 均无 `trademode`，若改为必填将拒绝全部发行配置；是否必填需另行裁定。

`src/WtBtCore/CtaMocker.cpp`、`SelMocker.cpp`
- 滑点非零时先检查 `getPriceTick()` 有限且 > 0，否则通过既有 preflight `fail()` 拒绝，信号保留、账本不变。HftMocker/UftMocker 无 price tick 除法；`volscale` 除数（TraderCTP 等 `getPositionCost()/.../getVolScale()`）由加载器 `volscale>0` 保证，未改适配器。

## ABI（`evidence/layout-*.txt`，`layout_probe.cpp`）

HEAD、修改前、修改后在本机 arm64 均为 `sizeof 632`、所有既有成员偏移一致；新字节位于偏移 620（原填充）。对象只由宿主侧 `WTSBaseDataMgr`（静态库）创建。旧二进制仍内联旧 getter：读取新对象时交易模式按原始值（加载器已显式设置，语义不变）；旧二进制无 ready/`TM_None` 门禁，需重编才能获得新语义。旧代码若自行 `create` 的对象，其填充字节未定义，新代码读取其状态不可靠——当前仓库无此调用方。MSVC x64 推理同样有该填充，但**未实测**。头文件被几乎全部模块包含：WtCore、WtUftCore、WtBtCore、WTSTools、WtDtCore/WtDataStorage(AD)/WtDtServo、全部 Parser*/Trader*/Loader、WtExeFact、各策略工厂、Porter/Runner 及全部 `CoverageTests_*`/`TestUnits` 均须重编。

## 运行与结果

```sh
bash src/TestUnits/fixtures/i03_commodity_ready_20261103/run.sh /tmp/wt-i03-commodity-ready-20261103 red
bash src/TestUnits/fixtures/i03_commodity_ready_20261103/run.sh /tmp/wt-i03-commodity-ready-20261103 green
bash src/TestUnits/fixtures/i03_commodity_ready_20261103/run-regression.sh /tmp/wt-i03-commodity-ready-20261103
bash src/TestUnits/fixtures/i03_commodity_ready_20261103/run.sh /tmp/wt-i03-commodity-ready-20261103-asan green asan
```

同一实体快照中只切换四个文件，`-j2` 构建 `WTSUtils WTSTools WtBtCore`，每个二进制 120 秒上限。配置 `-DWT_WARNINGS_AS_ERRORS=OFF` 为本地绕行：并行修改中的 `WTSDataFactory.cpp` 未用参数会被 `-Werror` 拒绝；本项四个文件无警告（`evidence/*-own-warnings.txt` 为空，`green-build-warnings.txt` 只有该外部警告）。普通红绿运行之后脚本仅为 ASan 变体追加了 `-include cstdlib`、`/usr/local/include` 与 `/usr/local/lib` 搜索路径，普通命令行未改变语义。

| 二进制 | 红（修改前） | 绿（修改后） |
|---|---|---|
| commodity_ready_test（8 项） | exit 1，2/8：未配置对象 `getTradingMode()==TM_Both(0)`、`canShort()==true`，无 ready API，加载器发布对象非就绪 | exit 0，8/8 |
| mocker_tick_divisor_test（CTA/SEL 各 4） | exit 1，6/8：零 tick + 固定滑点直接按 100 成交、滑点静默丢失；比例滑点产生 NaN 价格后已被既有 preflight 拒绝 | exit 0，8/8 |

相关回归（`evidence/regression/results.txt`，均为绿色快照）：`config_gate` 8/8、`fee_consumer` 23/23、`p03` 选定 7/7 通过。`l03`（commodity+contract）26/27：失败项 `l03_contract.storage_width_contract_limits_preserve_valid_edges_and_reject_before_rules_registration` 期待无 `pricetick/volscale` 且未加载 `ALLDAY` session 的 rules 合约可登记、混合文件部分发布；以修改前源码单独复测同样在第 253 行失败（`evidence/l03-baseline-check/run.log`），属已有整文件预校验与旧期望冲突，非本次引入，未改该断言。旧探针 `l03_commodity_state/init.cpp` 现报 `FAIL: default initialization`：其对象表示比较假设 `setTradingMode(0)` 不写入新字节，且期待未设置时读出 0；改为期待 `TM_None`、不比较该 setter 的变体（`evidence/regression/init_variant2.cpp`）exit 0。这是合同改变的预期结果，未隐藏。

ASan/UBSan/LSan（Homebrew LLVM 22.1.8，`detect_leaks=1`，库与测试均插桩，独立目录 `/tmp/wt-i03-commodity-ready-20261103-asan`）：绿色 commodity 8/8、mocker 8/8，均 exit 0，日志无 `leaked`/`ERROR: AddressSanitizer`/`runtime error`，见 `evidence/asan-green-*`。前三次 ASan 尝试分别因测试编译缺 `-include cstdlib`、Homebrew clang 不搜索 `/usr/local/lib`（GNU libiconv）及误选 `/opt/local` rapidjson / `/usr/local` 旧 fmt 而在构建或加载阶段失败（exit 13/14/134），不计测试结果；ASan 红色未单独运行（红绿由普通构建确定）。

## 未验证/限制

- `canShort()` 只门禁“交易模式已显式设置”，不要求完整 ready：约 50 个测试直接 `create` 并设置 `TM_Both` 后测试做空路径；完整 ready 准入由加载器发布门禁承担。直接使用 `create` 的调用方（测试/插件）仍须自行 `markReady()` 后再用于交易，未强制。
- 策略插件版本化导出与能力协商（4b=A）未在本项实现；旧插件二进制未加载测试。
- MSan 本机不可用（阻塞：工具链）；ASan/UBSan 不检测一般未初始化读取。依赖库未全部插桩。
- 未运行 `CoverageTests_*` 全体、Windows/MSVC、真实行情/交易或整体回归（`NOT_RUN`）。`r130_misc_tail_test` 等其他 lane 未在本轮运行。
- BUG-L03-009/010/012 为 Session offset/零宽时段/全日竞价语义，与本项“未初始化/未就绪”根因不同，未在本 fixture 处理（见 `i03_session_contract_20261103`）。
