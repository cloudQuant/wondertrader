# 10_data_services 第二轮复核

本组按输入快照逐条复核了 20 项，重新查看实现、公开接口、正常装配/回调路径和候选测试。只做静态核对；本组没有运行测试或构建。全部条目为 `STATIC`，父代理刷新的 CoverageTests target 结果不替代本组用例验收。

## 分类统计

| 决定 | 数量 | 编号 |
|---|---:|---|
| PRODUCT | 10 | BUG-R18-011、BUG-R44-005、BUG-R54-001、BUG-R5-003、BUG-R5-009、BUG-R66-001、BUG-R67-001、BUG-R67-002、BUG-L05-001、BUG-L05-007 |
| TEST | 3 | BUG-R154-001、BUG-R44-008、BUG-Q05-004 |
| NOT_BUG | 7 | BUG-R154-003、BUG-R18-004、BUG-R44-001、BUG-R44-003、BUG-R44-007、BUG-R82-001、BUG-L05-004 |
| 合计 | 20 | |

## 逐项结论

### BUG-R154-001 — TEST / STATIC

`UDPCaster::do_receive()` 的订阅分支确实会解引用基础数据管理器和数据管理器，但候选通过 `init(cfg, nullptr, nullptr)` 人工省略依赖。QuoteFactory 正常装配始终传入 `&g_baseDataMgr` 与 `&g_dataMgr`。当前没有公开 nullable 合同或广播-only 的有效依赖省略路径，因此该 null 依赖测试不构成产品复现。保留禁用状态，改正其“有效产品配置”注释。

### BUG-R154-003 — NOT_BUG / STATIC

二次 `stop()` 对 non-joinable thread 再 `join()` 可能抛 `std::system_error`；但析构函数为空，产品调用链没有自然的第二次 `stop()`，候选测试是手动连续调用两次。当前没有任意重复 stop 幂等合同，不据此要求产品修改。

### BUG-R18-004 — NOT_BUG / STATIC

直接重复调用 `ParserAdapter::release()` 存在 double-delete 风险，但这不是自然析构路径：adapter 析构为空，manager 的 `release()` 遍历一次后清空 map。候选 `DISABLED_DoubleReleaseDoubleDelete` 的测试注释和标题实际对应 BUG-R18-003，不是 R18-004；重复 `initExt` 是另一个独立场景。没有当前支持的二次 release 生命周期。

### BUG-R18-011 — PRODUCT / STATIC

`ShmCaster` 注释声称跨进程安全传递，但 `_writable`、`_readable` 是 `volatile uint64_t`，所有四种 payload 路径都以非原子 `++` 分配槽位，再普通写数据、普通写 `_readable`。DataManager 广播路径没有给 caster 加串行化；WtDataWriter 的同步模式从调用线程直接广播。ParserShm 读取 `_readable` 后直接读取槽，未与生产者建立 acquire/release 发布关系。现有 active 测试仅验证单次写后队列内容，没有验证并发生产者或发布可见性。

修复应定义真正可跨进程的队列协议：写槽分配和占用要同步，payload 完整写入后才发布给消费者；若采用单写者协议，必须在装配/调用边界实际串行化。验收使用独立 producer/consumer 校验唯一序号、数据类型、内容和容量回绕，另保留单生产者正常对照。

### BUG-R44-001 — NOT_BUG / STATIC

重复 `StateMonitor::initialize()` 会把新状态写入 `_map`，旧 session 不一定清掉，但 QuoteFactory 只在一次启动初始化中构造状态表，没有热加载或同一实例重启的调用链。当前不定义替换/合并/拒绝的重复初始化语义，不把禁用的人工重入用例列为产品故障。

### BUG-R44-003 — NOT_BUG / STATIC

`ParserAdapter::initExt()` 覆盖 `_parser_api` 的确会让人工复用同一 adapter 时失去旧 API 的释放路径。WtDtRunner 的实际扩展 parser 路径每次先创建新 adapter，再调用一次 `initExt()` 并登记该 adapter；没有同一 adapter 被生产路径重复注入的证据。候选测试已跳过等待所有权/替换合同，当前只要求新 adapter 单次注入和释放。

### BUG-R44-005 — PRODUCT / STATIC

`IndexWorkerMgr::loadFactories()` 只判断路径存在，随后直接构造 `directory_iterator`。普通文件、权限变化或遍历失败会抛出异常而不是返回 `false`；该方法从 QuoteFactory 启动路径加载插件目录，并以 `bool` 和错误日志表达加载成功/失败。仓库的其他 factory loader 也有相同的异常模式，这说明错误处理可能是共性技术债，不能证明异常是此 API 已承诺的错误返回。

建议检查目录类型并使用 `error_code` 迭代，将失效路径转换为日志加 `false`，让异常不越过初始化边界。候选禁用测试应在实现后断言普通文件返回 `false` 不抛，同时保留空目录和合法插件目录对照。

### BUG-R44-007 — NOT_BUG / STATIC

fresh `IndexWorkerMgr::push_tick()` 会访问未初始化的 `_data_mgr`，但这是测试直接对未初始化内部 manager 发起调用。正常 `init()` 在构造 worker 前先注入 `_data_mgr`，随后 worker 初始化和 ParserAdapter 的已验证 quote 路径才可能调用它。当前生命周期不支持在 `init()` 前把 `IIndexContext` 当作可用对象调用。

### BUG-R44-008 — TEST / STATIC

`DataManager::getSessionComms()` 等确实会访问 `_bd_mgr`，但候选测试把全天候模式对 `StateMonitor*` 的可空性外推成 `bdMgr` 可空；QuoteFactory 全天候初始化仍传有效 `&g_baseDataMgr`。该测试还在 `init()` 返回失败后继续查询。修正注释即可，不要求产品代码接受空基础数据管理器。

### BUG-R54-001 — PRODUCT / STATIC

TraderHTS 的真实查询路径先按 code/exchange 找到 `WTSContractInfo`，再创建 `WTSTradeInfo` 并回调 `onRspTrades()`，但没有 `setContractInfo(contract)`。`TraderAdapter::onRspTrades()` 只读 `getContractInfo()`，为空时直接 `continue`，已知成交因此不进入统计和后续风险判断。这里的缺失形态来自仓库内置 provider，不是只由测试伪造。

修复可在 HTS 创建回报时附上已查到的 contract，或在适配器按 code/exchange 回退查询；未知合约应显式诊断而非静默丢弃。保留并在产品修复后启用现有 HTS 形态统计/风险用例。

### BUG-R5-003 — PRODUCT / STATIC

接收端仅确认 UDP 长度等于固定包尺寸，随后将 1020 字节数组隐式作为 C 字符串交给 `StrUtil::split()`；接收端没有验证字段内 NUL。内置 ParserUDP 发送端把请求包清零，因此正常报文有终止符，但这不约束外部网络发送方。无终止符的定长字段可能让字符串扫描越过数组/当前报文边界。候选引用的 R154 用例只测 null manager；正常订阅的有效回环路径应以 R164 用例为准。

使用字段内有界查找 NUL；未终止或格式无效的请求在边界内拒绝并记录错误。测试在修复后增加满字段无 NUL 报文，保留零填充正常包作对照。

### BUG-R5-009 — PRODUCT / STATIC

`StateMonitor` 将起始时间换算成 `uint32_t` 分钟后执行 `stime--`；有效 00:00 起点会从 0 回绕到 `UINT32_MAX`，接着被当作 HHMM 转换。`WTSSessionInfo::offsetTime()` 对午夜左对齐和 24:00 右对齐有明确实现，区间查询也定义为 `[from,end)`，所以午夜是有效时间边界。

改用有符号或带日期的分钟边界计算，保留 00:00/24:00 语义。现用例只断言初始化和 `SS_ORIGINAL`，看不到 `_sections` 错值；需要可观察区间 seam 或边界时间状态测试，覆盖 0000、跨午夜及 2359→2400。

### BUG-R66-001 — PRODUCT / STATIC

`config_backtest()` 的头文件/实现注释承诺只配置一次、重复调用忽略，但局部 `inited` 从未置为 true；`WtBtRunner::_inited` 也只初始化和检查，没有成功置位。因此公开重复配置会再次进入 replayer 配置和 notifier 初始化。`EventNotifier::init()` 成功时创建 MQ server 并覆盖保存的 `_mq_sid`，析构只销毁当前保存的 server，前一次创建失去清理句柄。

应修复 `config_backtest`/runner 的一次性配置状态，失败时仍保留可纠正后重试的语义；无需把底层 `EventNotifier::init()` 扩展成任意重入幂等。当前 R66 测试包含的是 `WtCore::EventNotifier` `_worker` 路径；WtBtCore 的 R20 测试禁用了会启动真实 MQ server 的 active 分支，两者都不是重复 backtest config 证据。

### BUG-R67-001 — PRODUCT / STATIC

公开 `IBtDataLoader::loadFinalHisBars()` 文档只用 bool 表示加载是否成功，没有规定成功必须回调正数行。`cacheFinalBarsFromLoader()` 在 true 返回后无条件读取 `_bars[0]` 和 `_bars[_count-1]`；成功但空结果会触发越界。修复应在读取首尾前检查 count 与 vector size，并明确空结果的缓存/返回语义。验收覆盖 false+0、true+0、true+N，且任何返回路径都保持 count 与 vector.size() 一致。

### BUG-R67-002 — PRODUCT / STATIC

`sub_tick()` 接收标准代码后直接登记，没有检查该代码是否有基础商品/session。未订阅 daily bars 可能通过自定义 loader 或 CSV 进入合成 tick 路径；该路径对 `getCommodity()` 与 `get_session_info()` 的结果直接解引用。未知代码是公开订阅/外部数据边界，修复应在缓存或合成 tick 前校验 commodity/session，记录诊断并跳过，保留已登记商品的原有行为。

### BUG-R82-001 — NOT_BUG / STATIC

runner 的 `initEngine()` 根据单个 `env.name` 只初始化 CTA、SEL、HFT 中的一个；对应 context factory 明确以各自的 engine 创建 context。把 `env=cta` 与 `create_hft_context()` 混用会把未初始化 engine 交给 context，但没有证据表明单一 runner 的当前支持合同允许跨环境混用，也没有有效生产用例证明此路径。当前不将错误环境匹配当成产品缺陷；文档可进一步说明 context API 必须与配置环境匹配。

### BUG-L05-001 — PRODUCT / STATIC

`trans_csv_bars()` 将外部 CSV `date` 列传入 `strToDate()`；斜杠/连字符三段格式和纯数字格式都有有效样例，但解析器对任何多于一段的字符串都无条件访问 `ay[2]`。两段/空段输入会越界。应检查日期段数和年月日范围；有效日期保持既有转换，畸形行安全跳过并写出文件/行诊断。

### BUG-L05-004 — NOT_BUG / STATIC

`StatHelper.hpp` 有独立包含依赖/未使用 flag 等可维护性问题，但生产源码没有 include 或调用该 helper，现有命中限于 header/project filter 和测试。当前没有可达生产行为来支持将其列为 data-service bug；若未来要支持它，另开 API 完整性任务。

### BUG-L05-007 — PRODUCT / STATIC

读取 helper 的公开头文件只说明返回读取数量、日志 callback 可选；它们在长度校验前调用 `StdFile::read_file_content()`，而此函数对不存在文件抛异常。因而 missing path 不会进入 header-invalid 分支，也没有在导出 read helper 边界捕获异常。建议将该外部文件失败转换为稳定的 `0 + read-error`，并让异常不越过 C/插件调用边界；短文件仍独立返回 0 并记录 header-invalid。候选的三处测试把 missing path 误写成 header failure，应保留 short-file 对照并拆分缺失文件断言。此项修复合同推荐为零计数/日志，当前源码行为仍是抛出。

### BUG-Q05-004 — TEST / STATIC

`resample_bars()` 的 `cb`/`cbCnt` 没有 optional/default 语义，公开注释说明它们分别输出数据和计数，只有 `cbLogger` 标为可选；直接传 null 违反依赖前置。`extractKlineData()` 对空 slice 返回 null，调用方会先早退；有效非空 slice 与 `bIncludeOpen=true` 的提取路径构造非空结果，未发现 `newCnt==0` 的可达合法输入。当前候选的空回调/零输出主张无产品修复依据。

## 测试更正与执行边界

已向主代理发送需要审核的测试更正点：R154 null manager 与 R44 bdMgr 空指针前置；R18 测试编号错配；R44 loadFactories 普通文件错误路径；R54 HTS 成交构造形态；R5 UDP 无 NUL 用例归属；R5 状态区间可观察断言；R66 错测 `WtCore` 实现；R67 loader/未知代码边界；L05 日期及缺文件/short-file 区分。所有实现工作由主代理另行审核和分派，本组没有改测试或产品源码。

本组未运行任何测试，未运行 sanitizer，也未构建。静态调用链核对细节见 [static-callsite-audit.md](../evidence/10_data_services/static-callsite-audit.md)。
