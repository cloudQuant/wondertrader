# 需要用户裁定的决策卡草稿（s5 dt-parsers）

## D1：active parser 初始化失败时宿主是否终止启动（R104-002 残余、I03-NEW-003）

- 现状：WtRunner、WtDtServo（`initialize` 失败）、WtDtPorter 配置 parser（`_startup_blocked`）均 fail-closed；QuoteFactory 本次只做到“失败项不注册/不计入/不连接、仍持有 API 者隔离保留”，进程继续以其余 parser 运行。
- 选项：A 统一 fail-closed（任一 active 失败即启动失败、退出码非 0）；B 每项可配 `required`（缺省 true=A，false 时降级运行并告警）；C 保持各宿主现状。
- 建议：B（缺省等同 WtRunner）。影响：BUG-R104-002、I03-NEW-003、QuoteFactory 发行配置。

## D2：部分 parser 连接命令提交失败后的回滚（I03-NEW-003）

- 现状：三个 `ParserAdapterMgr` 均可 `runWithResult()`；WtDtServo/WtDtPorter 导出提交状态，QuoteFactory 现记 ERROR 并继续。
- 选项：A 仅报告（现状）；B 回滚：对已提交者 stop+drain 并使启动失败；C 有界重试后按 A/B。
- 建议：A 作为默认，B 随 D1 的 `required` 语义（required 项失败时回滚）。

## D3：`initExt` 外部 API 的释放归属（WtDtCore/WtDtServo 与 WtCore 不一致）

- 现状：WtCore `initExt` API 永不销毁（无 V2 证明即保留）；本次 WtDtCore/WtDtServo 保持旧语义——失败关闭宿主闸门、保留 API，显式 `release()` 视为调用方断言屏障后的拆除；WtDtPorter 进程退出时**不**释放 ExpParser（其 release 会回调已可能在析构的 Python），闸门关闭且排空后转入进程级保管。
- 选项：A 维持现状（宿主自有代理可证明，第三方外部 API 由调用方负责）；B 与 WtCore 统一为永不销毁；C 为 `initExt` 增加带 V2 表的重载。
- 建议：A，后续按 C 演进。影响：R18-006、R8-006、NEW-009、z15 发现 E（WtPorter）。

## D4：指数工厂 remover ABI（R68-003）

- 现状：`FuncDeleteIndexFact` 声明 `IIndexWorkerFact*&`，仓内导出 `delete_index_factory(IIndexWorkerFact*)`；无版本化导出，宿主无法判定外部模块签名。本次宿主不调用任何 remover，工厂与模块常驻到进程退出；worker 经工厂虚函数删除并 join 触发线程。
- 选项：A 现状（常驻）；B 新增版本化导出（如 `get_index_factory_abi_v2` + 按值 remover）并据此销毁，旧模块常驻；C 直接改 typedef 为按值（外部旧二进制 ABI 风险）。
- 建议：A + 后续 B（与卡4 4b 一致）。

## D5：NEW-029 产品修改归属

- `src/WtBtCore/HisDataReplayer.cpp` 不属于本包；卡E 建议合同与补丁草稿见 `patches/NEW-029-HisDataReplayer.proposed.diff`。需主线程把实现分配给 WtBtCore 回放 owner（或授权本包修改），测试放 R91 lane（q02_btcore3）。

## D6：同一 vendor 模块在某实例失败后，其他实例能否再次加载（I03-NEW-003 opens=2）

- 现状：ParserCTPMini 在“缺 creator 符号/creator 返回 NULL”后保留模块引用、不卸载，同实例不再初始化；其他实例仍会再次 `dlopen` 同一路径（引用计数 +1，静态初始化不重跑）。`ctpmini_api_init` 的 `cross-instance` 断言要求 opens==1。
- 本子任务曾实现进程级“隔离模块登记”（按规范化路径与句柄别名拒绝），但它使卡9 vendor V2 夹具 `V2Adapter.A_FailedInitBeforeSdkDrainsAndReleasesOwner`（creator 返回 NULL 后同进程后续实例应能正常初始化）之后的 9 例全部失败，与已落地的卡9 provider 合同（creator 未返回对象 = DRAINED、可销毁）冲突，已撤回（`withdrawn/`）。
- 选项：A 允许再次加载（卡9只禁止卸载；再次失败照常隔离）；B 进程级拒绝（需同时修改 vendor V2 合同与夹具，creator NULL 视为模块级永久隔离）；C 仅对“缺符号”进程级拒绝，“creator NULL”允许重试。
- 建议：A（或 C）；据此改写 `cross-instance` 断言。影响：I03-NEW-003、卡9 vendor V2。
