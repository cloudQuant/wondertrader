# p09_storage_readers 疑似缺陷清单（状态：待审核，未修改任何生产代码）

车道：p09（第四波，`src/TestUnits/coverage/p09_storage_readers/`）
范围：`src/WtDataStorage/WtRdmDtReader.cpp`、`src/WtDataStorage/WtDataReader.cpp`（**非 AD 本体**）
所有崩溃/UB 类触发输入都整类收进 `DISABLED_` 用例，常规用例零残留、零阻塞。

---

## BUG-P09-001（Blocker，待审核）`indexBarFromCacheByRange/ByCount` 在公开默认参数 `etime=0` 下迭代器回绕越界

- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1629`（`indexBarFromCacheByRange`）、`:1688`（`indexBarFromCacheByCount`）
- 现象：`etime` 是 `IRdmDtReader::readKlineSliceByRange/readTickSliceByRange` 等公开接口的**默认实参 0**
  （见 `src/WtDataStorage/WtRdmDtReader.h:441`）。函数内
  `rDate = etime/10000 = 0`，随后 `eBar.time = (rDate - 19900000) * 10000 + rTime` 走 uint32 运算，
  结果为 `2863462912`，**小于任何 2013 年以后 bar 的 time**（20200401 → 3004011500）。
  于是 `std::lower_bound` 返回 `begin()`，进入
  `if ((isDay && eit->date > eBar.date) || (!isDay && eit->time > eBar.time)) eit--;`
  而该处**没有 `eit == begin()` 守卫**（同文件 `readBarsFromCacheByRange` 在 `:1762` 有），
  `eit--` 使迭代器/`eIdx` 回绕，`&barsList._bars[sIdx]` 与 `curCnt = eIdx - sIdx + 1` 均越界。
- 影响：调用方只要使用默认 `etime`（即「读到最新」），日线/分钟线缓存索引即可读到随机内存；
  与 l04/p05 已记录的 BUG-P05-007 同型，但触发条件是**默认参数而非特例输入**，命中概率高一个量级。
- 复现：`p09_rdm_window_range_test.cpp` 的 `DISABLED_p09_default_etime_zero_underflows_the_range_indexers`
  （`--gtest_also_run_disabled_tests` 触发）。
- 建议（不修）：与 `readBarsFromCacheByRange` 对齐补 `eit == begin()` 保护，并在入口把 `etime==0` 归一为
  `barsList._bars.back().time`。

## BUG-P09-002（Major，待审核）`readBarsFromCacheByRange` 缺空缓存守卫 → `size()-1` 下溢

- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1729`
- 现象：`BarsList& barsList = _bars_cache[key];`（`operator[]` 会默认构造空 BarsList）后**立刻**
  `lower_bound`，`eit == end()` 分支执行 `eIdx = barsList._bars.size() - 1;` → 空 vector 时为 `SIZE_MAX`，
  随后 `uint32_t curCnt = eIdx - sIdx + 1;` 得到天文数字并 `ayBars.resize(curCnt)` + `memcpy`。
  同文件 `indexBarFromCacheByRange:1638`、`indexBarFromCacheByCount:1695` 都有
  `if (barsList._bars.empty()) return NULL;`，只有这个拷贝版漏了。
- 触发条件：key 未被预热（缓存未命中、历史文件缺失、或 `readKlineSliceByRange` 在
  `cacheHisBarsFromFile` 之前先查 `_bars_cache`）。
- 复现：`p09_rdm_bars_cache_test.cpp` 的 `DISABLED_p09_underflow_inputs_from_range_indexers` 第 (3) 段。

## BUG-P09-003（Major，待审核）`WtDataReader::init` 未校验 `cfg->get("path")` 的返回值

- 位置：`src/WtDataStorage/WtDataReader.cpp:161`
- 现象：`WTSVariant* cfgPath = cfg->get("path"); if(cfgPath->isArray())` —— 配置里缺 `path` 时
  `cfg->get()` 返回 NULL，直接空指针解引用；对比同文件其他取值（`cfg->has("adjfactor")`）都有前置判断。
  同类风险：`cfg->getUInt32("adjust_flag")` 未判 `has()`（缺项时取 0，行为可接受，一并备注）。
- 复现：`p09_reader_init_adjfactor_test.cpp` 的 `DISABLED_p09_init_without_path_crashes`。
- 建议（不修）：`if (cfgPath == NULL) { pipe_reader_log(sink, LL_ERROR, ...); return; }`

## BUG-P09-004（Minor，待审核）`getAdjFactors` 的 `thread_local static char key[20]` 无界格式化

- 位置：`src/WtDataStorage/WtRdmDtReader.h:538`、`src/WtDataStorage/WtDataReader.cpp:2351`
- 现象：`char key[20] = {0}` + `fmtutil::format_to(key, "{}.{}.{}", exchg, pid, code)`，无长度上限。
  `exchg.pid.code` 总长 ≥ 19 字符即栈溢出（如 `XINE.STK.600000SS` 之类的长代码或自定义 PID）。
  另外该 buffer 是 `thread_local static`，被当作 `const AdjFactorList&` 的键来源，
  同线程内多次调用互相覆写（当前用法安全，但脆弱）。
- 未写触发用例（越界类），只在 `p09_reader_init_adjfactor_test.cpp` 用常规长度代码覆盖正常分支。

## BUG-P09-005（Minor，待审核）空区间条数依赖 size_t 环绕「恰好」为 0；`count==0` 返回一位越界指针

- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1683`（`curCnt = eIdx - sIdx + 1`，
  左端整体越过最后一条时 `eIdx=3, sIdx=4` → `SIZE_MAX+1` → 0）、`:1724`
  （`indexBarFromCacheByCount` 中 `count==0` → `curCnt=0` → `sIdx = eIdx+1` →
  返回 `&bars[eIdx+1]`，可能等于 `end()` 一位越界）
- 现象：结果值正确但算术过程依赖无符号环绕，且返回指针不可解引用；调用方（`readKlineSliceByCount`）
  只看 `count` 才安全。已在 `p09_rdm_window_range_test.cpp` 的两个用例里按「不解引用」的约束固化断言。
- 建议（不修）：空区间显式 `return NULL`，与 `count==0` 一并前置校验。
