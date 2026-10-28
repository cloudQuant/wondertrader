# L06 WtBtCore 交付清单（迭代 01）

- Lane: L06，模块 `src/WtBtCore/`
- 产出目录: `src/TestUnits/coverage/l06_wtbtcore/`
- 语法门禁: 4/4 文件通过 `clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror`（0 错误 0 告警，macOS arm64 / Apple clang）
- 生产代码: 未修改（`src/WtBtCore/**` 全部只读）
- Bug 记录: `docs/迭代计划/迭代01/bugs/l06_wtbtcore.md`，共 7 条，状态均为「待审核」

## 新增测试文件与覆盖符号

### 1. `l06_match_engine_test.cpp`（约 400 行，13 个用例）
覆盖 `MatchEngine`（src/WtBtCore/MatchEngine.cpp，460 行）：
- `init`（NULL cfg / cancelrate 生效）、`regisSink`、`clear`
- `handle_tick`（NULL tick 防护、tick 缓存建立、fire→match 主链路、成交量为 0 跳过撮合）
- `buy` / `sell`：无缓存 tick 返回空、被动单买一/卖一排队（加权排队、`cancelrate` 削减）、主动单对手价触发 `_positive`
- 私有链路经由公开 API 全覆盖：`fire_orders`（激活 entrust/order 回报）、`match_orders`（被动排队消耗/越过排队部分成交、最新价穿越限价按剩余量成交、主动单对手价成交、全量成交删除订单、撤单回报 state 9→99）、`update_lob`（十档填充与中间价清理的安全路径）、`grab_last_tick`
- `cancel(uint32_t)`（未知 id 返 0、置撤单态）、`cancel(stdCode,isBuy,qty,cb)`（方向过滤、买正卖负回调）

### 2. `l06_split_volume_test.cpp`（约 140 行，10 个用例）
覆盖 `src/WtBtCore/HftMocker.cpp` 全局函数（UftMocker 亦复用）：
- `splitVolume(uint32_t)`：vol=0/1/253/100 的总量守恒、[1,100] 区间约束
- `splitVolume(double,min,max,tick)`：0/低于 minQty 单笔、qtyTick 网格对齐、minQty==maxQty 退化
- `genRand(maxVal)`：边界 <maxVal、maxVal=1 恒为 0（函数内部 `srand(time)`，断言全部为顺序无关不变量，保持确定性）

### 3. `l06_replayer_runtime_test.cpp`（约 160 行，8 个用例）
覆盖 `CHisDataReplayer`（src/WtBtCore/HisDataReplayer）运行期接口（手续费/minfee 分支已由 `src/TestUnits/test_replayer_fee.cpp` 覆盖，未重复）：
- 构造函数默认值 + `init`（tick=false 覆盖 `_tick_enabled(true)` 默认）
- `get_date/get_min_time/get_raw_time/get_secs/get_trading_date`、`get_hot_mgr`
- `update_price` + `get_cur_price` 往返、`get_day_price` 空缓存 0 值路径（tick 关闭分支）
- `get_session_info`（sid 与 isCode 两种查询、未知合约返回 NULL、同输入结果一致）
- `get_rawcode`（无品种规则 → ""）、`calc_fee`（未配置品种 → 0，回归锚点）
- `stop()`（未运行告警分支）、`clear_cache()`（空缓存清理）、`get_last_tick`（无数据 NULL）
- 复用只读样例 `dist/common/sessions.json`、`dist/common/stk_comms.json` 与既有 fixture `fees_minfee.json`；落盘目录使用 `temp_directory_path()/l06_replayer_*`，TearDown 清理。

### 4. `l06_wthelper_event_test.cpp`（约 130 行，6 个用例）
覆盖 `src/WtBtCore/WtHelper.cpp`（52 行，全量）与 `EventNotifier` 安全路径：
- `WtHelper::setInstDir/getInstDir` 往返、`getCWD`（含静态缓存二次调用分支）、`setOutputDir/getOutputDir`（目录缺失自动创建、重复调用稳定）
- `EventNotifier` 构造/析构、`init(active=false)` 早退、`init(active=true)` 模块缺失路径（CWD/InstDir 双查找失败分支）
- 输出目录写入 `temp_directory_path()/l06_wthelper_*`，TearDown 清理并恢复默认值。

## 跳过目标及原因

- `CtaMocker/SelMocker/HftMocker/UftMocker/ExecMocker` 主体（tick/bar 回放钩子、dump_outputs、策略 DLL 装载）：需要完整策略工厂 + 行情文件 + 事件线程环境，单元测试内构造成本远超本 lane 预算；且 CtaMocker 的 JSON dump 分支依赖运行时输出目录约定，建议后续以集成回测任务覆盖。
- `HisDataReplayer::run_by_bars/run_by_ticks/replayHftDatas*`：依赖 wtd 二进制行情文件，仓库内无只读样例可驱动（`sessions.json` 只能驱动基础数据装载，即本清单第 3 项范围）。
- `MatchEngine` 涨跌停拒单：现实现根本不存在涨跌停校验（`upper_limit/lower_limit` 未参与撮合），已作为行为缺口记入 bugs（BUG-L06-002/004 关联场景），不虚构断言。
- `src/WtBtCore/index/`（指标工厂）：该目录在本仓库不存在，指标实现位于 `src/WtCore/`（`WtStdUtils`/指标工厂由其他 lane 负责）；`WtBtCounter/WtBtUniverse/WtBtMatcher/WtBtOrder/WtBtPosition` 类在本仓库版本中亦不存在（属旧架构），对应能力已由 `MatchEngine` + `HisDataReplayer::calc_fee` 承担。
- `HisDataMgr`：全部方法依赖动态库 `createBtDtReader` 装载成功，失败路径均为日志早退，单测覆盖需注入 DLL，跳过。

## 验证命令（逐文件已执行）

```
clang++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror \
  -Wno-deprecated-copy-with-user-provided-copy "-Wno-#pragma-messages" \
  -DSPDLOG_FMT_EXTERNAL -DFMT_HEADER_ONLY \
  -include src/Share/fmt_enum_compat.h \
  -Isrc -Isrc/TestUnits/gtest -I/opt/homebrew/include -I/usr/local/include \
  src/TestUnits/coverage/l06_wtbtcore/<file>.cpp
```

链接说明：测试未定义 `makeLocalOrderID`（由 `WtBtCore` 库内 `HftMocker.cpp` 提供），`CoverageTests` 目标已链接 `WtBtCore`，全量构建由主控执行。
