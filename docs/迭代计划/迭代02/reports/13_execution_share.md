# 13_execution_share 逐条复核与迭代计划

日期：2026-10-29  
输入：`/tmp/wondertrader-review-20261029/groups/13_execution_share.json`（46 条）  
范围：仅静态对照原记录、当前实现、公开接口和现存测试源码。未运行测试、未构建、未联网，未修改原 bug 清单、测试、生产代码或 build；只新增本报告与 triage JSON。所有证据等级均为 STATIC；旧记录中的历史运行结果只作线索，不作为本轮复现证据。

## 裁定计数

| 裁定 | 数量 | 处理 |
|---|---:|---|
| PRODUCT | 19 | 静态路径对有效调用、正常生命周期或明确接口合同成立，作为修复候选 |
| UNDECIDED | 25 | 行为风险存在，但输入范围、生命周期或公开合同尚未确认；先补合同/可达性证据 |
| DUPLICATE | 2 | BUG-R7-010 并入 BUG-R17-002；BUG-N05-002 并入 BUG-R17-004 |
| TEST | 0 | 没有把仍含产品边界风险的候选整体判为纯测试误报；错误夹具在相应条目中单独说明 |
| **合计** | **46** | 逐条原因、修法、验收条件及测试边界见 `triage/13_execution_share.json` |

PRODUCT / UNDECIDED / DUPLICATE 项均标 STATIC；本轮没有 LOCAL_REPRO。测试目录中的既有通过、失败、disabled 或 child probe 仅作为测试源码证据，没有在本轮运行。

## 逐条结论

| 编号 | 裁定 | 结论 |
|---|---|---|
| BUG-R17-001 | PRODUCT | 终态订单仍留在超时扫描集合；保留晚到成交关联时应与超时资格分离。 |
| BUG-R17-002 | UNDECIDED | price mode 越界静态成立，但测试输入在声明的模式域之外；先定非法配置合同。 |
| BUG-R17-003 | PRODUCT | 差异执行器对手价模式卖侧 offset 重复应用，和买侧/普通 MinImpact 不一致。 |
| BUG-R17-004 | UNDECIDED | session 查询可能返回 NULL，初始化仍成功且行情转换无守卫；缺 session 是否为有效配置需按 schema 确认，与 N05 时钟字段问题分开。 |
| BUG-R17-005 | UNDECIDED | 直接 NULL cfg 崩溃，但 runner 当前先检查 data 节点，接口未承诺 cfg nullable。 |
| BUG-R19-001 | UNDECIDED | commit 只改 section 时间戳，slave 非强制更新只看 block 版本；新增 key 发布语义未定。 |
| BUG-R19-002 | PRODUCT | slave 刷新忽略 state tombstone，删除段仍可经 KV getter 读到旧值。 |
| BUG-R19-003 | UNDECIDED | 当前进程删除不释放槽位属源码事实；接口没有即时复用承诺，已有测试只锁定现状。 |
| BUG-R19-004 | UNDECIDED | init_master 的 state=0 有 mark-and-sweep 注释；同文件多 master alias 是否支持未定。 |
| BUG-R19-005 | PRODUCT | 64 字节字符串槽被拷贝 64 字节后再写 NUL，末尾越界且可能读过源字符串。 |
| BUG-R19-007 | PRODUCT | 每次附着映射都归一化共享队列游标，活动跨圈时会改写其他进程的序号。 |
| BUG-R19-008 | PRODUCT | connect 失败遗留 socket；manager 仍登记对象，start 可能起线程，未 ready 析构直接退出。 |
| BUG-R19-009 | UNDECIDED | _blocktime 未初始化，但不确定比较仅在把 master 传给 slave 更新接口时出现。 |
| BUG-R19-011 | UNDECIDED | 64 字节命令边界缺保护；MAX_CMD_SIZE 对 NUL 的计量和超限处理合同未明确。 |
| BUG-R19-012 | UNDECIDED | path 缺省固定为 .cmd；不同 name 是共享队列还是应隔离没有约定。 |
| BUG-R19-013 | UNDECIDED | 32 字节 name/key 写入可覆盖相邻字段；允许长度上限和 ABI 兼容策略未公开。 |
| BUG-R53-001 | PRODUCT | createCancelAction 裸 new 创建池对象，release 终态会使用未设置的池/锁。 |
| BUG-R53-002 | PRODUCT | map add 获得容器引用，remove/clear 会 release，erase 只摘节点而丢失该引用。 |
| BUG-R53-003 | UNDECIDED | disabled 测试本身把 16 字节写入 exchg[10]，夹具无效；旧格式满宽字节和 NUL 语义仍待确认。 |
| BUG-R56-001 | PRODUCT | 查询早于第一条换月规则时对 lower_bound(begin) 执行递减。 |
| BUG-R56-003 | UNDECIDED | 没有当前行时 getter 可能越界，但 API 只承诺 next_row 成功后装载一行，状态行为未明确。 |
| BUG-R56-004 | UNDECIDED | loadHots 忽略规则加载失败仍返回 true；文件必需性未定，活动测试把缺文件视为空规则成功。 |
| BUG-R7-001 | PRODUCT | VWAP 两个单元在总时长赋值前计算发单间隔；有效尾部时间会得到零间隔或无符号下溢。 |
| BUG-R7-002 | UNDECIDED | total_times=0 会除零，但次数范围和非法配置失败合同未明确，现有 child probe 未证明安全性。 |
| BUG-R7-003 | UNDECIDED | 缺失/短分布文件后仍可进入 unchecked vector 下标；profile 必需性和格式长度合同未确认。 |
| BUG-R7-004 | UNDECIDED | 净仓为零但仍有多头/空头子仓时兜底分支不可达；此账户语义是否受支持未定。 |
| BUG-R7-005 | PRODUCT | StockVWAP 在判空前读取 _last_tick；channel-ready 是独立公开回调顺序，可早于首 tick。 |
| BUG-R7-006 | UNDECIDED | 商品信息缺失可返回 NULL 且执行单元未一致守卫；有效执行代码是否必须有商品资料待合同确认。 |
| BUG-R7-007 | PRODUCT | StockVWAP 用 undone 推断买方向，并在卖分支覆盖切片量；方向和 VWAP 分片数量可错。 |
| BUG-R7-019 | PRODUCT | 实现把 true/false 仓位含义反用，和 ExecuteContext/TraderAdapter 明确合同相反；现有 fake 也反向，须修测试夹具。 |
| BUG-R7-010 | DUPLICATE | 与 BUG-R17-002 同为非法 price mode 索引越界，作为 canonical 的股票实现样本。 |
| BUG-R7-011 | UNDECIDED | 缺 basefiles 会解引用 NULL，但完整配置 schema 是否要求该字段未确认。 |
| BUG-R7-012 | PRODUCT | 启动时撤销未管理订单可早于本轮目标建立，撤单回报会读取未初始化 _this_target 并重发。 |
| BUG-R7-013 | PRODUCT | minopenlots 被代码当作可选项，但成员无默认初始化；省略字段后下单量不确定。 |
| BUG-R7-014 | UNDECIDED | 常规与 fire_at_once 的 mode=1 取价不同；撤单重发是否故意固定采用对手价未定。 |
| BUG-R7-015 | PRODUCT | 三类 do_calc 对成员持有的 _last_tick 做未配对 release；fire_at_once 才有匹配 retain。 |
| BUG-R7-016 | PRODUCT | 两个股票 helper 释放上下文透传的借用 commodity 指针，可能消耗不属于 helper 的引用。 |
| BUG-R7-017 | UNDECIDED | 当前生产 adapter 检查到的回调仅发送失败值；测试手工调用成功值不能证明生产影响，公开接口也未禁止成功回调。 |
| BUG-R84b-001 | UNDECIDED | nullptr sid 没有 nullable 契约，仓内调用使用非空字符串；仅凭直接 NULL 测试不能定性。 |
| BUG-N05-001 | PRODUCT | _cur_* 未初始化，getter 直接读取，首个有效行情也会先读 _cur_date/_cur_act_time。 |
| BUG-N05-002 | DUPLICATE | 与 BUG-R17-004 同为 session NULL 后行情转换解引用，交叉引用不重复计数。 |
| BUG-N05-003 | PRODUCT | WtOrdMon 写入上锁，但 empty 检查、has_order 和 clear_orders 未锁；重叠事件回调时共享 map 可竞争。 |
| BUG-WSN06-001 | UNDECIDED | createStrategy 以 strcmp 读取 name，但工厂未承诺 nullable，NULL 调用不构成已证有效输入。 |
| BUG-WSN06-002 | UNDECIDED | UFT/HFT init 解引用 params，runner 可传 NULL；params 是否允许省略、缺参失败语义未定。 |
| BUG-WSN06-004 | UNDECIDED | CTA 窗口为零会除零/空窗口计算，但零值是否非法需要配置 schema 证明。 |
| BUG-WSN06-005 | UNDECIDED | MQ manager 忽略 init 结果；C API 只规定 ID 返回，没有说明失败哨兵或 ID 可用性语义。 |

## 建议迭代分批

| 批次 | 范围 | 交付门槛 |
|---|---|---|
| 02-A 静态已确认的生命周期/内存/状态错误 | R17-001/003、R19-002/005/007/008、R53-001/002、R56-001、R7-001/005/007/019/012/013/015/016、N05-001/003 | 修复按公开输入和正常生命周期验收；R7-019 先纠正 fake 对 getPosition 参数的映射，测试 oracle 对齐 ExecuteDefs 与 TraderAdapter。 |
| 02-B 先补配置、调用和共享 ABI 合同 | R17-002/004/005、R19-001/003/004/009/011/012/013、R53-003、R56-003/004、R7-002/003/004/006/011/014/017、R84b-001、WSN06-001/002/004/005 | 明确有效值、生命周期、缺失资源/配置行为、字符串编码和所有权；按合同把每项转为 PRODUCT、TEST 或保留未决。 |
| 02-C canonical 合并 | R7-010 → R17-002；N05-002 → R17-004 | 原记录保留交叉引用；验收与修复只计一次。 |

## 测试边界与错误夹具

- 本轮没有运行测试；文件/行引用是测试源码位置，不代表本轮 PASS/FAIL。
- R7-019 的现有 fake 把 validOnly 参数反着实现。产品结论独立来自 ExecuteDefs 与 TraderAdapter；修复验收前必须让 fake 语义与合同一致。
- R7-017 的测试手工发成功回调，并明确说明不证明 adapter 会发送；在完整调用图确认前，只能作为直接方法行为，不认定生产重复下单。
- R53-003 测试对 exchg[10] 写 16 字节，测试前置自身越界；但旧格式是否允许满宽原始字段仍未解决，故整条保留 UNDECIDED。应拆出合法 max-1+NUL 对照与不越界原始字节合同测试。
- R19-003 当前测试断言不复用槽位，证明的是现状；不支持“删除即释放”这一未写入头文件的承诺。
- R19-011/013、R7-002/003/006/011、WSN06-001/002/004 都涉及越界值、缺失配置或基础资料，需先用配置 schema/API 合同确认输入有效性，避免把无效输入的崩溃直接升级为产品缺陷。
- 只把源码静态可达性写为静态结论；真实柜台、网络连接、并发 sanitizer、ASan/UBSan、生产资源泄漏均未在本轮验证。

