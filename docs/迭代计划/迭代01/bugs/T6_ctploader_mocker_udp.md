# T6 缺陷记录 — 收发族第六波（CTPLoader / TraderMocker / ParserUDP）

- 波次: 迭代01 第六波 · lane T6
- 用例: `src/TestUnits/coverage/t_CTPLoader/t6_ctploader_reachable_test.cpp`（15 启用 + 5 `DISABLED_`）、
  `src/TestUnits/coverage/t_TraderMocker/t6_tradermocker_reachable_test.cpp`（15 启用 + 3 `DISABLED_`）、
  `src/TestUnits/coverage/t_ParserUDP/t6_parserudp_reachable_test.cpp`（12 启用 + 2 `DISABLED_`）
- 本轮未修改任何 `src/**` 生产代码、`CMakeLists.txt`，未放宽任何断言。
- 行号一律取本轮 Read/Grep 工具结果（构建侧门禁：`gate_flags/t_CTPLoader.sh`、`t_TraderMocker.sh`、`t_ParserUDP.sh` 均 exit 0）。

---

## BUG-T6-001

- 状态: 待审核
- 严重级别: Blocker（成交回报数量错误，直接影响持仓/对账）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L422-423（`match_once()` 内）
- 相关测试: 无法直达（`match_once()` 是私有成员，见 BUG-T6-011）；本轮只能在 manifest 里以行号定桩
- 输入/前置条件: 任一委托撮合成交，`curVol > 0`
- 期望行为: `setVolTraded(getVolTraded() + curVol)` —— 已成交量累加
- 实际行为: `ordInfo->setVolTraded(ordInfo->getVolTraded() - curVol);`（L423）与 `setVolLeft(... - curVol)` 同向相减，
  于是 `VolTraded` 从 0 变负数，且每片成交都继续减；下游任何用 `getVolTraded()` 做均价/仓位推算的都是错值
- 复现步骤: 需先把 `match_once()` 暴露成可注入接缝（或驱动 `_io_service`），再喂一条 tick + 一条限价委托
- 影响范围: TraderMocker 全部撮合回报；仿真盘 `WtCore/TraderAdapter` 侧的订单成交累计与撤单可用量
- 备注: 只记录不实施。修复方向：L423 改成 `+ curVol`；同时补一条「部分成交后 VolTraded 单调递增」的接缝用例。

## BUG-T6-002

- 状态: 待审核
- 严重级别: Blocker（作为 SHARED 库会把宿主进程直接终止）
- 模块/文件: `src/CTPLoader/TraderSpi.cpp` L542-548（同类 L545、L574、L591）
- 相关测试: `DISABLED_t6_ctploader_spi_exit.instrument_last_response_calls_exit_zero`
- 输入/前置条件: `OnRspQryInstrument(pInstrument, pRspInfo, nRequestID, bIsLast=true)`，全局 `QRYFEES == false`
- 期望行为: 作为 `ADD_LIBRARY(CTPLoader SHARED)`（`src/CTPLoader/CMakeLists.txt` L38）被 LoaderRunner 装载时，
  应当以返回值/回调通知结束，而不是终止进程
- 实际行为: 走 `DumpToJson(); exit(0);` —— 进程立即退出，`run()` 之后的任何清理（含厂商 `RegisterSpi` 注销）都不执行
- 复现步骤: 单独跑该 DISABLED 用例即可（本轮禁用原因：会把测试进程杀掉，连带丢失同二进制其它结果的 profraw）
- 影响范围: 任何把 CTPLoader 当插件用的宿主；LoaderRunner 的多任务串行装载
- 备注: 命令行工具形态下 `exit(0)` 是「设计如此」，但模块形态不是。修复方向：以 `atexit`/回调或返回码替代。

## BUG-T6-003

- 状态: 待审核
- 严重级别: Major（配置错误 → 崩溃而不是报错）
- 模块/文件: `src/CTPLoader/CTPLoader.cpp` L90-91、L98-99（内联配置分支）与 L162-163、L170-171（文件分支）
- 相关测试: `DISABLED_t6_ctploader_run_crash.inline_config_without_ctp_section_dereferences_null`
- 输入/前置条件: 配置里缺 `ctp` 段（或缺 `config` 段）
- 期望行为: 与 L87-88、L159-160 一致 —— 打印/返回 0，走「配置无效」路径
- 实际行为: `WTSVariant* ctp = root->get("ctp");` 返回 NULL 后直接 `ctp->getCString("front")`，空指针解引用崩溃
- 复现步骤: `run("other:\n  a: 1\n", false, false)`
- 影响范围: 所有配置文件拼写错误/段落缺失的现场；LoaderRunner 传进来的任何非法配置
- 备注: 三个分支（内联/ini/文件）里只有 ini 分支因为 `IniHelper::readValue` 自带 try/catch 而安全（见 L130-153 用例）。

## BUG-T6-004

- 状态: 待审核
- 严重级别: Major（异常穿透 `extern "C"` 边界，UB 风险）
- 模块/文件: `src/CTPLoader/CTPLoader.cpp` L71/L76 声明与定义、L86（`WTSCfgLoader::load_from_content(cfgfile, true)`）、L158（`load_from_file`）
- 相关测试: `t6_ctploader_run.unparsable_inline_yaml_escapes_through_the_c_entry`（实测以 `EXPECT_TRUE(threw)` 钉住）
- 输入/前置条件: `cfgfile` 是语法非法的 YAML，例如 `"[unterminated"`
- 期望行为: `run` 是 C 导出入口，不得让 C++ 异常穿过它；应 catch 后返回错误码
- 实际行为: `WTSCfgLoader::load_from_yaml`（`src/WTSUtils/WTSCfgLoader.cpp` L233-247）里 `YAML::Load` 抛
  `YAML::ParserException`，`run()` 全程无 try/catch → 异常从 `extern "C"` 函数逃出
- 复现步骤: `run("[unterminated", false, false)` 并在外层 catch `std::exception`（实测会捕获到）
- 影响范围: 任何以 C 方式调用 `run` 的宿主（LoaderRunner 走的就是这条路）
- 备注: 修复方向：`run` 整体包 try/catch，或 `load_from_yaml` 内部 catch 后返回 NULL。

## BUG-T6-005

- 状态: 待审核
- 严重级别: Major（越界读 + 有符号 char 传入 isdigit 是 UB）
- 模块/文件: `src/CTPLoader/TraderSpi.cpp` L98-109（`extractProductName`），调用点 L403、L412 附近
- 相关测试: `DISABLED_t6_ctp_loader_names_crash.extract_product_name_on_empty_string_reads_out_of_bounds`；
  正常路径断言见 `t6_ctp_loader_names.extract_product_name_trims_trailing_month_digits`
- 输入/前置条件: ① `extractProductName("")`；② `extractProductName("沪银2512")`（UTF-8 中文，`InstrumentName` 的真实形态）
- 期望行为: 空串返回空串；含多字节字符的名字按字符边界处理
- 实际行为: ① `auto idx = strlen(cname) - 1;` 得到 `size_t` 的 `(size_t)-1`，随后 `cname[idx]` 越界读；
  ② `isdigit(cname[idx])` 收到 `char`（arm64 下默认有符号）的负值，超出 `isdigit` 定义域（UB），
  glibc/macOS libc 的表实现按偏移索引，负索引会读到表外内存
- 复现步骤: 直接调用（①在 DISABLED 用例里）
- 影响范围: 中文合约名是所有国内柜台的常态；一旦 `MAP_NAME` 未命中就走 L403 的自动取名，命中 UB 路径
- 备注: 修复方向：`size_t idx = strlen(cname); while (idx > 0 && isdigit((unsigned char)cname[idx-1])) --idx;`

## BUG-T6-006

- 状态: 待审核
- 严重级别: Major（UDP 报文无长度校验 + 类型分流可疑）
- 模块/文件: `src/ParserUDP/ParserUDP.cpp` L335-338（`handle_read`）、L357-365（`extract_buffer`）、L365（类型判定）
- 相关测试: 无法直达（`extract_buffer`/`handle_read`/`_b_buffer` 全私有，见 BUG-T6-011）；
  `DISABLED_t6_parserudp_net.extract_buffer_needs_a_seam_before_it_can_be_fed` 把接缝诉求钉在用例清单里
- 输入/前置条件: 收到一个 `bytes_transferred` 小于 `sizeof(UDPTickPacket)` 的包（4 字节头 + 结构体），或半包/截断包
- 期望行为: `extract_buffer(length, isBroad)` 应先校验 `length >= sizeof(UDPPacketHead)` 且
  `length >= 对应类型包体长度`，否则丢弃并记日志
- 实际行为: 形参被 `/*length*/` 注释掉（L357），直接把 `_b_buffer.data()` 强转成 `UDPTickPacket*`
  并读取 `packet->_data.exchg/code`（L367-368）—— 越界读 1024 字节缓冲之外；
  另外 L365 把 `UDP_MSG_SUBSCRIBE`（订阅回包，`UDPReqPacket` 形态）与 `UDP_MSG_PUSHTICK` 合并成
  同一条 tick 解析路径，字段布局完全不同
- 复现步骤: 需要 fake socket 注入点或用 protected 接缝后喂 8 字节包
- 影响范围: 行情链路收到畸形/截断包时读到脏内存，生成错误的 fullcode/tick
- 备注: `src/TraderMocker/TraderMocker.cpp` L985-989 是同一份代码的复制粘贴（同样忽略 length），见 BUG-T6-007。

## BUG-T6-007

- 状态: 待审核
- 严重级别: Major（与 BUG-T6-006 同源，落在交易侧仿真器）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L985-1006（`extract_buffer`）、L967-983（包结构定义）
- 相关测试: 无法直达（私有 + 需要 io_context 被 run）
- 输入/前置条件: `_b_buffer` 收到 `_type == UDP_MSG_PUSHTICK` 但长度不足 `sizeof(UDPTickPacket)` 的包
- 期望行为: 按 length 校验后再解析，且解析前先确认 `_type` 落在已知类型集合
- 实际行为: `void TraderMocker::extract_buffer(uint32_t /*length*/, bool /*isBroad*/)` 两个形参都未被使用，
  直接强转读字段；未知 `_type` 静默丢弃且无任何日志
- 复现步骤: 同上，需要接缝
- 影响范围: 仿真交易器的行情注入路径
- 备注: 该函数与 ParserUDP 的实现重复，建议合并到一处共享（只提方向，不实施）。

## BUG-T6-008

- 状态: 待审核
- 严重级别: Major（配置校验缺失/无效，且落盘路径不可配置）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L518-547（`init`）、L120-136（`makeEntrustID`）；
  `src/ParserUDP/ParserUDP.cpp` L98-113（`init`）
- 相关测试: `t6_parserudp_init.non_numeric_host_throws_uncaught_system_error`、
  `t6_parserudp_init.missing_host_config_throws_instead_of_reporting_failure`、
  `t6_init.missing_keys_fall_back_to_qty_defaults_and_zero_id`（正向侧）
- 输入/前置条件: ① ParserUDP 配置 `host` 非法或不存在；② TraderMocker `init(NULL)`；③ TraderMocker 配置缺 `udp_port/span/...`
- 期望行为: ①`init` 返回 false 并给出可读错误；②空指针防护后返回 false；③端口/周期等关键项缺失时报警
- 实际行为: ①`ip::make_address(_hots)`（L107）抛 `boost::system::system_error`，`init` 的 `bool` 返回值形同虚设
  （除了抛，永远 `return true`）；②L520 `params->getUInt32("span")` 无 NULL 防护 → 崩溃（DISABLED 原因，未建用例）；
  ③全部静默取 0，`_udp_port = 0` 会让 `reconn_udp()` 绑到随机端口而无人察觉；
  另外 `init` 把落盘目录硬编码成相对当前工作目录的 `"./mocker_<id>/"`（L538-541），
  用例/进程只能在自己的 cwd 里建目录（本文件用 `MockerDirGuard` 前后清理）
- 复现步骤: 见上述三条用例
- 影响范围: 仿真器/UDP 解析器的所有配置装载点
- 备注: 修复方向：`init` 内 try/catch + 必填项校验返回 false；落盘目录改为可配置项。

## BUG-T6-009

- 状态: 待审核
- 严重级别: Minor（未初始化成员被读；泄漏一个哈希表）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L81-95（构造函数列表）、L98-108（析构）
- 相关测试: `t6_tradermocker_factory.create_delete_roundtrip_clears_caller_pointer`
  （只构造不 init，正是本条的触发形态）；同目录 `seam_tradermocker_test.cpp` 的第 2 条用例同样在未 init 时调 `makeEntrustID`
- 输入/前置条件: 构造后不 `init()` 就调用 `makeEntrustID()`
- 期望行为: 成员有确定初值；`_awaits` 与 `_positions` 在析构时被释放
- 实际行为: 构造列表只给了 `_terminated/_listener/_orders/_trades/_ticks/_max_tick_time/_last_match_time/_b_socket`，
  `_mocker_id/_min_qty/_max_qty/_millisecs/_use_newpx/_udp_port` 全部未初始化，
  而 L127 的 `format_to(buffer, "me.{}.{}.{}", getCurDate(), _mocker_id, ...)` 会读 `_mocker_id` 的脏值；
  析构释放了 `_orders/_trades/_ticks`，独独漏掉 `_awaits`（L98-108）
- 复现步骤: `TraderMocker api; char b[64]; api.makeEntrustID(b, 64);` + valgrind/MSAN
- 影响范围: 初始化顺序敏感的所有调用点；长跑进程的内存增长
- 备注: 修复方向：类内默认成员初始化（`= 0`）+ 析构补 `_awaits->release()`。

## BUG-T6-010

- 状态: 待审核
- 严重级别: Minor（连接生命周期不对称，析构后 handler 仍可能引用 this）
- 模块/文件: `src/ParserUDP/ParserUDP.cpp` L94-96（空析构）、L268-281（`disconnect`）、L254-266（`connect`）
- 相关测试: `t6_parserudp_state.release_and_disconnect_without_connect_are_safe`、
  `t6_parserudp_spi.handle_event_is_not_emitted_without_io_context`（deferred 侧证）
- 输入/前置条件: `connect()` 之后再 `disconnect()`/`release()`
- 期望行为: `disconnect` 关闭并释放 `_b_socket` 与 `_s_socket`，停止并 join `_thrd_parser`
- 实际行为: `disconnect()` 只处理 `_b_socket`（L270-275），`_s_socket` 与 `_send_queue` 从不清理，
  `_thrd_parser` 既不 stop 也不 join，`release()` 是空函数（L115-118），
  `~ParserUDP()` 也是空的（L94-96）—— 对象销毁后 io_context 里仍留有捕获 `this` 的 handler
- 复现步骤: 需要真实端口（`DISABLED_t6_parserudp_net.connect_binds_ports_and_starts_io_thread`）
- 影响范围: 解析器热重载/退出路径的 use-after-free
- 备注: 只记录，不实施。

## BUG-T6-011（可测性缺陷，非现网故障）

- 状态: 待审核
- 严重级别: Major（阻断本迭代覆盖率目标）
- 模块/文件: `src/TraderMocker/TraderMocker.h` L31-41/L112-116、`src/ParserUDP/ParserUDP.h` L48-62
- 相关测试: 上述全部 `DISABLED_` 用例
- 输入/前置条件: 本轮门禁（不起线程、不 sleep、不绑真实端口、禁止 `#define private public`）
- 期望行为: 撮合/解析这类纯逻辑应当可以在无网络、无 io_context 的条件下被驱动
- 实际行为: `match_once()/makeTradeID()/makeOrderID()/load_positions()/save_positions()/extract_buffer()/handle_read()`
  全为 private，`_io_service/_positions/_awaits/_b_buffer` 也是 private，且没有任何测试接缝或 friend，
  导致：撮合价量与涨跌停判定、持仓增减与冻结量推进、委托状态机与回报字段映射、UDP 包头解析与长度校验
  这四项目标本轮 **一行都无法执行**（`TraderMocker.cpp` 只有 ~100/984 行可直达，`ParserUDP.cpp` ~90/458）
- 复现步骤: 尝试在测试里调用 `api.match_once()` → 编译期访问控制报错（本轮不允许用宏绕过）
- 影响范围: T6 三个模块的覆盖率天花板
- 备注: 建议评审方向（不实施）：把这些函数提为 `protected` 并加一个 `friend struct TraderMockerTestAccess;`
  或提供 `IOTestSeam`（公开的 `poll_one_handler()`）；UDP 侧再提供「喂内存 buffer 的解析入口」。
  另：`src/TraderMocker/TraderMocker.cpp` L149-151 里 `ct == NULL` 时用 `_bd_mgr->getContract(...)`，
  但 `_bd_mgr` 只有 `registerSpi` 后才非空、且 `commInfo`（L170）取回后未判空即 `commInfo->getPriceMode()`（L173），
  同一 lambda 里还有这两处空指针风险，一并待审核。
