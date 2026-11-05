# R7-012 TWAP 卖向和回报顺序反例

这是隔离构建用的红控，不修改产品行为。卖向撤单和最后一轮各出现反向买单；配置开仓最小量 3、卖出目标差 2 且品种最小量/步长均为 1 时卖出 3。买单 20 部分成交 10 后，如撤单回报先于仓位落账，重发 20；相同成交先落账的正控重发 10。后者说明回报顺序影响，但 fake 不能证明任何真实柜台的回报合同。`test-output.log` 是实际测试输出：4 个反例失败、1 个正控通过，退出 1。

复现基于 R7-002 原隔离快照；编译仅插桩该测试可执行文件及其编入的产品翻译单元，其他依赖及外部库不一定插桩。以下使用新目录（不要覆盖既有目录）：

```sh
mkdir /tmp/wt-r7-sell-repro
git archive ccaad010 src | tar -xf - -C /tmp/wt-r7-sell-repro
patch /tmp/wt-r7-sell-repro/src/TestUnits/coverage/CMakeLists.txt < 'docs/迭代计划/迭代03/实施跟踪/evidence/R7-002-total-times-ccaad010/CMakeLists.ubsan.patch'
cp src/WtExeFact/WtTWapExeUnit.cpp /tmp/wt-r7-sell-repro/src/WtExeFact/WtTWapExeUnit.cpp
cat src/TestUnits/fixtures/r7_twap_sell_sign/sell_sign_tests.inc >> /tmp/wt-r7-sell-repro/src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-r7-sell-repro/src -B /tmp/wt-r7-sell-repro/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-r7-sell-repro/build --target CoverageTests_exefact -j 6
UBSAN_OPTIONS=halt_on_error=1 /tmp/wt-r7-sell-repro/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact '--gtest_filter=r7_twap_sell_sign.*'
```

本次 TWAP 源 SHA-256 为 `59200d69aa16d0735d8b72051df1f7cb3d0b1a552a1a4132c17dfbcaa2f2126f`，测试追加文件为 `f71051816edf72ce837da9b1b2c3a67965cf8c59b800a6a3277a8d8ebe81eb97`，二进制为 `a73ac1b8a8188d41885dc2f4ebacd821bc5e261785e444262f0dd0d760b7e0fa`。当前完整工作树、交易所最小量与真实网关成交/撤单回报顺序未验证；红控不代表修复或验收通过。

## 有界数量候选与未闭合问题（2026-11-02）

当前候选在收尾时保留差值方向，将单轮数量限制在当前查询到的目标差内；净仓跨零时先按当前查询到的仓位收口；撤单立即重发保持原方向，且不超过该回报的 `leftover`。开仓差额不足配置 `minopenlots` 时不放大发单；平仓不强加开仓最小量。保留原整数均分规则，修正剩余发单次数的无符号下溢。**这些边界只依赖当时 `getPosition` 的返回值，不证明真实总仓目标、报单合法性或账本顺序正确。**

当前 `WTSCommodityInfo::create` 不保证商品的 `lotstick/minlots` 初始化；候选没有读取这两个未受约束的字段，因此并未实现交易所最小量或步长校验。配置加载器通常赋值，但真实柜台、股票奇数股和品种单位未验证。该候选也保留原 `round` 均分，不能声称符合所有小数策略要求。`getPosition` 默认 `validOnly=true`，TraderAdapter 返回可用净仓而非总净仓；T+1 或冻结仓位可能使方向错误。订单回报没有成交身份和入账完成屏障，即使单次重发受剩余量约束，后续 tick 仍可能使用旧仓位发出额外订单。多 localid 取消时只按最后一个回报的余量重发，丢失前序剩余量。**R7-012 保持未完成；不能据此声称实盘安全。**

隔离构建基于 `ccaad010` 的 `src` 快照，复制当前 `WtTWapExeUnit.cpp/.h`，测试文件追加原有的 `sell_sign_tests.inc`、`bounded_quantity_tests.inc` 和故意失败的 `unsettled_position_red.inc`。仅测试可执行文件及纳入的产品翻译单元由原补丁以 UBSan 插桩，外部依赖与其余工作树未统一重编。版本和 SHA-256 见 `evidence-bounded-20261102/source.sha256`：

- 原五项与新增七项边界测试：12/12，exit 0，`bounded.log`。覆盖卖向反转、撤单报告剩余上限、目标差上限、开仓最小量、买卖两侧平仓、跨零与一个小数差额；不代表其他单位合法。
- 仓库原 `r7_twap.*`：38/41，exit 1，`existing.log`。三个失败分别为 `cancel_report_refires_with_cancel_times_price_step`、`fire_at_once_mode_1_and_2_pricing`、`fire_at_once_unknown_price_mode_has_no_preclose_fallback`；都原期望撤单 `leftover=5` 时重发 25，候选为 5；两个价格断言并未失败。原测试未更改。
- 已知红控：3/3 失败，exit 1，`unsettled-red.log`。受控 fake 中分别复现部分成交未反映于仓位时再来 tick 会产生第三单；总仓 100、可用 10、目标 50 时反向买入；两个已取消子单剩余 8 与 4 时仅重发最后 4。没有真实交易或柜台回报测试。

可在仓库根目录以新目录复现（依赖上方同样的本机工具链；目录应不存在）：

```sh
work=/tmp/wt-r7-bounded-repro
mkdir "$work"
git archive ccaad010 src | tar -xf - -C "$work"
patch "$work/src/TestUnits/coverage/CMakeLists.txt" < 'docs/迭代计划/迭代03/实施跟踪/evidence/R7-002-total-times-ccaad010/CMakeLists.ubsan.patch'
cp src/WtExeFact/WtTWapExeUnit.cpp src/WtExeFact/WtTWapExeUnit.h "$work/src/WtExeFact/"
cat src/TestUnits/fixtures/r7_twap_sell_sign/sell_sign_tests.inc src/TestUnits/fixtures/r7_twap_sell_sign/bounded_quantity_tests.inc src/TestUnits/fixtures/r7_twap_sell_sign/unsettled_position_red.inc >> "$work/src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp"
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$work/build" --target CoverageTests_exefact -j 6
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap_sell_sign.*:r7_twap_bounded.*'
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap.*'
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap_unsettled_red.*'
```

后两条按上述已知现状应 exit 1。相关红控与旧用例留作证据，不应屏蔽后声称通过。尚未做完整树、跨平台、真实订单生命周期或 TSan 验证。

## 总仓方向和提交状态候选（2026-11-02，后续）

本节是上节的后续；上节的 T+1 红控结果属于当时版本，不能作为当前版本结果。当前单元分别读取总净仓、多空总仓和多空可用仓，拒绝数值非有限、方向/范围/净额不一致或连续两次不同的快照；目标方向按总净仓确定。卖单有多仓时以可用多仓为上限并传 `forceClose=true`，买单有空仓时以可用空仓为上限并强制平仓；被限制后余量继续保留。首次行情和取消后重发都采用此策略。没有可靠方式区分旧接口返回的“没有数据”和真实零仓，双次读取也不是原子快照、不能排除 ABA；不能据此证明并发下安全。

返回空数组、0、`UINT_MAX`、重复或已在监控中的订单 ID，以及提交期先到的订单、成交或委托回报，将锁存提交结果未确认，保留返回的有效 ID，阻止后续自动发单；提交异常同样锁存。首次提交未确认时不增加切片次数或发布新的切片目标。该锁存不会因目标修改、行情、通道重新就绪或再次 init 自动清除；当前没有可验证的恢复 API，实例必须由宿主在可靠对账后重新建立。提交期间先到的回报不会重放，可能造成监控状态不完整，故不声称事件已被持久记录。新增两个原子字段改变 `WtTWapExeUnit` 具体类布局，相关目标须统一重编，未验证旧新二进制混用。标志自身使用原子操作，但订单、行情、`_cancel_cnt`、位置等其他状态仍依赖宿主串行；检查到实际 provider 操作之间仍有间隙，没有持锁跨 provider，**未完成并发/TSan 验证**。

`evidence-position-20261102` 记录 macOS arm64、隔离快照的结果。初次尝试修正 fixture 时补丁 hunk 格式错误且私有测试直调 `fire_at_once` 的新参数未给默认值，构建失败见 `initial-build-failure.log`；修复后重新构建。仅测试 TU 和纳入的产品 TU 使用 UBSan，其他依赖未统一插桩。

- 按 `ExecuteContext::getPosition` 的 flag 合同修正的隔离 fake：原反例、新数量边界、仓位/提交状态及真实 adapter 规则测试共 28/28，`contract-target.log`。包含零可用仓、短仓、跨零、双边持仓、异常快照、同步回报、无/无效/混合 ID 和多有效 ID。
- 使用同一修正 fake 的仓库原组：38/41，`contract-old.log`；仍是上节列出的三条旧断言要求 `leftover=5` 时重发 25。未修改仓库原测试。
- 不修正原 fake：原组 33/41，`original-old.log`；原先的反例和新增数量组 4/12，`original-bounded.log`。原 fake 对任意 flag 均返回同一个 `pos`，不满足接口区分多、空及净仓的合同；对应五条旧组与八条定向失败不可隐藏，但也不能用它证明真实接口行为。仅隔离构建应用 `position_fake_contract.patch`，仓库测试未改。
- 真实当前 `TraderAdapter.cpp/.h` 重编进隔离快照，`ActionPolicyMgr` 配置 open 在 close 之前，fake provider 记录开平标志：买平、卖平首单和取消重发均发平仓；真实 adapter 在 fake provider 同步拒绝时返回 `[UINT_MAX]`，单元锁存且不再发单。3/3，`current-adapter.log`。源码/二进制哈希在 `current-adapter.sha256`；其他依赖仍为旧快照，非当前整合树或真实网关。
- 延迟仓位后额外行情第三单，以及多个 localid 只重发最后余量，两项红控仍失败，`contract-red.log`。对订单回报先于成交入账仍没有可信完成栅栏。

在上节建好的唯一新目录基础上，重建合同 fake 版本并运行（若此前已追加测试，先恢复该快照中的测试 TU）：

```sh
git show ccaad010:src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp > "$work/src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp"
patch -d "$work" -p1 < src/TestUnits/fixtures/r7_twap_sell_sign/position_fake_contract.patch
cat src/TestUnits/fixtures/r7_twap_sell_sign/sell_sign_tests.inc src/TestUnits/fixtures/r7_twap_sell_sign/bounded_quantity_tests.inc src/TestUnits/fixtures/r7_twap_sell_sign/unsettled_position_red.inc src/TestUnits/fixtures/r7_twap_sell_sign/position_snapshot_tests.inc src/TestUnits/fixtures/r7_twap_sell_sign/adapter_force_close_tests.inc >> "$work/src/TestUnits/coverage/r7_exefact/r7_twap_unit_test.cpp"
cp src/WtExeFact/WtTWapExeUnit.cpp src/WtExeFact/WtTWapExeUnit.h "$work/src/WtExeFact/"
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build "$work/build" --target CoverageTests_exefact -j 6
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap_sell_sign.*:r7_twap_bounded.*:r7_twap_position.*:TwapOpenFirstPolicyTest.*'
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap.*'
UBSAN_OPTIONS=halt_on_error=1 "$work/build/build_x64/Debug/bin/TestUnits/CoverageTests_exefact" '--gtest_filter=r7_twap_unsettled_red.*'
```

第二、三条运行仍应 exit 1。复验真实当前 adapter 时另将 `src/WtCore/TraderAdapter.cpp` 和 `.h` 复制进该快照、重建后单独运行 `--gtest_filter=TwapOpenFirstPolicyTest.*`；其余代码仍是快照。R7-012 的交易所最小单位/小数、交易回报乱序、未绑定状态、并发和真实系统验收仍未闭合。

## 迭代03 最终验收处置（2026-11-05，slice3-result）

- 分类：**SUPERSEDED**（不修）。本套件没有脚本入口，README 复现步骤依赖 `git archive ccaad010 src`、`docs/…/R7-002-total-times-ccaad010/CMakeLists.ubsan.patch` 与前一节工作目录，各节红控属于当时版本（4 个反例失败是 BUG-R7-012 修复前的已知红）。
- 由以下 coverage 用例取代（F3 均为 pass，`CoverageTests_exefact`）：`r7_twap.sell_side_keeps_sign_and_splits_evenly`、`r7_twap.opposite_live_order_canceled_first_without_new_order`、`r7_twap.cancel_report_refires_with_cancel_times_price_step`、`r7_twap.tiny_diffqty_floors_to_min_open_lots`、`r7_twap_legal_lot.sell_rounds_down_to_legal_unit_and_keeps_odd_remainder`、`r7_twap_legal_lot.t1_sell_is_capped_by_available_then_legal_unit`、`r7_twap_legal_lot.buy_and_target_change_use_legal_units_only`、`r7_twap_legal_lot.whole_lot_future_control_and_fractional_target_remainder`、`r7_twap_legal_lot.unavailable_lot_rules_hold_every_order`、`n05_unmanaged_cancel_latch.twap_ready_suppresses_latched_cancel_and_keeps_unlatched_behavior`。
- **缺口**：“撤单回报先于仓位落账时重发 20、先落账时重发 10”的回报顺序反例在 coverage 中没有直接对应用例；README 原文亦说明 fake 不能证明真实柜台回报合同。
