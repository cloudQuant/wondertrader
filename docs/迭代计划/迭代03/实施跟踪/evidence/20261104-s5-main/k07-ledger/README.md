# 决策 K07 b/c/d（BUG-Q04-002、BUG-I02-NEW-030）：逐单账本边界

裁定（2026-11-04）：
- a：先不建独立门禁，回收按现行；
- b：重复成交连持仓、通知一并去重；
- c：无成交号成交 fail-closed（拒绝记账并告警）；
- d：WtCore 同步回报口径统一为 UFT 现行口径（见到存活委托或成交即保留订单）；
- e：按现行。

## b（核对）
UFT `onPushTrade` 对本通道订单的重复成交号整笔忽略，不改未完成量、持仓、统计，也不通知 sink，与裁定一致。这是既有实现，有用例覆盖，本轮未改。

## c（UFT `src/WtUftCore/TraderAdapter.cpp`，CRLF 保持）
- 本通道订单（usertag 可解析出本地单号）的成交若无成交号，记 ERROR（提示人工对账），整笔不记账：不改持仓、统计、账本，不通知 sink。去重判断随之不再需要“空成交号放行”分支。
- 按裁定改夹具：`q04_trader_push_report_test.cpp` 的 `pushTrade` 改为给每笔成交唯一成交号（改前不设成交号）。新增 `k07c_own_trade_without_trade_id_is_not_booked`（空成交号不记账；带成交号正常记账）。
- 红控：改前 UFT TraderAdapter 重编，新用例失败（`k07-red.log` 前段）；改后 `q04_trader_push.*` 8/8 通过（`k07-green.log` 前段）；uftcore 逐例 305/305（`uftcore-summary.json`）。

## d（WtCore `src/WtCore/TraderAdapter.cpp`，CRLF 保持）
- `doEntrust`：只有“见到拒单或撤单，且没有存活委托或成交等接受证据”时才返回 UINT_MAX。一旦调用期间见到过接受证据，即使随后撤单、`orderInsert` 返回负值，也保留订单，记 ERROR；后续终态回报会释放它。改前见到拒单或撤单即返回 UINT_MAX。
- 用例 `n01_wtcore_tradepaths/s5_main_k07d_sync_report_test.cpp`：fake provider 在 `orderInsert` 内同步上报“部分成交存活委托 → 成交 → 撤单”并返回 -1，订单被保留；反控“只有撤单”时返回 UINT_MAX。
- 用例调试：fake 回报起初未带合约信息，被宿主以“contract unavailable”隔离，红控因此无效。经临时插桩定位后，fake 改为附带合约。插桩已撤销，改后文件哈希见 `sources.sha256`。
- 红控：用改前 WtCore TraderAdapter 重编，保留用例失败、反控通过（`k07d-red.log`）；恢复后两例通过（`k07d-green.log`）。
- 逐例回归：wtcore、porter、m_WtRunner、uftcore 共 1677 例，pass 1668、skipped 9（`regression-summary.json`）。

## 未覆盖
- 真实 provider 的回报时序、Windows。
- a、e 维持现行，未改。
