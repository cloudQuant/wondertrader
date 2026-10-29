# 13_execution_share：第二轮复核

本组复核 25 项，逐条重查当前实现、公开接口/调用路径和测试前提；未直接沿用首轮结论。

## 汇总

| 判定 | 数量 |
|---|---:|
| PRODUCT | 19 |
| TEST | 2 |
| NOT_BUG | 1 |
| DUPLICATE | 0 |
| NEEDS_DECISION | 2 |
| EXTERNAL_BLOCKED | 1 |
| LOCAL_INCOMPLETE | 0 |

证据级别：STATIC 20、LOCAL_REPRO 2、PENDING 3。

## 逐项结论

### BUG-R17-002 — PRODUCT / STATIC

- 理由：pricemode=3 可沿正常自动建单到 PriceModeNames[4]，表仅 4 项；外部配置边界没有校验。
- 根因：init 记录配置时直接按 _price_mode+1 索引，工厂仍接受未校验值。
- 修法：在索引/注册前校验 -1..2；非法值令 init/工厂失败并诊断，不擅自选默认模式。
- 验收：-1..2 均正常初始化；-2/3 安全失败、不注册、不越界。
- 测试动作：src/TestUnits/coverage/r7_exefact/r17_diff_minimpact_unit_test.cpp / init_pricemode_3_reads_past_name_table： 改为工厂/配置边界安全拒绝测试，并覆盖合法 -1..2 控制值；target=CoverageTests_exefact，filter=r17_diff_minimpact.*:r7_stkmin.*

### BUG-R17-004 — PRODUCT / STATIC

- 理由：session 查询可返回 NULL，WtSimpDataMgr::initStore 仍返回 true，runner 忽略 init 结果；后续 quote 解引用 _s_info。
- 根因：初始化没有检查 session 查找结果，runner 未传播数据管理器失败。
- 修法：缺失/未知 session 时返回初始化失败并阻断启动；quote 入口防御性判空。
- 验收：无效 session 在首个 quote 前失败；有效 session 正常推进日期、分钟、交易日。
- 测试动作：src/TestUnits/coverage/r7_exefact/r17_simpdata_porter_unit_test.cpp / configured missing/unknown session： 增加 runner data.store 缺/未知 session 配置，断言 quote 前初始化失败并保留有效 session 正控；target=CoverageTests_exefact，filter=r17_simpdata.*

### BUG-R17-005 — NOT_BUG / STATIC

- 理由：NULL 会在 cfg->get 前解引用，但正常 runner 路径不以 NULL 调用；外部 session 缺失另列 BUG-R17-004。
- 根因：测试使用不受支持的内部参数，未证明有效输入路径可达。
- 修法：撤回 NULL init 的产品缺陷主张；继续在外部配置边界校验必需 data/store。
- 验收：测试改为 runner 缺 data/store 时安全失败；不把内部非法 NULL 调用算产品回归。

### BUG-R19-001 — PRODUCT / STATIC

- 理由：新增 key 后 commit_section 仅改 section 时间戳，不推进 update_slave 使用的 block 时间戳；update_slave(false) 早退，get_keys 仅在段数不一致时强刷，既有段的新 key 对 slave 不可见。
- 根因：key/section 版本发布粒度未与 slave 的索引刷新条件连通。
- 修法：提交时推进共享 generation，slave 按 generation 更新 key 索引；如只支持静态 schema，应显式限制运行期创建。
- 验收：slave 初始化后 master 在既有段新增 key 并 commit；slave update(false)/get_keys 可见该 key；未提交变化按合同不可见。

### BUG-R19-003 — NEEDS_DECISION / PENDING

- 理由：删除后 block count 不减少，达到 MAX_SEC_CNT 时无法继续分配；即时复用是否为合同要求未确定。
- 根因：段回收时点与跨进程索引/布局语义无明确契约。
- 修法：明确立即复用、显式 compact 或下次 init_master 回收，再按合同实现兼容的回收协议。
- 验收：覆盖满 64 段、删除、重新分配和重启，断言只对应选定的回收时点。
- 待决：delete_section 后段槽何时必须可复用？；选项：立即复用/显式压缩后复用/下次 init_master 回收；建议：明确一个显式回收点并维护跨进程映射一致性。；影响：决定共享段索引、计数和 ABI 生命周期语义。

### BUG-R19-004 — PRODUCT / LOCAL_REPRO

- 理由：第二 domain 同文件 init 后，两个预置 key 都读为 -1；当前刷新二进制的定向回归 1 test/0 skipped FAIL。
- 根因：每次 init_master 都将已有段 state 置 0 并以当前实例使用的段 mark-sweep，未协调文件所有权。
- 修法：拒绝同一物理文件并存的 master alias，或增加 owner/generation 协议防止清除别的活跃实例。
- 验收：首次读 key 101/202 成功；同文件第二 init 按策略拒绝或安全共存且保留数据；唯一 master 重启另测。

### BUG-R19-009 — PRODUCT / STATIC

- 理由：ShmPair 未初始化 _blocktime；master 初始化仅某些重排分支赋值。删除后 get_keys 强刷，update_slave 在 bForce 判断前读该字段，且重建索引未过滤 tombstone，可能 UB 并复活已删段。
- 根因：master query 误入 slave rebuild，缺 role guard、确定的 blocktime 初始化及 state 过滤。
- 修法：master 走本地索引读取或在刷新前拒绝 master；初始化 blocktime；重建只加载 state==1。
- 验收：master 创建/删除/get_keys 后 tombstone 不复活且无未初始化读；slave 初始/增量只见有效段。
- 测试动作：src/TestUnits/coverage/r19_mq/r19_shareblock_master_test.cpp / master delete then get_keys： 修复 role/generation 后用临时映射执行创建、删除、枚举；target=CoverageTests_mqshare，filter=R19.*shareblock.*

### BUG-R19-011 — PRODUCT / STATIC

- 理由：strcpy 将长度>=64 的命令写出 _command，覆盖后续记录字段；没有长度检查。
- 根因：add_cmd 先推进 writable 后无界 strcpy。
- 修法：先验证最多 63 字节；超长返回 false 且游标/映射保持不变。
- 验收：63 字节可往返；64+ 拒绝且相邻记录与游标不变。
- 测试动作：src/TestUnits/coverage/r19_mq/r19_shareblock_cmd_test.cpp / command length boundary： 修复后覆盖 63+NUL、64 和超长，检查游标/邻接槽；target=CoverageTests_mqshare，filter=R19.*cmd.*

### BUG-R19-012 — NEEDS_DECISION / PENDING

- 理由：不同 name 的 registry 项仍映射同一个 .cmd 文件，可能共享命令与游标；语义是否错误未知。
- 根因：默认文件路径未纳入逻辑队列 name，默认共享策略未文档化。
- 修法：确认语义；若 name 表示队列隔离则对 name 安全编码为文件名，若全局共享则文档说明并提供显式共享参数。
- 验收：两个不同 name 省略路径时按选定契约共享/隔离；显式同路径另测。
- 待决：省略 init_cmder path 时不同 name 应隔离还是共用 .cmd？；选项：按 name 隔离/默认全局共享；建议：默认按 name 隔离；共享由调用方显式传同一路径。；影响：改变默认文件名可能影响现有依赖全局 .cmd 的调用方。

### BUG-R19-013 — PRODUCT / STATIC

- 理由：wt_strcpy 按 strlen 复制；32+ 字节名称越过固定 32 字节字段。
- 根因：共享内存字段固定宽度而写入无界。
- 修法：在修改共享状态前限制长度最多 31 字节；超长返回失败，保持旧 ABI 大小。
- 验收：31 字节成功且 NUL 终止；32+ 安全失败，段/key/邻接字段不变。
- 测试动作：src/TestUnits/coverage/r19_mq/r19_shareblock_persist_test.cpp / section/key length boundary： 修复后覆盖 31/32 字节安全边界及邻接字段；target=CoverageTests_mqshare，filter=R19.*shareblock.*

### BUG-R53-003 — EXTERNAL_BLOCKED / PENDING

- 理由：本地 git 历史显示旧 exchange 字段容量为 10（5f6fa321 前的 MAX_EXCHANGE_LENGTH）；旧 struct 构造时 memset 全零，内置 CTP/Femas 等 parser 用 strcpy 写基础资料 C 字符串。迁移提交 12098b5b 的旧格式赋值也用 strcpy，历史 reader 提交 007d7bc3/9bb6c1f2 按旧 struct size 读取并转换。这强烈支持官方生成的有效记录带 NUL，但仓库没有已发布旧 DSB/raw tick 样本或字段格式文档，无法覆盖外部 writer/所有存量版本。
- 根因：历史 DSB 的生产者版本和 exchg[10] 终止约定没有在仓库留档。
- 修法：取得指定旧发布版本的 DSB/SDK 格式证据前不改产品；若格式保证 NUL，撤回满宽风险；若允许满宽，采用不跨字段读取的定长/有界转换。
- 验收：核对带生产者版本的真实旧 DSB 或正式旧版格式规范；当前 max-1+NUL 测试只作合法字符串对照，不宣称验证满宽历史格式。
- 外部阻塞：需要明确版本的已发布旧 DSB tick 文件（附生产者版本）或对应旧 SDK/磁盘格式文档。本地已检查 5f6fa321^、12098b5b、007d7bc3、9bb6c1f2 历史及仓内读写路径，未找到旧二进制 fixture 或文件格式规范；源码历史只能证明内置 writer 的字符串写法，不能证明所有历史文件来源。下一步由主代理获取上述材料并确认字段实际终止字节，再据结果撤回或立具体修复。

### BUG-R56-003 — PRODUCT / STATIC

- 理由：check_cell 仅检查 header map，getter 直接索引较短 _current_cells；短行 next_row 成功后取缺列可越界。旧 disabled 用例在 next_row 前读取的前提无效，但不否定独立短行风险。
- 根因：未检查实际当前行字段数。
- 修法：next_row 或 getter 校验当前行实际列数；缺列返回错误/安全默认。
- 验收：加载 header a,b,c 与短行 1；取 c 安全返回缺列错误/默认；完整行正控不回归。
- 测试动作：src/TestUnits/coverage/l02_wtstools_mq/r56_csv_test.cpp / short row after successful next_row： 改成 header a,b,c、短行 1 成功读取后取 c；保留完整行正控；target=CoverageTests，filter=R56Csv.*

### BUG-R56-004 — PRODUCT / LOCAL_REPRO

- 理由：loadHots 忽略 loadCustomRules false，仍标 initialized 并返回 true。现活动测试把显式缺文件当空成功，混淆了未配置可选路径与显式坏路径。
- 根因：显式配置错误没有传播，manager 进入 initialized 却无规则。
- 修法：缺 hot 字段维持可选不调用；显式缺失/坏格式路径传播 false，保持未初始化，runner 报错。
- 验收：未提供 hot 不加载；有效文件成功；显式缺失/坏格式 false 且未初始化。
- 测试动作：src/TestUnits/coverage/l02_wtstools_mq/r56_hotmgr_test.cpp / MissingFilesKeepManagerEmpty： 拆分 runner 不提供字段与显式 missing/malformed 文件；显式文件错误必须传播；target=CoverageTests，filter=R56HotMgr.*

### BUG-R7-002 — PRODUCT / STATIC

- 理由：VWAP/TWAP/StockVWAP 缺少次数校验，0 会进入整数除法。
- 根因：init 直接除以由外部配置得到的零值。
- 修法：派生前验证 total_times>0；缺省/0 失败且不注册。
- 验收：缺项/0 安全失败；1/N 正控周期跨度正确。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp / zero total_times boundary： 改成 init/factory 失败与合法 1/N 正控，不跑崩溃输入；target=CoverageTests_exefact，filter=r7_vwap.*:r7_twap.*:r7_stkvw.*

### BUG-R7-003 — PRODUCT / STATIC

- 理由：缺失或短 profile 时 init 提前返回但未标失败；后续 do_calc 按分钟直接访问 vector。
- 根因：init 失败信号未传至工厂，运行路径假设向量完整。
- 修法：检查文件格式/长度/分钟边界，失败不注册且下单路径防护。
- 验收：missing/empty/short 不得进入计算或下单；完整 profile 正控覆盖合法分钟边界。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp / profile missing/short： 测试初始化拒绝和有效 profile 对照，坏文件不进入 tick/order；target=CoverageTests_exefact，filter=r7_vwap.*:r7_stkvw.*

### BUG-R7-004 — PRODUCT / STATIC

- 理由：VWAP/TWAP/StockVWAP 在 diffQty==0 先返回，后面的 clear sentinel 多空子仓清理不可达；净仓抵消会隐藏残仓。
- 根因：清仓逻辑在净差零早退之后。
- 修法：对 clear sentinel 先按可用 long/short 分别生成平仓，再处理普通 diffQty==0。
- 验收：测试净零但 gross long/short 非零、仅 long、仅 short、flat、非零净额；只在支持双向账户时发方向平仓。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp / clear on gross positions with net zero： 构造 long/short/net 视图，覆盖净零毛仓、单边仓与 flat；target=CoverageTests_exefact，filter=r7_vwap.*:r7_twap.*:r7_stkvw.*

### BUG-R7-006 — PRODUCT / STATIC

- 理由：VWAP/TWAP/StockVWAP 初始化或价格路径解引用 commodity/session/tmode 指针，缺资料时没有一致门禁。
- 根因：自动工厂先保存 unit 再 init；依赖缺失没有阻止注册。
- 修法：统一校验必需元数据，失败不加入 unit_map/不调度，后续保留空值防护。
- 验收：未知 code 安全拒绝、无订单；有效元数据正控完成 init/tick/下单。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp / required commodity metadata missing： 增加未知 code/缺 commodity/session 失败和有效依赖控制；target=CoverageTests_exefact，filter=r7_vwap.*:r7_twap.*:r7_stkvw.*

### BUG-R7-011 — PRODUCT / STATIC

- 理由：config 直接解引用 _config->get(basefiles) 返回的 NULL；语法合法的 {} 可触发。
- 根因：必填字段校验晚于指针使用。
- 修法：先检查 basefiles 并报告缺字段，再继续配置。
- 验收：完整最小配置成功；缺 basefiles 返回 false 和诊断，不发生空指针访问。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_exec_runner_test.cpp / config missing basefiles： 隔离临时配置调用 config，断言 false 与缺字段诊断；target=CoverageTests_exefact，filter=r7_exec_runner.*

### BUG-R7-014 — PRODUCT / STATIC

- 理由：VWAP/StockVWAP 常规 mode1 买取 bid/卖取 ask，重发 mode1 却与 mode2 都取对手价；已有用例把不一致实现锁定。
- 根因：fire_at_once 错将 mode1 映射为对手价。
- 修法：统一常规与重发的 mode 映射；撤单次数偏移另行叠加，保留涨跌停限制。
- 验收：同一行情买卖分别核对 VWAP/StockVWAP mode0/1/2；mode1 重发保持本方最优，mode2 对手价。
- 测试动作：src/TestUnits/coverage/r7_exefact/r7_vwap_unit_test.cpp / fire_at_once_price_mode_1_takes_ask_price： 更新买卖常规/重发对照，mode1 采用本方最优，mode2 对手价；target=CoverageTests_exefact，filter=r7_vwap.*:r7_stkvw.*

### BUG-R7-017 — TEST / STATIC

- 理由：旧 fake success 用例 undone=0，与 ExecMocker::buy 先加 undone、MatchEngine 下一 tick 才发 success 的实际顺序不同；当前修正测试先设成已下单量后回调，do_calc 命中 undone 门禁，因此只买一次。TraderAdapter/Uft Adapter 当前实际 entrust sink 只发送失败回调，未证实 live-success 可达。
- 根因：重复下单结论由不真实的 fake 状态触发，忽略了真实路径的未完成量更新时序。
- 修法：撤回当前重复下单产品主张；保留按真实 ExecMocker 时序构造的回归。
- 验收：ExecMocker buy 后 undone 已更新，下一 tick success callback 时 undone 非零；当前测试回调后 buys 仍为 1。失败回调继续按原错误重算路径测试。

### BUG-R84b-001 — TEST / STATIC

- 理由：候选唯一触发是 disabled 测试人工将 nullptr 传给内部 StateMonitor::isInState；其实现按非空字符串查 map，仓内正常调用 DataManager::canSessionReceive 传入会话标识，现未发现正常可空路径或 nullable 契约。因此该测试不能支持产品缺陷主张，应撤回，而不是要求用户决定人工参数的语义。
- 根因：原结论从不受支持的 nullptr 直接调用外推；正常调用者均提供 sid。
- 修法：撤回产品修复要求；若未来 SDK 声明 nullable，再由入口判空并单独设计公共契约。
- 验收：移除或保持禁用人工 nullptr 崩溃断言；现有非空、空串和未知非空 sid 行为用例继续验证正常路径。

### BUG-WSN06-001 — PRODUCT / STATIC

- 理由：直接 createStrategy(nullptr) 的 disabled 用例不代表正常 runner 参数，getCString 缺字段返回默认空串；但这条外部输入使 factory 返回 NULL，WtRunner CTA 与 WtRtRunner CTA/Sel 随后无判空解引用。产品风险可达，根因是 runner 未验证配置名称和 factory 结果，而非正常路径传了 NULL 给 strcmp。
- 根因：配置缺 name 时得到空串，unsupported/empty name 的工厂返回 NULL；部分 runner 继续调用 stra->self()/init，造成可达空指针解引用。
- 修法：在 runner 配置边界要求非空且受支持的 name；所有 factory result 在 init/self 前检查，失败返回配置错误并跳过/终止注册。
- 验收：用外部配置分别省略 name、提供未知名和有效名；前两者在 context 安装/回调前返回明确失败且不崩溃，合法名正常初始化。测试不得将直接 createStrategy(nullptr) 当作生产可达证据。
- 测试动作：src/TestUnits/coverage/m_WtRunner/wt_runner_strategy_name_test.cpp / missing_or_unknown_strategy_name_fails_before_context_registration： 新增 WtRunner 配置入口回归：省略 name、未知 name 与合法 name；检查初始化结果及 context 注册，覆盖 runner null-result guard。；target=CoverageTests_wtrunner，filter=wt_runner_strategy_name.*

### BUG-WSN06-002 — PRODUCT / STATIC

- 理由：UFT/HFT demo init 直接解引用 cfg；runner 可将 NULL 传入并继续安装未初始化策略。
- 根因：缺少 params 验证，且 init 失败没有阻止注册。
- 修法：按策略 schema 校验 params，传播 init false；失败不注册；可选项显式转为空对象/默认值。
- 验收：缺/非法 params 安全失败不注册；合法配置完成 init 与首个 callback。
- 测试动作：src/TestUnits/coverage/m_WtUftRunner/wtuftrunner_params_test.cpp / active_strategy_missing_params_is_not_registered： 新增 UFT runner 配置入口缺 params、空对象和合法对象测试；断言 init 失败不会安装未初始化策略。；target=CoverageTests_wtuftrunner，filter=wtuftrunner_params.*

### BUG-WSN06-004 — PRODUCT / STATIC

- 理由：TwoMa short_days/long_days 为 0 会参与周期除法；混合用例中的 DualThrust 不应归入同一错误。
- 根因：TwoMa 未验证周期参数并在调度时直接使用。
- 修法：TwoMa init 检查正周期与历史长度；失败不注册调度；DualThrust 独立定义零值。
- 验收：TwoMa 非正值/历史不足安全失败，正周期正常；DualThrust 单独测试其零值语义。
- 测试动作：src/TestUnits/coverage/n06_strategies_mq_share/n06_cta_stra_test.cpp / TwoMa zero periods： 拆分两种策略；TwoMa 非正值拒绝，DualThrust 单独定义零窗口 oracle；target=CoverageTests_ctafact，filter=n06_two_ma.*

### BUG-WSN06-005 — PRODUCT / STATIC

- 理由：MQManager 忽略 init bool，仍登记并返回正常 ID；调用者只能由后续日志发现不可用。
- 根因：资源初始化状态与成功 ID 返回脱节。
- 修法：定义不与有效 ID 冲突的 invalid sentinel；init 失败不登记且返回 sentinel。
- 验收：有效 endpoint 返回可用 ID；非法协议/绑定冲突返回 sentinel、不登记，后续操作 not-exists。
- 测试动作：src/TestUnits/coverage/r19_mq/r19_mq_manager_test.cpp / invalid_protocol_logs_bind_failure_but_handle_still_returned： 改断言无效 sentinel、无注册、后续 not-exists，并加有效 endpoint 控制；target=CoverageTests_mqshare，filter=r19_mq_server.*:r19_mq_client.*

## 定向执行证据与边界

- BUG-R19-004：主代理在刷新后的 CoverageTests_mqshare 上运行 r19_second_init_master_preserves_first_domain_data，1 test、0 skipped、FAIL、exit 1；binary SHA-256 bda2bc3bf170afb5575087691eb2c680f2913da0b7ee739fe4d4c5af547b5611。第二 domain 指向同一临时文件后，两个原 key 均读为 -1。原始输出见 evidence/core-fix/mq_preserve.log/.xml、runs.json。mq_skip 是 SKIP，未计作 PASS。
- BUG-R56-004：刷新后的 CoverageTests 上，OptionalHotRulesOmittedLeavesManagerUninitialized 为 PASS 1/1；ExplicitMissingHotRulesFileFailsWithoutInitializingManager 为 FAIL 1/1、exit 1。失败断言显示 loadHots 实际返回 true 且 manager initialized，支持显式坏路径未传播错误。binary SHA-256 048544934fca9415ea008648b8955f71edd566c2d7af0e85080df9b4e9024f36；原始 log/xml 与 runs.json 均已引用。
- BUG-R53-003：已查 2021-12-30 本地历史并追到官方旧 parser/reader；内置代码将 exchange 当 NUL 字符串处理，但没有存量旧 DSB 或正式格式规范能覆盖外部 writer 和已发布文件。因此需要指定旧版本真实 DSB/SDK 材料，不能由用户选择历史格式。
- BUG-R7-017：ExecMocker::buy 在 MatchEngine 成功回报之前先更新 undone；当前修正测试使用同样状态并断言不会重复买入。真实 TraderAdapter/Uft Adapter 回调目前只传失败状态。旧 undone=0 手工成功回调不能支持生产重复下单主张，判 TEST。
- BUG-R84b-001：唯一 nullptr 用例是人工调用且现在为安全 skip；StateMonitor 正常调用点传非空 sid，nullable 语义无证据，撤回该输入主张。
- BUG-WSN06-001：缺 strategy name 时 getCString 返回空串而非 NULL；空/未知名令 factory 返回 NULL，WtRunner CTA 与 WtRtRunner CTA/Sel 对结果无判空解引用。这是可达 runner 配置失败路径，判 PRODUCT；direct strcmp(NULL) 测试本身不构成产品证据。
- 未运行全量构建/覆盖率、sanitizer 或真实柜台/插件；STATIC/PENDING 结论不外推为运行时或生产接受。

## 保留决策项

2 项 NEEDS_DECISION：R19-003（段回收时点）、R19-012（默认命令文件语义）。R53-003 改列 EXTERNAL_BLOCKED：需要具体旧发布 DSB/SDK 证据，不是用户偏好决策。
