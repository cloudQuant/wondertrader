# Lane R8 交付清单 — WtDtServo/WtDtRunner.cpp + WtDataManager.cpp + ParserAdapter.cpp（迭代01 第七波）

- 派生自: `manifests/api-brief-R8.md`（本波 3 个文件合计 missed 1,107 行，同属 `CoverageTests_dtservo` 一个二进制）
- 落点目录: `src/TestUnits/coverage/q05b_dtservo/`（已登记目录，前缀 `r8_`；未新建目录、未改 `src/TestUnits/coverage/CMakeLists.txt`，拆分表 `dtservo|WtDtServo|dir:q05b_dtservo` 自动收纳）
- 生产代码改动: **无**。`src/**`（除本 lane 三个新 .cpp）一字未动；`q05b_dtservo/q05_servo_wtdatamgr_test.cpp` 的 4 条既有用例未改一字，本波只做互补（不重复 `FreshInstanceGuardsReturnDefaults` 已打的构造初值/`update_bars` 守卫/`clear_cache` 取消分支）。
- 同名类隔离: 只用 `src/WtDtServo/ParserAdapter.h`（2 参构造 `:28`）与 `src/WtDtServo/WtHelper.h`（`class WtHelper` 静态目录工具），未 include `WtDtCore/ParserAdapter.h`（3 参）/`WtDtCore/WtHelper.h`；`WtPorter/WtBtPorter/WtDtPorter` 的同名 C 入口一概未引用。
- 门禁（真实构建口径，非自造 flags）:
  `cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests_dtservo.dir/q05b_dtservo/q05_servo_wtdatamgr_test.cpp.o | tail -1`
  → 去掉 `-o X -c Y` 后对三个新 .cpp 各加 `-fsyntax-only` 执行；**3/3 exit 0**（该目标带 `-Wall -Wextra -Werror -fprofile-instr-generate -fcoverage-mapping`，零告警）。
- 用例总数 **37**：**启用 32**，`DISABLED_` 5（5 条都会真崩宿主，触发条件逐条写在用例注释里）。不起线程、不 sleep、不加载任何插件 DLL、不建 socket/shm。
- 替身复用: 建块工具（`make_tmp_dir`/`write_file`/`loadCfg` + 空模块目录钉死 `WtHelper::set_module_dir`）沿用 `q05_dtsrv/q05_dtcore_datamgr_test.cpp` 与 `q05b_dtservo/q05_servo_wtdatamgr_test.cpp` 的写法；新增的唯一建块是 `WtDtRunner` 的录制回调对（`FuncOnTickCallback`/`FuncOnBarCallback`，`src/WtDtServo/PorterDefs.h:62`、`:72`），因为本波 30+ 条断言全靠它才「可证伪」。
- 基线数据口径（期望值来源）: sessions/commodities/contracts/hots/seconds/holidays 六份 JSON 的最小样例结构照抄 `dist/common/*.json`；`category:1` 对应 `CC_Future`（`src/Includes/WTSTypes.h:20-23`，`CC_Stock` 是 0），因此 `isStock()` 为假（`src/Includes/WTSContractInfo.hpp:80-83`）；分月码标准化期望串 `CFFEX.IC.2204` 来自 `src/Share/CodeHelper.hpp:303-332`（pid 长度 2 + 4 位月串原样保留）；复权后缀期望串用 `SUFFIX_QFQ='-'` / `SUFFIX_HFQ='+'`（`CodeHelper.hpp:28,31`）；因子 1.0 来自 `src/WTSTools/WTSHotMgr.cpp:257`（`oldclose==newclose`）与 `:61-72`。

## 1. `src/TestUnits/coverage/q05b_dtservo/r8_parseradapter_test.cpp`（351 行 / 13 例，13 启用 + 0 DISABLED）

| 用例（行号=测试文件内） | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `InitWithNullCfgRejectsAndKeepsIdEmpty` L88 | `src/WtDtServo/ParserAdapter.cpp:30-38`、`:82-90`、`:40`、`:293-296` | ctor 五个 NULL 初值；`cfg==NULL` 早退；`_id=id`(:87) 排在判空之后 ⇒ `id()` 仍是空串；`getBaseDataMgr` 透传同一指针 |
| `InitWithEmptyModuleRejectsButStillBindsIdAndCfg` L99 | `:87-98`、`:89-90`、`:221-222` | `module` 为空 ⇒ `:97-98` 拒绝（此时 `_cfg` 已 retain）；第二次 init 走 `_cfg!=NULL` 拒绝分支，但 `id` 已被覆盖（BUG-R8-006②，断言 `id()=="pa_second"`）；`_parser_api` 仍 NULL ⇒ `run()` false |
| `InitWithUnloadableModuleReturnsFalse` L121 | `:100-113`、`:102-107`、`:205-217` | `DLLHelper::wrap_module` 后 `StdFile::exists` false ⇒ 退化到 `<moduledir>/parsers/…` 二次拼装；`load_library` 失败 ⇒ `:110-113` 报错返回 false；随后 `release()` 走 `delete NULL` 分支不崩 |
| `InitExtWithNullApiIsRejected` L141 | `:44-47` | `initExt` 的 `api==NULL` 早退，`_id` 不赋值 ⇒ `id()==""` |
| `RunOnFreshAdapterIsFalseAndReleaseIsSafe` L149 | `:219-226`、`:205-217` | `_parser_api==NULL` ⇒ run false（不调 `connect()`）；`_remover==NULL` ⇒ `delete _parser_api`；重复 release 幂等 |
| `HandleQuoteIgnoresNullAndZeroDateTicks` L164 | `:276-283` | `quote==NULL`；新建 tick 的 `action_date/trading_date==0`（`WTSStruct.h:150-151` + `WTSDataDef.hpp:1114-1117`）；日期补齐后 `_dt_runner==NULL` ⇒ 不转发，代码串与 price 均不动 |
| `HandleQuoteAfterReleaseStopsForwarding` L192 | `:205-207`、`:276-283`、`:235-236`、`:250-251`、`:264-265`、`:285-288` | `_stopped=true` 后 quote/transaction/ordDtl/ordQue/parserLog 五条「停表先返」守卫；停表时明细回调传 nullptr 也不解引用 |
| `HandleParserLogAliveBranchIsSafe` L216 | `:285-291`、`:228-231` | 未停表 ⇒ `WTSLogger::log_raw_by_cat("parser", ...)`（简报点名的 :290）；`handleSymbolList` 空实现；末尾 `run()` false 作为状态未被日志改写的旁证 |
| `DetailHandlersReturnOnZeroDates` L229 | `:233-246`、`:248-260`、`:262-274` | 三个明细入口的 `actiondate()==0 \|\| tradingdate()==0` 早退（`create(const char*)` 只填 code，`WTSDataDef.hpp:1192-1197/1228-1233/1265-1270`） |
| `DetailHandlersReturnOnUnknownContract` L254 | `:239-244`、`:253-258`、`:267-272` | 日期非 0 后 `_bd_mgr->getContract(code, exchg)` 返回 NULL ⇒ 三条 `contract==NULL` 早退（`_bdMgr` 未装载任何合约，`WTSBaseDataMgr.cpp:161-173` 空表语义） |
| `AddAdapterRejectsNullPtrAndEmptyId` L298 | `:311-315`、`:328-336`、`:84` | 空 `shared_ptr` ⇒ `adapter==NULL`；`strlen(id)==0`；`size()` 恒 0；`getAdapter` 未命中返回空指针 |
| `AddAdapterKeepsFirstOnDuplicateId` L314 | `:316-325`、`:301-309`、`:328-336`、`:339-347` | 同名 ⇒ `:317-321` 拒绝且第一个不被替换（两条 EXPECT 分别钉住 first/second 指针）；run 扇出 + release 清空后 `size()==0` |
| `ManagerReleaseOnEmptyMapIsSafe` L335 | `:339-347`、`:301-309`、`:219-222` | 空表 run（`"{} parsers started"` 日志）与空表 release；随后登记 1 个无 api 的适配器再 run/release |

本文件未覆盖：`:120-135`（真加载 parser 插件取 `createParser`/`deleteParser` 符号）、`:138-147`（`filter` 逗号切分，位置在模块加载失败之后 ⇒ 无插件不可达）、`:149-200` 的 `subscribe`/`registerSpi` 全段与 `:156-175` 的六种过滤器匹配、`:52-78` 的 initExt 成功段。这些都需要一个活的 `IParserApi`：`init()` 只能从 DLL 里 `createParser()` 拿对象，`initExt()` 虽然接受外部 `IParserApi*`，但本二进制里没有可链接的 parser 实现（`libParserCTPOpt.dylib` 等会拉起 CTP 线程/socket，且同名 C 入口属别的二进制）。合计约 **110 行**，留给主控裁定（需要接缝：把 `createParser` 工厂或 `initExt`+可控 `IParserApi` 替身纳入测试目标）。

## 2. `src/TestUnits/coverage/q05b_dtservo/r8_wtdtrunner_test.cpp`（536 行 / 16 例，13 启用 + 3 DISABLED）

| 用例（行号=测试文件内） | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `UninitializedRunnerGuardsEveryQueryEntry` L199 | `src/WtDtServo/WtDtRunner.cpp:28-32`、`:202-208`、`:242-248`、`:284-290`、`:300-306`、`:311-317`、`:351-357`、`:367-373`、`:405-408`、`:645-648` | 7 个取数入口的 `!_is_inited` 守卫（简报点名的 :206/:246/:288/:304/:315/:355/:371 七条 error 日志）；`start()` 空表；`clear_cache()` 转发 |
| `InitializeWithMissingConfigFileStopsBeforeBasefiles` L218 | `:43-64`、`:52-53`、`:58-64` | `WTSCfgLoader::load_from_file` 返回 NULL ⇒ `:61-62` 两条日志后早退；回调已在判空前绑定；后续守卫仍生效 |
| `InitializedRunnerForwardsStdAndHotTick` L239 | `:66-71`、`:73-148`、`:150-153`、`:182-189`、`:192-200`、`:410-459`、`:461-476`、`:557-605` | `disable_dump` 真分支；basefiles 六段（session/commodity 字符串形态/contract/holiday/hot/second）全装载 ⇒ `:77/:115/:121/:127` 四条 info；`:130-148` 热点码回写（`WTSHotMgr.cpp:263-264` 令 `CFFEX.IC2204` 成为 HOT）；`proc_tick` 命中合约 + `rawMonthCodeToStdCode` 改写 + `:445-458` 热点二次转发 ⇒ 2 条回调（可证伪主断言） |
| `QfqFlagSubscribesWrappedCodeAndReusesSameTick` L267 | `:585-603`、`:461-482` | `sub_tick` 尾字符 `SUFFIX_QFQ('-')` ⇒ flag=1、键截断；`trigger_tick` flag==1 分支拼回 `CFFEX.IC.2204-` 且直接用原 tick 结构（价格不变） |
| `HfqFlagGoesThroughCopyAndKeepsSourceTickUntouched` L286 | `:593-601`、`:483-503`、`WtDataManager.cpp:490-504` | flag==2 ⇒ `:485-502` 复制 newTick + `get_exright_factor` 复权乘法；`WtDtRunner.cpp:490` 走的是 `_data_mgr` 的期货分支（`_hot_mgr` 已绑定）；断言源 tick price 仍 100.0（证明乘法发生在副本上） |
| `InnerSubscribeDoesNotFireCallbackAndReplaceIsPerTable` L307 | `:557-605`（`:559-578` 内表 / `:581-603` 外表）、`:510-515` | 内/外两张表各自 `bReplace`（`:562-563` / `:582-583`）；内部订阅只进 `_tick_innersub_map`，外发计数不增（回调数 1→2→2 三点可证伪） |
| `UnmatchedCodeIsFilteredOutBySubTable` L334 | `:461-467`、`:510-514` | 订阅表命不中 ⇒ 外表 `sit==end` 与内表 `sit==end` 两条 continue/return；回调 0 次 |
| `TriggerBarDispatchesOnlyWhenCallbackBound` L348 | `:637-643`、`:52-53` | `_cb_bar!=NULL` ⇒ 透传 code/period/bar 三个实参；另一 runner 用 `cbBar=nullptr` 初始化 ⇒ `:639-640` 早退，计数不增 |
| `MissingDataSectionStillCompletesInitialization` L372 | `:150-153`（`config->get("data")==NULL`）、`:192-195`、`:182-183`、`:187-189` | 无 data 段 ⇒ `initDataMgr` 早退；无 parsers 段 ⇒ `:183` 告警；能跑到 `:189` 才可能触发回调（proc_tick 1 次为证据） |
| `SecondInitializeIsRejectedAndStatePreserved` L390 | `:46-50`、`:52-53` | `_is_inited` 已 true ⇒ 第二次 initialize（且传的是不存在的配置 + `nullptr` 回调）只能记 error 后返回；回调未被置空 ⇒ proc_tick 仍 1 次 |
| `UnloadableParserArrayStillCompletesStartup` L411 | `:152-153`、`:177-181`、`:378-403`、`:405-408` | parsers 数组 ⇒ `initParsers`；active=true 但模块加载失败（`ParserAdapter.cpp:110-113`）仍 `addAdapter`；`start()`→`ParserAdapterMgr::run`→`ParserAdapter::run` false，**不调 connect()**（不起线程） |
| `InactiveAndAutoIdParserEntriesAreBothHandled` L434 | `:380-399`（`:383-384` continue / `:390-395` auto id） | `active:false` 跳过；缺 `id` ⇒ `auto_parser_1000`（函数级静态，见 BUG-R8-007）；initialize 仍完整收尾 |
| `ParserConfigGivenAsExternalFileOrMissingFile` L455 | `:155-171`、`:172-175` | parsers 为字符串且文件存在 ⇒ 读外部 `parsers.json` 后 `initParsers(var->get("parsers"))`；文件不存在 ⇒ `:174` error 分支。两个 runner 各验一次链路仍可用 |
| `DISABLED_InitializeWithoutBasefilesSectionSegfaults` L499 | `:73-74` | 触发条件：根配置缺 `basefiles` ⇒ `cfgBF==NULL` 后 `cfgBF->get("session")` 空指针（BUG-R8-001） |
| `DISABLED_SubBarWithoutReaderSegfaults` L514 | `:607-635`（`:632-634`） | 触发条件：`sub_bar` 的周期解析（m/m5/d 三条 kp 分支）后 `_data_mgr.subscribe_bar` 在 `WtDataManager.cpp:516` 解引用 NULL `_reader`（BUG-R8-003） |
| `DISABLED_GetBarsByRangeWithoutReaderSegfaults` L529 | `:210-239`（`:239` 转发） | 触发条件：已初始化 runner 上 `get_bars_by_range("…","m1",…)` ⇒ `WtDataManager.cpp:203` 解引用 NULL `_reader`（BUG-R8-003） |

## 3. `src/TestUnits/coverage/q05b_dtservo/r8_wtdatamgr_sinks_test.cpp`（223 行 / 8 例，6 启用 + 2 DISABLED）

| 用例（行号=测试文件内） | 生产文件:行段 | 打到的分支 |
| --- | --- | --- |
| `FreshInstanceExposesNullSinks` L101 | `WtDataManager.cpp:29-36`、`WtDataManager.h:95`、`:100`、`WtDataManager.cpp:490-493` | 构造六成员置空；两个 sink 访问器初值 NULL；`commInfo==NULL` ⇒ 基准因子 1.0（不碰 `_reader`） |
| `InitBindsRunnerSinksEvenWhenStoreIsMissing` L111 | `:91-105`、`:49-52`、`:607-612`、`:107-110` | `runner!=NULL` ⇒ `_bd_mgr/_hot_mgr` 取 runner 的两个引用（两条 EXPECT_EQ 是 :93-98 的唯一可证伪证据）；`align_by_section=false` 分支；`initStore(NULL)` ⇒ init 返回 false；随后 `clear_cache` 取消分支 + `reader_log` |
| `InitWithAlignFlagTrueTakesSecondBranch` L128 | `:100-102`、`:104` | `getBoolean("align_by_section")` 真分支（`:102` 的 yes/no 三元两条都被这两个用例分掠） |
| `InitWithoutRunnerLeavesSinksNull` L140 | `:93-98`（false 分支） | `runner==NULL` ⇒ `:94` 不进，两个 sink 仍 NULL（与 L111 成对，排除「访问器只是恒 NULL」的替代解释） |
| `NonStockCommodityTakesHotRuleFactorBranch` L151 | `:91-104`、`:490-504` | `commInfo->isStock()` 为假（`category:1==CC_Future`）⇒ `:498-500` 取 `_hot_mgr->getRuleTag`；未装载切换规则 ⇒ `WTSHotMgr.cpp:36-37` 返回 ""，`:500` strlen==0 ⇒ `:504` 返回 1.0；带 `+` 后缀时 `:39-41` 的截尾分支同结果 |
| `InitStoreWithEmptySectionIsNotEvenAttempted` L174 | `:49-52`、`:104` | 空对象 `{}` 的 data 段：`cfg->get("store")` 仍返回 NULL ⇒ 走 `:51-52`，**不进**模块名回落（与 L194 的 DISABLED 形态互斥） |
| `DISABLED_InitStoreWithDefaultModuleNameDerefsNullReader` L194 | `:54-56`、`:58-59`、`:81-88` | 触发条件：`{"store":{}}` ⇒ module 回落默认名 `WtDataStorage`，模块目录为空 ⇒ `load_library` NULL，`:87 _reader->init` 空指针（BUG-R8-002；q05 波次的 DISABLED 是「显式写了错名」的形态） |
| `DISABLED_TickSliceDelegationWithoutReaderSegfaults` L212 | `:119-122` | 触发条件：`init` 未成功时 `get_tick_slice_by_date` 无条件 `_reader->readTickSliceByDate`（BUG-R8-003 的代表入口） |

## 未进入的面（含估计行数）

| 位置 | 估计面值行数 | 为什么进不去 | 需要的接缝 |
| --- | --- | --- | --- |
| `WtDataManager.cpp:112-117,119-122,124-129,131-136,138-143,145-156,158-189,191-197,199-335,337-482,484-488,507-558,572-604` | **452**（实测：`awk` 逐段求和，占本文件 missed 434 的几乎全部） | 全部解引用 `_reader`；`_reader` 只能由 `initStore` 从 `libWtDataStorage.dylib` 里 `createRdmDtReader()` 得到，而 `WtRdmDtReader::init`（`src/WtDataStorage/WtRdmDtReader.cpp:87`）无条件起 5 秒巡检线程（`:76-77` 的早退在起线程之前，且 `initStore` 永远传非空 cfg，拿不到「无线程 reader」）。缓存命中/增量更新、区间二分定位、交易日边界（`WTSBaseDataMgr::getBoundaryTime`）、按日期取数、实时 K 线合成与 `_rt_bars` 扇出 ⇒ 全部不可达 | 允许注入 `IRdmDtReader*`（setter 或 `initStore` 接受工厂），或给 `WtRdmDtReader::init` 加 `cfg` 开关跳过巡检线程。**留给主控裁定** |
| `WtDtRunner.cpp:210-239,258-269,276-281,292-297,308,319-348,359-364,375,607-635` | **121** | 同上：这些入口的周期解析结果只为了转交 `_data_mgr`，一交到就命中 NULL `_reader`。`get_bars_by_date` 的分钟周期主体（`:258-269,276-281`）与 `sub_bar` 的 kp 三分支（`:609-631`）在 `_is_inited=true` 后全部不可达；未初始化时又被守卫截住，只有 `:270-274` 的「非分钟周期直接 NULL」一条能测（已在 L239 用例里钉住） | 同上。**留给主控裁定** |
| `ParserAdapter.cpp:52-78,120-135,138-147,149-200` | **约 110** | `init`/`initExt` 的成功段需要一个活的 `IParserApi`：`init` 只能从插件 DLL 取（本二进制不可链接、真插件会起线程/socket），`initExt` 需测试自带 `IParserApi` 替身——本波未做，因为替身要实现 `registerSpi/init/subscribe/release/connect` 六个纯虚函数并驱动 `WTSArray` 合约集，属另一条工作线；`filter` 六字段匹配（`:156-175`）也在模块加载失败之后才可达 | 在 `q05b_dtservo` 里放一个可控 `IParserApi` 替身 + 一份含 `filter` 的配置（可在下波补 8-10 条） |
| `WtDtRunner.cpp:68-70` | 3 | `#ifdef _MSC_VER` 的 `CMiniDumper::Enable`，macOS 口径不参与编译，非缺陷 | — |
| `ParserAdapter.cpp:197-200` | 4 | **恒真守卫导致的死代码**（`:128-132` 已在 `_parser_api==NULL` 时 return false），不是接缝问题 | 见 BUG-R8-006① |

## 交付自检
- 三个文件都取同一门禁命令行（同目录既有对象 `q05_servo_wtdatamgr_test.cpp.o`），**3/3 exit 0**。
- 用例内不出现：线程、`sleep`、socket/shm、`#define private public`、对 `_reader`/`_parser_api` 的越权访问（`WtDataManager` 的私有成员一律经 `init(cfg, &runner)` + 两个 public sink 访问器间接观测）。
- 5 条 `DISABLED_` 全部注明触发条件与对应 BUG 编号，未放宽任何断言；`src/**` 无改动。
