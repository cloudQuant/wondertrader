# lane P11 疑似缺陷登记 — src/WtCore/TraderAdapter.cpp（交易回报/风控实体分支）

工作流: 测试覆盖率迭代01 / 第四波 lane P11。
被测范围: `src/WtCore/TraderAdapter.cpp`（2423 行，实测未覆盖 1751 行）与 `TraderAdapter.h`。
**未修改任何生产代码**，以下全部状态一律「待审核」。证据均为本 lane 用例的实测行为或源码行号。

---

## BUG-P11-001 handleEvent 无条件解引用 `_cfg`，initExt 通道必段错误 —— Blocker，待审核

- 位置: `src/WtCore/TraderAdapter.cpp:1411`（`handleEvent` 的 `WTE_Connect`/`ec==0` 分支）
- 现象: `_trader_api->login(_cfg->getCString("user"), _cfg->getCString("pass"), WT_PRODUCT)`，
  而 `initExt()`(:209-235) 全程不设置 `_cfg`（构造里 `_cfg(NULL)`），
  `init()` 也只在 `_cfg==NULL` 时才赋值 (:110-113)。
  用 `initExt` 装配的交易通道（回测/Mock/外部注入 api 的用法）一旦收到「连接成功」事件就是空指针解引用。
- 复现: `p11_crash_class_test.cpp` 的 `DISABLED_report_paths_that_dereference_null_before_run` 第 1 条。
- 建议: `if(_cfg == NULL) { 打错误日志并 return; }`，或让 initExt 也建立最小 `_cfg`。

## BUG-P11-002 回报数组/指针的空元素与空对象不判空 —— Blocker，待审核

- 位置:
  - `TraderAdapter.cpp:1524-1531` `onRspAccount`: `at(idx)` 取到 NULL 后直接 `fundInfo->getCurrency()`。
  - `TraderAdapter.cpp:1743-1747` `onRspTrades`: 未判 `tInfo == NULL` 就 `tInfo->getContractInfo()`
    （同函数上游 `onRspOrders` 是有 `if (orderInfo == NULL) continue;` 的，两条链不对称）。
  - `TraderAdapter.cpp:2148-2150` `onPushTrade`: `tradeRecord` 本身不判空（`onPushOrder` 判了）。
- 现象: 柜台实现只要多塞一个 NULL、或上层把空指针直接推给 `onPushTrade`，进程立即段错误；
  而 `onTraderError` 的 `if(err)` 判空后又在 `_notifier` 分支里无条件 `err->getMessage()`（:2339-2343），
  即「配了事件通知 + 空错误指针」也是段错误。
- 复现: 同 DISABLED 用例第 5/6/7 条。
- 建议: 统一在循环里 `if (obj == NULL) continue;`，`onTraderError` 把 notifier 分支挪进 `if(err)` 内。

## BUG-P11-003 流量风控的时间缓存键与检查键不一致，频率窗口永不命中 —— Major，待审核

- 写入侧: `TraderAdapter.cpp:517` `_order_time_cache[entrust->getCode()]`，而 `entrust->getCode()`
  在 :498 已被改写为**交易所原始代码**（如 `ag2512`）；撤单侧 `:1286` `_cancel_time_cache[ordInfo->getCode()]` 同理。
- 检查侧: `checkOrderLimits`(:631) / `checkCancelLimits`(:566) 用上层传来的**标准代码**（如 `SHFE.ag.2512`）查表。
- 实测: `p11_preorder_risk_test.cpp::order_time_cache_never_hits_std_code` ——
  连下 4 单（边界 `order_times_boundary=2`）后 `checkOrderLimits("SHFE.ag.2512")` 仍返回 true；
  撤单一次后 `checkCancelLimits` 同样返回 true。即 :575-591 / :640-656 的
  `lower_bound` 裁剪与「超出频率封禁」整段逻辑在生产路径上不可达，
  「为控制电光流量而设」的风控实际失效。
- 次生风险: 若上层真的按原始代码调用（想让它命中），`getRiskParams`(:394) 里的
  `CodeHelper::extractStdCode("ag2512", NULL)` 在找不到 '.' 时 `idx = npos`，
  随后 `wt_strcpy(_exchg, stdCode, npos)` 与 `stdCode + npos + 1` 都是越界（`Share/CodeHelper.hpp:610-616`）。
- 建议: 写入侧统一用回报链算好的标准代码，或检查侧同时查两种键；`extractStdCode` 补 `idx == npos` 防御。

## BUG-P11-004 `_orders` 为空时死单被塞进订单表，与「移除死单」分支相反 —— Major，待审核

- 位置: `TraderAdapter.cpp:2118-2131`

```cpp
if (!orderInfo->isAlive() && _orders) { _orders->remove(localid); }
else { if (_orders == NULL) _orders = OrderMap::create(); _orders->add(localid, orderInfo); }
```

- 现象: 订单已终态（全部成交/已撤销/错单）时，若订单表还没建立，走 else 分支——
  建表并把这笔死单加进去；等表已存在时同样的死单却是 remove。
  后续 `getOrders()`/`cancel(stdCode,...)` 会看到这笔试图撤掉的死单（`doCancel` 靠 `isAlive()` 兜住才没发撤单）。
- 实测: `p11_push_notify_test.cpp::external_orders_do_not_touch_undone`
  断言「首笔终态单推送后 `getOrders("")` 的 size==1」。
- 建议: `else if (orderInfo->isAlive())` 才入表。

## BUG-P11-005 doCancel / onRspEntrust 不校验合约信息 —— Minor，待审核

- 位置: `TraderAdapter.cpp:1249-1250`（`doCancel`: `cInfo->getCommInfo()` 未判 `cInfo`）、
  `:1455-1456`（`onRspEntrust` 错单分支同样直接用 `entrust->getContractInfo()`）。
- 现象: 由 `onRspOrders`/`onPushOrder` 建起来的订单一定带 `cInfo`，所以撤单链暂时安全；
  但委托错单回报是柜台直接喂的 `WTSEntrust`，外部构造（未 setContractInfo）即段错误。
- 复现: 同 DISABLED 用例第 7 条。

## BUG-P11-006 getRiskParams 无 default 时返回 end() 解引用（复核 BUG-N01-003） —— Major，待审核

- 位置: `TraderAdapter.cpp:396-401`
- 现象: 品种未命中且 `_risk_params_map` 里没有 `default` 时返回 `&it->second`（`it == end()`），
  因此 `checkOrderLimits`/`checkCancelLimits` 里的 `if (riskPara == NULL) return true;`（:553/:618）永不成立，
  拿到的是野指针指向的内容。`init()` 只在缺 default 时打一条 WARN（:147-151），不阻断装配。
- 处置: 本 lane 所有风控用例的配置都带 `default` 规则，未启用该路径；已在 manifest 的跳过清单点名。

## BUG-P11-007 initSaveData/saveData 落盘根目录不可注入 —— Minor，待审核

- 位置: `TraderAdapter.cpp:242-245`（`WtHelper::getBaseDir() << "traders/" << _id << "//"`）、
  `:379-389`（`rtdata.json`）
- 现象: `initExt()` 无条件 `_save_data = true`（:223），因此任何用 initExt 的通道都会在
  `WtHelper::getBaseDir()`（进程级函数内 static，`WtHelper.cpp`）下建目录写文件，测试侧无法改道；
  用例只能在同一规则下算出路径并在 `TearDown` 里 `remove_all` 清理。
  另外 `saveData` 用 `create_new_file` 且不截断，`rtdata.json` 变短时尾部会残留旧字节。
- 建议: `_save_data` 与输出根目录改为配置项，或提供 setter 供测试注入。

---

统计: 7 条（Blocker 2、Major 3、Minor 2），全部「待审核」，源码零改动。
