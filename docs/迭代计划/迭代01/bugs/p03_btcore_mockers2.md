# P03 WtBtCore 模拟交易主路径 疑似缺陷（迭代 01 第三波）

- Lane: p03，模块 `src/WtBtCore/`（CtaMocker/SelMocker/UftMocker 下单—成交—持仓—盈亏）
- 状态: 全部**待审核**，未修改任何生产代码

## BUG-P03-001 calc_fee 按额计费时空解引用 WTSCommodityInfo（待审核）
- 位置: `src/WtBtCore/HisDataReplayer.cpp` L2983-L3019（`calc_fee`）
- 复现: 费用模板 `_fee_map["DCE.m"]` 存在且 `by_volume=false`，但 `DCE.m` 未注册 commodity → L2993 `commInfo == NULL` 未判空，L3007 直接 `commInfo->getVolScale()` → 段错误。
- 期望: commInfo 为 NULL 时返回 0 或记录错误日志后早退。
- 测试: `p03_cta_trade_path_test.cpp` 中 `DISABLED_CalcFeeByAmountCrashesWhenCommodityMissing`（整类空解引用收进同一 DISABLED 用例，启用用例零残留）。

## BUG-P03-002 update_dyn_profit 对缺失合约信息无判空（待审核）
- 位置: `src/WtBtCore/CtaMocker.cpp` L985-L990（`SelMocker.cpp` 同构路径同样存在）
- 现象: 持仓 `volume != 0` 时 `_replayer->get_commodity_info(stdCode)` 返回值直接解引用取 `getVolScale()`。正常链路下持仓必伴随已注册合约，但该不变量仅靠调用顺序维持，基础数据被重载/剔除（如运行期 `clear_cache`、commodities 配置变更）即崩。与 BUG-P03-001 同属"计费/盈亏路径不判空 commInfo"缺陷类，已在 p03 的 DISABLED 用例说明中合并归档。

## BUG-P03-003 UftMocker::PosItem 构造函数遗漏 `_long` 初始化（待审核）
- 位置: `src/WtBtCore/UftMocker.h` L305-L332（`_PosItem`）
- 现象: `_PosItem()` 显式清零 `_prevol/_newvol/_preavail/_newavail/_closeprofit/_dynprofit`，但 bool 成员 `_long` 未初始化（同文件 `_DetailInfo/_OrderInfo/_StraFundInfo` 均 `memset(this,0,...)`）。读取默认构造 PosItem 的 `_long` 为不确定值（UB）。当前持仓推进路径以外部 map key 方向为准未见实际错算，但属初始化遗漏类缺陷。
- 测试: `p03_pos_structs_test.cpp` 刻意不断言默认 `_long` 的值，仅覆盖其初始化缺口。

## 其它观察（不构成缺陷条目）
- `stra_exit_short`（CtaMocker.cpp L1453）用**总量**仓位而非 valid 仓位做前置判断，与 `stra_exit_long`（L1402，非末根 bar 用 valid）不对称；期货无 T+1 场景下等价，暂不立条。
- 测试通过重定义 `private/protected` 访问宏获取 Mocker 私有持仓结构与 `HisDataReplayer::_bd_mgr/_fee_map` 的可见性，未改动任何头文件与源码；clang/Itanium ABI 下不改变对象布局，链接期符号与生产一致。
