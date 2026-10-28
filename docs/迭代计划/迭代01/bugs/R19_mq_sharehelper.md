# 缺陷记录 — R19_mq_sharehelper（WtShareHelper 全量 + WtMsgQue 同步可达片）

本轮范围：`src/WtShareHelper/ShareBlocks.cpp`(790 行)、`ShareBlocks.h`、`WtShareHelper.cpp`(166 行)、
`src/WtMsgQue/MQManager.cpp`(105)、`MQServer.cpp`(248)、`MQClient.cpp`(198)、`WtMsgQue.cpp`(65)，
清单与行数按 `manifests/api-brief-R19.md`。
用例：`src/TestUnits/coverage/r19_mq/r19_*.cpp`（6 个 .cpp + 1 个 r19_support.h，30 条 TEST：启用 28、DISABLED 2）。
前账对照：`manifests/n06_strategies_mq_share.md` 的 `n06_mq_share_test.cpp` 只做了「主端往返 +
inproc 收发」的冒烟，没有触碰容量边界、持久化重装载、命令通道状态机与失败分支。

## BUG-R19-001

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:202-225（对照 :227-250、:130-163）
- 相关测试: r19_shareblock_slave_test.cpp `r19_update_slave_ignores_commit_until_blocktime_changes`
- 输入/前置条件: master 建段写 key → `init_slave` 同名文件 → master 连续 `commit_section`
- 期望行为: commit_section 是文档语义上的「提交一次版本」，slave 用 `update_slave(name, false)`
  轮询就应当感知到有新版本可装载
- 实际行为: commit_section 只写 **section 级** `_updatetime`（L221），从不写 **block 级**
  `_updatetime`；而 update_slave 的门槛是 block 级时间戳（L136-137）。全文件里只有
  `delete_section`（L247）和 init_master 的压缩分支（L57）会改 block 级时间戳，
  所以「只提交不删除」的常态用法下，非强制刷新永远返回 false
- 复现步骤: 启用用例即见 `commit_section` 返回 true 之后 `update_slave(dom, false)` 仍为 false，
  而 `update_slave(dom, true)` 为 true
- 影响范围: 所有依赖 commit/轮询刷新做进程间数据可见的下游（WtDtServo/ShareBlock 消费者）
  在不强制刷新的路径上永远读到旧缓存，缓存与文件不一致
- 备注: 建议在 commit_section 里同步 `shm._block->_updatetime`（不实施）。

## BUG-R19-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:143-156（对照 :398、:110-113）
- 相关测试: r19_shareblock_slave_test.cpp `r19_update_slave_reloads_after_blocktime_change_and_release_rules`
- 输入/前置条件: master 两个段 alive/doomed → slave init → master `delete_section(doomed)` → slave 非强制刷新
- 期望行为: 被删除的段在 slave 刷新后应从缓存消失，读它必须回落默认值
- 实际行为: update_slave 的重装载循环只过 `_count == 0`（L146），**不看 `_state`**；
  而 `delete_section` 只做 `_state = 2`（L246），不清 `_count`、不动槽位
  → doomed 段被重新装回 slave 缓存，`get_int32` 仍返回旧值 42；
  同一次刷新之后 `get_sections` 又因为 L398 过滤 `_state != 1` 只返回 1 个段
  → 枚举与读取两条路径对同一个段给出相反结论
- 复现步骤: 启用用例即见 `get_keys/get_sections` 与 `get_int32` 结果矛盾
- 影响范围: slave 侧读到已经作废的 KV（价格/合约一类的映射数据），且无从判断
- 备注: 建议 L146 补 `_state != 1` 判据（不实施）。

## BUG-R19-003

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:227-250 与 :273-278
- 相关测试: r19_shareblock_master_test.cpp `r19_section_slot_limit_64_and_delete_does_not_reclaim`
- 输入/前置条件: 一个域里连建 64 个段（MAX_SEC_CNT）→ 再建第 65 个 → 然后把 64 个段全删
- 期望行为: 删段后释放块内槽位，新段可以立即复用
- 实际行为: delete_section 只写 `_state = 2` + 更新块时间戳，`_block->_count` 不回退；
  make_valid 的容量判据是 `_count == MAX_SEC_CNT`（L273）→ 段数一旦打满，
  **即使全删也再也建不了新段**，必须重启进程走 init_master 的压缩分支才恢复
- 复现步骤: 启用用例即见 64 段全删之后 `allocate_int32("after_delete", ...)` 返回 nullptr，
  日志仍是 "No spare slot for new section"
- 影响范围: 长跑进程里动态增删 section 的场景（按合约/按产品建段）会一次性把段槽位烧完，之后永久不可写
- 备注: 建议 delete_section 做槽位回收或压缩（不实施）。

## BUG-R19-004

- 状态: 待审核
- 严重级别: Critical
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:61-82（L72）与 :36-59（L40）
- 相关测试: r19_shareblock_persist_test.cpp `r19_second_init_master_on_same_file_destroys_all_sections`、
  `r19_init_master_compacts_dead_sections_and_hides_live_ones`
- 输入/前置条件: 磁盘镜像里 2 个 `_state==1、_count>0` 的有效段 → 域 X `init_master` → 域 Y `init_master`（同一文件）
- 期望行为: 已经生效的段被装载后仍保持可见，且第二次加载不得销毁数据
- 实际行为: L72 无条件把「刚从文件读出来」的段 `_state = 0`，而 L40 的过滤判据是 `_state != 1`
  → 第二次加载时所有段都被判为失效 → 走压缩分支：L50 把 `_count` 清零、L51 把整段 `_sections`
  区 memset → **历史 KV 在文件里被物理擦掉**。旧句柄 X 的缓存指针还指着被清零的位置：
  `get_int32` 因 `_type` 归零而回落默认值，`set_int32` 因段名不匹配被 L294-303 拒写
  （日志 "Cache zone ... overwrote, cannot be modified"）
  同一条 L72 还让 `get_sections` 在加载后返回空表（段有 2 个可用 key 却不可见）
- 复现步骤: 启用两条用例即见（第一段断言 `has_log("cache resized")`、第二段断言 X 侧读回 -1）
- 影响范围: 任何「一个进程里多个 domain 指向同一份映射文件」或「重启后老句柄仍在用」的用法，
  都会静默丢全部数据；这是本轮唯一会**破坏已落盘内容**的缺陷
- 备注: L69-71 的注释说明作者本意是「没人用就下次删掉」，但删的方式是就地 memset 整块，
  且置 0 的时机在同一个循环里，建议改成延后到下一次真正 commit 之后再回收（不实施）。

## BUG-R19-005

- 状态: 待审核
- 严重级别: Critical
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:323、:446、:545 + `src/Includes/WTSMarcos.h:98-105`
- 相关测试: r19_shareblock_persist_test.cpp `r19_last_string_key_in_section_clobbers_next_section_name`
- 输入/前置条件: 段 before 里连建 16 个 string key（offset 0/64/…/960），紧随其后建段 after
- 期望行为: 每个 string key 只占 `SMVT_SIZES[SMVT_STRING]` = 64 字节，段内偏移检查 `> 1024` 应当够用
- 实际行为: `wt_strcpy(des, src, 64)` 是 `memcpy(des, src, 64)` + `des[64] = '\0'`，
  **一次写 65 字节、一次读源 64 字节**（短字符串还会越界读入参，属未定义行为）；
  落在段内最后一格（offset=960）时第 65 个字节写穿 `_data[1024]`，
  按 pack(1) 布局正好是块内下一个 `SecInfo::_name[0]`
  → 用例实测 after 段的段名首字节被置 NUL，`get_sections` 返回 `{"before", ""}`；
  重新加载该文件后这段只能以「空段名」回到缓存，按原名 after 读值返回默认值
- 复现步骤: 启用用例即见 `secs[1].empty()` 与 `get_int32(dom2,"after",...)` 回落 -1
- 影响范围: 只要某一段的 string key 用满 1024 字节预算，就会破坏相邻段的段名（数据身份丢失）；
  最后一格（块内第 64 段）的越界字节落在 `_updatetime/_count` 上，可把 `_count` 低位清零，
  下一次 init_master 会把整个块判空
- 备注: 建议偏移判据改成 `_offset + len + 1 > 1024`，或拷贝长度用 `min(64, strlen+1)`（不实施）。

## BUG-R19-006

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:751-791（L772-776）+ :720-749（L743-747）
- 相关测试: r19_shareblock_cmd_test.cpp `r19_cmd_cmder_writes_and_consumer_joining_later_skips_first`
- 输入/前置条件: 命令块里已经落了 1 条命令（`_readable = 0`）→ 消费者第一次 `get_cmd(name, lastIdx=UINT32_MAX)`
- 期望行为: 首帧对齐应当把 lastIdx 置成「上一条已消费的下标」（readable-1），使下一次调用投喂 readable 处的命令
- 实际行为: L772-776 直接把 `lastIdx = _readable`（=最新可读下标）并返回空串
  → `_commands[_readable]` 上的那条命令**永远不会被投喂**；
  用例实测：写 c0 之后接消费者拿到 `""` 且 idx=0，再写 c1 之后读到的是 "c1"，
  把 idx 强行拨回 0 也只会重新读到 c1（c0 不可达）
- 复现步骤: 启用用例即见三处断言
- 影响范围: 消费者比下达者晚启动（正常启动顺序就是这样）时，第一条命令被吞；
  对照 L777-781 的 999999 分支（能正确投喂 `_commands[0]`）可见两条路径语义不一致
- 备注: 建议 L774 改成 `lastIdx = _readable - 1`（不实施）。

## BUG-R19-007

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:675-718（L709-715）
- 相关测试: r19_shareblock_cmd_test.cpp `r19_cmd_wrapped_block_reads_by_capacity_modulo`
- 输入/前置条件: 块里 `_writable=131、_readable=130`（已跨圈、有在途消费者）→ 用另一个 domain 名
  对同一份文件再 `init_cmder(name, true, path)`
- 期望行为: 后加入的映射不得改写共享计数器；归一化只应发生在真正冷启动时
- 实际行为: L709/L712 就地写回 `_writable %= capacity`、`_readable %= capacity`，
  而这块内存是全进程共享的映射 → 第二个映射把 131 改成 3、130 改成 2，
  紧接着它的 `add_cmd` 又把 `_readable` 从 130 写成 3（L747）
  → 老消费者的 `lastIdx(130) >= _readable(3)` 恒成立，此后**再也读不到任何命令**；
  用例实测把 lastIdx 拨回 2 反而能读到刚写入槽位 3 的那条
- 复现步骤: 启用用例即见 `EXPECT_STREQ("", get_cmd(domW, 130))` 与 `EXPECT_STREQ("mine", get_cmd(domW, 2))`
- 影响范围: 一个进程里出现两个命令域（或多进程各自 init 同一份 .cmd）时通道直接瘫掉，
  并且已经写过的命令槽会被复用覆盖
- 备注: 建议归一化结果只写进各自的私有游标，不改共享块（不实施）。

## BUG-R19-008

- 状态: 待审核
- 严重级别: Critical
- 模块/文件: WtMsgQue / src/WtMsgQue/MQClient.cpp:96-100（对照 :78-84、:87-93）+ :51-62、:110-123
- 相关测试: r19_mq_manager_test.cpp `r19_invalid_protocol_logs_connect_failure_and_logger_can_be_unregistered`
  （证据链）+ `DISABLED_start_client_on_failed_init_leaks_recv_thread`（触发顺序，本轮不跑）
- 输入/前置条件: `create_client("r19bogus://x", cb)` → `start_client(id)` → `destroy_client(id)`
- 期望行为: init 失败要回滚 socket 并把 `_sock` 复位 -1（同函数前两个失败分支 L81/L89 都这么写），
  且 destroy 必须能收掉已起的接收线程
- 实际行为: `nn_connect` 失败分支只记日志后 `return false`，**既不 nn_close 也不复位 `_sock`**：
  1) socket 泄漏；2) `MQClient::start` 的 `if (_sock < 0)` 闸门（L115）失效，照常用坏套接字起线程；
  3) `~MQClient` 的 `if (!m_bReady) return;`（L53-54）提前退出，不置 `m_bTerminated`、不 join
  → 线程持有已析构的 `this`，且 `MQManager::create_client`（L60-66）忽略 init 返回值，
  这个半成品实例还挂在 `_clients` 表里可被 `start_client` 调到
- 复现步骤: DISABLED 用例按上述顺序调用即触发（预期表现为宿主挂死/崩溃，所以本轮标 DISABLED）
- 影响范围: 端口未起、URL 写错等最常见的启动竞态；后果是 fd 泄漏 + 后台野线程
- 备注: 另需注意 `m_bTerminated` 不是 volatile/atomic（MQClient.h L128），接收线程读它无同步保证。

## BUG-R19-009

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.h:176-187（`_ShmPair` 构造只初始化 `_block/_master`）
- 相关测试: r19_shareblock_master_test.cpp `r19_init_master_creates_block_file_and_is_idempotent`
  （本轮刻意不断言该点，见用例注释）
- 输入/前置条件: `init_master(name, path)` 命中「文件已存在且段数没变」的分支（不走 L58 赋值）
- 期望行为: `_blocktime` 应有确定初值（0 或当前 block 时间戳）
- 实际行为: `_blocktime` 取分配器给到的随机字节 → 第一次 `update_slave(name, false)` 的比较
  （L136）结果不可复现，可能凭空触发一次全量重装载，也可能永远不触发
- 复现步骤: 无法稳定复现（这正是问题）；本轮用例避免对该点做真伪断言
- 影响范围: 所有以 master 身份注册、又用非强制刷新轮询的调用方
- 备注: 建议 `_ShmPair() :_block(nullptr), _master(false), _blocktime(0) {}`（不实施）。

## BUG-R19-010

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtMsgQue / src/WtMsgQue/MQManager.cpp:6-18、:58-67
- 相关测试: r19_mq_manager_test.cpp `r19_invalid_protocol_logs_bind_failure_but_handle_still_returned`
- 输入/前置条件: `create_server("r19bogus://x", false)` / `create_client("r19bogus://x", cb)`
- 期望行为: init 失败应当销毁实例并返回 0（Porter 侧约定 0 为无效句柄）
- 实际行为: `server->init(...)` / `client->init(...)` 的返回值被直接丢弃，失败的实例仍然写进
  `_servers`/`_clients` 并返回一个非 0 id，调用方以为可用
- 复现步骤: 启用用例即见 `id > 1000` 与 `has_log("failed")` 同时成立
- 影响范围: 上层（WtPorter/WtBtPorter 的 createServer 包装）拿到假句柄后一路 publish 不上报错误
- 备注: 与 BUG-R19-008 是同一条因果链的上游。

## BUG-R19-011

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:746（`strcpy` 写 `CmdInfo::_command`，
  字段定义在同目录 ShareBlocks.h:80-88，`char _command[MAX_CMD_SIZE]`，MAX_CMD_SIZE=64）
- 相关测试: r19_shareblock_cmd_test.cpp `DISABLED_r19_cmd_longer_than_64_bytes_writes_past_the_block`
- 输入/前置条件: `add_cmd(name, cmd)`，cmd 长度 ≥ 64
- 期望行为: 与同文件其它写入一致的越界防护（或明确拒绝超长命令）
- 实际行为: 全文件唯一一处不受限的 `strcpy`，直接写进 64 字节的槽位；
  长度 ≥64 时依次冲掉本槽 `_state` 之外的字节并覆盖后续命令槽；
  落在最后一个槽（realIdx=127）时越过映射文件尾 36 字节
- 复现步骤: DISABLED 用例传 100 字节即触发；本轮不跑（会写坏映射块，属未定义行为）
- 影响范围: 命令通道里任何长参数（带完整指令行的调用方）
- 备注: 同函数 L740-742 的注释还停留在「先移动写下标」，代码已是另一种写法，注释与实现脱节。

## BUG-R19-012

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:681-683
- 相关测试: 评审级登记（不执行：会在进程 CWD 落一个 `./.cmd`，本轮规则要求用例自带清理且不留垃圾）
- 输入/前置条件: `init_cmder(name, isCmder)` 不传 path（头文件默认 `path = ""`，ShareBlocks.h:171）
- 期望行为: 和 init_master/init_slave 一样，path 缺省时以 **name** 作为文件名（L15-17/L93-95 就是这么做）
- 实际行为: 文件名被硬编码成 `".cmd"`，`name` 完全不起作用
  → 同一进程里所有命令域共用 CWD 下的同一个 `./.cmd`，互相串台；
  用例 `r19_cmd_cmder_writes_and_consumer_joining_later_skips_first` 用显式 path 才避开这条
- 复现步骤: 连调两次 `init_cmder("a", true)`、`init_cmder("b", false)`（都不传 path），
  再 `add_cmd("a", ...)` → `get_cmd("b", ...)` 能读到，即为串台
- 影响范围: WtRunner 一类按名字起多个命令通道的用法；此外隐藏文件落在 CWD 也无人清理
- 备注: 另外 `_cmd_blocks` 没有对应的 release 入口（只有 release_slave），命令通道一旦 init 就无法摘除。

## BUG-R19-013

- 状态: 待审核
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:281、:330（`wt_strcpy(des, src)` 缺省长度取
  `strlen(src)`，见 `src/Includes/WTSMarcos.h:98-105`）
- 相关测试: r19_shareblock_persist_test.cpp `r19_section_name_longer_than_32_bytes_is_stored_mangled`
- 输入/前置条件: `allocate_int32(dom, <40 字节段名>, "k0", 9)`
- 期望行为: 段名/key 名超过定长字段（`_name[32]`、`_key[32]`）时应拒绝或截断并告警
- 实际行为: 无长度上限的 memcpy 直接写穿字段：
  40 字节段名先占掉 `_keys[0]._key[0..7]`，随后 L330 写第一个 key 名只覆盖了前 3 个字节
  → 落盘后段名在文件里变成「前 32 字节 + "k0"」共 34 字节；
  用例实测 `get_sections` 返回的段名长度 34、后 2 个字符正是第一个 key 名，
  重新加载后按原 40 字节名读不到（`get_int32` 回落 -1），只能按被改短的 34 字节名读到
- 复现步骤: 启用用例即见 `EXPECT_EQ(34u, secs[0].size())`
- 影响范围: 段名/key 名一旦超长（例如把整条合约代码拼进段名）就静默改姓，跨进程读不到；
  key 名超长的形态在「新建 key」路径上会被后续字段赋值掩盖（`_type`/`_offset` 在 L331-332 又被写回），
  但 `_key` 尾字节与 `_type` 的公共部分仍取决于偏移值，属同类未定义行为
- 备注: 建议 make_valid 入口统一做长度校验（不实施）。

## BUG-R19-014

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:407-433（L416-419）
- 相关测试: r19_shareblock_slave_test.cpp `r19_update_slave_reloads_after_blocktime_change_and_release_rules`
- 输入/前置条件: 只读调用 `get_keys(domain, section)`
- 期望行为: 查询接口不应隐式改写内部缓存状态；缓存失效判据要覆盖「段被删除」
- 实际行为: 以 `shm._sections.size() != shm._block->_count` 作为唯一失效判据，
  并在只读接口里直接调 `update_slave(domain, true)` 全量重装载（含写日志、清缓存）；
  而 delete_section 不减 `_count`（BUG-R19-003），所以「段被删掉」这一种最该重载的情况
  恰恰触发不了，反而「块里有空槽未装进缓存」这种正常状态每次都触发全量重载
- 复现步骤: 启用用例即见 `get_keys(domS,"third")` 的返回 size 由 0 变 1（内部发生了重载）
- 影响范围: 读放大（每次 size 不等都全量重装载 + 刷日志），以及删除态检测缺失
- 备注: 登记为设计缺陷，建议按 block 版本号判失效（不实施）。

## 需要接缝（本轮留白，未覆盖行数已量）

- 状态: 待审核
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtMsgQue / src/WtMsgQue/MQServer.cpp:109-249 中的 **L120-246（127 行）**
  —— `publish` 的入队 + 广播线程体（SpinLock 入队 L120-125、线程起停 L127-132、
  60s 心跳 L142-153、分包/满缓冲强发 L166-224、统计同步 L226-245）
- 期望行为: 本轮硬规则禁止起线程/sleep/真实网络；这段逻辑只有在「非空 payload 触发
  `new StdThread`」或「时钟接缝」下才进得去
- 实际行为: 无法在不违反规则的前提下覆盖
- 复现步骤: 需要的接缝 = 把线程体抽成可同步调用的 `flush_once(now_ms, conn_cnt)`
  （注入时间与连接数），即可无测试专用线程覆盖 127 行
- 影响范围: WtMsgQue 的本轮可达覆盖率上限被压到 ~35%（同步路径），心跳/分包/统计逻辑仍是盲区

- 状态: 待审核
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtMsgQue / src/WtMsgQue/MQClient.cpp:110-199 中的 **L110-173（64 行）+ L175-199（25 行）= 89 行**
  —— `start()` 的接收线程体（含 L160 的 sleep）与 `extract_buffer()` 分包/主题过滤
- 期望行为: 同上，`extract_buffer` 是 private 成员且 `_buffer` 无外部注入口
- 实际行为: 只能靠真收发（inproc + 线程 + 等待）才能进，违反本轮规则
- 复现步骤: 需要的接缝 = 暴露一个 `feed_buffer(const char*, size_t)`（或在 MQClient 上加
  friend 测试钩子的正式版本），即可确定性地测分包边界（半包/粘包/超长 topic）与 `is_allowed` 过滤
- 影响范围: 粘包/半包解析（L181-198）是历史上最容易出问题的地方，目前完全无测

- 状态: 待审核
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtMsgQue / MQServer.cpp:68-69、74-79、82-88 与 MQClient.cpp:66-67、71-76、78-84
  （合计 **32 行**）
- 期望行为: 「重复 init 早退」与「nn_socket/nn_setsockopt 失败」两条分支需要可注入的失败点
- 实际行为: MQManager 每次都是新实例（init 只可能被调一次），且 nanomsg 在本机不会因 socket/
  setsockopt 失败；用例层无法触发
- 复现步骤: 需要的接缝 = 一个 `FuncNnBackend` 后端注入（或允许 MQServer/MQClient 由用例直接构造，
  但这需要把类导出，属产品侧改动）
- 影响范围: 失败回滚路径（含 BUG-R19-008 提到的未复位分支）只能靠代码评审兜底

- 状态: 待审核
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:165-184 的对称缺口
- 期望行为: master 域与命令域应有对应的 release/清理入口（现在只有 release_slave）
- 实际行为: `_shm_blocks`/`_cmd_blocks` 只增不减，单例里的映射活到进程结束；
  本轮用例只能靠「同文件换域名」来重走装载分支（也因此撞上 BUG-R19-004）
- 复现步骤: 需要接缝 `release_master(name)` / `release_cmder(name)` 后，
  同一 domain 的重复装载与容量回收才可以在不泄漏映射的前提下测完
- 影响范围: 用例隔离度（域名必须逐个换新）+ 长跑进程的地址空间占用
