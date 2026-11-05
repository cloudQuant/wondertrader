# 第五会话子任务回报要点（主线程整合待复核清单）

本文件只摘录子任务自述，**未经主线程复核不作为通过证据**。复核方式：全部包完成后在统一快照上默认 -Werror 构建，逐包复跑定向用例与红控。

## traders-other（20:4x 完成；证据 `../20261104-s5-traders-other/`；夹具 `src/TestUnits/fixtures/i03_traders_other_20261104s5/`）

- 建议：T4-002 本地验证通过（完整目标原定向 7/7，Femas lane 73/73+3 disabled）；T6-009 本地验证通过（完整目标 10/10，含 t6_init；DestructorAloneReturnsPendingOrderRefs 改前 9/10 为测试竞态，加 queryAccount 屏障、断言值不变；Mocker lane 75/75）；T4-003 本地验证通过（统一停止协议，红 9/9 失败→绿 9/9，×10 90/90，TSan/ASan+UBSan+LSan 各 17 进程）；R60-001 本地验证通过（红 3/4→4/4，TSan 0；异步 connect 后立即 release 一例无红控）；R60-003 本地验证通过（11/11，TSan 11/11）；T4-015 阻塞（代码在 `src/CTPOptLoader/TraderSpi.cpp:497-501` 收末包即 exit(0)，需决策卡 A）；R93-005 阻塞 X（审查列 5 缺口）；R93-006 阻塞（逐单冻结来源与比较方向修复，3 场景红→绿；部分成交/首包终态/乱序需 YD 语义）；R60-006 Z 建议用户指示忽略（决策卡 C2）；I03-NEW-005 阻塞（决策卡 B）。
- 文件改后哈希（主线程 20:5x 核对一致）：TraderFemas.cpp ac8c64feff1c、.h 420fa31725ee；TraderMocker.cpp 98c5ab245405、.h 37b803bb5f75；TraderYD.cpp b701a12bbd48、.h 136e45ac9360；femas_trader_test a76b82bbf389；t6_009 ddf5b46b2a68；r93_traderYD_api_test c3b25fa1baee；新 t4_003 b7e699aa2850、r60_001 183ef3c29315；i03_leaks_main manual_build.sh 0b74f9164dc2。
- 新发现：(1) TraderYD release 不等 notifyAfterApiDestroy、查询 worker 不 join；(2) TraderYD notifyTrade 平仓比较方向写反致今仓不扣减；(3) TraderXTP 回调 `_sink` 未判空、回调线程 deleteTrader terminate；(4) r214_mocker_branch_test.cpp:59 测试侧泄漏 1280 B；(5) 仓库根 default.profraw 19:51 份来源为其他包。
- 决策卡：A loader 异步完成交付（建议 A1 去异步）；B ITraderApi 版本化“提交不确定”结果；C isConnected 语义（建议 C2 忽略）。

## traders-ctp（20:5x 完成；证据 `../20261104-s5-traders-ctp/`）

- 建议：T25-005 本地验证通过（disconnect 2 s 有界、join worker、Release 恰一次；红 98/98/98/1/1）；T1-CTPOpt-02 本地验证通过（无锁在途 lease；红 1/1/98）；T1-CTPOpt-01 本地验证通过（析构归还查询数组）；T2-CTP-02 本地验证通过（仅 macOS；connect 失败报 WTE_Connect -1）；T3-006 本地验证通过；T3-012 本地验证通过（红 134/134/1/98）；T2-CTP-08 本地验证通过（进程内原子 OrderRef 分配，三 provider；跨进程/重启未证）；T3-015 待验证（无效交易日 fail-closed 登录、无日期回报丢弃；人工恢复策略待确认）；T2-CTP-03 Z→按 9c 修复（实例单次使用，release 后拒绝重连），建议不忽略；T2-CTP-07 阻塞（决策 D-T2-CTP-07，建议 B 映射 WOS_Submitting；红控为 disabled 测试）；T6-002 阻塞（CTPLoader 异步模式回调线程 11 处 exit()；决策 D-T6-002 建议 B）。
- 产品：新 `src/TraderCTP/VendorTdLifecycle.hpp`；deleteTrader 对曾创建 SDK 对象的实例保留到进程退出并 pin 模块；Dumper 单例永不销毁。
- 改写旧测试：t25_traderctp_gap 两例、r61_traderctpopt_api 三例、cov_traderctpopt_spi 一例、r61_traderctpmini_api 两例、ctpmini_trader_callbacks 夹具日期（卡9/卡6d）。
- 运行（混合依赖重链，非干净整合树）：TraderCTP 80 例 55 过（25 例改前同败）、CTPOpt 52/52、CTPMini 49/49、Dumper 33/33；无 sanitizer 运行。
- 新发现：N-1 CTPMini/CTPOpt 把 CTP NotTouched('b') 映射为 WOS_Cancelling；N-2 TraderCTP lane 25 例既有失败（登录夹具不符 11-03 CTP 身份规则）；N-3 Dumper 全局 g_adapterMgr 退出期析构与厂商线程回调竞态；N-4 WtCore/WtUftCore/TraderDumper 的 TraderAdapter 宿主在 deleteTrader 后无条件卸载模块，忽略“实例保留”信号。

## storage-read（21:0x 完成；证据 `../20261104-s5-storage-read/`，决策卡 `决策卡草稿.md`）

- 建议：L04-004 待验证（键有界复制+invalid_argument+static_assert 52/56 布局；三个 AD 读端 10 入口在副作用前拒绝；写端子项交 dt-writer）；R100-003/R32-003 阻塞（读端 2033 起明确拒绝、64 位计算；改前 2033 cutoff 返回无关 bar 红控；待 S5-A，若选维持 V1+2032 上限 fail-closed 可转通过）；P05-010 阻塞 X；R122-002 阻塞（有界暂存/两阶段/损坏零提交、只读目录缺库 NULL+ERROR；有界内存为静态分析；待 S5-B）；R21-BUG-07 阻塞（无新提交复用快照不重扫；待 S5-C）；R100-004 阻塞（S5-D）；R132-001 实施中（S5-E 预算；7 处打包缓冲未对齐 UB 已用 UBSan alignment 复现于 R162，因 date-session 并行改 WtRdmDtReader/WtDataReader 未修）；R123-003 待验证（产品在 WtDtServo；i03_slice_servo ASan/UBSan/LSan 5/5；需补 runner 停止后析构无 parser 回调访问的计数 fake）；NEW-095 本地验证通过（默认 -Werror btcore；R180 三例、R210 两例各 1/1；关联 16 例 15 执行 14 过 1 失败为 NEW-096）。
- Z：NEW-032/082 同一用例（整块拒绝 vs 期望保留前缀），建议用户指示忽略或经用户确认后改断言；NEW-059 现通过但断言曾在 ccaad010 无合同改写，各读端区间端点语义不一致，建议忽略并报告；NEW-060 当前 1/1 通过、无公开合同，建议忽略不计通过；NEW-096 判定为测试期望问题（在用序列每日 mark 清零），需用户确认“在用序列不淘汰”。
- 旧断言改写（无裁定卡依据，主线程认可）：(a) `fixtures/l04_lmdb_key_bounds/key_bounds.cpp`（本迭代未跟踪夹具）去掉“构造器拒空字段”两处，改为构造器接受空字段、`lmdbKeyIdentityValid` 入口拒空身份——HEAD 的 l04 lane `ExchgPrefixDoesNotCollideAcrossContracts` 依赖接受空字段，原夹具断言无合同依据；(b) `r21_rdmbars_kline_test.cpp` `RepeatKlineReadReloadsWholeRangeBecauseLastBarTimeIsNeverUpdated` 2→1：该例刻画的正是 R21-BUG-07 缺陷，按原验收条件“同窗口复读不扫全量”改写；另 `p05_rdmdtreader_ad_slices_test` 一例只在 setup 加一次同内容提交强制重扫，断言不变。
- 文件改后哈希：LMDBKeys.h 3d7127b66af6；WtDataReaderAD.cpp ecce89cb3e2b/.h 26aff1e22e8b；WtRdmDtReaderAD.cpp 356bed19e744/.h 12609305d06a；WtBtDtReaderAD.cpp 8939131b3ee7；r21 test 4291b5c9c4e2；p05_rdmdtreader_ad_slices 8bea1f9f186b；key_bounds.cpp a0b4c1a5ba1e；新 s5_reader_ad_bounds ec5b6b11a3ad、s5_rdm_ad_snapshot 4c896a510c6c。
- 运行：s5_ad_read LLVM22 ASan+UBSan(含 alignment)+LSan 29/29 ×25，改前 7/25 失败；l04 lane 17/17；p05 lane 夹具 ASan 289 例 273 过 0 失败 16 disabled（LSan exit 1 来自 R162/R172/R182/R203 既有测试夹具泄漏）；in-tree CoverageTests_adstorage 290 例 273 过 1 失败（既有写端 R122WriterADUpdateTickCacheUninitialized）16 disabled。
- 新发现 → I03-NEW-038~042。
- 决策卡：S5-A V2 分钟键（建议维持 V1+2032 上限 fail-closed）；S5-B HOT/custom 换约来源（先 fail-closed 再物理分段）；S5-C 增量游标与修订可见性（游标+有界陈旧）；S5-D RT 末槽可见（按卡13 统一可见）；S5-E CMP_V2 解压预算（绝对上限+压缩比上限可配置）。
- 待追加：date-session 完成后，storage-read 继续修 R132-001 七处未对齐、I03-NEW-038 的非 AD 读端、复核 I03-NEW-041。

## dt-parsers（21:2x 完成；证据 `../20261104-s5-dt-parsers/`，决策 `decision-drafts.md`）

- 建议：R18-006 本地验证通过（V2 表+preflight 后才创建、失败经 V2 回滚、DRAINED 才销毁并归还模块与 cfg；夹具 10/10、旧码 7/7 判红含 UAF、TSan×3；R18/R44 33 例 0 失败）；R18-012 本地验证通过（cfg 引用回基线；R18 两条“失败后保留到析构”旧断言按卡9 改写）；R8-006 本地验证通过（servo 夹具 9/9、旧码 7/7 红、TSan×3；R45 三条 remover 用例改写为 V2 等价）；NEW-009 本地验证通过；R123-003 本地验证通过（新计数 fake：回调阻塞时 delete runner，旧码 abort 134、新码绿；reader 不卸载为静态核对）；R8-003 阻塞（完整 CoverageTests_dtservo 125 例 0 失败 5 disabled exit 0，二进制复制到模块目录运行；无 store 合同冲突未决）；R5-007 待验证（WtDtPorter 新 shutdown 顺序；StateMonitor TSan 15/15；writer 回调模式探针因 dt-writer 改 DataManager 未跑）；R22-002 待验证（TSan 15/15；self-join/异常模式未跑）；T1-XTP-02 本地验证通过（XTP 9/9、V2 11/11，旧码 6/9 红；t_ParserXTP 76/76；disconnect 为终止性关闭，重连用新实例）；T27-004 本地验证通过（既有两例 SIGSEGV 红控）；T6-010 本地验证通过（t_ParserUDP 46/46，UDP V2 8/8、legacy 20/20）；R68-003 待验证（IndexWorkerMgr 先排空再经工厂删除、不调 remover；WtBlkIdxWorker 析构 join，旧码 terminate 134；R68 lane 27/27、idxfactory 14/14；remover ABI 待 D4）；NEW-029 阻塞（产品在 HisDataReplayer.cpp，补丁草稿在 patches/，转 btcore）；R104-002 Z 缺陷部分已修（active parser init 失败不注册/计数/connect，R104 旧断言改写；m_QuoteFactory 24/24；“失败是否终止启动”D1 未裁定则该子项忽略）；NEW-026 Z 本地验证通过（只改夹具：pricetick/volscale、holiday、PAUSE 段 0-0→2358-2359；R145 11/11，周末分支未覆盖）；NEW-116 Z 本地验证通过（依调用者合同 exchg.rawcode，t_ParserShm 23/23）；I03-NEW-003 阻塞（runWithResult；模块登记候选与卡9 vendor V2 夹具冲突已撤回，待 D6/D2）；I03-NEW-012 本地验证通过（测试缺陷：sport=0 发往 127.0.0.1:0 EADDRNOTAVAIL；300 次失败 101→0）。
- 产品：新 `src/WtDtCore/ParserLifecycleHostV2.hpp` 7f6e80d07dce；WtDtCore/ParserAdapter.h 49c827df765c/.cpp 6c7c95cc942d；WtDtServo/ParserAdapter.cpp 01791009317a；WtDtPorter/WtDtRunner.cpp b9f19ccba906；另 IndexWorkerMgr、WtBlkIdxWorker、WtDtServo 其余、WtDtPorter、QuoteFactory/main.cpp（见其 hashes）；ParserCTPMini.cpp 改后撤回，fcb141e36ecd。
- 运行：默认 -Werror 覆盖率 9 目标 exit 0；唯一不通过 R5StateMonitorBdtTest 13/14 SetUp 失败（夹具漂移，I03-NEW-043）。未跑 ASan。
- 新发现 → I03-NEW-043~046。
- 决策：D1 active parser 失败是否终止启动；D2 部分连接命令提交失败是否回滚；D3 initExt 外部 API 释放归属（与 I03-NEW-027 合并）；D4 指数工厂 remover ABI；D5 NEW-029 归属（主线程分配给 btcore，非用户决策）；D6 某实例失败后同模块其他实例能否再加载。

## dt-writer（21:3x 完成；证据 `../20261104-s5-dt-writer/`）

- 环境：其改前快照与构建 `/tmp/wt-i03-s5-dt-writer-before` 约 18:2x 被外部整目录删除（磁盘 96%→82%，非本任务所为），全树哈希清单丢失；改前覆盖率日志/XML 已先行归档；后以 `before/` 原件重建对照树 `/tmp/wt-i03-s5-dt-writer-base2`。R23 写端 17 组只有改后结果。
- 建议：NEW-038 本地验证通过（回调内 release 只排队，活动写调用返回后才释放/删除/卸载；改前 SEGV 139、abort 134；normal/ASan+UBSan/TSan 通过；旧 ABI 第三方 writer fail-closed 隔离）；R18-002 本地验证通过（宿主侧：仅 V1 DRAINED 才删除卸载，R18 原 5/5）；R23-001 本地验证通过（停止期并发写入排空不取消、重复 init 拒绝、三类线程确认退出后 join、300 s 等待可中断；release_timeout_ms 默认不限时见草稿 A）；P10-008 本地验证通过（三流按块有序、同块重入排队、回调不持块锁、异常逐事件隔离；改前 4 例自旋死锁 142）；P10-007 本地验证通过（六类首插 normal 6/6、TSan 6/6；P10Rt/TaskQueue/DayDsb 41/41）；P10-001 本地验证通过（无链接重编 15/15）；L05-002 建议本地验证通过（strFilter 按公开头注释单一通配；空目录/无匹配返回新状态 NO_INPUT=6；未知版本/标识独立负控；57/57）；P10-006 阻塞（草稿 B；另需 WtDtRunner 关闭顺序）；R18-011 阻塞（卡13b 不覆盖 ShmCaster 队列；草稿 C）；NEW-056 Z 用户指示忽略（仅消除 Q05 空指针 SEGV，断言未改）；NEW-104 Z（三条泄漏已修：3 leaks/224 B、10 leaks/832 B→0；times=1 透传语义无合同，建议忽略该子项）；I03-NEW-007 写端子项通过、整项阻塞（ASan+UBSan 24/24）；I03-NEW-024 已随 NEW-104 修复；I03-NEW-038 写端本地验证通过；L04-004 写端子项本地验证通过（红控 3 例失败→通过）。
- 文件：IDataWriter.h 3af18810c624（只追加 V1 C ABI）；WtDataWriter.h 98a5a77bebc2/.cpp 05a8d8185082；DataManager.h f92611937dfe/.cpp 14c085a84671；WtDtHelper.h f785d2243b85/.cpp e0ea16a4d3e4；WtDataWriterAD.h 610a37905a38/.cpp 09e9d46d370c；r18_writer_fixture.cpp dc7af4ee1c72；新测试 l05_dt_helper_v2_filter_test、q05_resample_leak_paths_test、i03_writer_ad_log_identity_test；夹具 i03_dt_writer_lifecycle_20261104s5。
- 运行：新夹具 normal 15/15、改前 3/15（3 例为护栏）、ASan+UBSan 15/15、TSan 14/15（隔离 writer 线程未 join 的 thread leak 为隔离语义预期，关 thread leak 报告单跑 1/1）；dtcore DataManager 组失败集合与改前一致；R102 3/3；Q05ResampleLeakPaths 4/4；P10ProcBlock 15/15；L05 57/57；R23 写端 82 例 0 失败 2 disabled；R23 300 s 等待夹具改前 exit 5、改后 normal/ASan/TSan 0。
- 行为变化：AD writer 未导出 V1，经 DataManager release 后被隔离到进程退出；caster 广播异常逐个捕获记日志；三流广播解锁后有序投递，可能由另一线程投递。
- 决策：草稿 A release_timeout_ms 默认（建议不限时、超时隔离）；草稿 B P10-006 版本化注册接口+共享所有权+重复 ID 拒绝；草稿 C R18-011 ShmCaster 进程内单生产者、跨进程按卡13b。
- 新发现 → I03-NEW-047~050；R44DataManagerEdgesTest 6 例 SetUp 于 StateMonitor::initialize 失败并入 I03-NEW-043。

## date-session（21:4x 完成；证据 `../20261104-s5-date-session/`，决策 `决策卡草稿.md`）

- 建议：R12-13 实施中（新 `src/Includes/DateMappingClient.hpp`：A 路径具体 manager V1 表、C 路径旧 virtual 调用前后校验，date=0 需先取得校验过的时钟快照；已迁移 4 个读端、Replayer 回放循环与任务回放、ParserXeleSkt、StateMonitor；剩余 ticker（Cta/Hft/Sel/Uft）、DataManager::getTradingDate、TraderCTPMini 夜盘，插件侧取 V1 表待 D4）；L01-018 阻塞（l01_timeutils 24/24，新增 tryGetNextDate 边界矩阵；待 Windows 与 R12-13 剩余调用点）；L03-009 实施中（getOffsetDate 无生产调用方；剩余他包调用点、异步 writer 失败回执、DST、旧 oracle 漂移）；L03-010 阻塞（0–0 与其他段混合两种顺序均拒、整份不发布；D1/D2/D3；重叠拒绝试做后撤回）；L03-002 两旧用例本地通过（storage_width 1/1；l03_commodity_state/init.cpp 改前普通/ASan FAIL→PASS；卡4a），整条待验证（4b 在 engine-lifecycle）；I03-NEW-006 实施中（旧时钟函数不再把哨兵 99 当小时偏移、失败发布非法日期 19000100、getNextDate 纯公历；剩余引擎/TraderAdapter/CTPOpt/writer 调用方，D5）；I03-NEW-011 本地验证通过（改前 UBSan 越界 134、TSan 竞争；改后 5/5、TSan 0；Xele lane 34/34）；I03-NEW-043 R5 本地验证通过（14/14，改前 0/13），R44 SetUp 已修 4/6，剩 2 例为 DataManager 失败初始化后 getBDMgr/canSessionReceive 语义（转 dt-writer）。
- 文件改后：TimeUtils.hpp 91f36dd1ef32；WTSSessionInfo.hpp 0abfa6b168f8；DateMappingClient.hpp c18e24c80aed；WtRdmDtReader.cpp 7cd12942a414；WtDataReader.cpp bfd7e7d5f48a；WtDataReaderAD.cpp ecce89cb3e2b；WtRdmDtReaderAD.cpp 356bed19e744；HisDataReplayer.cpp 4c48babd2702（含 btcore 20:24 改动）；ParserXeleSkt.cpp 1420f4e93164；StateMonitor.cpp 11fee8c2d907/.h 63a2690cacde；测试 l03_contract_info 183e54104fe0、l03_session_info 5118bc1eb2f2、l03_commodity_state/init.cpp 8d7df3e5948d、i03_session_contract check.cpp fd338499e673、r5_state_monitor_bdt d60667a2bae5、r44_datamgr_caster_edges 3be59767f6df；新夹具 i03_date_mapping_20261104s5。
- 运行（覆盖率默认 -Werror 6 目标 exit 0）：l01 24/24；l03 67/68（l03_object.release_is_idempotent_at_zero_refs 未改区域）；p09 读端 32/33（替身 date=0 返 0 按卡1 判失败）；q03 读端 170/171（day_tail_fragment_is_dropped 基线同败）；r23 25/25；AD 读端 165/165、R122 8/8；Xele 34/34；servo 24/24；R5 14/14；R12 13/21、R1 26/40、q05 StateMonitor 其余 26/56（夹具前置 loadSessions/loadCommodities 失败）；btcore 回放组 220/276（prepare/init 失败，25 例 btcore 自身基线同败，其余缺 holiday 一类，静态归因，无去改动对照）。新夹具红绿：tz_legacy 改前 0/2（+99 小时得 20261108）、monitor_chain 1 例失败、reader_chain 3 例失败、xele_chain 134、xele_tsan 134 → 改后全过。2026-11-03 Session 夹具 49/49、日期映射夹具 5/5。
- 影响他包：Xele 生命周期 V2 夹具替身 getCommodity 返回空，当前模块 0/5（补带会话商品后 5/5，`runs/xelev2-impact/suggested-fake.diff`），events.cpp 预计同受影响。
- 新发现 → I03-NEW-051~054。决策：D1 空会话（建议未配置拒绝）；D2 跨零点 autoAdjust 空档（建议保持 INVALID）；D3 普通段重叠（建议拒绝）；D4 插件侧取 V1 表（建议可选导出绑定函数由宿主注入）；D5 时钟失败是否暂停接收行情（建议冻结+告警）。

## traders-ctp 第二轮（22:0x 完成；README “第二轮”节）

- sanitizer：四 lane 新生命周期用例 + T2-CTP-08 身份 15 例，共 51 例×(ASan+UBSan+LSan, TSan) 102 进程 0 报告；工具有效性正控（故意竞态/UAF 各 1 报告）；首轮身份用例 15 份 LSan 报告全部来自 T25 夹具 SetUp 未释放对象，已在 TearDown 释放后 0。
- I03-NEW-035（N-2）：25 例逐例归因均为测试前置与 11-03 CTP 身份规则（及卡9c/6d）不符（broker 8888/9999、缺 user/交易日、虚构请求号、回报缺 broker/investor/交易日、重复委托号、结算查询前强置 ready、attach API 后 connect），只改前置不改断言（逐例表在 README）；两例缓存初始化失败测试期望由“告警继续”改为登录 fail-closed（身份规则）；产品 `TraderCTP::login` 恢复在无 API 时保存 user/pass/productInfo（身份重写曾无裁定地去掉，CTPMini 仍保留；不触发登录）。TraderCTP lane 80/80；CTP 身份夹具全量 58/58。
- I03-NEW-034（N-1）：CTPMini/CTPOpt NotTouched→WOS_Nottouched、Unknown→WOS_Submitting，与 TraderCTP 一致；改前 98≠99 exit 1→通过。Touched 待 D-T2-CTP-07。
- I03-NEW-036（N-3）：Dumper adapter 管理器状态移入永不释放的堆对象、管理器仅持指针且平凡析构（static_assert）；改前子进程退出期析构后回调 SIGBUS→通过。
- 结果：TraderCTP 80/80、CTPOpt 53/53、CTPMini 50/50、Dumper 35/35；四模块默认 -Werror exit 0。
- 文件改后：TraderCTP.cpp b5b2404c3b9c；TraderCTPOpt.cpp 586eaf2a4915；TraderCTPMini.cpp 37f94224df50；TraderDumper/TraderAdapter.h 9c2e207c4359/.cpp 295723ae4862；t25_traderctp_gap 0fb0552520ff；t2_traderctp_conversion 545af64823f7；i03s5 ctpopt 5e29943d8fda、ctpmini f92fdf9eb06a、dumper 7b45b77705bd。
- 仍待：D-T2-CTP-07、D-T6-002；I03-NEW-037（宿主无条件卸载）在他包文件。

## uftcore（22:1x 完成；证据 `../20261104-s5-uftcore/`，决策 `决策卡草稿.md`）

- 红控说明：“改前”均为改前产品+改后测试的构建，非改前二进制原样运行。
- 建议：Q04-002 待验证（UFT TraderAdapter 逐单账本：预占、同步回报与返回值联合判断只回滚一次、剩余量单调、订单/成交分别累计、首终态墓碑、成交按订单+成交号去重、usertag 严格解析、快照按查询版本；trader 124 例红控 15 失败（5 旧反例+6 新例全失败）→改后 123/124，唯一失败 R11TraderDeep.logErrorAndLogoutAreNarrow 既有；按卡5 改写 6 处旧断言/夹具；未跑 TSan，无账户门禁/落盘/重启恢复）；Q04-007 阻塞（实施完成：切日重载准备-提交-一次发布、失败不发布不推进、新锁串行化；4→8 用例转绿；ctx 改前 2/35 失败、红控 6/39 失败、改后 39/39；待 Q04-007-A a–d）；R78-001 待验证（UFT ParserAdapter 卡9：回调准入屏障、回调内 release 推迟、V2 表强制、无 remover initExt 隔离；基线 24 例因 libWtExecMon 导出的 WtCore 同名类运行时替换 UFT 内联 hasApi（lldb 确认），改为非内联后恢复；parser 红控 26/39 失败→39/39；夹具 7/7 normal/TSan，改前 normal 6 失败、TSan 7 失败；m_WtUftRunner 新增 4 例失败（无 remover 假 API 期望关闭时销毁，与卡9 冲突），建议补丁 `patches/SUGGESTED_not_applied_m_WtUftRunner_r220.diff` 未应用，旁路 6/6；其余 11 例改前同败）；NEW-030 Z 建议不忽略、随 Q04-002 复核后关闭（5 例失败即 Q04-002 反例；同配方全插桩 ASan 33/33 无报告；迭代02 混合插桩容器越界未复现构造）。
- 文件改后：WtUftCore/TraderAdapter.h e8113067ab3f/.cpp 208eb84bda45；UftStraContext.h 48e7349be0a1/.cpp 34f83e7fd2fc；ParserAdapter.h 806bc6c7a5ec/.cpp f4d0c812bec0；测试 q04_* 若干、r11_parser_adapter、r11_trader_reply_deep、r54_*、r78_uftcore_adapter_init、n03_uftcore_helper_adapter；夹具 i03_uft_parser_lifecycle_20261104s5。
- 构建注意：他包在途改动使 TraderCTP/TraderXTP 编译失败、WtDtPorter 链接失败，链接测试时去掉两个 dylib。
- 新发现 → I03-NEW-055~061。决策：Q04-007-A（手工持仓解析失败是否归档、未知合约部分导入或整份拒绝、归档失败只导入一次、上日活单迟到回报归属、加载失败暂不通知就绪、热切日与冷启动一致）；Q04-002-A（墓碑回收与 UFT 账户门禁、重复成交是否连持仓通知一起去重、无成交号成交、同步回报口径与 WtCore 统一）；R78-001-A（带 remover 的 initExt 能否以 remover 置空作销毁确认、R130 夹具补 V2）。

## engine-lifecycle（22:3x 完成；证据 `../20261104-s5-engine-lifecycle/`，决策 `decision-drafts.md`）

- 已写入台账（待验证/阻塞），见执行跟踪.json 对应行。注意：覆盖率 CoverageTests_wtcore 早于其最后两次 WtEngine.cpp 费用修改（终版 f756f9579048 仅经独立 ASan/UBSan 链接验证）；R111 套件因他包中途改 HisDataReplayer.h 致对象布局混杂 SetUp abort，需重建复跑。
- I03-NEW-008 待验证：实盘费用经 FeeSchedule（合约优先、品种回退、连续合约按映射实际合约、显式零有效；缺费记错误并计数、成交保留零费）；修复实盘 SEL/引擎组合按目标量收开仓费（I03-NEW-063）；R130EngineFee 三处首版行为变化均已撤回、断言未改；新费用用例 ASan 9/9、R130EngineFee 6/6；HftStraBaseCtx 三处 commInfo->calcFee 待 wp1-ledger 改为 _engine->calc_fee；决策 G（实盘 minfee）、H（缺费保留成交 vs 阻断信号）。
- 跨包：UftStrategyMgr.cpp 接入 probeModule/create_factory（uftcore）；fixtures/r95_uft_strategy.cpp（uftcore）与 i03_exec_eager_validation_20261103/sel_factory.cpp（runners）补 V2 getter；q06 R82/R124 前提与 R42/R130 parser fake（runners、dt-parsers）。
- 决策卡 A–H：A R10-004 SEL 调度线程（建议 on_schedule 进 ticker strand）；B NEW-155 挂起增量合并与换月；C R211 拒绝启动 vs 降级；D R160 MQ confirm（建议固定 false）；E R41 有界停止后隔离；F P01 历史快照格式与恢复期 OOM；G 实盘 minfee；H 缺费保留成交 vs 阻断信号。
- 新发现 → I03-NEW-062~066。

## date-session 增量（完成；README 第 5 节）

- I03-NEW-043/053/054 见新增问题.md 状态；产品改动 WTSBaseDataMgr.cpp d9270f8e09a2→c2f5338ed432（预校验拒绝补诊断日志）。测试改后哈希：r12_base_datamgr 378c338bd10f、r1_basedatamgr 7f248637d9af、r68_state_monitor_inputs 78974ca571fa、r84b 9111b2398627、r154_test_util.h c0424e2308bb、r164 6258e5bc63d6、p09_rdm_window_range 55d4996c4f72、xele_fixture.hpp 5db4e98eb0bc、l03_object_pool af759fddc80f、r5 d60667a2bae5、r44 f3213e535dc6。删除记录仅自建路径。

## 统一快照逐包复核运行 int1→int3 归因汇总（2026-11-05，主线程）

> 非首轮集中回归、非整体回归；全量回归仍 NOT_RUN。运行器 `regression-tools/run_regression.py`（逐例独立进程/独立 cwd；`--provision-only/--provision-exclude` 按用例复制模块、不建链接）。

| 组 | 失败表现 | 归因（合同/缺陷） | 处理 |
|---|---|---|---|
| btcore 成交类 | 持仓/成交为 0 | 卡13d：夹具缺物理合约与费率模板；HFT/UFT 撮合额外要求 tick 携带合约 | `coverage/s5_bt_basefiles.h`；p08 手工夹具登记合约+`_fee_schedule`；HFT/UFT 改由 `prepare_fee_context` 裁定（I03-NEW-080） |
| btcore 0 量/0 价 | 回放被费用错误终止 | 改造后 0 量成交锁存费用错误；UFT 丢失 0 价回退 | 产品修复 I03-NEW-095 |
| btporter/porter/r9/R69/R134 | prepare 失败、K 线为空 | 卡1：缺交易日历（MissingCalendar） | 夹具补 holiday/品种 holiday 模板 |
| uftcore/延迟工具/r90 | 会话无时段 | 卡2b：0-2400 被拒，全天只写 0-0 | 夹具改 0-0（r90 另因 K30 改为单一全天会话） |
| R12/R56/r195/l03/R22/P01/q05 | 加载被整份拒绝 | K28/K30/I03-NEW-053 整批校验 | 夹具与断言按合同改写 |
| R192/R211 ticker | 未闭合/跨日不符/SIGSEGV | 卡3 3a（:60 非法、保留 2400）；热合约扇出空合约解引用 | :59 等价时刻、断言按 3a；产品 I03-NEW-094；R211 补 run/init/次日日期 |
| WtCore ParserAdapter initExt | release 后状态不符 | 卡9a+K15=A：外部 API 归调用方，release 仅通知停止一次、不销毁 | 产品实现 K15（通知一次）；n02/N01 断言改写 |
| R28ParserCommands | SIGABRT | I03-NEW-045：无法证明排空的 runner 析构 fail-closed | 用例显式保留（同 n01 做法） |
| R160/R170/r10 notifier | 编译失败/泄漏 | K24 内部结构改为 core；队列 handler 持有 core 形成循环 | handler 改持 weak_ptr；测试改访问 `_core` |
| r28 配置 | 配置失败 | 配置 NUL 拒绝（20261103-fees-groups-review）；卡13d 缺费用模板 | 断言按合同；补空费用模板 |
| exefact | 不下单/诊断不符 | I03-NEW-074 替身多空净不一致；R7-012 有界重发；卡8 8b eager 校验；VWAP 曲线静默丢段 | 替身修正、断言按 R7-012、补 scale/policy；产品 I03-NEW-096 |
| adstorage | 写入被拒 | 卡9 写端生命周期 V1；卡1 日期核验 | 断言按合同（I03-NEW-085） |
| t26/t6 CTPLoader | 退出码 42/超时/回调被忽略 | 卡14 14a 请求号登记；K02=B；T26-007 | 共享友元头登记请求；t6 假 API 走真实 SPI 失败终态（I03-NEW-093） |
| R1 HFT 切片 | at() 越界不返回 NULL | 既有缺陷（迭代02 同败） | 产品 I03-NEW-092 |
| 环境 | 模块找不到/插件目录 | 模块需与 cwd 同目录；`find_bin_dir` 受自建副本误导；R59 需 `WT_COV_PLUGIN_BIN`；R45 C 接口按 exe 目录找 parser | 运行器按用例复制；删除自建副本；设置环境变量；R45 夹具输出到 exe 目录 |

仍未通过（截至 int3 前定向复跑）：Q05 日线 times=1（BUG-I02-NEW-104 阻塞待裁定）、R222 `isAsync()`（I03-NEW-081 待复核）、porter `ExecUnitStatusApiV2.ParserRelease*` 两例（待 K15 实现后复验，曾误按卡9a 改写已撤回）。
