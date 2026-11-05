# 决策 K05=C（BUG-P02-005、BUG-R202-003）：账户净持仓限额的下单入口复核

裁定（2026-11-04）：采用 C，即 B（账户级配置 + 执行器目标层，既有，见 `src/WtCore/AccountNetPositionLimits.h`）之上，再在下单入口按“实际持仓 + 在途”复核。

## 产品
- `AccountNetPositionLimits.h`：`Registry::limitFor(account, code, limit)` 返回同账户所有 owner 为该合约声明的最严限额。
- `TraderAdapter::doEntrust`（WtCore，CRLF 保持）：账户（`AccountTradingGate` 身份）有该合约限额时，计算当前净值 = 实际净持仓（多减空）+ 在途未完成量，预计净值 = 当前净值 + 本单的有向数量。|预计净值| 超过限额且大于 |当前净值| 时拒单并记错误；不增加 |净值| 的委托（减仓）始终允许。
- 无限额声明时不限制，保持原行为。
- 该复核覆盖执行器目标层之外的入口：同一账户的直接下单、HFT，以及在途叠加。

## 用例（`n01_wtcore_tradepaths/s5_main_k05_entry_limit_test.cpp`）
- 声明限额 2：
  - 3 手被拒；
  - 2 手通过；
  - 在途 2 手时再开 1 手被拒（尚无成交持仓）；
  - 反向 1 手（减少 |净值|）允许。
- 无声明时 5 手照常下单。
- 红控：用改前 `TraderAdapter.cpp` 重编，限额用例失败、无声明用例通过（`k05-red.log`）；恢复后两例通过（`k05-green.log`）。
- 逐例回归：wtcore、porter、m_WtRunner 共 1374 例，pass 1365、skipped 9（`regression-summary.json`）。

## 未覆盖
- 跨进程同账户、外部客户端下单、真实柜台持仓口径（昨今仓、冻结）。
- 本复核读取的是本适配器视角的持仓与在途；同一账户多适配器的在途合计未纳入。
