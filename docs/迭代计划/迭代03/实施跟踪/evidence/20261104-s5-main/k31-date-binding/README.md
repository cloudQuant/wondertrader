# 决策 K31=A（BUG-R12-13 剩余项）：插件侧经可选版本化导出取得 DateMappingV1

## 合同（`src/Includes/DateMappingBinding.h`，新增）
- 按对象类型分开的可选导出：`bindDataReaderDateMappingV1(IDataReader*, const DateMappingV1*)`、`bindRdmDtReaderDateMappingV1(IRdmDtReader*, …)`、`bindParserDateMappingV1(IParserApi*, …)`，返回 1 绑定、0 拒绝，不抛异常。
- 宿主在工厂返回对象后、init 前调用一次；插件校验 size/version/context 与全部 5 个函数指针，复制表后返回 1，否则 0 并保持路径 C；第二次绑定拒绝并保留首张表。表的 context（宿主管理器）须比插件对象活得久（与 init 时的 `IBaseDataMgr*` 相同）。
- 双向兼容：旧插件无导出、旧宿主不调用时均保持路径 C（`DateMappingClient(IBaseDataMgr*)`）。

## 插件侧
- `WtDataStorage`：`WtDataReader`（6 处）、`WtRdmDtReader`（9 处）映射改经 `_date_binding.client(_base_data_mgr)`，导出两个读端绑定函数。
- `WtDataStorageAD`：`WtDataReaderAD`（`ad_reader_trading_date` 2 处）、`WtRdmDtReaderAD`（3 处），导出两个读端绑定函数。
- `ParserXeleSkt`：`tryXeleTradingDate`（2 处），导出 `bindParserDateMappingV1`。

## 宿主侧（持有具体 `WTSBaseDataMgr` 的创建点）
- WtCore `WtDtMgr::set_date_mapping` + `initStore` 注入；`WtRtRunner`/`WtRunner` 初始化数据管理器前登记。
- WtDtServo `WtDataManager::initStore`（运行器的基础数据管理器）。
- WtExecMon `WtSimpDataMgr::initStore`（`WtExecRunner::get_date_mapping`）。
- 解析器：WtDtCore/WtDtServo `ParserAdapter`（自身持有 `WTSBaseDataMgr*`）；WtCore/WtUftCore `ParserAdapter::setDateMapping`，由 `WtRunner`/`WtRtRunner`/`WtUftRunner` 在创建模块解析器时登记。外部注入的解析器（initExt）不经模块工厂，不适用。
- 绑定结果记日志：1 记 info、0 记 warn（保持路径 C），无导出仅读端记 info、解析器不记（避免普通解析器噪声）。

## 验证
- 用例 `n01_wtcore_tradepaths/s5_main_k31_date_binding_test.cpp`（4 例）：持有器校验/只绑一次/未绑定走路径 C；真实 `libWtDataStorage`/`libWtDataStorageAD`/`libParserXeleSkt` 导出与 0/1/0 合同；`WtDtMgr` 注入后经 AD 读端读 K 线，交易日映射走注入表（缺日历时 fail-closed）；未登记时不调用该表。
- 红控：插件三模块用改前源码重编（宿主与用例为改后），导出用例与注入链用例失败（“binder is not exported”“did not use the host DateMappingV1 table”，`k31-red.log/xml`）；恢复改后源码（哈希见 `sources.sha256`）全量重编 4/4（`k31-green*.log`）。全部目标 -Werror 编译通过。
- 未覆盖：TraderCTPMini 无日期映射调用（夜盘日期以已校验的登录交易日推算，T3-015 已对无效交易日 fail-closed），不需要绑定；Windows/MSVC 导出与外部第三方插件未验。
