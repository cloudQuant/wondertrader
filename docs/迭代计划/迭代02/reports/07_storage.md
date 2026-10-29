# 07_storage 逐条复核与迭代计划

日期：2026-10-29  
输入：`/tmp/wondertrader-review-20261029/groups/07_storage.json`（40 条）  
范围：仅静态对照候选、当前实现、公开接口和现存测试。未运行测试、未使用/修改共享 build，未改原 bug 清单、测试、生产代码或 build 文件；只新增本报告和 triage JSON。所有结论均不宣称本轮动态复现。

## 裁定计数

| 裁定 | 数量 | 含义 |
|---|---:|---|
| PRODUCT | 28 | 当前实现对可达输入/生命周期/公开合同有具体错误，保留为迭代修复项 |
| UNDECIDED | 8 | 源码行为或风险可见，但输入合同、缓存语义或边界定义未证实；先补合同证据 |
| DUPLICATE | 3 | 与已有 canonical 的实现和根因相同，不重复计数 |
| NOT_BUG | 1 | 所述错误仅在当前不可达的私有 helper 中，未证明生产调用路径 |
| TEST | 0 | 本轮没有足够证据把候选整体判作纯测试误报；部分测试只能证明现状或覆盖不到该问题 |
| **合计** | **40** | 每条的来源行、测试行、修复和验收条件见 `triage/07_storage.json` |

PRODUCT / UNDECIDED 的证据等级分别标为 STATIC / PENDING；本轮没有 LOCAL_REPRO。候选旧记录中的历史运行结果仅作线索，不作为本轮执行证据。

## 逐条结论

| 编号 | 裁定 | 复核结论与修复方向 |
|---|---|---|
| BUG-R100-001 | PRODUCT | AD reader 在混合日线/分钟线 cache 遍历中复用右界变量；EOD 日线分支会改写后续分钟分支的时间格式。分开边界局部变量；测试须覆盖两种遍历顺序，不能假定 unordered map 每次随机。 |
| BUG-R100-002 | PRODUCT | 历史 queue/detail/trans 的右界回退没有 begin 保护，单记录且右界早于首条时发生指针及无符号索引下溢。逐类型在公开 range API 覆盖 before-first。 |
| BUG-R112-001 | PRODUCT | 非复权 K 线 RT range/count 对空 block 用 `_size-1` 定位；先检查 pair、block 与非零条数，再回到历史/空结果路径。 |
| BUG-R132-001 | PRODUCT | WtDataReader 的三条 CMP_V2 HFT 读取路径都在 string resize/append 后写旧头指针；采用重建后重取指针或独立目标 buffer，并用强压缩输入触发重分配。 |
| BUG-R132-002 | PRODUCT | WtDataReader 的 detail/trans 首次加载误查 queue map，queue-first 同 key 可使文件存在的 detail/trans 返回 NULL。分别查询对应 map。 |
| BUG-R142-001 | PRODUCT | WtRdmDtReader 的 queue/detail/trans 解压重建有同型悬垂头指针写；属于另一 reader 实现，需独立修复。 |
| BUG-R161-001 | PRODUCT | 回放侧忽略块解析失败后，以整条数量分配却复制原始完整文件长度；整条加余数的坏输入会越过 vector 有效长度。失败时拒绝缓存并验证 payload 对齐。 |
| BUG-R1-002 | PRODUCT | WtRdmDtReader 的 detail/trans 历史 cache hit 也误查 queue map；这是与 R132/R74（WtDataReader 实现）相似但独立的 reader 路径。 |
| BUG-R1-003 | UNDECIDED | 无点 PID 的 `ay[1]` 越界静态成立，但标准 PID 是否允许该输入未证实；先确认 PID 格式合同，不能把 malformed 参数直接算有效产品场景。 |
| BUG-R1-005 | PRODUCT | 历史截止日在当前交易日以前且无历史 bar 时，`readKlineSlice` 仍取空 vector 下标 0。空历史是正常查询结果，必须跳过取址。 |
| BUG-R1-006 | PRODUCT | 接口把 etime 定义为结束时间；非当日历史分支按全缓存末尾取 count，能返回截止时间之后的 bar。先按 etime 找右端，再向前取数量。 |
| BUG-R23-001 | PRODUCT | writer 的检查线程捕获 `this`，release 未停止/唤醒/join 它，空析构也不回收；init→release→destroy 仍留下 joinable thread。明确状态机并安全 join 全部工作线程。 |
| BUG-R23-002 | PRODUCT | `writeTransaction` 检查委托明细开关而不是逐笔成交开关；修正独立开关并覆盖实时写入与收盘转储。 |
| BUG-R23-003 | PRODUCT | tick count 查询用合约号而不是商品 PID 查商品，标准商品表按 exchange.product 查找，因此有效合约输入可空指针解引用。改用已获得的商品对象/产品 ID 并判空。当前有启用的严格商品键子进程 probe，但本轮未运行。 |
| BUG-R23-004 | UNDECIDED | 失败校验后将空 HisTBlockPair 留在 map，reader 后续不重读；“修复文件后同 reader 自动刷新”合同未找到。确认热替换语义；若支持重试，不缓存失败项；否则测试调用公开 clearCache。 |
| BUG-R23-005 | UNDECIDED | RT 与历史 etime 等值记录一含一不含，但接口没说明右边界闭/开。两个通过用例只分别锁定现状，不证明合同；先明确语义再统一比较规则和测试。 |
| BUG-R23-006 | PRODUCT | RT tick 缓存扩容两种失败路径返回 NULL，调用立即解引用；扩容失败还释放旧映射。需事务式 remap、失败回滚及故障注入。 |
| BUG-R23-007 | PRODUCT | 空 K 线缓存时 helper 返回 NULL 但 count 不归零，调用创建 `size>0`、无数据块的幻影 slice。空缓存要返回零 count，调用也需核验指针。 |
| BUG-R36-007 | DUPLICATE | 与 BUG-R142-001 是 WtRdmDtReader 同一 CMP_V2 queue/detail/trans 重建悬垂写路径；canonical 采用 BUG-R142-001。 |
| BUG-R52-001 | PRODUCT | HOT 包裹文件循环中的同名 filename 遮蔽使循环后读取空路径，catch 后缓存返回空历史。统一文件变量并验证首读、重读、切段拼接。 |
| BUG-R52-002 | PRODUCT | 后复权 RT vector 的 lower_bound 若返回 end，随即解引用 `[size]`；按实际有效长度处理 end/begin，不超范围生成 slice。 |
| BUG-R52-003 | PRODUCT | 后复权 ByRange 首次 RT 拷贝的长度多一条；修正为从复制起点到 newSize 的实际数量。与 ByCount 首次拷贝的逻辑对照。 |
| BUG-R74-001 | DUPLICATE | 与 BUG-R132-002 是同一 WtDataReader 两个 map lookup 的错误，canonical 采用 BUG-R132-002；R74 fixture 可作为第二组数据样本。 |
| BUG-R94-001 | DUPLICATE | 与既有 BUG-P05-007（历史 helper 早窗口回退）同一实现及根因；canonical 采用 BUG-P05-007。 |
| BUG-L04-001 | NOT_BUG | 私有 AD helper 的源数组误拷贝和右边界 key 错误确实存在，但仓库只有声明/定义、没有生产调用点；现存 LMDB 原语测试没有调用它，不能计为当前产品影响。 |
| BUG-L04-002 | PRODUCT | 通用 block parser 读 12 字节头和压缩 20 字节头前均无长度检查，老格式短输入还可能触发长度下溢。分版本先校验头长、拒绝输入且不改 buffer。 |
| BUG-L04-004 | UNDECIDED | 固定宽度 key 的 strcpy 风险只在超出数组合同的字符串下触发；当前只有 MAX 宏和合法上限测试，未证实这些超长值是受支持的产品输入。 |
| BUG-L04-005 | UNDECIDED | LMDB 短 value 会被 cast 成 bar 的静态风险存在，但没有证明仓库 writer/公开调用可生成这种非规范值；现存测试复刻 query，不调用目标私有函数。先确认 storage corruption 的读端合同/可达性。 |
| BUG-L04-006 | PRODUCT | `WtLMDB::open` 忽略 `mdb_env_set_mapsize` 错误却返回 true，调用方不能判断请求配置失败；补错误传播与资源回收。旧 tick 的无 NUL overread 仅属损坏字段条件，本条不把它当成有效格式缺陷。 |
| BUG-P09-003 | UNDECIDED | 非空配置缺少 path 会空指针解引用，但 path 必填合同/schema 未找到；按 schema 做判空与安全拒绝后再定级。 |
| BUG-P09-004 | PRODUCT | 两条 getAdjFactors 路径均把无界格式化写进 20 字节静态 buffer；宏许可的组合 key 无 19 字节上限。换为拥有长度的 string，测试 19/20/长 key。 |
| BUG-P09-005 | PRODUCT | 公共 range 左界超过缓存尾端或 count=0 时 helper 仍以 vector[size] 取址；空结果计数回绕不会消除越界访问。提前返回安全空值并定义 pointer/count 合同。 |
| BUG-P10-001 | PRODUCT | writer 的独立解码器同样缺 12/20 字节头长验证；修 parser 头长与 payload 校验，失败不变更输入。不可把短输入无 sanitizer 的一次通过当安全证据。 |
| BUG-P10-003 | UNDECIDED | decoder 不校验 magic 是当前行为；writer 会写 magic 但没有 decoder 必须拒绝错误 magic 的规范。维持合同待定，不将“当前接受”作为测试预期；type 应由调用边界判。 |
| BUG-P10-004 | PRODUCT | header-only 日线文件解码为空后仍返回 dump 成功，却不写新 bar。要么持久化新 bar，要么显式失败且不改文件。 |
| BUG-P10-005 | PRODUCT | task/proc 两个异步队列都存在无共同锁的检查/访问与无谓词等待丢唤醒窗口；“`_async_task` 未初始化”是误归因，实际 `_async_proc` 在 init 赋值。统一 mutex、predicate wait、终止唤醒与 join。 |
| BUG-Q03-001 | PRODUCT | HOT raw bar 载荷非完整结构体倍数时仍按原字节数 memcpy 到按整条数分配的 vector，并访问不存在的末条；拒绝不完整载荷、使用 RAII。崩溃信号未定位，本裁定只据静态路径。 |
| BUG-Q03-002 | PRODUCT | 多个 HOT/复权 reader 调用忽略 parser 的 false，可能继续把压缩原 buffer 作为 bars 处理；各调用点要传播失败并清除候选缓存。与解析器本身缺头长的 L04-002 分责。 |
| BUG-Q03-004 | UNDECIDED | 无符号 `tcnt<=0` 只是 `==0`；排除末条的 lower_bound 及后续回退是否符合 etime 语义，接口未规定。先明确边界合同，不凭写法定性。 |
| BUG-Q03-005 | PRODUCT | WtRdmDtReader 自身文档说 clearCache 清所有缓存，但实现只清 bars 和 RT maps，遗留四个按日期增长的 history map。补清理与历史文件重载测试；不把未量测 RSS 说成已证明无限增长。 |

## 建议迭代分批

| 批次 | 范围 | 交付门槛 |
|---|---|---|
| 02-A 解析安全与失败传播 | BUG-L04-002、BUG-P10-001、BUG-Q03-001/002、BUG-R161-001 | 按 block 版本校验头与 payload；坏文件不越界、不进入 cache；每种格式保留正控。 |
| 02-B 查询正确性与数据完整性 | BUG-R1-005/006、BUG-R23-002/003/007、BUG-R52-001/002/003、BUG-R100-001/002、BUG-R112-001、BUG-P09-004/005 | 对公开 API 用明确的 fixture 输入验收截止、空块、错误商品查找、HOT 拼接和 slice 数量；不把 disabled 组合测试通过当验收。 |
| 02-C writer 生命周期与并发 | BUG-R23-001/006、BUG-P10-005 | 全工作线程可停止/唤醒/join；扩容失败保留一致缓存；用确定性并发/故障注入验证无挂起、崩溃或丢数据。 |
| 02-D HFT 解压与缓存 | BUG-R132-001/002、BUG-R142-001、BUG-R1-002、BUG-Q03-005 | 两个 reader 实现分别测强压缩重分配和 same-key cache order；clearCache 后重读历史 HFT。BUG-R36-007、BUG-R74-001 不另计。 |
| 02-E 合同确认与范围收敛 | 8 条 UNDECIDED | 为 PID、文件修复/缓存失效、etime 端点、key 长度、LMDB 损坏值、reader path 配置、block magic、tick 末条边界补协议或调用方证据；确认后再转 PRODUCT / TEST / NOT_BUG。 |

## 测试证据边界与误报防护

- `r23_rdm_count_slice_test.cpp:186-236` 的 RT/历史 etime 边界用例互相断言不同结果；未定闭/开合同前只能说明现状。
- `r52_rdm_hot_adj_test.cpp:303-333` 的 disabled 组合路径同时触发 002/003 且无结果断言；不能作为动态复现或通过证据，应拆成独立测试。
- R161 的两个启用 size-mismatch 对照使用结构体整数倍 payload；尾部余数候选在 disabled test，前者 PASS 不能覆盖 memcpy 尾越界。
- R132/R142 的 roundtrip fixture 明确回避 append 扩容；成功不能证明悬垂指针安全。
- L04-001 的 LMDB 写读/去重用例没有调用 private `read_bars_to_buffer`；L04-005 的 query 用例也没有执行短 value cast。
- R23-003 当前仓库已存在启用严格商品键 probe：`R23RdmCountStrictProbe.CountByCodeShapedPidUsesProductForCommodityLookup`（测试文件 596-646 行）。旧候选状态写成 DISABLED 已过期；本轮未运行，不升级为动态复现。
- R23-004 的文件修复后自动刷新断言和 P09-003 的无 path 崩溃输入都依赖尚未确认的配置/缓存合同；不要把测试失败简单归类为源错误或测试错误。
