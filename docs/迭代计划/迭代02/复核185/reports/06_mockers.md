# 06_mockers 第二轮复核

## 汇总

7 项完成逐条源码、接口、正常调用链和测试前置复核：**PRODUCT 4、NOT_BUG 3、TEST 0、DUPLICATE 0、NEEDS_DECISION 0、EXTERNAL_BLOCKED 0、LOCAL_INCOMPLETE 0**。所有行证据等级均为 `STATIC`；本轮没有运行会触发目标缺陷的 active MQ、disabled UB 或 malformed-snapshot 探针。

主代理已刷新 CoverageTests 目标。定向运行使用 `CoverageTests_btcore`，SHA-256 `430676e26c035f16dc733f23f4e72196a86ed5d4373f3b86ebce7d554be370d4`；每个 filter 独立进程、独立临时 cwd、独立 LLVM profile/XML、15 秒 timeout。命令与原始 stdout/return code 在 `evidence/06_mockers/runs.json` 和各 `.log`。10 个单例中 8 PASS、2 FAIL；失败均已判断性质，不把单测结果冒充产品缺陷复现。

## PRODUCT

### R20_wtbtcore#3：必需 MQ 符号校验与资源配对缺失

**结论：PRODUCT。** `WtMsgQue.h` 明确 server ABI 包含 `regiter_callbacks`、`create_server`、`destroy_server`、`publish_message`。`EventNotifier::init` 只验证 `create_server`；`regiter_callbacks` 可空解引用，`destroy_server` 缺失则创建的 server 无法被成员析构释放。只有 publisher 有清晰的缺失 no-op 注释和守卫。

正常生命周期也有资源未释放：`release_backtest` 的 API 文档要求回测后释放系统资源，但 Runner::release 只停 logger；Notifier server 只在静态 Runner 进程析构时释放。`config_backtest` 的注释说重复调用忽略，实际 static guard 从未置位；Runner `_inited` 也未置位，重复配置可以覆盖 `_mq_sid`，使后续清理漏掉旧 ID。

**修复/验收：** 创建前验证 create/register/destroy 三个必需符号；publisher 继续可选。保存模块句柄，用幂等 release 按 `destroy_server -> 清状态 -> unload module` 配对清理；Runner 的 release 与析构调用它，配置成功后设置 one-shot guard。用 fake plugin（不打开 socket）测试符号矩阵、release 恰好销毁一次、析构不重删及重复 config 不新增 server。

### R20_wtbtcore#5：重复撤单重复扣未完成量

**结论：PRODUCT。** 订单进入 `state=9` 后，下一 tick 之前重复调用 cancel 仍返回同一 `_left`；ExecMocker 每次收到非零变化都扣 `_undone` 并增加撤单计数。同一 pending order 因重试被记账两次，可能使 undone 为负。

**测试前置错误：** 当前 `DISABLED_CancelByIdDoesNotRestoreUndoneTwice` 使用未成功 init 的裸 ExecMocker，不能证明账本行为。已在 JSON 中提出有效 executer/fake fixture 的最小替换与 filter。

### BUG-P08-006：未知商品的 UFT exit 空解引用

**结论：PRODUCT（仅限未知 code exit）。** `stra_enter_long/short` 对 `get_commodity_info()==NULL` 明确报错并返回0；`stra_exit_long/short` 却先插入 `_pos_map` 项，再对 NULL `commInfo` 调 `getCoverMode()`。这是公开策略上下文中不对称的输入处理。未知代码应安全返回0且不污染 state。

将候选中的“撤单时运行中商品 metadata 消失”排除：当前 replayer 基础目录初始化后没有支持的运行时删除流程，正常订单只能由已注册商品建立。现有 disabled 家族先触发 null strategy 回调，需拆出隔离未知 exit 用例。

### BUG-Q02-004：增量快照缺少 schema/长度验证与原子失败

**结论：PRODUCT。** 公共 `load_incremental_data` 读取上一轮输出文件，却未检查文件打开结果、JSON parse 状态、根/字段类型或 required members；坏 JSON 可触发 RapidJSON assertion/非法访问。`opentag` 与条件 `usertag` 使用 `strcpy` 进入 32 字节数组，长输入越界。CSV logs 在 JSON 验证前已追加，positions/details/fund/signals/conditions 又逐项写入，后续失败会留下半状态。

此外 writer 保存 `conditions.settime`，loader 只读取 `conditions.items`；`_last_cond_min` 构造时没有初始化，随后 dump 会读出未定义状态。验收需同时保证 writer-reader 字段闭环。

**本轮测试错误：** `Q02CtaDumpTest.strategyDumpRoundTripsIntoSecondMocker` 本轮运行 1/1 失败于断言文本：writer 先写 `settime` 再写 `items`；条件 item 用 `usertag`，不是 `opentag`。已给出 `q02_cta_dump_incremental_test.cpp:357-358` 精确修正，同时提出 malformed/schema/type/length 与失败原子性回归用例。当前 round-trip 失败归为测试 oracle 错误，不改变独立的产品输入安全结论。

## NOT_BUG

### BUG-L06-006：`EventNotifier::init(nullptr)`

**结论：NOT_BUG。** 产品调用入口 `WtBtRunner::initEvtNotifier` 对 NULL/non-object 配置先返回，仓库未定义直接 NULL 为有效契约。现有 disabled case 只有 GTEST_SKIP，属于不支持的内部直接调用；建议撤销其缺陷主张或明确标成前置条件说明。

### BUG-P03-002：CTA/SEL 动态盈亏的缺商品解引用

**结论：NOT_BUG（当前支持的正常回测流程）。** CTA/SEL 的正常 `proc_tick` 才进入更新器；建仓/设仓路径先校验 commodity，缺商品时不会建立订单或 tick 订阅。replayer 的 tick 排程在向 sink 回调前也需要对应 session；`get_session_info` 对缺商品返回 NULL，不能构成正常动态盈亏调用路径。增量快照可以恢复历史 position，但未知 code 本身不产生合法 replay tick。该结论只撤销动态盈亏路径可达主张，不替 `calc_fee` 或其他空指针风险背书。

本轮有效的 CTA futures 完整开平仓/浮盈用例和 unknown-code enter guard 均 PASS。

### BUG-Q02-006：同一 Mocker 重复加载会追加重复行

**结论：NOT_BUG。** 追加行为确实存在，但接口注释描述从一个上次回测恢复以继续运行；仓库正常闭环是一新实例加载一次，round-trip 也使用第二个新 Mocker。没有 API/测试约定要求同一快照重复导入是幂等替换或拒绝，不按当前候选推导产品缺陷。若未来支持 retry/多源导入，应先定义语义和结果返回。


## 定向运行观察

- PASS：Notifier inactive 配置、ExecMocker 缺模块早退、CTA futures 正常开平仓与浮盈、CTA unknown-code guard、增量缺目录、合法缺 details、UFT unknown enter guard、known-code 无仓位 exit。
- FAIL：`Q02CtaDumpTest.strategyDumpRoundTripsIntoSecondMocker`：两条错误的 JSON 子串 oracle（已上报主代理）。
- FAIL（范围外，不计入本组 7 项）：`P08UftOrderPosition.CloseTodayBranchScalesDynProfit` 的全平 detail 清理断言正确，产品 offset=2 分支在 `UftMocker.cpp:1308-1310` 用半开区间 `erase(sit, eit)` 漏掉 `eit`；已即时通知主代理作为额外产品缺陷候选。未改测试。

## 证据与限制

测试原始输出、每条过滤器、时间、XML count 与目标 SHA 位于 `evidence/06_mockers/`。本轮未运行 real MQ、disabled UB、malformed snapshot、ASan/UBSan；这些未验证项没有写成 PASS。产品修复与后续回归不在本审计的源码/测试写入范围内。
