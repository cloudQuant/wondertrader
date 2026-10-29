# 缺陷记录 — R19_mq_sharehelper（WtShareHelper 全量 + WtMsgQue 同步可达片）

本轮范围：`src/WtShareHelper/ShareBlocks.cpp`(790 行)、`ShareBlocks.h`、`WtShareHelper.cpp`(166 行)、
`src/WtMsgQue/MQManager.cpp`(105)、`MQServer.cpp`(248)、`MQClient.cpp`(198)、`WtMsgQue.cpp`(65)，
清单与行数按 `manifests/api-brief-R19.md`。
用例：`src/TestUnits/coverage/r19_mq/r19_*.cpp`（6 个 .cpp + 1 个 r19_support.h，30 条 TEST：启用 28、DISABLED 2）。
前账对照：`manifests/n06_strategies_mq_share.md` 的 `n06_mq_share_test.cpp` 只做了「主端往返 +
inproc 收发」的冒烟，没有触碰容量边界、持久化重装载、命令通道状态机与失败分支。

## 本次审查边界（2026-10-29）

- 逐项核对了下列源码、对应测试源码及公开头文件；**本次没有运行测试**。文中的测试名/断言只表示源码中存在这些断言，不代表本次执行结果。
- 已确认的源码问题：R19-002、004、005、007、008、011、013。
- 需确认接口/生命周期契约：R19-001、003、006、009、010（服务端句柄语义）、012，以及末尾的覆盖接缝项。
- 已合并：R19-014 并入 R19-002；R19-010 的 client 失败链并入 R19-008。R19-009 的 `_blocktime` 未初始化属源码事实，但 master 是否可调用 `update_slave` 未由接口明确约束，保留待验证。

## BUG-R19-001

- 状态: 待验证（源码行为已确认；`commit_section` 是否必须触发非强制重载，公开接口未说明）
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:202-225（对照 :227-250、:130-163）
- 相关测试: r19_shareblock_slave_test.cpp `r19_update_slave_ignores_commit_until_blocktime_changes`
- 输入/前置条件: master 建段写 key → `init_slave` 同名文件 → master 连续 `commit_section`
- 期望行为: 若接口契约要求 slave 以 `update_slave(name, false)` 轮询发现新 key/元信息，则 commit 应推进相应版本；当前公开头文件没有说明 `commit_section` 与 block 版本/`update_slave` 的关系
- 实际行为: commit_section 只写 **section 级** `_updatetime`（L221），从不写 **block 级**
  `_updatetime`；而 update_slave 的门槛是 block 级时间戳（L136-137）。全文件里只有
  `delete_section`（L247）和 init_master 的压缩分支（L57）会改 block 级时间戳，
  所以「只提交不删除」的常态用法下，非强制刷新永远返回 false
- 影响范围: 已存在 key 的值位于同一映射区，读值无需重装载；风险集中在 slave 本地 key 索引尚未包含的新 key（尤其是已有 section 新增 key），此时 `get_keys` 的 section-count 检查也可能不触发刷新
- 证据: `r19_update_slave_ignores_commit_until_blocktime_changes` 源码断言了非强制刷新返回 false；本次未运行。需要产品契约确认该结果是否不符合预期
- 修复方向: 若需 commit 驱动元信息可见性，定义并推进可比较的版本戳（或让刷新比较 section 版本）；不能仅凭本用例把现有值缓存称为过期

## BUG-R19-002

- 状态: 已确认（源码核实；本次未运行对应测试）
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
- 证据: 对应测试源码包含删除、非强制刷新、旧值读取及枚举断言；本次未运行，行为结论来自 L143-156、L227-249、L383-404 的静态路径
- 影响范围: slave 的 KV 读取会返回已删除段的旧值；`get_sections` 虽过滤该段，但 KV 读取不校验 `_state`，造成枚举与读取矛盾
- 证据界限: 对应测试源码包含删除、非强制刷新、旧值读取及枚举断言；本次未运行，行为结论来自 L143-156、L227-249、L383-404 的静态路径
- 修复方向: 重装载只纳入 `_state == 1 && _count > 0` 的段；同时让读取/枚举使用同一有效性规则

## BUG-R19-003

- 状态: 待验证（槽位不回收的源码行为已确认；删除后是否须在本进程复用未见接口契约）
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:227-250 与 :273-278
- 相关测试: r19_shareblock_master_test.cpp `r19_section_slot_limit_64_and_delete_does_not_reclaim`
- 输入/前置条件: 一个域里连建 64 个段（MAX_SEC_CNT）→ 再建第 65 个 → 然后把 64 个段全删
- 期望行为: 若 `delete_section` 承诺进程内释放槽位，删除后应能复用；当前头文件未规定回收时点，init_master 注释显示清理也可能被设计为下次装载时进行
- 实际行为: delete_section 只写 `_state = 2` + 更新块时间戳，`_block->_count` 不回退；
  make_valid 的容量判据是 `_count == MAX_SEC_CNT`（L273）→ 段数一旦打满，
  **即使全删也再也建不了新段**，必须重启进程走 init_master 的压缩分支才恢复
- 证据: `r19_section_slot_limit_64_and_delete_does_not_reclaim` 源码断言了该路径；本次未运行。L227-249 将段标为 2 但不减 `_count`，L273-278 在 `_count == MAX_SEC_CNT` 时拒绝新段
- 影响范围: 若没有重启/重载回收，进程内累计创建 64 个段后不能继续新建；并非永久不可恢复，重新 init_master 会走压缩分支
- 修复方向: 先明确容量和回收生命周期契约；若要求在线复用，增加安全回收/压缩并保持所有缓存索引有效，否则应把固定槽位及需重启回收写入接口说明

## BUG-R19-004

- 状态: 已确认（源码核实；本次未运行对应测试）
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
- 证据: 对应两条测试源码覆盖首次重载可见性和同文件二次 init 清零路径；断言未在本次运行。L36-59 的筛选与压缩配合 L63-80 无条件置 `_state=0`，足以构成静态证据
- 影响范围: 任一 `init_master` 会把刚加载的有效段 `_state` 置 0；同一进程里另一个 domain 再映射同一文件，或后续进程重新 init 时，会把这些段当无效并 memset，静默破坏已落盘内容。进程重启后旧句柄不再存在，故不表述为“重启后老句柄仍在用”
- 备注: L69-71 的注释说明作者本意是「没人用就下次删掉」，但删的方式是就地 memset 整块，
  且置 0 的时机在同一个循环里，建议改成延后到下一次真正 commit 之后再回收（不实施）。

## BUG-R19-005

- 状态: 已确认（越界写边界由源码布局可证；本次未运行测试）
- 严重级别: Critical
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:323、:446、:545 + `src/Includes/WTSMarcos.h:98-105`
- 相关测试: r19_shareblock_persist_test.cpp `r19_last_string_key_in_section_clobbers_next_section_name`
- 输入/前置条件: 段 before 里连建 16 个 string key（offset 0/64/…/960），紧随其后建段 after
- 期望行为: 每个 string key 只占 `SMVT_SIZES[SMVT_STRING]` = 64 字节，段内偏移检查 `> 1024` 应当够用
- 实际行为: `wt_strcpy(des, src, 64)` 是 `memcpy(des, src, 64)` + `des[64] = '\0'`，
  **一次写 65 字节、一次读源 64 字节**（短字符串还会越界读入参，属未定义行为）；
  落在段内最后一格（offset=960）时第 65 个字节写穿 `_data[1024]`，
  按 pack(1) 布局正好是块内下一个 `SecInfo::_name[0]`
  → 测试源码断言 after 段的段名首字节被置 NUL，`get_sections` 返回 `{"before", ""}`；
  重新加载该文件后这段只能以「空段名」回到缓存，按原名 after 读值返回默认值
- 证据界限: 测试源码断言了相邻段名被清空；本次未运行。测试传入短字符串而 `wt_strcpy` 固定读取 64 字节，本身包含越界读的未定义行为，因此断言不能作为稳定运行证据；`_data[1024]` 后写 `des[64]` 的越界写则可由结构布局和函数实现静态确认
- 影响范围: 某段的 string key 用满 1024 字节预算时，越界 NUL 会覆盖紧邻下一段的 `_name[0]`；若是块内最后一段，紧邻字节是 `ShmBlock::_updatetime` 的首字节，**不会直接写到 `_count`**。对块时间戳的影响需按共享时间戳后续使用路径另行评估
- 备注: 建议偏移判据改成 `_offset + len + 1 > 1024`，或拷贝长度用 `min(64, strlen+1)`（不实施）。

## BUG-R19-006

- 状态: 已撤回（实现有意跳过启动前命令；公开接口可补充说明）
- 严重级别: 不适用（接口文档改进）
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:751-791（L772-776）+ :720-749（L743-747）
- 相关测试: r19_shareblock_cmd_test.cpp `r19_cmd_cmder_writes_and_consumer_joining_later_skips_first`
- 输入/前置条件: 命令块里已经落了 1 条命令（`_readable = 0`）→ 消费者第一次 `get_cmd(name, lastIdx=UINT32_MAX)`
- 期望行为: 需先明确首次接入是跳过已存在命令还是重放尚未消费命令；`get_cmd` 的公开接口没有说明 `lastIdx` 初值/历史命令策略
- 复核: `ShareBlocks.cpp` 在首次接入分支明确注释“刚启动，之前的命令全部作废”。现有测试刻画此策略；没有产品合同要求重放，不能把跳过历史命令记为缺陷。建议在公开接口说明启动游标与历史命令处理方式，若未来需要重放应另定策略和测试。
- 实际行为: L772-776 直接把 `lastIdx = _readable`（=最新可读下标）并返回空串
  → `_commands[_readable]` 上的那条命令**永远不会被投喂**；
  测试源码断言：写 c0 之后接消费者返回 `""` 且 idx=0，再写 c1 之后读到的是 "c1"，
  把 idx 强行拨回 0 也只会重新读到 c1（c0 不可达）
- 证据: L772-776 在首次调用且队列非空时把游标对齐到当前 `_readable` 并返回空串；空队列首次调用则走 999999 分支，后续会读首条。测试源码断言了差异，本次未运行。仓内未找到生产调用方或公开契约来确认应否重放
- 影响范围: 队列非空后首次接入的消费者看不到已有命令；这可能是避免执行过期命令的有意策略，不能仅凭两个启动状态不同断为缺陷
- 修复方向: 由接口契约决定；若要求重放，应使用不与 `UINT32_MAX` 冲突的初始化状态并明确回绕处理，不能直接将游标设为 `readable - 1`（readable 为 0 时会撞上现有哨兵）

## BUG-R19-007

- 状态: 已确认（活动映射重入时共享游标被改写；本次未运行对应测试）
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
  测试源码断言把 lastIdx 拨回 2 会读到刚写入槽位 3 的那条（本次未运行）
- 证据: L709-714 对映射区共享的 `_writable/_readable` 原位取模；测试源码用第二个域映射同一文件断言旧游标追不上，本次未运行
- 影响范围: 第二个进程/域在生产者仍活动且计数器已跨圈时重新初始化，可把其他消费者看到的单调序号改小，造成旧消费者停滞及槽位覆盖；冷启动单映射不触发此场景
- 修复方向: 映射初始化时不改写共享序号；如需恢复/归一化，必须由明确的独占冷启动协议完成，并保持读写序号对所有进程一致

## BUG-R19-008

- 状态: 已确认（失败 socket 未回滚与启动闸门静态成立；DISABLED 线程路径本次未执行）
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
- 证据界限: `r19_invalid_protocol_logs_connect_failure_and_logger_can_be_unregistered` 只确认失败日志和句柄注册；线程触发用例为 DISABLED，本次未运行。源码可确认失败后 `_sock >= 0`、`start()` 会建线程且析构提前返回；若该 `std::thread` 仍 joinable，成员析构会调用 `std::terminate`，不能把具体宿主表现写成已复现的 UAF/挂死
- 影响范围: 端口未起、URL 写错等最常见的启动竞态；后果是 fd 泄漏 + 后台野线程
- 修复方向: `nn_connect` 失败时关闭 socket 并复位；manager 不应允许失败对象进入可启动状态。另有独立风险：正常路径对 `m_bTerminated` 的线程读写也无同步（MQClient.h:135），应单独跟踪，避免与本条失败初始化链混成一个缺陷

## BUG-R19-009

- 状态: 待验证（未初始化字段的源码事实成立；master 调用 `update_slave` 是否违反契约未说明）
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.h:176-187（`_ShmPair` 构造只初始化 `_block/_master`）
- 相关测试: r19_shareblock_master_test.cpp `r19_init_master_creates_block_file_and_is_idempotent`
  （本轮刻意不断言该点，见用例注释）
- 输入/前置条件: `init_master` 未走压缩分支后，以同一 master domain 调用公开的 `update_slave(name, false)`；当前实现没有 master-mode 拒绝分支
- 期望行为: `_blocktime` 应有确定初值（0 或当前 block 时间戳）
- 实际行为: `_ShmPair` 构造没有初始化 `_blocktime`，但 `init_slave` 在 L104 从映射块赋值；本记录的不确定比较只在把 master 域传给 `update_slave` 时出现，未证明这是受支持的调用方式
- 证据: 头文件构造未初始化 `_blocktime`；`init_slave` 在 L104 赋值，而 `init_master` 仅压缩分支 L58 赋值；`update_slave` 未检查 `_master`。现有测试刻意不对该行为作断言，本次未运行
- 修复方向: 明确并实现 master-mode 契约：若允许此调用则初始化 `_blocktime`；若只允许 slave 调用则在入口拒绝 master domain

## BUG-R19-010

- 状态: 已合并（client 失败启动链并入 R19-008）；服务端失败句柄契约待验证
- 严重级别: Major
- 模块/文件: WtMsgQue / src/WtMsgQue/MQManager.cpp:6-18、:58-67
- 相关测试: r19_mq_manager_test.cpp `r19_invalid_protocol_logs_bind_failure_but_handle_still_returned`
- 输入/前置条件: `create_server("r19bogus://x", false)` / `create_client("r19bogus://x", cb)`
- 期望行为: init 失败应当向调用方给出不可误用的结果；当前公开头文件未声明 `0` 是无效句柄
- 实际行为: `server->init(...)` / `client->init(...)` 的返回值被直接丢弃，失败的实例仍然写进
  `_servers`/`_clients` 并返回一个非 0 id，调用方以为可用
- 证据界限: 测试源码包含非零 id 与失败日志的并存断言；本次未运行。返回非零 id 是静态事实，是否违反服务端句柄契约仍待确认
- 影响范围: manager 忽略 init 返回值这一源码行为成立；client 失败后可继续 start 的危险路径已归 R19-008。server 失败后仍返回非 0 id，但 publish 会因 `_sock < 0` 早退，其是否违反 API 契约待确认
- 修复方向: 定义并公开失败句柄/创建失败契约，再由 manager 传播 init 结果；不要把未见于头文件的 `0` 约定当作既定事实

## BUG-R19-011

- 状态: 已确认（静态边界检查；超长写入测试为 DISABLED，本次未运行）
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:746（`strcpy` 写 `CmdInfo::_command`，
  字段定义在同目录 ShareBlocks.h:80-88，`char _command[MAX_CMD_SIZE]`，MAX_CMD_SIZE=64）
- 相关测试: r19_shareblock_cmd_test.cpp `DISABLED_r19_cmd_longer_than_64_bytes_writes_past_the_block`
- 输入/前置条件: `add_cmd(name, cmd)`，cmd 长度 ≥ 64
- 期望行为: 与同文件其它写入一致的越界防护（或明确拒绝超长命令）
- 实际行为: `strcpy` 写入 `char[64]`；长度 63 及以下才容纳末尾 NUL，长度 ≥64 即越界，额外字节从紧随的下一个 `CmdInfo::_state`/映射尾开始覆盖；长度 100 落在最后槽时会写出映射末尾 37 字节（含 NUL）
- 证据界限: DISABLED 测试源码传 100 字节；本次未运行。数组大小和 `strcpy` 调用足以证明越界边界，未声称观察到实际破坏结果
- 影响范围: 命令通道里任何长参数（带完整指令行的调用方）
- 备注: 同函数 L740-742 的注释还停留在「先移动写下标」，代码已是另一种写法，注释与实现脱节。

## BUG-R19-012

- 状态: 待验证（默认路径行为已确认；默认路径应否按 name 分隔未见接口契约）
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:681-683
- 相关测试: 评审级登记（不执行：会在进程 CWD 落一个 `./.cmd`，本轮规则要求用例自带清理且不留垃圾）
- 输入/前置条件: `init_cmder(name, isCmder)` 不传 path（头文件默认 `path = ""`，ShareBlocks.h:171）
- 期望行为: 若 `name` 表示独立命令通道，则 path 缺省时应按 name 分文件；但头文件没有说明空 path 的命名规则
- 实际行为: 文件名被硬编码成 `".cmd"`，`name` 完全不起作用
  → 同一进程里所有命令域共用 CWD 下的同一个 `./.cmd`，互相串台；
  用例 `r19_cmd_cmder_writes_and_consumer_joining_later_skips_first` 用显式 path 才避开这条
- 验证前提: 需先以 `init_cmder("a", true)` 和 `init_cmder("b", false)`（均省略 path）映射同一默认文件；在写命令前让 b 首次 `get_cmd` 初始化为空队列哨兵，再由 a 写入并由 b 读取。若不先初始化消费者游标，R19-006 的首次接入策略会影响结果
- 影响范围: 多个 name 且均省略 path 时会映射同一 CWD 下的 `.cmd`；仓内未找到生产调用点验证该形态是否受支持。缺少 `release_cmder`/master release 也只是生命周期/测试接缝候选，尚无资源释放契约
- 修复方向: 明确 name/path 的默认语义与映射生命周期；若每个 name 必须独立，采用 name 派生文件名并给清理规则

## BUG-R19-013

- 状态: 已确认（无界拷贝与定长字段静态成立；本次未运行测试）
- 严重级别: Major
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:281、:330（`wt_strcpy(des, src)` 缺省长度取
  `strlen(src)`，见 `src/Includes/WTSMarcos.h:98-105`）
- 相关测试: r19_shareblock_persist_test.cpp `r19_section_name_longer_than_32_bytes_is_stored_mangled`
- 输入/前置条件: `allocate_int32(dom, <40 字节段名>, "k0", 9)`
- 期望行为: 段名/key 名超过定长字段（`_name[32]`、`_key[32]`）时应拒绝或截断并告警
- 实际行为: 无长度上限的 memcpy 直接写穿字段：
  40 字节段名先占掉 `_keys[0]._key[0..7]`，随后 L330 写第一个 key 名只覆盖了前 3 个字节
  → 落盘后段名在文件里变成「前 32 字节 + "k0"」共 34 字节；
  测试源码断言 `get_sections` 返回的段名长度 34、后 2 个字符正是第一个 key 名，
  重新加载后按原 40 字节名读不到（`get_int32` 回落 -1），只能按被改短的 34 字节名读到
- 证据: 测试源码断言超长段名落盘后被后续 key 写入串改；本次未运行。`wt_strcpy` 缺省长度取 `strlen`，目标字段固定 32 字节，静态越界路径成立
- 影响范围: 段名/key 名一旦超长（例如把整条合约代码拼进段名）就静默改姓，跨进程读不到；
  key 名超长的形态在「新建 key」路径上会被后续字段赋值掩盖（`_type`/`_offset` 在 L331-332 又被写回），
  但 `_key` 尾字节与 `_type` 的公共部分仍取决于偏移值，属同类未定义行为
- 备注: 建议 make_valid 入口统一做长度校验（不实施）。

## BUG-R19-014

- 状态: 已合并（删除态缓存读取并入 BUG-R19-002）
- 严重级别: Minor
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:407-433（L416-419）
- 相关测试: r19_shareblock_slave_test.cpp `r19_update_slave_reloads_after_blocktime_change_and_release_rules`
- 输入/前置条件: 只读调用 `get_keys(domain, section)`
- 期望行为: 查询接口不应隐式改写内部缓存状态；缓存失效判据要覆盖「段被删除」
- 实际行为: `get_keys` 以 map size 与 `_count` 不等作为强制重载条件；删除不减 `_count`，单靠该条件不会发现删除。触发重载后又因 `update_slave` 不检查 `_state` 而把删除段装回缓存
- 证据: `get_keys` 的比较与测试源码中的 `third` 新段断言可静态核实；本次未运行。删除态读取根因与 BUG-R19-002 相同
- 影响范围: 删除后仍读到旧 KV 的问题并入 BUG-R19-002；map-size 触发重载的读放大/一致性语义另需接口设计确认

## 需要接缝（本轮留白，未覆盖行数已量）

- 状态: 待验证（覆盖缺口；非产品缺陷，本次未运行相关测试）
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

- 状态: 待验证（覆盖缺口；非产品缺陷，本次未运行相关测试）
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtMsgQue / src/WtMsgQue/MQClient.cpp:110-199 中的 **L110-173（64 行）+ L175-199（25 行）= 89 行**
  —— `start()` 的接收线程体（含 L160 的 sleep）与 `extract_buffer()` 分包/主题过滤
- 期望行为: 同上，`extract_buffer` 是 private 成员且 `_buffer` 无外部注入口
- 实际行为: 只能靠真收发（inproc + 线程 + 等待）才能进，违反本轮规则
- 复现步骤: 需要的接缝 = 暴露一个 `feed_buffer(const char*, size_t)`（或在 MQClient 上加
  friend 测试钩子的正式版本），即可确定性地测分包边界（半包/粘包/超长 topic）与 `is_allowed` 过滤
- 影响范围: 粘包/半包解析（L181-198）是历史上最容易出问题的地方，目前完全无测

- 状态: 待验证（覆盖缺口；非产品缺陷，本次未运行相关测试）
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtMsgQue / MQServer.cpp:68-69、74-79、82-88 与 MQClient.cpp:66-67、71-76、78-84
  （合计 **32 行**）
- 期望行为: 「重复 init 早退」与「nn_socket/nn_setsockopt 失败」两条分支需要可注入的失败点
- 实际行为: MQManager 每次都是新实例（init 只可能被调一次），且 nanomsg 在本机不会因 socket/
  setsockopt 失败；用例层无法触发
- 复现步骤: 需要的接缝 = 一个 `FuncNnBackend` 后端注入（或允许 MQServer/MQClient 由用例直接构造，
  但这需要把类导出，属产品侧改动）
- 影响范围: 失败回滚路径（含 BUG-R19-008 提到的未复位分支）只能靠代码评审兜底

- 状态: 待验证（生命周期/覆盖接缝候选；未证明缺少 release API 违反产品契约）
- 严重级别: 阻塞项（非产品缺陷）
- 模块/文件: WtShareHelper / src/WtShareHelper/ShareBlocks.cpp:165-184 的对称缺口
- 期望行为: master 域与命令域应有对应的 release/清理入口（现在只有 release_slave）
- 实际行为: `_shm_blocks`/`_cmd_blocks` 只增不减，单例里的映射活到进程结束；
  本轮用例只能靠「同文件换域名」来重走装载分支（也因此撞上 BUG-R19-004）
- 复现步骤: 需要接缝 `release_master(name)` / `release_cmder(name)` 后，
  同一 domain 的重复装载与容量回收才可以在不泄漏映射的前提下测完
- 影响范围: 用例隔离度（域名必须逐个换新）+ 长跑进程的地址空间占用
