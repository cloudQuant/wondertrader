# T26 manifest — CTPLoader 数据采集器（TraderSpi.cpp / CTPLoader.cpp）

- 波次: 迭代01 · lane R26
- 用例目录: `src/TestUnits/coverage/t_CTPLoader/`（前缀 `t26_`；未动 `t6_*`、`gate_placeholder_*`、`coverage/CMakeLists.txt`）
- 测试目标: `CoverageTests_t_CTPLoader`（产品 TU：`src/CTPLoader/CTPLoader.cpp` 290 行 + `src/CTPLoader/TraderSpi.cpp` 867 行）
- 门禁实测（唯一合法口径，`docs/迭代计划/迭代01/tools/case_runner.sh`，一条用例一个进程，60s 上限）:
  - build: `cmake --build build-cov --target CoverageTests_t_CTPLoader` → **exit 0**（含 `-Wall -Wextra -Werror`）
  - 两轮全新运行目录（/tmp/t26_e、/tmp/t26_f）：**48/48 PASS**（本 lane 32 条启用 + t6 存量 15 条 + gate 占位 1 条），0 FAIL / 0 CRASH / 0 HANG / 0 NORUN
  - 另对 worker 族用例做了 56×8 并行 stress（filter 覆盖 czce_*/last_instrument*/fee_filter*/login_*/incremental_*）：**56/56 rc=0**
- 用例合计: **32 条启用 + 3 条 `DISABLED_`**（DISABLED 全部带实测过的「不能启用」证据，见 §四）
- 缺陷: 10 条（Major 7 / Minor 3），全部 `- 状态: 待审核` → `docs/迭代计划/迭代01/bugs/T26_ctploader.md`
- 未改动: 任何 `src/**` 生产代码；没有 `#define private public`；没有降低告警门槛。

## 夹具与接缝（本轮立起来的，后续 lane 可直接复用）

1. **`t26_ctp_api_stub.hpp`**：`CThostFtdcTraderApi` 的 107 纯虚机械替身（签名逐条对过
   `src/API/CTP6.6.9/ThostFtdcTraderApi.h`），5 个查询/登录入口把请求结构体**拷贝**进成员供断言。
   `pUserApi`（`CTPLoader.cpp:27` 全局裸指针）指到替身即打通 OnFrontConnected→ReqAuth→
   OnRspAuthenticate→ReqUserLogin→OnRspUserLogin 全链（顺序按 FAIL裁定 **A-07**：
   FRONT_ID/SESSION_ID/ORDER_REF 只由 `OnRspUserLogin`（L158-177）写，前面任何一步都不写）。
2. **`t26_ctploader_common.hpp`**：生产全局（BROKER_ID/CLASSMASK/_contracts/_commodities/_fees/_margins/MAP_* 等，
   命名空间作用域非 static，extern 直链）+ `setup_defaults()`（把这些「run() 才置定、否则不确定」的全局逐用例钉死）+ 轮询等待 + 两种 stderr 工具。
   **血泪条款**：凡起过 AppendQuery worker 的用例，stderr 只能用 fd 级 `FdRedirectGuard`（dup2），
   禁用 `CoutCapture`（rdbuf 换体与 worker 并发写同一个 ostringstream → libc++ 堆破坏，
   stress 40 分 1 次 SIGABRT/SIGTRAP 实测复现，崩溃报告栈为
   `malloc "POINTER BEING FREED WAS NOT ALLOCATED" ← basic_string::__grow_by ← ostreambuf_iterator::__pad_and_output`）。
3. **fork 隔离 exit(0)**：三个终局出口（L542-547、L570-575、L587-592）放进 fork 子进程，
   父进程 `waitpid` 断言「子进程以退出码 0 正常结束」（exit(0) 本身就是被测契约），再回读磁盘 JSON 做可证伪断言。
   fork 前不建线程（队列路径一律被 bIsLast=false / SET_FILTERS / QRYFEES=false 挡住）。
4. CZCE 用例需要 `_trading_day`：该成员无构造初始化、只由 OnRspUserLogin 写，故经替身登录后**故意泄漏 spi**
   （析构会挂死，BUG-T26-001）；门禁一条一进程，泄漏只活到该用例结束。

---

## 一、`t26_ctploader_flow_test.cpp`（suite `t26_ctploader_flow`，12 启用 + 2 DISABLED）

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `front_connected_builds_auth_request_from_config_globals` | `TraderSpi.cpp` L118-123 + L125-136（ReqAuth 直发、不落队列） | 替身捕获的请求体 BrokerID/UserID/AppID/AuthCode 逐字段等于配置全局；iRequestID 0→1；stderr 含 `Requesting authentication: succeed` |
| `auth_success_last_triggers_user_login_with_password` | L138-145 → L147-156（ReqUserLogin） | 成功+last 才发登录；捕获 req 的 BrokerID/UserID/Password；请求编号 2；日志 `Requesting user login: succeed` |
| `auth_non_last_or_error_never_reaches_login_request` | L141 守卫两侧 + L860-867（IsErrorRspInfo） | bIsLast=false / ErrorID=502 两种都 0 次 ReqUserLogin、iRequestID 不消耗 |
| `login_success_writes_session_refs_and_queues_instrument_query` | L158-177（会话三元组写序）+ L179-188 + L230-273（worker 首任务） | FRONT_ID=321/SESSION_ID=654/ORDER_REF="100"（MaxOrderRef 99+1）；替身收到 ReqQryInstrument 且 reqid=1；worker 消费完成才退场 |
| `login_non_last_response_keeps_session_untouched` | L162（`bIsLast &&` 短路侧） | 哨兵值 777/888/"untouched" 原样保留、0 次队列 |
| `incremental_login_reloads_commodities_and_contracts` | L173 → L595-682（LoadFromJson 主体） | 品种 9 字段逐项读回（session 只认 MAP_SESSION，文件 session 被忽略 L619）；合约 12 字段读回；**期权块嵌套写/平铺读全部读丢**（OT_None/underlying 空/strike 0/scale 0）+ altcode 无读取语句（BUG-T26-003/004/005 的行为钉子） |
| `incremental_login_with_missing_files_still_reaches_query` | L597-603 / L639-640（两处 load NULL 短路侧） | 缺文件不拦登录链后半段：映射保持空 + ReqQryInstrument 仍发出 |
| `last_instrument_response_with_fees_switch_queues_both_rate_requests` | L527-541（QRYFEES 的 bIsLast 不 exit）+ L190-208 + L210-228（两个任务体） | 替身收到 commission+margin 各 1 条；BrokerID/InvestorID/InstrumentID(=m_strCode, altCode 空)/ExchangeID 逐字段；margin 的 HedgeFlag 恒 `THOST_FTDC_HF_Speculation` |
| `fee_filter_only_queues_contracts_matching_code_or_product` | L534-536（SET_FILTERS 三键 find + continue） | 两条合约只放行 product 命中的 au2512：多等一个限速窗后 comm/margin 各自 size 恒为 1 |
| `commission_response_switches_between_volume_and_money_modes` | L550-568 + decimal::eq(1e-6) 语义 | 0→按量三元组 3/4/5；0.0001→按额 0.0001/2/3；5e-7→**非零但被判 0 走按量**（eps 语义钉子）；`_byvol` 布尔逐一断言 |
| `margin_response_records_long_short_money_ratios` | L578-585 | `_margins["rb2510"]` 的 _long=0.07/_short=0.09（EXPECT_NEAR 1e-9） |
| `gbk_error_message_is_transcoded_to_utf8_before_report` | L27-36（encode_text→ChartoUTF8）+ L860-867 | stderr 出现「中文」的 UTF-8 字节 E4B8AD/E69687 且**不含**原 GBK 字节 D6D0CEDC（t6 只证过纯 ASCII） |
| `DISABLED_t26_ctploader_flow_hang.destructor_joins_forever_after_queue_drained` | L230-273 worker 内层空转（L243-246 不回看 `_stopped`）+ `TraderSpi.h` L24-29 join | **实测**：打开后 25s 不退出需 kill -9（rc=137），启用必判 HANG → BUG-T26-001；这也是本轮所有触发队列的用例「spi 不析构」的原因 |
| `DISABLED_t26_ctploader_flow_crash.fill_czce_code_on_letters_only_scans_past_terminator` | L318-324（`for(;;)` 逐字符找数字、不判 `'\0'`） | 越界读；实测本机恰好撞到数字字节 2ms 返回垃圾（不崩但结果不可复现），换内存布局即段错误风险 → 不可写可证伪断言 → BUG-T26-002 |

## 二、`t26_ctploader_instrument_test.cpp`（suite `t26_ctploader_instrument`，14 启用）

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `classmask_gates_future_option_and_other_classes` | L357-368（isOption/isFuture/isGranted 两侧 + do-while 空转） | mask=0 期货拒、mask=1 期权拒、mask=2 期货拒；Spot/Combination/EFP 在 mask=7 全拒；对照：mask=2 期权准入 1 条 |
| `shfe_future_maps_contract_and_commodity_fields` | L368-452 + L454-517（SHFE 侧） | 合约 16 项（name="螺纹钢2510" 来自 pname+substr、code/alt 空/exchg/product、四限量、日期 20170101/20251015、双保证金率、optType=OT_None、strike/scale=0）+ 品种 12 项（CM_CoverToday/PM_Limit/TM_Both/CC_Future/CNY/session/volscale=10/precision=0） |
| `ine_takes_shfe_branch_and_missing_session_warns` | L469-471（warning 支）+ L485-503（交易所分侧） | INE=平今+限价、DCE=开平+双模式；stderr 含 `No session configured for DCE.au` 且 session 留空 |
| `price_tick_precision_ladder_and_zero_volscale_fallback` | L474 + L506-515 | tick 1/0.5/0.05/0.005/0.0005 → precision 0/1/2/3/4 五档逐品种；VolumeMultiple=0 → volscale=1 |
| `product_name_falls_back_to_exchange_scoped_key` | L370-376（兜底键 `EXCH.PID`） | MAP_NAME 只有 "DCE.i" 时合约名 "铁矿石2509"、品种名 "铁矿石" |
| `commodity_row_created_once_per_exchange_product` | L455-457（find 命中即跳过） | 同品种第二条（tick 改 0.5）不回写 precision(仍 0)/pricetick(仍 1.0)，合约却有 2 条、第二条名 "螺纹钢2601" |
| `onlyincfg_drops_unknown_product_and_logs_instrument` | L379-385（ONLYINCFG break 侧） | 无名分合约不进两张表且 stderr 有 `OnRspQryInstrument: GFEX.si512`；补名分后同合约准入 |
| `autoname_future_strips_month_and_transcodes_gbk_name` | L386-391 + L414-418 + L458-461（GBK→UTF8 双侧） | InstrumentName 给 GBK「中文2512」：合约名落 UTF-8「中文2512」、品种名 extractProductName 截尾后落 UTF-8「中文」 |
| `autoname_option_keeps_full_instrument_name` | L386-391 期权侧 + L440 | 无名分期权 pname=InstrumentName 全名；optType=(OptionType)'1'=OT_Call；underlying 非 CZCE 原样 "m2509" |
| `spot_option_maps_to_spot_option_category` | L399-406（substr(underlyPID)）+ L472（wrapCategory） | cname="股指期权2510-C-4000"（underlyPID="IF" 的拼接语义钉子）；category=CC_SpotOption；CFFEX→OpenCover；optType=OT_Put、scale=0.5 |
| `duplicate_instrument_is_skipped_with_log` | L349-355（已存在即 return） | 第二条改限量后：`already exists, skipped` 日志 + size 不涨 + 首条 m_maxMktQty=500 未被改成 888 |
| `margin_sentinels_dblmax_and_fltmax_become_zero` | L109-115（checkValid）+ L451-452 | DBL_MAX/FLT_MAX 两哨兵都归 0（EXPECT_NEAR） |
| `czce_future_code_is_expanded_and_altcode_kept` | L421-425 + L441-444（underlying 也补位）+ fillCzceCode L303-340 | 登录后 _trading_day=20251027：code "SR510"→"SR2510"、alt="SR510"、underlying "SR510"→"SR2510"、名 "白砂糖510"、CZCE→OpenCover/PM_Both |
| `czce_option_expands_code_and_strikes_through` | L400-406 + L421-424 + L441-446 | code "SR510CS5600"→"SR2510CS5600"、cname="白砂糖510CS5600"（substr("SR") 语义）、strike 5700.5、scale 2.5、OT_Call |

## 三、`t26_ctploader_names`（同文件，2 启用）

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `fill_czce_latches_decade_on_first_call_2025` | L303-340（static yearFlag 首调用锁存 + 分支 A） | 空码提前返回**不消耗锁存**；"SR510"(20251027)→"SR2510"；锁存后 "MA412"(换 tdate 也无效)→"MA3412"（借下个十年位 3）；"CF901"→"CF2901" |
| `fill_czce_decade_zero_prefers_current_decade_digit` | L326（`yearFlag==0` 短路侧） | 首调用 tdate=20200106：yearFlag=0 → 一切码走 yearPref 分支："CF012"→"CF2012"、"SR510"→"SR2510" |

## 四、`t26_ctploader_dump_test.cpp`（suite `t26_ctploader_dump`，4 启用 + 1 DISABLED）

| 用例 | 生产文件:行段 | 断言的可证伪点 |
| --- | --- | --- |
| `last_instrument_response_dumps_commodities_and_contracts` | L527-547（!QRYFEES 的 bIsLast 终局）+ DumpToJson L734-844 | 子进程 exit 码 0；commodities.json：SHFE.rb 的 10 字段（含 session/holiday、**currency 不落盘**的行为钉子）+ DCE.m category=2/covermode=0；contracts.json：rb2510 的 13 字段 + OT_None 无 option 成员 + 期权嵌套块四字段 |
| `czce_contract_is_keyed_by_expanded_code_with_altcode_kept` | L816（键=m_strCode）+ L786（altcode） | 直接装配映射：CZCE 下键是四位码 "SR2510"，altcode 字段="SR510" |
| `margin_response_dumps_fees_to_fixed_relative_name_with_product_fallback` | L587-592（margin 终局）+ DumpFees L684-731（相对分支 L717-721 + 品种兜底 L696-698） | fees.json 落盘而 `custom_fees.json` **不存在**（硬编码名，BUG-T26-006）；键 "SHFE.ao2512" 的 open/close/closetoday 取自**品种级**费率 0.5/0.6/0.7、byvolume=true、margin=多头 0.07 |
| `commission_response_with_absolute_feesfile_dumps_to_that_path` | L570-575（commission 终局）+ L713-716（绝对分支） | 绝对 FEES_FILE 原样生效；行值取按额三元组、byvolume=false、margin=0.11；同出口顺带落的 commodities/contracts 存在 |
| `DISABLED_t26_ctploader_dump_crash.fees_dump_of_unknown_margin_code_dereferences_end_iterator` | L696-699（两次 find 皆 miss 后无 end() 判定直接 `it->second`） | 实测：不崩但费率行取到未定义值，且随后 `exit(0)` 直接杀进程（rc=0、"PASSED 0 tests"）——双重不可用可证伪断言表达 → BUG-T26-007 |

## 五、留白面（没覆盖的行 + 原因 + 量级）

| 位置 | 行数量级 | 为什么进不去 |
| --- | --- | --- |
| `CTPLoader.cpp` L206-291（standardisePath 之后的全部：SET_FILTERS 解析 L208-218、mapfiles 装载 L220-254、`__APPLE__` 直取厂商 `CreateFtdcTraderApi` + `RegisterFront/Init/Join` L256-288） | ~80 行 | L201-204 的五必填守卫是「全有才往下」；一过守卫，APPLE 分支**没有函数指针 seam**（L258 直接取厂商符号），必起厂商线程连柜台 → 门禁禁止（t6 的 `DISABLED_..._run_net` 同判）。`g_ctpCreator` 虽是全局、可在外部赋值指向替身，但赋值时机只能在 run() 之后，无法阻止 L256-278 执行。mapfiles/SET_FILTERS 两段与终段物理不可分 → 整块留白。**下一波若要吃这块，需求是给 run() 加「注入 creator」的测试接缝或 OBJECT 化**，属产品侧改动，不在本 lane 授权内。 |
| `CTPLoader.cpp` L78-80（_WIN32 分支）、L115-116/L150-152（WIN dll 默认名）、L164（WIN 分支） | ~8 行 | 平台死分支，macOS 编译不到。 |
| `TraderSpi.cpp` L298-300（wrapCategory default throw） | ~3 行 | 不可达：wrapCategory 只在 isGranted（期货/期权/现货期权）块内被调，三种入参都有映射。 |
| `TraderSpi.cpp` L107（`prod_set` 定义后从未使用）、L349（`fullInstId` 算了不用） | ~2 行死代码 | 被执行覆盖但无语义可断言；已在 BUG 文档备注（非缺陷级，算代码卫生）。 |
| `TraderSpi.cpp` L603-635 的「getUInt32/getCString 命中缺键」组合分支 | ~10 行 | 已在 f6 用平铺缺键夹具踩到默认值路径；WTSVariant 内部语义属别的二进制，不在这里重复。 |
| OnRspUserLogin 的「成功应答但 pRspUserLogin==NULL」 | ~2 行 | 厂商契约不会发生；强行构造只会得到确定的段错误，与 t6 的 null-api 崩溃同类，不重复登记。 |

## 六、回执数字

- 启用 32 / DISABLED 3；build exit 0；两轮全新 gate 目录各 **48 PASS / 0 FAIL / 0 CRASH / 0 HANG / 0 NORUN**（48 含本目录 t6 存量 15 + gate 1）。
- worker 族 56×P8 stress 全绿；三条 DISABLED 的「不能启用」都用 `--gtest_also_run_disabled_tests` 实测过（HANG 25s kill rc=137 / 越界不崩但输出不可复现 / exit(0) 杀进程）。
- 最大留白 = `CTPLoader.cpp` L206-291（约 80 行，厂商创建/Init 无接缝）；`TraderSpi.cpp` 本轮后残余约 15 行（不可达/死代码为主）。
