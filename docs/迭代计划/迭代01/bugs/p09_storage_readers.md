# p09_storage_readers 疑似缺陷清单（2026-10-29 逐条复核；未修改生产代码、测试）

车道：p09（第四波，`src/TestUnits/coverage/p09_storage_readers/`）
范围：`src/WtDataStorage/WtRdmDtReader.cpp`、`src/WtDataStorage/WtDataReader.cpp`（**非 AD 本体**）
原始审查将部分预期崩溃输入放入 `DISABLED_` 用例；现有启用用例也覆盖了零条数边界，但只核对 count，不能证明返回指针安全。每项复核结果及尚未验证之处见下文。

---

## BUG-P09-001（复核：重复，默认参数前提不成立）`indexBarFromCacheByRange/ByCount` 的 begin() 回退缺守卫

- 状态: 已合并（底层早窗口问题归 BUG-P05-007）
- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1629`（`indexBarFromCacheByRange`）、`:1688`（`indexBarFromCacheByCount`）。
- 原记录把直接调用私有 indexer 的 `etime=0` 等同于公开 API 默认调用；源码不支持这个前提。`readKlineSliceByRange` 在 L1807-1808、`readKlineSliceByCount` 在 L2238-2239 都先把 0 改为 `203012312359`，随后才调用 indexer，因此默认值不会以 0 进入本候选的计算路径。API header 将 0 描述为当前时间；2030 sentinel 的契约是否准确另待确认，但不构成这里所称的 `etime=0` 越界。
- `DISABLED_p09_default_etime_zero_underflows_the_range_indexers` 直接调用 `indexBarFromCacheByRange/ByCount`，绕过两个公开 wrapper。本轮执行 `/Users/yunjinqi/Documents/new_projects/wondertrader/build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_also_run_disabled_tests --gtest_filter=P09WindowTest.DISABLED_p09_default_etime_zero_underflows_the_range_indexers` 后进程 rc=139；这证明直接把 0 传给私有 helper 可触发危险的 `eit--`，不证明公开默认参数会触发。
- 对显式早于首根 bar 的非零 `etime`，`:1666-1669` 与 `:1715-1718` 仍会在 `eit==begin()` 时 decrement；这是同一代码根因，已由 `p05_datastorage2.md` BUG-P05-007 作为公开早窗口问题登记，P05-007 作 canonical，本条不另计。
- 裁定：P09-001 的“公开默认 etime=0”主张为误报/重复；底层 begin 边界缺陷仍成立并并入 P05-007。修复方向是在 range/count helper 中先判 `eit==begin()` 且首条晚于结束时间，返回 NULL/0；测试使用明确的非零早窗口经公开 API 覆盖，不应直接调用私有 helper 代替公开前提。

## BUG-P09-002（复核：误报）`readBarsFromCacheByRange` 空缓存下 `size()-1` 下溢

- 状态: 已撤回（原大分配及越界拷贝结论不成立）
- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1729`
- 空 cache 时 `eit==end()==begin()`，代码令 `eIdx=size()-1=SIZE_MAX`。后续 `lower_bound(begin, eit, sBar)` 的范围仍是合法空范围，返回 `begin()`，所以 `sIdx=0`；`eIdx-sIdx+1` 在 `size_t` 无符号算术中回绕为 0（这是定义好的模运算），`curCnt==0` 使 `if (curCnt > 0)` 跳过 `resize` 和 `memcpy`。在所述“vector 为空”的前提下，并不会得到天文数字或发生该候选声称的越界拷贝。
- 可达性：本仓库检索 `readBarsFromCacheByRange(` 只有 `WtRdmDtReader.h` 声明、`WtRdmDtReader.cpp` 定义，以及测试中的白盒直接调用；未发现生产调用点。因此即使输入可由直接调用构造，目前也不能据此证明公开读路径受影响。
- 复现：`DISABLED_p09_underflow_inputs_from_range_indexers` 把空 key 输入放在 begin() 越界调用之后；本轮执行 `/Users/yunjinqi/Documents/new_projects/wondertrader/build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_also_run_disabled_tests --gtest_filter=P09RdmCacheTest.DISABLED_p09_underflow_inputs_from_range_indexers` 以 rc=139 退出，未到达或单独证明第 (3) 段。应将空 key 情形拆为独立测试，断言返回 0 且 `out.empty()`；源码推导当前空 vector 返回 0。

## BUG-P09-003（复核：配置错误路径崩溃；契约待确认）`WtDataReader::init` 未校验 `cfg->get("path")` 的返回值

- 位置：`src/WtDataStorage/WtDataReader.cpp:161-162`；配置接口来自 `WTSVariant::get`，初始化接口 `IDataReader::init` 返回 `void`（`src/Includes/IDataReader.h:206`）。
- 源码：非空 `cfg` 下 `cfg->get("path")` 的结果未经判空即调用 `isArray()`；缺字段时空指针解引用。现有 `DISABLED_p09_init_without_path_crashes` 构造只有 `adjust_flag` 的配置并调用 init。本轮执行 `/Users/yunjinqi/Documents/new_projects/wondertrader/build-cov/build_x64/Debug/bin/TestUnits/CoverageTests --gtest_also_run_disabled_tests --gtest_filter=P09DataReaderTest.DISABLED_p09_init_without_path_crashes` 退出码 139，确认此配置输入会导致进程崩溃；没有验证调用方是否允许省略必填 `path`，也没有验证 sanitizer。
- 裁定：缺失 path 配置下的 fail-fast/健壮性缺陷成立；`path` 是否为强制配置仍需由配置样例/上层契约确认，因此不把它表述为正常配置下故障。`adjust_flag` 缺项在当前代码路径经 `getUInt32` 读取，现有样例未证明其有独立故障，本条不扩展该主张。
- 最小修复方向：判空 `cfgPath` 后记录包含字段名的错误并返回，或按已定义配置默认值初始化路径；因 init 返回 void，需确认上层能从日志/状态获知初始化失败。补一个启用的缺字段断言（如记录错误、目录保持空）并验证不崩溃。

## BUG-P09-004（复核：条件性产品缺陷）复权因子 key 写入固定缓冲区时未限制长度

- 位置：`src/WtDataStorage/WtRdmDtReader.h:538-539`、`src/WtDataStorage/WtDataReader.cpp:2351-2352`；格式化实现 `src/Share/fmtlib.h:15-19`。
- 两处均将 `exchg.pid.code` 无界写入 `thread_local static char key[20]`。`fmtutil::format_to` 调用 `fmt::format_to(buffer, ...)` 后在返回迭代器处再写 `s[0]='\0'`，未提供容量参数；因此 20 字节缓冲区最多容纳 19 个内容字节，格式化结果达到 20 字节即覆盖缓冲区边界。`MAX_INSTRUMENT_LENGTH=32`、`MAX_EXCHANGE_LENGTH=16` 也没有建立该组合 key 的 19 字节上限。
- 裁定：源码层面条件性缓冲区越界成立，原记录所称“栈溢出”和“≥19”不精确；这是 thread-local 静态缓冲区覆盖，首个越界长度为 20 个内容字节。长 key 触发尚未动态复现，现有 `load_adj_factors_key_rules_and_ordering` 只覆盖 `SSE.STK.600000`（14 字节）并通过；没有 long-key 测试。
- `thread_local` 导致同线程覆写尚不足以单独认定为缺陷：map key 为 `std::string`，容器查找/插入会复制 key；`IHisDataLoader::loadAdjFactors` 的 `const char*` 是否可能被异步保留，接口未说明，需单独确认，不在本条下结论。
- 最小修复方向：用拥有长度的 `std::string key = fmt::format("{}.{}.{}", ...)`，并把该字符串传给 map 和 loader；新增 20 字节及更长 key 的边界测试，或若业务明确限制长度则在入口验证并拒绝超长 key。与 R2 的固定缓冲区问题是不同调用点/数据流，不能仅凭同型写法判为重复。

## BUG-P09-005（复核：低影响潜在 UB）空区间/零条数分支会形成 one-past 下标

- 位置：`src/WtDataStorage/WtRdmDtReader.cpp:1683`（`curCnt = eIdx - sIdx + 1`，
  左端整体越过最后一条时 `eIdx=3, sIdx=4` → `SIZE_MAX+1` → 0）、`:1724`
  （`indexBarFromCacheByCount` 中 `count==0` → `curCnt=0` → `sIdx = eIdx+1` →
  返回 `&bars[eIdx+1]`，可能等于 `end()` 一位越界）
- 源码：Range 分支 `eIdx - sIdx + 1` 在左端整体越过末条时依赖无符号模运算得到 0，随后仍返回 `&barsList._bars[sIdx]`；当 `sIdx == size()` 时，通过 `vector::operator[](size())` 形成引用/地址违反其下标前提。ByCount 分支 `count==0` 时 `curCnt=0`、`sIdx=eIdx+1`，当右端落在末条时同样会取 `operator[](size())`。无符号回绕本身有定义，越界下标访问才是问题。
- 对外影响受限：`readKlineSliceByRange/ByCount` 只在条数大于 0 时创建 slice，零条数不会解引用该指针；但 helper 已经在返回前形成了越界下标访问。`IRdmDtReader` 对 range/count API 没有禁止零条数或空区间的说明。
- 关联测试：旧 `right_edge_beyond_last_bar_and_left_edge_beyond_all` 和 `index_bar_by_count_walks_back_from_right_edge` 直接运行空结果 helper，只检查 `count==0`，无法排除 UB。现保留非空正控，并将左端超界及 count=0 的危险调用拆为独立 DISABLED 候选；候选断言空结果 `count==0` 且 `p==nullptr` 是建议合同，helper 当前没有明文承诺 NULL，需先确定语义并修复 UB 后再启用。sanitizer/debug STL 未运行。
- 裁定：低影响、源码可确认的潜在产品缺陷，不是 P09-002 的空 cache 大分配问题；也不是 R23-007 的 ByCount phantom slice（输出对象和行为不同）。
- 最小修复方向：在 helper 里对 `count==0`、`sIdx>eIdx` 或无重叠区间提前返回 `NULL` 并置 `count=0`；测试保留计数断言并增加 `EXPECT_EQ(p, nullptr)`，如需确证 UB 再用 sanitizer/debug STL 构建复核。
