# R5 缺陷清单（迭代01 第七波 · WtDtCore/StateMonitor.cpp + WtDtCore/UDPCaster.cpp）

- 范围: 本 lane 仅记录，未修改任何生产代码；`src/**`（除 `src/TestUnits/coverage/q05_dtsrv/r5_*.cpp`）一字未动。
- 关联用例: `src/TestUnits/coverage/q05_dtsrv/r5_udp_caster_test.cpp`、`src/TestUnits/coverage/q05_dtsrv/r5_state_monitor_bdt_test.cpp`
- 关联既有记录: BUG-Q05-005（StateMonitor 配置泄漏 + `_session[16]` strcpy 溢出）已由 q05 波次登记，本文件不重复主张新发现，只做 R5 侧复述与交叉引用。

## BUG-R5-001 UDPCaster::start 吞掉构造异常后 do_receive 必空指针解引用
- 状态: 待审核
- 严重级别: Blocker
- 位置: `src/WtDtCore/UDPCaster.cpp:110-119`、`src/WtDtCore/UDPCaster.cpp:145-147`
- 现象: `start()` 用 `try { m_sktSubscribe.reset(new UDPSocket(m_ioservice, endpoint(udp::v4(), sport))); } catch(...) { WTSLogger::error(...); }` 包住 socket 构造——端口被占用 / sport 越界 / 权限不足时 `new` 抛出，`reset` 不执行，`m_sktSubscribe` 保持 NULL；紧接着 L119 **无条件**调用 `do_receive()`，而 `do_receive()` 第一句就是 `m_sktSubscribe->async_receive_from(...)`，直接段错误。`catch(...)` 把唯一的降级机会（return / 置停止位）吃掉了，日志打完仍然继续走到崩溃路径。
- 影响范围: dt_srv/wtdtserver 启动时订阅端口冲突是最常见部署故障，进程以 SIGSEGV 退出而不是「订阅服务未起、广播仍可用」；`init()` L93-96 里 `sport` 缺省回落到 `bport`，两个服务撞同一端口的概率不低。
- 复现: 本轮**未写用例**——该路径必须真 `bind(udp)`，违反 lane 约束「不建 socket、不 bind」。需要主控提供 socket 接缝（把 `UDPSocket` 构造抽成可注入工厂）后补 DISABLED 用例。

## BUG-R5-002 StateMonitor 状态机整体缺时钟接缝，544 行只能靠起线程+sleep 才可达
- 状态: 待审核
- 严重级别: Blocker（覆盖率阻塞，非产品缺陷）
- 位置: `src/WtDtCore/StateMonitor.cpp:145-530`（run 线程 lambda）、`StateMonitor.cpp:168`（`sleep_for(2ms)`）、`StateMonitor.cpp:537-543`（stop）
- 现象: `SS_ORIGINAL → SS_INITIALIZED/SS_RECEIVING/SS_PAUSED/SS_CLOSED/SS_PROCING/SS_PROCED/SS_Holiday` 的全部 7 路 switch 分支只写在 `run()` 里 `StdThread` lambda 内部；循环的输入只有 `TimeUtils::getCurDate()` / `TimeUtils::getCurMin()` 两个本地真实时钟读数，且 `lastTime` 是 `static uint64_t`（首次调用后按墙上时钟节流，跨用例互相污染）。类里没有任何可注入时钟、没有可单步的 `checkOnce(curDate, curMin)`，`_map`/`_stopped` 也全是 private 且无 getter，状态只能靠 `isAnyInState/isAllInState/isInState` 三个查询间接观测。
- 影响范围: `StateMonitor.cpp` 的 611 个可计数行里约 400 行（本文件 L145-543）在任何「不起线程、不 sleep」的口径下都不可达，本 lane 只能把可测面推满 initialize()；剩余行需要接缝，留给主控裁定。
- 建议: 抽出 `void StateMonitor::checkOnce(uint32_t curDate, uint32_t curMin)` 供 run() 与测试共用（顺带去掉 `static lastTime`），即可用纯内存用例覆盖全部转换分支。已用 `r5_state_monitor_bdt_test.cpp::DISABLED_StateMachineAdvanceNeedsInjectableClockSeam` 占位说明触发条件（调用 `run()` 即建线程并 2ms 轮询，`stop()` join）。

## BUG-R5-003 do_receive 把未保证 NUL 结尾的 1020 字节缓冲当 C 字符串切分（越界读）
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/UDPCaster.cpp:30-34`（`UDPReqPacket`）、`src/WtDtCore/UDPCaster.cpp:156-176`
- 现象: 收包侧只判 `bytes_recvd == sizeof(UDPReqPacket)`（定长 1024），随后 `StrUtil::split(req->_data, ",")` 以 `const char*` 接口消费 `char _data[1020]`。发送方不写字符串结束符时，split 会越过 1020 字节边界继续找 `\0`，读越 `m_data[2048]` 之后最多 1028 字节的未初始化成员区；对端只需发一份「全 40 字节代码、无逗号、无 NUL」的合法长度报文即可触发。同一 `_data` 里的 `.` 分隔 substr 也建立在同一未校验缓冲上。
- 影响范围: 订阅回放（L162-200）路径的越界读，可能带出邻近成员数据并被 memcpy 进回包 `_data` 一起发给请求方；UDP 无连接，任何同网段主机都能打这个口。
- 复现: 本轮**未写启用用例**——需要真实 `async_receive_from` 回调（活体 socket + io_context::run），且 BUG-R5-001 会让任何真 bind 的尝试直接崩。留给主控裁定接缝方案。

## BUG-R5-004 handle_send_broad/handle_send_multi 是死代码，16 行永不达
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/UDPCaster.cpp:51-52`（声明）、`src/WtDtCore/UDPCaster.cpp:402-416`（定义）
- 现象: 两个 private 成员函数在整个 WtDtCore 内没有任何引用点——`do_receive` 的发送用内联 lambda（L190-199、L206-215），`do_broadcast` 用同步 `send_to(..., ec)` 错误码出参（L370-376、L382-388）。它们既不是回调也未被 `boost::bind`，且是 private，外部无从调用，本轮硬规则禁止 `#define private public`，因此这 16 行不可达。
- 影响范围: 仅代码体积/覆盖率分母；也说明历史上「异步发送 + 错误日志」的设计被静默替换，错误处理口径不一致（现在广播失败只有 do_broadcast 里一条 error 日志，do_receive 回包失败另有一套 lambda）。
- 复现: 未写用例（不可达）。

## BUG-R5-005 addBRecver/addMRecver 对未知 type 静默丢弃且仍返回 true
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/UDPCaster.cpp:222-241`（`if(type==0)/else if(1)/else if(2)` 无 else）、`src/WtDtCore/UDPCaster.cpp:244-266`（同形状）
- 现象: `type` 只有 0/1/2 三个合法桶（flat/json/raw），写成 3 或 99（配置里把 type 当成 bool 或写成 "raw" 解析成 0 之外的数值都很常见）时地址解析成功、不进任何接收方列表，函数依旧 `return true`，既不告警也不返回失败。`init()` L73-88 忽略两个 add 的返回值，所以整份 caster.json 里写错 type 会表现为「服务健康、订阅端零收包」。
- 影响范围: 广播/组播静默失联，排障成本高；`do_broadcast` 的 `m_listRawRecver`/`m_listRawGroup` 为空还会让 L325 的组包分支整体跳过。
- 复现: `r5_udp_caster_test.cpp::AddBRecverAcceptsValidIPv4IntoTypeBucket`（`type=3`/`type=99` 断言返回 true，锁定当前宽松语义；若产品侧改为返回 false，本断言会失败并暴露改动）。

## BUG-R5-006 addBRecver/addMRecver 不校验端口，int 到 uint16 静默截断
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/UDPCaster.cpp:226-227`、`src/WtDtCore/UDPCaster.cpp:248-249`
- 现象: 形参是 `int port`，直接进 `EndPoint(addr, port)`（`boost::asio::ip::port_type` 为 `uint16_t`）。`-1` 被转成 65535、`65536` 被转成 0、`70000` 被转成 4464，都不抛异常，因此不会被 `catch(...)` 兜住，函数返回 true；caster.json 里 `port` 写成负数或超过 65535 时，接收方会被登记到一个完全无关的端口上。`addMRecver` 的 `sendport` 同理会 bind 到截断后的端口。
- 影响范围: 数据发到错误端口，表现为「链路通但收不到」；`start(sport)` 的订阅端口同样从配置直取 int32，`init()` L93-96 里 `sport==0` 才回落 `bport`，负值不回落。
- 复现: `r5_udp_caster_test.cpp::AddBRecverAcceptsPortBoundariesAndDuplicates`（port=0/65535/-1 全部 EXPECT_TRUE，锁死「不做范围校验」的现状）。

## BUG-R5-007 跨线程标志位与队列读写无同步（data race）
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/UDPCaster.h:102`（`bool m_bTerminated`）+ `src/WtDtCore/UDPCaster.cpp:135`（写）与 `290`、`302`（读）；`src/WtDtCore/UDPCaster.cpp:304`（锁外 `m_dataQue.empty()`）对 `306-307`/`313-314`（锁内 wait/swap）；`src/WtDtCore/StateMonitor.h:231`（`bool _stopped`）+ `StateMonitor.cpp:153`、`164`、`172`（线程内读）对 `539`（stop 主线程写）
- 现象: 两侧都用裸 `bool` 做线程终止信号，既非 `std::atomic` 也不在同锁内读写；`std::shared_ptr` 的 `m_thrdCast`/`m_thrdIO` 判空与 `reset` 也跨线程无锁。`do_broadcast` 更是先在锁外读 `m_dataQue.empty()`，再在锁内 `wait`/`swap`——生产者是另一线程在锁内 `push`（L294-296），`empty()` 本身与 `push` 就是无同步并发访问 `std::deque`，TSan 会直接报 race。`StateMonitor::_stopped` 同样被监控线程每 2ms 读、被 `stop()` 写，无内存序保证，实测可能因编译器 hoist 而延长退出等待。
- 影响范围: 未定义行为（数据竞争），表现为停服偶发挂住或读到过期标志；本 lane 无启用用例覆盖该并发窗口（需要线程，按约束留白）。

## BUG-R5-008 do_broadcast 用 break 处理单条坏数据，整批积压被丢弃
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/UDPCaster.cpp:317-392`（L321-322 的 `if (castData._data == NULL) break;`、L360-363 未知 `dataType` 的 `else break;`）
- 现象: 工作线程把 `m_dataQue` 整队 `swap` 到 `tmpQue` 后逐条组包；遇到队首对象为空、或 `dataType` 不在 0x200/0x201/0x202/0x203 四个常量之内时，用 `break` 跳出**整个 while 批处理**而不是 `continue` 跳过该条。`tmpQue` 里剩下的数据随局部对象析构被 release，也就是这一批行情（可能上百条 tick）整体不发出去，且没有任何日志。外层 `while(!m_bTerminated)` 下一轮再从空队列 `wait`，表面看服务仍在跑。
- 影响范围: 只要有任何一条非 tick/ordQue/ordDtl/trans 的类型进入队列（当前 `init`/`broadcast` 没有暴露入口，但 `do_broadcast` 是 `protected`-可见的 `IDataCaster` 扩展面），就会造成成批数据静默丢失；NULL 分支还会让后续条目连带丢。
- 复现: 本轮未写启用用例——`do_broadcast` 是 private，且合法入口要求 `m_sktBroadcast != NULL`（真 socket）；需要注入接缝，留给主控裁定。

## BUG-R5-009 initialize 的区间时间换算对 0000 下溢、跨零点半夜不回绕，且无输入校验
- 状态: 待审核
- 严重级别: Major
- 位置: `src/WtDtCore/StateMonitor.cpp:90-105`（L99 `stime--`、L100 `etime++`）、`src/WtDtCore/StateMonitor.cpp:76-88`（auction 换算同形状但无 ±1）
- 现象: 交易区间换算先把 `HHMM` 折成分钟数再 `stime--`/`etime++`，然后 `/60*100 + %60` 还原成 `HHMM`。当 sessions.json 里出现 `{"from": 0, ...}`（或偏移后为 0）时，`uint32_t stime` 从 0 减 1 下溢成 4294967295，还原表达式得到 7158278255 又溢出回绕，最终写进 `StateInfo::_sections` 的是一个完全无意义的边界，`isInSections()` 从此对任意时间给出错误答案；`etime++` 在 `to=2359` 时同样会算出 `2400` 这类不存在的钟点（代码注释 L86/L103 自己承认「先不考虑半夜12点的情况」）。整个换算链对 `inittime/closetime/proctime` 也没有范围校验，`getUInt32` 缺省 0 直接可用（见 `MissingTimeFieldsDefaultToZeroAndStillAssembleSections`）。
- 影响范围: 状态机的进入/退出时刻（SS_RECEIVING/SS_PAUSED/SS_CLOSED 判定全依赖 `_sections`）被一个畸形配置项整体带偏，且 initialize 返回 true，故障不可见。
- 复现: `r5_state_monitor_bdt_test.cpp::ZeroStartSectionStillAssemblesWithoutValidation` 断言「零起点会话照样入表、initialize 返回 true」——锁定当前无校验行为；真正错误的 `_sections` 数值不可观测（`_map` private 且无 getter），因此不断言具体边界，接缝需求见 BUG-R5-002。

## BUG-R5-010 StateMonitor 的 strcpy 定长缓冲与配置树泄漏（复述 BUG-Q05-005）
- 状态: 待审核
- 严重级别: Minor
- 位置: `src/WtDtCore/StateMonitor.cpp:74`（`strcpy(stateInfo->_session, sid.c_str())` 对 `StateMonitor.h:47` 的 `char _session[16]`）、`src/WtDtCore/StateMonitor.cpp:49-54`（`config` 全程未 `release()`）
- 现象: 与 BUG-Q05-005 同一问题，R5 侧补充可测边界：15 字符会话名恰好写满 `char[16]`（含结尾 `\0`），一旦 JSON key 到 16 字符就写入结构体相邻的 `_init_time` 字段（uint32_t）及其后成员。本 lane 只把 15 字符边界做成启用用例，16 字符溢出不写用例（崩溃/内存踩踏类输入，按 wave 约定交排除清单评审）。
- 影响范围: 配置驱动的内存踩踏 + 每次 initialize 泄漏一棵配置树。
- 复现: `r5_state_monitor_bdt_test.cpp::FifteenCharSessionIdFitsStateKeyBuffer`。合并建议：主控可与 BUG-Q05-005 并条，不重复计账。

## 需要接缝/环境、留给主控裁定的面（非缺陷，登记以免下波重复踩）
1. `UDPCaster::start`（L101-131）、`do_receive`（L145-220）、`init` 的 `active=true` 分支（L70-96）、`do_broadcast` 的组包与 `send_to` 循环（L300-395）——全部要求真实 `bind`/`async_receive_from`/`io_context::run` 线程，本轮硬约束禁止；即使允许，`start` 的 BUG-R5-001 也会让任何真 bind 的尝试崩在半路。
2. `UDPDataPacket`/`UDPReqPacket` 的包头布局（L28-47）是 .cpp 内部 `#pragma pack` 类型，头文件不导出，外部用例无法直接构造 `UDPTickPacket` 做长度/截断校验；若要覆盖「截断包、`_type` 非法、长度不符」这三条解析分支，需要把它们提到 `WtDtCore` 的私有头或提供 `handle_packet_for_test(const char* buf, size_t len)` 一类的内存入口。
3. `addMRecver` 的合法地址分支（L249-259）需要 `new UDPSocket(..., sendport)` + `join_group`，等价于 bind 一个真实端口，已用 `DISABLED_AddMRecverWithValidAddressNeedsRealSocket` 占位。
4. `StateMonitor::run`/`stop` 的全部状态推进见 BUG-R5-002；本轮已把 `initialize` 的可测面（含 L109-134 的品种/节假日循环，q05 波次因 `_session_to_pids` 为空而未走到）打到 100%。
