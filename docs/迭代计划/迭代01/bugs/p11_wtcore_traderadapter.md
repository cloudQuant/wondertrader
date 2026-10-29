# lane P11 疑似缺陷登记 — src/WtCore/TraderAdapter.cpp（交易回报/风控实体分支）

工作流: 测试覆盖率迭代01 / 第四波 lane P11。
被测范围: `src/WtCore/TraderAdapter.cpp`（2423 行，实测未覆盖 1751 行）与 `TraderAdapter.h`。
**未修改任何生产代码**，以下状态均待裁定。本轮按当前 WtCore 源码、`ITraderApi` 接口和测试源文件复核；没有运行测试或实盘。文中测试名和断言只说明测试代码写了什么，不表示本轮已执行。

---

## BUG-P11-001 initExt 缺登录配置，成功连接事件会解引用空 `_cfg` —— Major，条件性候选

- 位置: `src/WtCore/TraderAdapter.cpp:1411`（`handleEvent` 的 `WTE_Connect`/`ec==0` 分支）
- 证据: 构造函数把 `_cfg` 置 NULL；`initExt()`(:209-235) 调 `api->init(NULL)` 但不赋 `_cfg`；`run()`(:404-416) 先注册 SPI 再 connect；`handleEvent(WTE_Connect, ec==0)`(:1405-1412) 随后从 `_cfg` 读取账户并调用 login。只要实现 `ITraderApi` 的 API 在成功 connect 时按接口事件回调 `WTE_Connect`，该路径会空指针解引用。
- 证据边界: `ITraderApi` 分开声明 connect/login，但没有说明 `initExt` 是否用于 API 自行管理登录。P11 的 DISABLED 用例直接调用 `handleEvent`，不是由 FakeTraderApi 经 `run/registerSpi/connect` 发出事件；因此是源代码路径证据，不是 API 端到端复现。
- 影响范围: 使用 `initExt` 且连接成功事件交由 Adapter 登录的 API 会触发；不据此推断所有外部注入 API 必然走此模式。
- 修复方向: 为 `initExt` 明确配置来源：接收并保存登录参数，或明确标记 API 自行登录并绕过 Adapter 的 login 分支。简单判空后静默 return 会避免崩溃但可能让通道永远未登录；应明确失败状态并测试真实 SPI 事件链。

## BUG-P11-002 回报元素空指针缺少边界处理 —— 待验证，不作为已确认柜台缺陷
- 状态: 已撤回（迭代02：TEST，原产品主张不成立）

- 位置: `TraderAdapter.cpp:303-389`（`saveData`）、`:1514-1531`（账户数组元素）、`:1739-1749`（成交查询元素）、`:2148-2152`（成交推送对象）、`:2337-2344`（错误对象与 notifier）。
- 代码事实: `onRspAccount`/`onRspTrades` 已允许整个数组为 NULL；实际 `TraderOES` 在空结果时会传 NULL（`:481`、`:547`），这两处由外围 `if (array)` 安全处理。数组非空但元素为 NULL 时会解引用；账户回报在 `_save_data` 为真时先进入 `saveData()`，其 `:357-372` 也直接解引用元素。`onPushTrade(NULL)` 会在首行解引用；`onTraderError(NULL)` 只有在 `_notifier != NULL` 时会在日志守卫之外再解引用。
- 接口与生产证据边界: `ITraderApi.h` 回调签名没有写明 payload/数组元素的 nullability。已查到的 OES 空查询结果是 NULL 整体而非 NULL 元素，且当前 Adapter 已防护整体 NULL；本轮没有证据表明任何具体柜台会把 NULL 元素、NULL 成交对象或 NULL 错误对象传入这些路径。
- 测试边界: P11 禁用用例依次先调用 BUG-P11-001 的成功连接分支，后续空元素分支不能从该函数得到独立运行证明。本轮未运行该用例。
- 裁定与修复方向: 这是可防御性缺口，是否属于生产缺陷取决于各 API 的回调契约。若 SPI 允许空对象，数组循环 `continue`、空成交早退、错误对象为空时不通知并写出可诊断日志；若 API 明确禁止，则补接口契约和适配器断言/测试，避免把人工构造的非法输入当作柜台故障。严重级别在契约确认前不定为 Blocker。

## BUG-P11-003 流量风控缓存键与标准代码检查键不一致 —— Major，重复项

- 状态: 已合并（BUG-R3-001）
- 写入侧: `TraderAdapter.cpp:517` `_order_time_cache[entrust->getCode()]`，而 `entrust->getCode()`
  在 :498 已被改写为**交易所原始代码**（如 `ag2512`）；撤单侧 `:1286` `_cancel_time_cache[ordInfo->getCode()]` 同理。
- 检查侧: `checkOrderLimits`(:631) / `checkCancelLimits`(:566) 用上层传来的**标准代码**（如 `SHFE.ag.2512`）查表。
- 证据: `doEntrust` 在 `:498` 把委托代码改成合约 alt/raw code，再于 `:517` 用该值写 `_order_time_cache`；撤单侧 `:1286` 用 `ordInfo->getCode()` 写 `_cancel_time_cache`。检查侧 `:566`、`:631` 按传入代码查表，`doCancel` 在 `:1259` 传标准化 `stdCode`。代码路径能证明标准代码查询和原始代码写入不匹配。
- 测试边界: R90 的旧测试把标准码放行当作期望；现改为按标准码拒绝，并确认 3 笔委托和 3 笔撤单请求均使用 raw code。修正后的本地替身用例 FAIL：两次标准码频控检查均返回 true。P11 的旧 `order_time_cache_never_hits_std_code` 也已改为 `order_frequency_window_rejects_standard_code_after_successful_orders`，4 笔成功委托后正确拒绝断言同样 FAIL；未达撤单限额及未下单品种的对照断言未失败。夹具先附加 fake API，缺模块配置的 init 按前提返回 false，结果属于局部状态验证，不是正常柜台端到端证明。`R3_wtcore.md` BUG-R3-001 是同一键空间缺陷，P11-003 不重复计数；不能笼统写成“频率分支不可达”。
- 影响范围: 对传入标准代码的检查会查不到 raw-key 缓存；不声称所有对外查询键都无法命中。
- 修复方向: 缓存写入和读取统一用标准代码（优先在 `doEntrust`/`doCancel` 处）；补标准键和原始键输入的明确契约及频率阈值回归。原登记中关于 `extractStdCode` 接收 raw code 的越界推断与该根因无关，未在本轮确认，移出本条。

## BUG-P11-004 首次收到本地终态单时仍创建订单表并入表 —— Major

- 位置: `TraderAdapter.cpp:2118-2131`

```cpp
if (!orderInfo->isAlive() && _orders) { _orders->remove(localid); }
else { if (_orders == NULL) _orders = OrderMap::create(); _orders->add(localid, orderInfo); }
```

- 现象: `localid != 0` 时，终态分支只有 `_orders` 已存在才 remove；如果 map 尚未创建，代码落入 else 并建表加入终态对象。对照 `onRspOrders`(:1695-1708) 会在 `!isAlive()` 时跳过入表。后续 `getOrders()` 因此可能返回不活跃记录；按 localid 撤单会由 `doCancel` 的 `isAlive()` 拒绝。
- 测试前提核对: 旧 `external_orders_do_not_touch_undone` 前两次 `PX1` 是外部单，随后 `wrong` 使用 `kTag1001`，实际为本地终态单；旧断言把待审行为固定为 map 大小 1。现拆成独立外部单对照和 `first_local_canceled_push_is_not_returned_as_alive`，后者接受 NULL 或空表作为无活单结果。
- 证据边界: 重建后外部单对照 PASS；首次本地终态单用例 FAIL，实际返回的表含 1 笔终态订单。与 `onRspOrders` 排除非活单及已有表时移除终态的逻辑不一致；仍未确认所有柜台首个回报的时序，也需明确 `getOrders()` 的终态语义。
- 修复方向: 若订单表语义是当前活单，改为终态时无条件不入表，并仅在已有 map 时 remove；更新测试期望为无订单表或空表，并覆盖“先活单后终态”与“首回报即终态”。

## BUG-P11-005 撤单/错单处理未校验合约信息 —— 生产路径待验证

- 位置: `TraderAdapter.cpp:1249-1250`（`doCancel`: `cInfo->getCommInfo()` 未判 `cInfo`）、
  `:1455-1456`（`onRspEntrust` 错单分支同样直接用 `entrust->getContractInfo()`）。
- 代码事实: `doCancel()`(:1244-1257) 解引用 `getContractInfo()` 和 `getCommInfo()`；但 `_orders` 的已知写入点 `onRspOrders`(:1615-1621) 与 `onPushOrder`(:1873-1880) 都先检查 `cInfo`，故从 Adapter 正常维护的订单表进入的撤单路径不会因缺少 `cInfo` 触发此项。`onRspEntrust` 错误分支(:1450-1458) 无同等检查。
- 测试与接口边界: DISABLED 用例用 `WTSEntrust::create(...)` 直接构造未附合约信息的对象，再直接调用 SPI handler；本轮未运行。已检查的 CTP、XTP、OES `makeEntrust` 实现会先查合约，缺合约时返回 NULL，成功时设置 `ContractInfo`；这不证明所有第三方 API 都遵守相同约束，`ITraderApi` 也未明文声明该字段必非空。
- 裁定与修复方向: `doCancel` 这一报告入口按当前内部 map 不成立；`onRspEntrust` 对未知/第三方 API 的输入仍待接口契约确认。可在公共回调边界安全早退并记录缺失字段，但先补真实适配器回调构造测试；不把人工构造裸对象当作已确认柜台故障。

## BUG-P11-006 getRiskParams 无 default 时返回 end() 解引用 —— 重复项，不另计

- 状态: 已合并（BUG-N01-003）
- 位置: `TraderAdapter.cpp:396-401`
- 证据与处置: 与 `n01_wtcore_tradepaths.md` BUG-N01-003、`R181.md` 的复核为同一函数及同一条件，不另计。源码静态确认；P11 禁用用例没有单独到达本分支，本轮未运行。缺 default 时不能简单返回 NULL，因为调用点会把 NULL 当作允许交易；请按 BUG-N01-003 的初始化 fail-closed 修复方向统一处理。

## BUG-P11-007 initExt 强制启用落盘，测试输出目录仅有进程级接缝 —— Minor，需裁定是否产品需求

- 位置: `TraderAdapter.cpp:242-245`（`WtHelper::getBaseDir() << "traders/" << _id << "//"`）、
  `:379-389`（`rtdata.json`）
- 代码事实: `initExt()` 无条件 `_save_data=true` 并调用 `initSaveData()`(:223-225)，CSV 文件和 `rtdata.json` 位置取 `WtHelper::getBaseDir()/traders/<id>`(:237-278)。这意味着外部注入 Adapter 即使不要求落盘也会创建目录/CSV，并在账户回报时写快照。
- 修正错误论据: 测试并非完全不能改根目录；`WtHelper::setGenerateDir()`(:WtHelper.h:132) 可设置 `getBaseDir()` 使用的全局生成目录。不过 `getBaseDir()` 在 `WtHelper.cpp:95-100` 用函数局部 static 缓存首次结果，必须在首次读取前设置，且不是 adapter 级隔离。P11 fixture 当前按同一路径清理，但本轮未运行。
- `rtdata.json` 行为: `saveData()`(:379-388) 调用 `BoostFile::create_new_file()`；`Share/BoostFile.hpp:44-50` 明确在文件打开后执行 `truncate_file(0)`，因此重写变短不会残留旧尾部。原登记关于不截断的次生问题不成立。
- 裁定与修复方向: 固定输出根目录是现有全局配置设计；是否应让 `initExt()` 默认保存数据需产品契约确认。若外部/测试通道不应落盘，提供明确 savedata 开关或 per-adapter 输出目录；仅测试隔离时可在任何 `getBaseDir()` 调用前设置 generate dir，并清理私有临时目录。

---

统计: 7 条记录：2 项条件性源码候选（P11-001/004）、2 项接口前提待验证（P11-002/005）、2 项重复引用（P11-003/006）、1 项落盘需求与测试隔离待裁定（P11-007）。本轮未运行测试，未改动源码。
