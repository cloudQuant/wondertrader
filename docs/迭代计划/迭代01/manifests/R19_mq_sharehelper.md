# 用例清单 — R19_mq_sharehelper（CoverageTests_mqshare / dir:r19_mq，前缀 r19_）

拆分表登记：`mqshare|WtShareHelper,WtMsgQue|n06_strategies_mq_share/n06_mq_share_test.cpp+dir:r19_mq`
（`src/TestUnits/coverage/CMakeLists.txt` 本轮未改动，只新增 `dir:r19_mq` 里的文件）

本轮范围 = `manifests/api-brief-R19.md` 列出的 5 个文件 + 同目录连带 TU：
`src/WtShareHelper/ShareBlocks.cpp`(790 行)、`ShareBlocks.h`(213)、`WtShareHelper.cpp`(166)、
`src/WtMsgQue/MQServer.cpp`(248)、`MQClient.cpp`(198)、`MQManager.cpp`(105)、`WtMsgQue.cpp`(65)。
按 api-brief 口径 missed ≈433 行；本轮可同步覆盖的部分全部落到用例里，
需要起线程/故障注入的部分登记为「需要接缝」（见缺陷文件最后一节，合计 ≈273 行）。

用例：6 个 .cpp + 1 个 `r19_support.h`，30 条 TEST（**启用 28、DISABLED 2**）。
行号均为本轮 `Read` / `cat -n` 实读，不是估算。

## 门禁（语法口径，含 -Werror）

```
cd build-cov && ninja -t commands \
  TestUnits/coverage/CMakeFiles/CoverageTests_mqshare.dir/n06_strategies_mq_share/n06_mq_share_test.cpp.o | tail -1
# 去掉 `-o X -c Y`，换 `-fsyntax-only <用例>`
```

6/6 PASS：

| 用例文件 | -fsyntax-only |
| --- | --- |
| r19_shareblock_master_test.cpp | PASS |
| r19_shareblock_slave_test.cpp | PASS |
| r19_shareblock_persist_test.cpp | PASS |
| r19_shareblock_cmd_test.cpp | PASS |
| r19_mq_manager_test.cpp | PASS |
| r19_shareblock_capi_test.cpp | PASS |

本轮按规则未执行 `cmake --build` / `ninja`（构建）/ 运行二进制 / ctest，所以以上是语法门禁而非链接+运行结果。
所有映射都走 `init_master/init_slave/init_cmder(name, path)` 的**显式 path 文件映射**（ShareBlocks.cpp L15-17、L93-95、L681-683），
文件落在 `temp_directory_path()/r19_<用例名>_<pid>_<seq>/` 下，由 `R19TempDir::TearDown()` 用
`std::filesystem::remove_all` 自清理；没有 POSIX `shm_open`、没有起线程、没有 sleep、没有真实网络。
没有使用 `#define private public`，没有在外层重新声明产品 TU 内的 inline/static 函数
（`makeMQSvrId`/`makeMQCientId`（MQServer.cpp L32-36、MQClient.cpp L32-36）只通过
「返回的 id 数值」间接断言，未做任何 extern 声明）。

## 1) r19_mq/r19_shareblock_master_test.cpp（7 条，全启用）
生产文件 `src/WtShareHelper/ShareBlocks.cpp`（另含 `ShareBlocks.h` 的 inline 体）

| 用例 | 生产行段 |
| --- | --- |
| r19_init_master_creates_block_file_and_is_idempotent | .cpp:9-30（建块 L19-25、幂等早退 L12-13）、L130-137（update_slave 见 BUG-R19-009 注释）、L186-195、L202-217 |
| r19_master_allocate_set_get_exact_roundtrip | .cpp:452-535（六个 allocate_*）、L537-613（六个 set_*）、L615-673（六个 get_*）、L252-292+L305-338（新建段/key）、L383-404、L407-432；ShareBlocks.h:56-60（`SecInfo::get<T>`） |
| r19_reallocate_existing_key_keeps_stored_value | .cpp:307-342（key 复用分支 L339-342）、L442-447/L459-464/L527-532（`_type==0` 为假）、L469-484、L486-518、bForceWrite 分支 L442/L459/L476/L493/L510/L527 |
| r19_check_valid_rejects_type_mismatch_and_missing_keys | .cpp:347-381（L349-351 无域、L357-360 无段、L369-372 无 key、L376-377 类型不符）、L254-256（make_valid 无域）、L186-195、L227-234、L385-392 |
| r19_key_slot_limit_is_128_per_section | .cpp:317-321（MAX_KEY_CNT=128，ShareBlocks.h:19）、L615-673 回落默认值 |
| r19_string_byte_budget_stops_at_1024_per_section | .cpp:323-327（`_offset + len > 1024`）、L31（SMVT_SIZES[STRING]=64）、L435-450、L537-548 |
| r19_section_slot_limit_64_and_delete_does_not_reclaim | .cpp:273-278（MAX_SEC_CNT=64，ShareBlocks.h:18）、L227-250（delete 只置 `_state=2`）、L396-402（枚举过滤）→ BUG-R19-003 |

## 2) r19_mq/r19_shareblock_slave_test.cpp（4 条，全启用）
生产文件 `src/WtShareHelper/ShareBlocks.cpp`

| 用例 | 生产行段 |
| --- | --- |
| r19_init_slave_missing_file_false_and_reads_master_kv | .cpp:87-128（L97-98 文件不存在、L100-104 映射、L110-123 装载、L126 日志）、L186-200、L383-404、L407-432、L615-673 |
| r19_slave_cannot_create_section_or_key | .cpp:264-271（非 master 不建段）、L308-315（非 master 不建 key）、L435-535/L537-613 的 nullptr/false 返回 |
| r19_update_slave_ignores_commit_until_blocktime_changes | .cpp:202-225（L221 只写段级时间戳）、L130-163（L136-137 门槛、L139-162 强制重装载）→ BUG-R19-001 |
| r19_update_slave_reloads_after_blocktime_change_and_release_rules | .cpp:227-250（L246-247）、L130-162、L143-156（重装载不看 `_state`）→ BUG-R19-002；L165-184（release_slave 三分支 L168-169/L174-175/L177-183）；L407-433（L416-419 隐式重载）→ BUG-R19-014 |

## 3) r19_mq/r19_shareblock_persist_test.cpp（4 条，全启用）
生产文件 `src/WtShareHelper/ShareBlocks.cpp` + `src/Includes/WTSMarcos.h:98-105`
（输入镜像由 `r19_support.h` 的 `write_raw_shm_block/put_int32_key` 造在临时目录，TearDown 清理）

| 用例 | 生产行段 |
| --- | --- |
| r19_init_master_compacts_dead_sections_and_hides_live_ones | .cpp:36-59（`_state!=1`/`_count==0` 过滤 L40、压缩 L46-58、write_log L48-49）、L61-82（key 装载 L74-80、**L72 置 0**）、L186-200、L202-225、L227-250、L130-163 → BUG-R19-004 |
| r19_second_init_master_on_same_file_destroys_all_sections | .cpp:36-59（第二次加载把 `_count` 清零 + `memset(_sections)`）、L294-303（段名不匹配 → 拒写并报错）、L61-82、L435-535（Y 侧新建段恢复可用）→ BUG-R19-004 |
| r19_last_string_key_in_section_clobbers_next_section_name | .cpp:323（`> 1024` 判据差 1）、L446/L545（`wt_strcpy(des, src, 64)` 实写 65 字节）、L280-285（下一段 `_name` 被置 NUL）、L396-402、L615-623 → BUG-R19-005 |
| r19_section_name_longer_than_32_bytes_is_stored_mangled | .cpp:281（`wt_strcpy(secInfo->_name, section)` 无长度上限）、L329-337（第一个 key 名写回 `_keys[0]._key`）、L396-402（枚举读到 34 字节的混名）→ BUG-R19-013 |

## 4) r19_mq/r19_shareblock_cmd_test.cpp（6 条：5 启用 + 1 DISABLED）
生产文件 `src/WtShareHelper/ShareBlocks.cpp`（命令通道 L675-791）

| 用例 | 生产行段 |
| --- | --- |
| r19_cmd_unknown_name_add_false_get_empty | .cpp:722-724（add_cmd 未知域）、L751-755（get_cmd 未知域，`lastIdx` 不动） |
| r19_cmd_cmder_writes_and_consumer_joining_later_skips_first | .cpp:675-698（建块 + `truncate_file(sizeof(CmdBlock))` L689、capacity==0 → placement-new L697-698）、L700-705（写 pid）、L709-715、L720-748、L751-791（L763-764 cmder 拒读、**L772-776 首帧吞掉第一条**）→ BUG-R19-006 |
| r19_cmd_cold_start_sentinel_then_delivers_first_command | .cpp:767-771（`_readable==UINT32_MAX` → 999999 哨兵）、L777-781（哨兵接续 `_commands[0]`）、L678-679（重复 init 幂等）、L710-715 |
| r19_cmd_foreign_pid_blocks_add_and_init_normalizes_indexes | .cpp:731-737（pid 不符拒写）、L709-714（下标归一化）、L782-784（追平）、L786-790（按 capacity 取模读） |
| r19_cmd_wrapped_block_reads_by_capacity_modulo | .cpp:709-715（`readable > writable` → `writable += capacity`）、L743-747（跨圈写）、L786-790（`lastIdx % capacity`）、第二个映射重置共享计数器 → BUG-R19-007 |
| DISABLED_r19_cmd_longer_than_64_bytes_writes_past_the_block | .cpp:746（不受限 `strcpy` 写 `CmdInfo::_command`，ShareBlocks.h:80-88）→ BUG-R19-011；触发条件：命令长度 ≥64（写在槽位 127 时越出映射尾 36 字节），属未定义行为，本轮不跑 |

## 5) r19_mq/r19_mq_manager_test.cpp（6 条：5 启用 + 1 DISABLED）
生产文件 `src/WtMsgQue/MQManager.cpp`、`MQServer.cpp`、`MQClient.cpp`、`WtMsgQue.cpp`
（端点只用 `inproc://` 与一个不存在的协议名；**不调用 start_client 于活客户端、不用非空 payload 打 publish**，
因为那两处会由产品侧起线程，违反本轮规则）

| 用例 | 生产行段 |
| --- | --- |
| bind_destroy_and_rebind_same_inproc_endpoint | WtMsgQue.cpp:29-37（create_server/destroy_server 包装）、MQManager.cpp:6-18、L20-31、L46-50；MQServer.cpp:39-48（ctor + `makeMQSvrId` 起始 1001）、L50-64（析构 nn_close）、L66-71、L81-102（setsockopt/nn_bind 成功路径） |
| invalid_protocol_logs_bind_failure_but_handle_still_returned | MQServer.cpp:92-98（绑定失败 → 日志 + `nn_close` + `_sock=-1`）、L109-115（publish 走「还没初始化」）、MQManager.cpp:10-17（忽略 init 返回值）→ BUG-R19-010；WtMsgQue.cpp:33-41 |
| publish_with_empty_payload_logs_nothing_and_starts_no_thread | MQServer.cpp:117-118（`data==NULL \|\| dataLen==0` 早退，不入队、不起线程）；MQManager.cpp:20-31、L33-44、L52-56（bServer=false 通道）、L69-80、L82-93、L95-106（五条未知 id 入口各一条 "not exists"） |
| connect_subscribe_destroy_without_start | WtMsgQue.cpp:42-50、MQManager.cpp:58-67、L69-80、L82-93；MQClient.cpp:39-49（ctor + `makeMQCientId` 起始 5001）、L51-62（析构）、L64-107（`nn_socket`/两次 setsockopt/`nn_connect` 成功 + 日志 L103）；MQClient.h:90-92（`sub_topic` inline）、L76（`id()` inline） |
| invalid_protocol_logs_connect_failure_and_logger_can_be_unregistered | MQClient.cpp:96-100（connect 失败：只记日志、不复位 `_sock`）→ BUG-R19-008；MQManager.cpp:46-56（`if (_cb_log)` 真/假两侧）；WtMsgQue.cpp:23-27（regiter_callbacks） |
| DISABLED_start_client_on_failed_init_leaks_recv_thread | MQClient.cpp:110-123（`_sock < 0` 闸门失效 → 起接收线程）+ L51-62（`!m_bReady` 提前返回，不置终止位、不 join）→ BUG-R19-008；触发顺序 `create_client("r19bogus://x") → start_client(id) → destroy_client(id)`，预期宿主挂死/崩溃，本轮不跑 |

## 6) r19_mq/r19_shareblock_capi_test.cpp（3 条，全启用）
生产文件 `src/WtShareHelper/WtShareHelper.cpp`（extern "C" 包装层）

| 用例 | 生产行段 |
| --- | --- |
| r19_capi_master_slave_and_all_type_wrappers | .cpp:6-29（init_master/init_slave/update_slave/release_slave/register_logger）、L49-62（updatetime/commit/delete）、L64-92（六个 allocate_*）、L94-122（六个 set_*）、L124-152（六个 get_*）；ShareBlocks.h:119-131（register_logger 与 write_log 的空 logger 早退） |
| r19_capi_enum_callbacks_count_and_args | .cpp:31-47（get_sections/get_keys 的回调遍历与计数）+ ShareBlocks.cpp:383-404、L407-433 |
| r19_capi_cmd_wrappers_forward_to_shareblocks | .cpp:154-167（init_cmder/add_cmd/get_cmd）+ ShareBlocks.cpp:675-718、L720-749、L751-791 |

## 未覆盖（需要接缝，行数已量）

| 面 | 生产行段 | 行数 | 阻塞原因 |
| --- | --- | --- | --- |
| MQServer 广播线程体（入队/心跳/分包/满缓冲强发/统计同步） | MQServer.cpp:120-246 | 127 | 需要非空 payload（产品侧 `new StdThread`，MQServer.cpp:129）或时钟+连接数接缝；本轮禁止起线程/sleep |
| MQClient 接收线程体 + `extract_buffer` 分包/主题过滤 | MQClient.cpp:110-173、L175-199 | 89 | `extract_buffer` 是 private，`_buffer` 无注入口；跑通需要起线程 |
| 重复 init 早退 + `nn_socket`/`nn_setsockopt` 失败回滚 | MQServer.cpp:68-69、74-79、82-88；MQClient.cpp:66-67、71-76、78-84 | 32 | MQManager 每个 id 只 new 一次实例，nanomsg 在本机不会失败；需要后端注入 |
| 心跳/超时通知（60s 阈值） | MQServer.cpp:142-153；MQClient.cpp:148-160 | 25 | 需要可注入的 `now_ms` 时钟接缝 |
| master/cmder 域的释放入口缺失 | ShareBlocks.cpp:165-184（只有 release_slave，无 release_master/release_cmder） | 0（缺口，不是漏覆盖行） | 需要新增公开入口；本轮只能靠「同文件换域名」重走装载分支 |
