# Q06 - WtPorter / WtBtPorter 实盘与回测 C 导出层疑似缺陷

> 本轮只写测试，不改任何生产代码。以下条目均由 q06 用例的工具结果/源码行号推导，
> 状态一律 `待审核`。

## Q06-001 `cta_get_last_entertag` 未命中句柄时返回空指针

- 状态: 待审核
- 严重级别: 中
- 位置: `src/WtPorter/WtPorter.cpp:839-845`（实盘）、`src/WtBtPorter/WtBtPorter.cpp:729-734`（回测）
- 现象: `CtaContextPtr ctx == NULL` 分支写的是 `return 0;`，即返回 `NULL` 的 `WtString`；
  同一族的 `cta_load_userdata` 在同样分支返回 `defVal`，`hft_buy/hft_cancel_all` 返回 `""`。
- 影响: 调用方（Python/其他语言绑定）通常直接对返回值做 `strlen`/`strncpy`，空指针即段错误；
  三族字符串入口的失败约定不一致。
- 期望: 与 `hft_*` 一致返回 `""`，或补一个默认值参数。
- 用例: `Q06PorterCapi.CtaStringGettersUnknownHandle`、`Q06BtPorterCapi.CtaEntriesRefuseWithoutMocker`
  （当前按实现现状断言「NULL 或空串」，未强行固化可疑行为）。

## Q06-002 引擎/回放器直取的导出入口没有 NULL 校验

- 状态: 待审核
- 严重级别: 高
- 位置: `src/WtPorter/WtPorter.cpp:859/874/885/896/907/1217/1228/1239/1411/1423/1745/1757/1769/1781`
  （`getRunner().getEngine()->...`）、`src/WtPorter/WtRtRunner.cpp:361-366`（`get_raw_stdcode` 内 `_engine->get_rawcode`）
- 现象: `getEngine()` 在 `config_porter()/run_porter()` 之前返回 NULL（`WtRtRunner.h:324`），
  但这些导出入口直接解引用；`cta_get_price / cta_get_day_price / cta_get_tdate / cta_get_date /
  cta_get_time` 与 sel/hft 同名入口、以及 `get_raw_stdcode` 全族无保护。
- 影响: 外部集成方在初始化前调用即崩进程，且这是对外 C API。
- 对照: 回测侧同名入口走 `replayer()`（成员对象，构造已把 `_cur_date/_cur_time/_price_map` 置空），
  因此 `src/WtBtPorter/WtBtPorter.cpp:744-790` 一族是安全的，两侧行为不对称。
- 用例: 收进 `Q06PorterCapi.DISABLED_UninitializedEngineGettersCrash`（不启用，避免崩整条用例）。

## Q06-003 `WtBtRunner::_hft_mocker` 未进构造初始化列表

- 状态: 待审核
- 严重级别: 高
- 位置: `src/WtBtPorter/WtBtRunner.cpp:47-88`（初始化列表含 `_cta_mocker(NULL)`、`_sel_mocker(NULL)`，
  缺 `_hft_mocker`）；成员声明 `src/WtBtPorter/WtBtRunner.h:635-638`
- 现象: `_hft_mocker` 是裸指针成员且构造未赋值。当前只有 `getRunner()` 的函数级静态实例
  依赖「静态存储零初始化」才恰好为 NULL；任何栈上/堆上 `WtBtRunner` 对象都会让
  `hft_get_position / hft_buy / hft_cancel ...`（`WtBtPorter.cpp:1470-1476/1900-1906` 等）
  对不确定指针做 `mocker == NULL` 判断后解引用。
- 影响: 未定义行为；`initHftMocker` 里的 `if (_hft_mocker) delete _hft_mocker;` 会删除野指针。
- 用例: `Q06BtPorterCapi.HftEntriesRefuseWithoutMocker` 把该前提写成显式
  `ASSERT_EQ(getRunner().hft_mocker(), nullptr)`，前提不成立即失败而不是踩 UB。

## Q06-004 `WtRtRunner::feedAdjFactors` 无任何守卫，`feedRawBars` 守卫对象错位

- 状态: 待审核
- 严重级别: 高
- 位置: `src/WtPorter/WtRtRunner.cpp:277-280`（`feedAdjFactors` 直接 `_feeder_fcts(_feed_obj, ...)`）、
  `src/WtPorter/WtRtRunner.cpp:283-292`（`feedRawBars` 判 `_ext_fnl_bar_loader == NULL`，随后调用 `_feeder_bars`）
- 现象: `_feeder_fcts/_feeder_bars` 只在 `loadAdjFactors/loadFinalHisBars/loadRawHisBars` 里被赋值，
  导出层 `feed_adj_factors`（`WtPorter.cpp:209`）在未注册 loader 或未 load 之前调用即跳空函数指针；
  且 `feedRawBars` 用「fnl loader 非空」推断「_feeder_bars 有效」，`register_ext_data_loader` 之后、
  任何 `load*` 之前同样为空。
- 对照: 回测侧 `src/WtBtPorter/WtBtRunner.cpp:182-213` 三个 feed 函数都先判
  `_ext_*_loader == NULL` 再返回（虽然仍有同样的错位，但至少不会在未注册时崩溃）。
- 用例: 正向路径在 `Q06PorterRunner.FeedRawBarsUsesCachedFeeder`（先 load 再 feed）；
  未注册路径在 `Q06PorterRunnerNoLoader.LoaderFreePathsReturnFalseOrWarn`（只覆盖 feedRawBars 有守卫的一支）。
  `feed_adj_factors` 未注册即调用属崩溃类，未纳入启用用例。

## Q06-005 回测侧导出入口忽略 `CtxHandler`，多策略场景语义不成立

- 状态: 待审核
- 严重级别: 中
- 位置: `src/WtBtPorter/WtBtPorter.cpp:589/729/913/1470/1759/1900` 等（形参写作 `CtxHandler /*cHandle*/`）
- 现象: 回测侧所有 cta_/sel_/hft_ 入口都取单例 `getRunner().cta_mocker()/sel_mocker()/hft_mocker()`，
  句柄只做透传给回调，不参与查找；任意句柄值都会命中同一个 mocker。
- 影响: 与实盘侧 `getCtaContext(id)` 的句柄表语义不一致；`init_cta_mocker` 两次调用时
  前一个 mocker 被 `delete` 覆盖（`WtBtRunner.cpp:266-283`），旧句柄继续可用且指向另一个对象。
- 用例: 期望值按实现现状推导（断言中性返回值时统一传非 0 句柄 `kAnyHandle`），标注为「待裁定」。

## Q06-006 `write_log` 对 `catName` 不做 NULL 校验

- 状态: 待审核
- 严重级别: 中
- 位置: `src/WtPorter/WtPorter.cpp:333-343`、`src/WtBtPorter/WtBtPorter.cpp:285-295`
- 现象: 首句 `if (strlen(catName) > 0)`，`catName` 为 NULL 时未定义行为；
  同一头文件里其他字符串参数（`message`）同样无校验。
- 期望: 参照 `WTSLogger::log_raw_by_cat` 内部已有 `logger == NULL` 兜底，导出层应先判空。
- 用例: 启用用例只覆盖 `""` 与非空两类（`*.WriteLogAcceptsNamedAndEmptyCategory`），
  NULL 入参属崩溃类，未纳入启用用例。

## Q06-007 回测侧通道事件常量缺失，用字面量 1000 代替

- 状态: 待审核
- 严重级别: 低
- 位置: `src/WtBtPorter/WtBtRunner.cpp:413-417`（`_cb_hft_chnl(cHandle, trader, 1000/*CHNL_EVENT_READY*/)`）
- 现象: `src/WtBtPorter/PorterDefs.h` 只定义了 `EVENT_ENGINE_INIT..EVENT_BACKTEST_END`
  （37-41 行），没有实盘侧的 `CHNL_EVENT_READY/CHNL_EVENT_LOST`（对比 `src/WtPorter/PorterDefs.h:45-46`），
  所以通道事件只能写魔法数，绑定层无法用符号匹配。
- 用例: `Q06BtRunner.HftMicroAndTradeEventsPassThrough` 断言 `== 1000U` 并注明来源。
