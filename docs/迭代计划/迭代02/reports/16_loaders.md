# 16_loaders 候选复审（2026-10-29）

## 范围与证据口径

复核 /tmp/wondertrader-review-20261029/groups/16_loaders.json 的 33 条候选，逐项对照对应的 bugs 原记录、生产实现、仓库 SDK/接口声明、现有测试的前置状态与断言。本轮只做静态审阅；没有运行测试、构建、网络访问或真实柜台，也没有改原始 bug 台账、测试、生产代码或构建文件。

本表中的 PRODUCT 表示实现缺陷受有效数据、正常生命周期或仓库公开契约支持，适合进入迭代计划；不表示本轮动态复现。当前没有任何条目标为 LOCAL_REPRO。静态依据完整但仍缺运行时证据的候选标为 STATIC；依赖厂商时序/字段规范才能定级的标为 PENDING。

## 计数

| lane | PRODUCT | UNDECIDED | NOT_BUG | TEST | DUPLICATE | 合计 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| T26_ctploader | 7 | 3 | 1 | 0 | 0 | 11 |
| T4_femas_ctploader_mini | 8 | 2 | 2 | 0 | 0 | 12 |
| T6_ctploader_mocker_udp | 10 | 0 | 0 | 0 | 0 | 10 |
| **合计** | **25** | **5** | **3** | **0** | **0** | **33** |

本轮没有可判为 DUPLICATE 的同一实现重复记录。T4/T6 的配置段空指针、两种 CTP loader 的 exit、增量 JSON 字段错误、两个 UDP 消费者的长度检查，虽然形态相似，均位于独立生产实现，须分别修复验收。T26-003/004 也拆成嵌套层级读取与 strikeprice 键名两项独立错误。

## 分类结果

### PRODUCT（25）

- **T26_ctploader：** BUG-T26-003、004、005、006、009、010、011。覆盖期权 JSON 往返、altcode/费率请求字段、feesfile 相对路径、增量加载泄漏、查询队列数据竞争及“待发送队列为空即响应完成”的异步收尾误判。
- **T4_femas_ctploader_mini：** BUG-T4-002、005、007、014、015、016、017、018。覆盖持仓数组漏 append、公开缓冲区容量未检查、Femas/CTPOptLoader 加载失败空函数调用、异步回调结束宿主进程、增量 JSON 泄漏/字段丢失及配置段缺失崩溃。
- **T6_ctploader_mocker_udp：** BUG-T6-001 至 010 全部为 PRODUCT。包括撮合已成交量符号错误、CTPLoader 异步进程退出、配置段/解析错误处理、UTF-8 产品名扫描、两处 UDP 短帧长度校验、适配器传 NULL 配置时崩溃或异常、TraderMocker 缓存泄漏及 ParserUDP 活跃连接收尾。

每项的具体源行、测试行、修复建议和验收条件见 [16_loaders.json](../triage/16_loaders.json)。

### UNDECIDED（5）

- BUG-T26-001：worker 空队列时析构会卡在 join；当前 run() 分配 SPI 后没有仓库内可见的正常析构路径，需先确定 SPI 所有权与收尾契约。
- BUG-T26-002：无数字字符串会越界扫描；现有禁用例触发输入 SRCS 是否为有效 CZCE 合约码缺 SDK/柜台字段格式依据。
- BUG-T26-007：两级费率查找均 miss 后解引用 end iterator 是静态危险，但禁用测试直接伪造无 contract、无 fee 的 _margins["ZZ99"] 状态，正常查询是否能形成该状态待确认。
- BUG-T4-003：Init 返回前可能触发回调的时序尚无证据；当前禁用测试用 sentinel 伪造“未 connect 但 API 存在且已 ALLREADY”的状态。
- BUG-T4-004：当前禁用测试将不可访问的 sentinel 指针注入 API 后直接调成功登录回调，没有经过 connect/API 注册。要判断产品可达性，需可控 API stub 或 vendor 时序证据。

这些候选不计入 25 项产品缺陷。特别是 T4-003/004 的现有 DISABLED 形状不能作为产品崩溃复现。

### NOT_BUG（3）

- BUG-T26-008：生产成功登录先写 _trading_day，之后才请求合约；正常合约回报顺序中先写后读。
- BUG-T4-006：认证请求结构的三个语义字段均被赋值；未清零部分是 C 字符串 NUL 后尾字节/结构填充，没有证据表明 vendor 将其作为语义输入。
- BUG-T4-019：extractProductID 的 ASCII 范围确实包含标点，但仓库生产合约回调没有调用该 helper；产品字符契约也未给出。

不建议把这些条目作为已确认生产缺陷排期。可选的初始化防御不构成当前问题已被证明的证据。

## 测试前置与定向验收建议

- **现有有效 T26 回归：** t26_ctploader_flow.incremental_login_reloads_commodities_and_contracts 已覆盖嵌套期权、strikeprice 与 altcode；t26_ctploader_dump.margin_response_dumps_fees_to_configured_relative_name_with_product_fallback 和 commission_response_with_absolute_feesfile_dumps_to_that_path 覆盖相对/绝对 feesfile；这些是产品修复后的定向验收入口。
- **T26-005 缺的断言：** 增量 fixture 有 CZCE altcode=SR510，但费率队列 fixture 只用无 altcode 的 SHFE 合约。新增 altcode 非空的 commission/margin request 断言；当前 commission 条件在 altcode 非空时跳过字段写入。
- **T4-002/T4-005：** 持仓用例 FemasContractFixture.position_query_response_appends_the_reported_item 已对一条结果做非空断言；TraderFemasTest.make_entrust_id_rejects_insufficient_logical_capacity 当前是启用的 canary 测试，虽然旧 bug 文档将它标为 DISABLED。两个过滤器可供主控 stub runner 定向执行。
- **T6-001：** R113 sink 已采集 vol_traded，但部分成交测试只断言 10 手委托剩余 9 手/成交 1 手，没有断言已成交字段。增加推送与查询两处 vol_traded == 1 断言，filter 为 R113MockerPartialCancel.R113Mocker_PartialFillThenCancelPushesCanceled。
- **不合格/占位用例：** T4-003/004 当前禁用用例人为制造不可达状态；T4-015 和 T6-002 的 exit 用例只确认旧 exit(0) 行为，启用可能把 gtest 二进制以成功码提前终止；T6-005 的空串用例只调用后 SUCCEED()，T6-006 的 parser seam 用例也只 SUCCEED()。T6-005 的同形 extractProductName 实现在 CTPOptLoader 中也有生产调用，修复和 UTF-8 回归应覆盖两份实现。这些都不能作为产品通过证据。T6-006/007 需要受控内存/假 socket 发送有完整接收长度的短帧、完整帧；不可把“需要测试接缝”的占位用例算作覆盖。

## 后续边界

本轮静态复审不代替主控统一 stub runner。修复后仍须按单项验收记录命令与结果；没有运行、vendor 时序不确定或需要厂商字段契约的条目，继续保持 NOT_RUN/PENDING，不能声称真实柜台、发行或生产验收通过。
