# T6 缺陷记录 — 收发族第六波（CTPLoader / TraderMocker / ParserUDP）

- 波次: 迭代01 第六波 · lane T6
- 用例: `src/TestUnits/coverage/t_CTPLoader/t6_ctploader_reachable_test.cpp`（15 启用 + 5 `DISABLED_`）、
  `src/TestUnits/coverage/t_TraderMocker/t6_tradermocker_reachable_test.cpp`（15 启用 + 3 `DISABLED_`）、
  `src/TestUnits/coverage/t_ParserUDP/t6_parserudp_reachable_test.cpp`（12 启用 + 2 `DISABLED_`）
- 本记录未修改 `src/**` 生产代码或 `CMakeLists.txt`；非法内联 YAML 用例已改为断言失败返回，详见 T6-004 的当前定向结果。
- 行号一律取本轮 Read/Grep 工具结果（构建侧门禁：`gate_flags/t_CTPLoader.sh`、`t_TraderMocker.sh`、`t_ParserUDP.sh` 均 exit 0）。

---

## BUG-T6-001

- 状态: 确认的撮合字段更新缺陷（源码与默认初始值一致；撮合路径本轮未运行）
- 严重级别: Major（成交回报数量错误，会污染仿真订单状态与基于成交量的统计）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L422-423（`match_once()` 内）
- 相关测试: 无直接覆盖。`t6_tradermocker_reachable_test.cpp` 只验证可公开调用面和 `splitVolume`，撮合线程用例被禁用；本轮未运行撮合路径。
- 输入/前置条件: 任一委托撮合成交，`curVol > 0`
- 期望行为: `setVolTraded(getVolTraded() + curVol)` —— 已成交量累加
- 实际行为: L423 将 `getVolTraded()` 减去成交量。订单由 `WTSOrderInfo::create()` 创建时 `m_dVolTraded` 初始化为 0（`WTSTradeDef.hpp:310,318-326`），`orderInsert` 没有覆盖该值，因此每片成交后累计成交量为负；成交后的订单推送也会携带该错误值。
- 证据边界: 加减方向和新订单默认值可由源码确认；本轮没有驱动 tick→撮合→推送，不能声称已动态复现。
- 影响范围: TraderMocker 全部撮合回报；仿真盘 `WtCore/TraderAdapter` 侧的订单成交累计与撤单可用量
- 备注: 修复方向：按 `+ curVol` 更新；通过不启动线程的撮合接缝喂入部分成交，断言成交前为 0、成交后等于累计量、VolLeft 同步减少，并检查推送快照。

## BUG-T6-002

- 状态: 待审核
- 严重级别: Major（同进程宿主的终止风险；现有证据不足以认定为所有受支持宿主的 Blocker）
- 模块/文件: `src/CTPLoader/TraderSpi.cpp` L542-548（同类 L545、L574、L591）
- 相关测试: `DISABLED_t6_ctploader_spi_exit.instrument_last_response_calls_exit_zero`
- 输入/前置条件: `OnRspQryInstrument(pInstrument, pRspInfo, nRequestID, bIsLast=true)`，全局 `QRYFEES == false`
- 期望行为: 若 CTPLoader 要支持被同进程宿主复用，应通知宿主完成并返回，而不是结束整个进程。
- 实际行为: 命中 `DumpToJson(); exit(0);` 时，调用进程以状态 0 退出，不会返回到调用方。
- 复现步骤: 在独立测试进程中以 `--gtest_also_run_disabled_tests` 运行该用例，观察到 `rc=0`，输出停在 `[ RUN ]`，没有 gtest 收尾摘要。不得在承载其他测试或业务任务的进程中启用此用例。
- 影响范围: 任何在仍需继续工作的宿主进程中调用该回调的场景。仓库内 `src/CTPLoader/CMakeLists.txt:42` 将模块构建为 SHARED；但 `src/LoaderRunner/LoaderRunner.cpp:98-110` 展示的仓库宿主只调用一次 runner 后返回，尚无仓库证据证明存在“多任务串行装载”场景或通用的可复用宿主契约。
- 证据边界: 进程退出行为已复现；`CTPLoader::run()` 在 `Join()` 后没有显式的 `RegisterSpi(NULL)` / API 释放清理代码，故不能据此声称 `exit` 跳过了这些并不存在的清理步骤。是否违背产品契约取决于 CTPLoader 是否承诺可嵌入并复用。
- 备注: 若产品契约要求库可嵌入，应由回调或完成状态通知 `run()`，停止并释放 API 后返回；若仅支持一次性 LoaderRunner 进程，应把进程退出责任放在宿主层并明确该限制。避免用 `atexit` 代替宿主通知。

## BUG-T6-003

- 状态: 待审核
- 严重级别: Major（配置错误 → 崩溃而不是报错）
- 模块/文件: `src/CTPLoader/CTPLoader.cpp` L90-91、L98-99（内联配置分支）与 L162-163、L170-171（文件分支）
- 相关测试: `DISABLED_t6_ctploader_run_crash.inline_config_without_ctp_section_dereferences_null`
- 输入/前置条件: `isFile=false` 且 YAML 根对象缺 `ctp` 段，或存在 `ctp` 但缺 `config` 段。INI 分支不构造 `WTSVariant` 根节点，按 `IniHelper` 的默认值读取后进入必填字段检查，是不同路径。
- 期望行为: 验证根节点和必需配置段后，按已定义的配置错误状态返回；错误路径不得崩溃或留下半更新的全局配置。
- 实际行为: `WTSVariant* ctp = root->get("ctp");` 返回 NULL 后直接 `ctp->getCString("front")`，空指针解引用崩溃
- 复现步骤: `run("other:\n  a: 1\n", false, false)`
- 影响范围: 通过内联 YAML 或 JSON/YAML 文件分支加载、且缺少必需段的配置。缺失文件本身不是此问题：`WTSCfgLoader::load_from_file()` 会返回 NULL，`run()` 在 L159-160 提前返回；对应 CTPLoader 缺失文件用例通过。
- 备注: INI 分支安全是因为读取接口为缺失键提供默认值，随后由必填字段检查挡住加载；现有证据不支持“`readValue` 的 try/catch”这一归因。CTPOptLoader 的同形条目是 BUG-T4-018；两者位于不同 loader 的生产实现，保留独立条目，但不要把 R36-003 再登记成第三条。修复时应在读写全局配置之前校验根节点及 `ctp`/`config` 段，并用独立用例覆盖两种缺段输入。

## BUG-T6-004

- 状态: 本地契约回归 FAIL（解析异常逃出 loader）；产品修复与复核待办
- 严重级别: Major（非法配置异常逃出 loader API，宿主可能被终止）
- 模块/文件: `src/CTPLoader/CTPLoader.cpp` L71/L76 声明与定义、L86（`WTSCfgLoader::load_from_content(cfgfile, true)`）、L158（`load_from_file`）
- 相关测试: 原用例把异常外抛当作通过条件；修正后的 `t6_ctploader_run.unparsable_inline_yaml_reports_failure_without_throwing` 单独运行 FAIL，实际抛出 `YAML::ParserException`，未返回失败值。
- 输入/前置条件: `cfgfile` 是语法非法的 YAML，例如 `"[unterminated"`
- 期望行为: loader 对非法配置执行统一错误处理，向调用方返回文档规定的失败状态；不把解析异常留给外部调用方处理。
- 实际行为: `WTSCfgLoader::load_from_yaml`（`src/WTSUtils/WTSCfgLoader.cpp` L233-247）里 `YAML::Load` 抛
  `YAML::ParserException`，而 `run()` 没有 try/catch。相同 YAML 加载器也用于 `load_from_file()` 的 `.yaml/.yml` 路径。
- 复现步骤: 构建 `CoverageTests_t_CTPLoader`，对上述用例单独运行；Google Test 捕获到解析异常并报告不符合无异常失败协议。
- 影响范围: C++ 调用方若无 catch，异常可继续传播并终止进程；C 调用方也无法通过 C 错误码协议接收该异常。仓库内 LoaderRunner 调用 runner 后没有 catch（`src/LoaderRunner/LoaderRunner.cpp:108-110`）。
- 备注: `extern "C"` 语言链接本身不能单独证明异常外溢必然是 UB；缺陷是公开 loader 边界没有把解析失败转成其错误协议。`load_from_yaml()` 注释承诺解析失败返回 NULL，但 `YAML::Load` 的解析异常没有被捕获。修复可在该函数捕获解析异常并返回 NULL，或在 `run()` 边界 catch 并返回明确的失败状态；应同时覆盖非法内联内容和非法 YAML 文件。

## BUG-T6-005

- 状态: 待审核
- 严重级别: Major（空串索引越界及 ctype 参数域风险；当前未证明必然崩溃）
- 模块/文件: `src/CTPLoader/TraderSpi.cpp` L98-109（`extractProductName`），调用点 L403、L412 附近
- 相关测试: `DISABLED_t6_ctp_loader_names_crash.extract_product_name_on_empty_string_reads_out_of_bounds`；正常路径断言见 `t6_ctp_loader_names.extract_product_name_trims_trailing_month_digits`。独立运行禁用用例时进程 `rc=0`，未观察到崩溃。
- 输入/前置条件: ① `extractProductName("")`；② `extractProductName("沪银2512")`（UTF-8 中文，`InstrumentName` 的真实形态）
- 期望行为: 空串返回空串；对非 ASCII 名称只按预期的 ASCII 月份数字扫描，不把多字节文本误作非法 ctype 输入。
- 实际行为: ① 空串时 `strlen(cname)-1` 无符号下溢，随后表达式读取无效索引，属于未定义行为；② 若普通 `char` 为有符号，非 ASCII 字节传给 `isdigit` 可能是负值，超出其允许参数域。两者可能不崩溃；本轮空串禁用用例实测未崩溃。
- 复现步骤: 现有禁用用例只调用函数并执行 `SUCCEED()`，没有验证返回值或安全性，因此不能作为“必崩”复现。以静态代码判据登记；修复后增加空串、ASCII 月份后缀及非 ASCII 名称断言。
- 影响范围: `MAP_NAME` 未命中时，期货合约自动取名可触及该扫描逻辑；具体名称编码及是否进入该分支需按柜台配置确认。
- 备注: 修复时先处理空串，避免 `strlen()-1`；扫描前先保证索引有效，并将字节转为 `unsigned char` 后传给 `isdigit`。现有启用用例规定 `extractProductName("2512") == "2"`，修复应保留此行为或先明确调整其契约。

## BUG-T6-006

- 状态: 确认的 UDP 报文长度校验缺陷；原“订阅回包结构不匹配”判断撤回
- 严重级别: Major（短报文可按未收到的字节解释包头或包体；本轮无动态复现）
- 模块/文件: `src/ParserUDP/ParserUDP.cpp` L318-338（`handle_read` 传入 datagram 长度）、L357-441（`extract_buffer` 忽略长度并按类型读取结构体）
- 相关测试: `DISABLED_t6_parserudp_net.extract_buffer_needs_a_seam_before_it_can_be_fed` 仅说明无内存注入接缝，不执行解析；本轮未运行。
- 输入/前置条件: 收到长度不足包头（`sizeof(UDPPacketHead)`）或不足对应消息结构体的 datagram。
- 期望行为: 先验证包头长度，再按消息类型验证完整结构体长度；不完整报文丢弃，并在需要时记录诊断。
- 实际行为: `handle_read` 将 `bytes_transferred` 传给 `extract_buffer`，但后者把参数注释掉；收到任何正长度后就读 `_b_buffer`/`_s_buffer` 的包头，匹配某个类型后再访问整份数据结构。读取位置在固定 1024 字节数组之内，但可能超出本次 datagram 实际写入的字节，因而会用未收到或上次报文残留的数据解析。
- 证据边界: 缺少长度检查和后续字段读取由源码静态确认；不能描述为“越过 1024 字节缓冲分配边界”，本轮也没有动态喂入短包。
- 类型契约复核: `UDP_MSG_SUBSCRIBE` 不是错误分流。`UDPCaster::do_receive` 收到该订阅请求后，构造 `UDPTickPacket` 并将响应 `_type` 设为原请求类型（`src/WtDtCore/UDPCaster.cpp:184-188`）；因此 ParserUDP 将 `UDP_MSG_SUBSCRIBE` 与 `UDP_MSG_PUSHTICK` 作为 tick 解析符合仓库内协议。
- 修复方向: 对每种已知类型分别校验 `sizeof(UDPPacketHead) + sizeof(payload)`；建议用 `memcpy` 将完整帧复制到本地对象后再处理。用内存接缝覆盖短于包头、短于各类型包体、完整 tick/订阅响应及完整 0x201–0x203 帧。

## BUG-T6-007

- 状态: 确认的 UDP 报文长度校验缺陷；与 BUG-T6-006 为不同消费者中的同类问题
- 严重级别: Major（截断行情包可被当成完整 tick；本轮无动态复现）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L935-953（接收长度传入）、L964-1006（`extract_buffer`）；包结构在 L967-984。
- 相关测试: 当前 T6 用例没有把报文送入 `extract_buffer`；`connect_binds_real_udp_socket_and_runs_io_context` 被禁用。本轮未运行。
- 输入/前置条件: 广播 socket 收到包头标识为 `UDP_MSG_PUSHTICK`、但 datagram 长度小于 `sizeof(UDPTickPacket)` 的报文。
- 期望行为: 核对包头和完整 tick 长度后再读取；截断帧丢弃。
- 实际行为: `handle_read` 传入接收长度，`extract_buffer(uint32_t /*length*/, bool /*isBroad*/)` 忽略它，并从固定 `_b_buffer` 读取 `_type` 和完整 `WTSTickStruct`。越界的是收到的报文长度边界，非固定缓冲数组边界；尾部可能是未收到或上次报文残留的数据。
- 证据边界: 长度未校验可由源码确认；本轮未运行 UDP 接收或短帧测试。未知类型无分支会静默返回，但当前未发现要求对未知包记录日志的协议/产品契约，不另列为已确认缺陷。
- 修复方向: 加入最小头部与完整 tick 长度校验，解析前使用完整帧副本；通过内存输入接缝测试短头、短 tick、完整 tick 和未知类型。与 T6-006 可共享“校验长度”的规则，但保留两个独立入口的回归覆盖。

## BUG-T6-008

- 状态: 拆分复审：初始化错误协议问题成立；缺键默认值和目录可配置性未证为缺陷
- 严重级别: Major（无效 ParserUDP 地址抛异常；initExt 传 NULL 时两模块均可能解引用空指针）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L518-547（`init`）、L120-136（`makeEntrustID`）；
  `src/ParserUDP/ParserUDP.cpp` L98-113（`init`）
- 相关测试: 原 UDP 两例把异常外抛作为预期，与 `IParserApi::init`“返回是否初始化成功”的 bool 契约相冲突。现改为 `non_numeric_host_reports_initialization_failure` 和 `missing_host_reports_initialization_failure`，定向均 FAIL，异常仍外抛；有效地址对照 PASS。`t6_init.missing_keys_fall_back_to_qty_defaults_and_zero_id` 属于 TraderMocker 的独立默认值场景。
- 输入/前置条件: (1) ParserUDP 收到非数字地址或缺少 host；(2) ParserUDP/TraderMocker 被 `ParserAdapter::initExt` / `TraderAdapter::initExt` 调用时，adapter 会向 API 的 `init` 传 `NULL`（`WtCore/ParserAdapter.cpp:60`、`WtCore/TraderAdapter.cpp:228`）；(3) TraderMocker 普通配置缺可选键。
- 期望行为: 无效地址通过初始化失败协议返回 false；`init(NULL)` 应按明确约定拒绝并返回 false，或由 adapter 提供有效配置，不应直接解引用。哪些配置键必填需由配置 schema 明确。
- 实际行为: ParserUDP 在 L100 直接读取 `config`，在 L107 对缺失/非法 host 调 `ip::make_address`，异常未在插件或 `ParserAdapter::init` 边界捕获。TraderMocker L520 起直接读取 `params`，通过两个 `initExt` 入口传 NULL 时会解引用。普通非空配置中缺失的 `span/newpx/mockerid/maxqty/minqty/udp_port` 被读成默认值；其中 max/min qty 有显式回退，`udp_port == 0` 会在 connect 时绑定系统分配端口。
- 证据边界: NULL 解引用由调用顺序静态确认，未动态运行；UDP 异常逸出有本地定向测试证据。未发现配置 schema 证明 `udp_port` 必填或 `mocker_<id>` 必须可配置；当前用例有意验证缺键默认值并清理相对目录，因此不能把这些行为报成已确认缺陷。
- 影响范围: 通过通用 initExt 接入这两个 API 时，init(NULL) 可能崩溃；ParserUDP 的地址配置错误会以异常离开 bool 初始化接口。`udp_port=0` 是否导致不可接收预期广播依赖部署端口契约。
- 备注: 修复方向：两模块显式处理空配置；ParserUDP 捕获地址解析失败、记录错误并返回 false，同时把现有异常用例改成 bool 失败断言。对 `udp_port` 建立 schema 后再决定是否拒绝 0。`./mocker_<id>/` 是当前实现和测试可见的默认存储布局；只有产品要求用户指定目录时，才增加目录配置项。

## BUG-T6-009

- 状态: 确认的资源泄漏；未初始化成员读取仅在未遵循 init 生命周期时触发
- 严重级别: Minor（未释放的订单缓存及其仍持有的对象）
- 模块/文件: `src/TraderMocker/TraderMocker.cpp` L81-95（构造函数列表）、L98-108（析构）
- 相关测试: `t6_tradermocker_factory.create_delete_roundtrip_clears_caller_pointer` 只构造、查询状态并删除，没有调用 `makeEntrustID`；`seam_tradermocker.entrust_id_buffer_is_written` 在未 init 前调用但只执行 `SUCCEED()`，没有断言返回值或格式。T6 启用的编号用例先调用 `init`。本轮未运行。
- 输入/前置条件: (1) 构造后跳过 `init` 就调用编号 API；(2) 至少一次有效订单插入进入 lambda，创建 `_awaits` 缓存。
- 期望行为: 支持的 API 生命周期中不读取未初始化配置；订单缓存和缓存内对象在实例释放时全部归还。
- 实际行为: 构造函数没有初始化 `_mocker_id` 等配置成员；但正常编号用例在 `init` 后调用，且 `TraderAdapter::initExt` 会先调用 `init(NULL)`，该入口问题单列 T6-008，当前证据不支持把“构造后立即编号”判为已确认的生产缺陷。资源方面，`orderInsert` 在 L306-309 创建 `_awaits` 并通过 `add(..., true)` retain 订单；析构 L98-108 释放 `_orders/_trades/_ticks`，未释放 `_awaits`，故缓存对象及仍在缓存中的订单引用不会在实例销毁时归还。
- 证据边界: `WTSHashMap::release()` 会清空映射并释放其对象（`WTSCollection.hpp:728-745`）；对缺失释放的判断由所有权实现与析构路径确认。当前 T6 用例未执行订单 lambda，也没有运行时泄漏测量。
- 影响范围: 删除含有尚在等待撮合的订单缓存的 TraderMocker 实例时，缓存及其持有的订单对象可能泄漏；已撮合并从缓存移除的订单不因本项被重复保留。
- 备注: 修复方向：由析构或幂等 `release()` 唯一负责 `_awaits->release()` 并置空；补用例创建一个未撮合订单后销毁实例，验证缓存对象和订单引用释放。若保留未 init 编号行为，应先为成员增加确定的默认值并把该调用状态纳入 API 生命周期契约。

## BUG-T6-010

- 状态: 确认的连接资源收尾缺陷（静态生命周期链；本轮未运行连接路径）
- 严重级别: Major（释放活动 API 时线程未 join，可能终止宿主或遗留异步访问）
- 模块/文件: `src/ParserUDP/ParserUDP.cpp` L94-96（空析构）、L268-281（`disconnect`）、L254-266（`connect`）
- 相关测试: `t6_parserudp_state.release_and_disconnect_without_connect_are_safe` 仅在无 socket/线程时调用；`t6_parserudp_spi.handle_event_is_not_emitted_without_io_context` 只验证尚未驱动 strand 时事件延迟。连接用例 `DISABLED_t6_parserudp_net.connect_binds_ports_and_starts_io_thread` 未运行。
- 输入/前置条件: `connect()` 成功后，调用 `disconnect()` 或宿主退出时调用 `release()`/删除 ParserUDP。
- 期望行为: 关闭/取消两路 socket，停止事件循环并 join 线程，待挂起 handler 完成后再销毁对象；重复 release/disconnect 安全。
- 实际行为: `connect()` 启动 `io_context::run` 线程；`subscribe()/do_send()` 可在 `_s_socket` 上挂接收和发送操作。`disconnect()` 只关闭 `_b_socket`、设置 `_stopped` 并 post 一个捕获 `this` 的回调，不关闭订阅 socket、不停止 io_context，也不 join 线程。`release()` 和析构函数为空。`StdThreadPtr` 是 `shared_ptr<std::thread>`，销毁仍 joinable 的线程对象会触发 `std::terminate`；未完成的异步操作还持有原始 `this`/队列缓冲引用。`_send_queue` 本身是值成员，会随对象自动析构，不能把它表述为独立堆泄漏。
- 证据边界: 活动连接的未收尾路径由源码及 `IParserApi::release`“用于退出时”契约确认；`ParserAdapter::release()` 依次调用 API `release()` 再删 API（`WtCore/ParserAdapter.cpp:214-225`），没有替插件调用 disconnect。当前 T6 状态测试不覆盖该生命周期；本轮未动态复现线程终止或 use-after-free。
- 影响范围: 活动解析器经 adapter 正常退出/热卸载时，线程和异步操作可能越过 API 销毁边界；仅在未 connect 的实例上调用 release 的测试不能排除该问题。
- 备注: 修复方向：实现幂等 stop/release，关闭两路 socket、取消挂起 I/O、停止 io_context 并从非工作线程 join，再释放对象；析构作为最终保护。用可注入 socket/执行器测试 connect→disconnect→release 顺序，以及 pending receive/send 在释放时的完成行为。

## BUG-T6-011（测试性/架构改进项，非现网故障）

- 状态: 已撤回（测试性/架构改进项保留备查，不作为独立产品缺陷）
- 严重级别: 不作产品缺陷定级
- 模块/文件: `src/TraderMocker/TraderMocker.h` L31-41/L112-116、`src/ParserUDP/ParserUDP.h` L48-62
- 相关测试: 上述全部 `DISABLED_` 用例
- 输入/前置条件: 本轮门禁（不起线程、不 sleep、不绑真实端口、禁止 `#define private public`）
- 期望行为: 撮合/解析这类纯逻辑应当可以在无网络、无 io_context 的条件下被驱动
- 实际行为: 撮合、缓存/落盘、UDP 收包解析等函数及状态为 private，现有公开接口没有纯内存报文或手动步进 io_context 的接缝。因测试门禁不允许线程、sleep 和真实端口，本轮选择用禁用用例记录不可安全直达的路径。此为覆盖能力/设计限制，不意味着私有函数本身是产品故障，也不证明“完全无法测试”；可以通过抽取纯解析/撮合组件或依赖注入实现隔离验证。
- 证据边界: `DISABLED_t6_parserudp_net.extract_buffer_needs_a_seam_before_it_can_be_fed` 仅为 `SUCCEED()` 占位；private 访问在当前测试结构下不可直调是代码事实，但“阻断覆盖率目标”属于迭代验收判断。本轮未改测试或接缝。
- 附带风险（单独待验证）: `TraderMocker::orderInsert` lambda 在 entrust 无 contract 时调用 `_bd_mgr->getContract`（L149-151），且之后不校验 `ct->getCommInfo()` 返回值（L170-173）。若通过 `registerSpi` 得到空 base-data manager，或 contract 没有关联 commodity，存在空指针路径；正常 adapter/基准数据契约是否允许这些状态尚未核清。启用用例提交 lambda 但不运行；其 sink 的 `getBaseDataMgr()` 返回 NULL，不能作为成功安全执行的证明。
- 影响范围: 这些边界使撮合、委托状态推进和 UDP 报文类型/长度分支难以用当前禁止副作用的门禁验证；其余两处空指针候选的产品影响待契约确认。
- 备注: 建议把纯撮合/解析提取为可传入 tick/frame 的函数，或引入仅测试可用的友元访问器与可注入 io_context/socket；将真实线程/端口生命周期留给受控集成测试。对附带空指针候选，先确认 BDM/commodity 必填契约，再选择安全拒绝或补前置断言及错误回报用例。
