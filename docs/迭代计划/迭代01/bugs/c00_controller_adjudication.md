# 主控裁定过程中新登记的缺陷（状态：待审核，未改任何产品代码）

来源：`reports/FAIL裁定记录-20261027.md` 的逐条裁定。每条都由「用例日志 + 源码行」双向核对得出，
可复算路径写在「复现步骤」里。

## BUG-C00-001 `WTSKlineSlice::extractData` 的区间夹取只夹一端，越界窗口静默返回空数组

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/Includes/WTSDataDef.hpp:384-399`
- 相关测试: `ws03_includes_data/ws03_data_def_test.cpp:281-287`（`ws03_kline_slice.extract_data_fields`）
- 输入/前置条件: 4 根 bar 的 slice，调用 `extractData(KFT_CLOSE, 10, 20)`
- 期望行为: 越界窗口要么被完整夹取到合法区间（返回可读的那几根），要么直接返回 `NULL`，与同文件里
  `WTSKlineData::extractData`（:746-755，`begin/end` 越界时 `return NULL`）保持同一种约定
- 实际行为: `begin = max(0, min(head,tail)) = 10`，`end = min(max(head,tail), size()-1) = 3`，
  于是 `for(i=10; i<=3; i++)` 一次都不执行 → **返回一个 size==0 的非空对象**。
  调用端只判 `NULL` 的话，会拿着空数组继续读第 0 个元素。
- 复现步骤: 跑该用例即可：日志里 `clamp->size()` 实得 0、期望 1；紧跟着 `clamp->at(0)` 返回
  `1.7976931448623157e+308`（见 C00-002）。或单测 `WTSKlineSlice` 的三段：`(0,20)`、`(10,20)`、`(2,1)`。
- 影响范围: 两个同名函数语义不一致（slice 走「静默空数组」，data 走「NULL」），上层按同一套约定写代码时
  一条路径判空生效、另一条形同虚设。本轮统计里「数值为 0」形态的 FAIL 有 50 条，这类语义分歧是候选原因之一。
- 备注: 修复方向由评审定：要么 `begin = min(begin, size())` 并同时把 `end` 夹到 `>= begin`，
  要么与 `WTSKlineData::extractData` 一致越界返回 `NULL`。**本轮未改产品代码，也未改该用例的断言**，
  用例保持红。

## ~~BUG-C00-002~~ 撤回：`WTSValueArray::at()` 的 DBL_MAX **是文档化的越界哨兵，不是缺陷**

- 状态: 已撤回（不进入评审清单）
- 我原先写的是：「`at()` 越界返回 DBL_MAX 而不拒绝访问（可诊断性 Minor）」。
- 撤回依据（同一文件，实测行号）：`src/Includes/WTSDataDef.hpp:62-64` 的注释已经写明
  「读取指定位置的数据，**如果超出范围,则返回 INVALID_VALUE**」，`:66-73` 的实现是
  `if(idx < 0 || idx >= m_vecData.size()) return INVALID_DOUBLE;` —— 有边界检查，返回值是设计好的哨兵。
  所以 `clamp->at(0)` 读到 `1.7976931448623157e+308` 是**规定行为**，不是未防护读取。
- 我错在哪：把日志里出现的 DBL_MAX 直接读成「越界没被拦住」，没有去读被调函数本体就下了结论。
  仍然成立的部分只有一句：`INVALID_DOUBLE` 与业务侧「无结算价时下发 DBL_MAX」（对照
  `T1_ctpopt_parserxtp.md` 的 XTP `pre_settle` 分支）用的是同一个数值，二者在下游会互相冒充——
  这是**观察**，不是本轮能定级的缺陷，且要产品侧确认语义后才谈得上登记。
- 真正站得住的只有 C00-001（`extractData` 的区间夹取不对称），该条保留。

## BUG-C00-003 `HftMocker::stra_cancel(按代码+方向)` 返回的撤单清单与实际被移除的订单数不一致

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtBtCore/HftMocker.cpp:574-592`（`stra_cancel(const char*, bool, double)`）与 `:548-572`（按 localid 的重载）
- 相关测试: `p08_btcore_mockers3/p08_hft_order_cancel_test.cpp:228-238`（`P08HftOrderCancel.CancelByCodeAndDirection`）
- 输入/前置条件: 同一合约下先建立 3 笔挂单（2 买 1 卖之类组合，见该用例 SetUp），
  再以 `stra_cancel(kCode, false, 10.0)`（qty 大于任何单笔剩余量）撤卖单
- 期望行为: 返回值 `ret` 里列出的 localid 数量 == 之后真正从 `_orders` 中消失的订单数量
- 实际行为: 用例观察到 **返回 1 个 id，但 `_orders` 少了 2 笔**（`canceled2.size()==1` 断言通过，
  随后 `_orders.size()` 实得 1、期望 2）。也就是说调用方拿到的「撤了哪些单」与真实结果不一致。
- 复现步骤: 直接跑该用例：`CoverageTests_btcore --gtest_filter=P08HftOrderCancel.CancelByCodeAndDirection`，
  失败点在 `:233` 与 `:236`（两处都是 `_orders.size()` 少 1）
- 影响范围: 依赖返回值做后续对账/风控计数的高频链路会低估已撤单量。
- 可疑机制（**我只写到实测能支撑的程度**）：`:578` 在遍历 `_orders` 的过程中，对每个命中项调用
  `:548` 的按 id 重载，而该重载 post 的任务体在 `:567` `erase(localid)` —— 「遍历中的容器被后续任务修改」
  与 `:587-590` 的 `if (left < qty) qty -= left; else break;` 组合，使「命中即 break」的判定
  与 `ret` 的实际收集数不同步。**具体是 break 条件少算还是 erase 溢出，需要产品侧确认**，
  本条只登记可复现的输入输出事实与嫌疑位置，不下未经证明的结论。
- 备注: 该用例保持红；本轮未修改产品代码，也未修改用例断言（先前对同文件其它断言的改动是
  `_hook_valid` 初值那条，依据 `HftMocker.cpp:128`，见 `reports/FAIL裁定记录-20261027.md` A-06）。
