# 10_data_services 静态调用链记录

本目录只包含静态核对记录。本组没有运行 GTest、崩溃探针、ASan/TSan 或构建；父代理的 39 个 CoverageTests target 刷新 PASS 不作为本组动态证据。

## 生命周期与正常调用

- `UDPCaster::~UDPCaster()` 为空；`UDPCaster::stop()` 只有实现和测试中的直接调用，没有 QuoteFactory/runner 的正常调用者。QuoteFactory 创建 `UDPCaster` 时以 `&g_baseDataMgr`、`&g_dataMgr` 装配，见 `src/QuoteFactory/main.cpp:181-184`。因此 null manager 和二次 stop 测试都不能被描述成受支持装配/自然生命周期。
- `ParserAdapter::~ParserAdapter()` 为空。`ParserAdapterMgr::release()` 单次遍历、调用每个 adapter 的 `release()` 后清空 map。WtDtRunner 的扩展 parser 每次新建 adapter 后调用一次 `initExt()`，见 `src/WtDtPorter/WtDtRunner.cpp:349-357`；没有同一实例被生产路径重复注入的证据。WtRtRunner 的 `initExt` 调用使用的是 `WtCore::ParserAdapter`，不是本组的 `WtDtCore::ParserAdapter`。
- QuoteFactory 在单次 `initialize()` 中至多初始化一次 StateMonitor，并且全天候模式只把 `StateMonitor*` 置空；DataManager 的 `bdMgr` 始终传真实基础数据管理器。IndexWorkerMgr 在工作器创建/初始化前保存其依赖指针；正常 quote 回调经 ParserAdapter 到 DataManager 后才调用 IndexWorkerMgr。
- WtRtRunner 的 `initEngine()` 根据一个 `env.name` 只初始化 CTA、SEL、HFT 中的一个引擎。对应 context factory 使用各自类型引擎；本次没有找到单一 runner 混用未选中 engine context 的公开使用样例或测试契约。

## 外部数据和接口边界

- `ShmCaster` 文档称队列用于进程间安全传输；生产者对 `volatile uint64_t` 使用普通 `++` 和普通读写发布，ParserShm 消费者读取同一字段后直接读取槽内容。DataManager 广播路径没有在 caster 边界串行化；WtDataWriter 的同步模式会从调用线程直接调用 `procTick()` 并广播。现有 active 测试只做单进程单次写入后的字节/字段检查，没有验证并发生产或生产者到消费者的发布顺序。
- `UDPCaster` 收包只按完整 1024 字节检查长度，订阅字段固定为 1020 字节。内置 ParserUDP 发送端清零包缓冲，但接收端没有验证字段内 NUL；网络来源不只限于内置 sender。
- `StateMonitor` 的交易区间来自基础数据的 `addTradingSection()`；`WTSSessionInfo::offsetTime()` 明确处理 0 点左对齐及 24 点右对齐，`StateInfo::isInSections()` 采用半开区间 `[from,end)`。候选用例只断言初始化和 ORIGINAL 状态，没有观察构造后的 section 数值。
- `IBtDataLoader::loadFinalHisBars()` 公开接口只以 bool 表示成功，不规定成功时必须返回正数；`HisDataReplayer` 在成功后未检查 vector 是否为空就读取首尾元素。
- 回测 daily synthetic tick 路径会从外部 bar 代码解析 commodity/session。公开 `sub_tick()` 只记录标准代码，没有先确认基础数据登记；未知代码与自定义 loader/CSV 日线组合可到达未判空解引用。
- `WtDtHelper` 的导出函数返回数据条数，日志 callback 是可选参数，声明位于 `extern "C"` 区域。`StdFile::read_file_content()` 对缺失文件抛异常；读取 helper 在长度/头部校验之前调用它，没有把异常转成 API 错误结果。短文件才会进入头部长度分支。该差异已记录在 triage；推荐的 0+read-error 是修复合同，不是当前行为。

## 搜索结果约束

- `rg -n 'UDPCaster|\.stop\(\)|->stop\(\)' src/WtDtCore src/QuoteFactory src/WtPorter`：生产文件只见 UDPCaster 定义与 QuoteFactory 全局装配；`stop()` 没有生产调用点。
- `rg -n 'ParserAdapter::release|initExt\(' src/WtDtCore src/QuoteFactory src/WtPorter src/WtDtPorter`：WtDtCore `initExt()` 的 runner 调用是 WtDtRunner 为新 adapter 的单次装配；重复 initExt 候选来自测试复用对象。
- `rg -n 'StatHelper' src --glob '!TestUnits/**'`：生产源码未发现 StatHelper 消费者；相关命中限于 header/project filter 与测试。
- `rg -n '_inited|_mq_sid|initEvtNotifier|config_backtest' src/WtBtPorter`：`config_backtest` 的局部 guard 与 `WtBtRunner::_inited` 没有成功置位；`EventNotifier` 析构只销毁当前 `_mq_sid`。
- `rg -n 'loadFactories\(const char\* path\)|directory_iterator iter' src`：多个插件 factory 使用相同的存在检查后构造 `directory_iterator` 形态；这说明是同类错误处理模式，不能反推该方法的 bool 返回值已涵盖 filesystem exception。

