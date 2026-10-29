# R16 Share / WTSUtils / WTSTools 头文件层 缺陷登记

登记人：lane R16　登记日期：2026-10-28
范围：`src/Share/TimeUtils.hpp`、`src/Share/StrUtil.hpp`、`src/Share/StdUtils.hpp`、`src/Share/IniHelper.hpp`、`src/Share/BoostFile.hpp`、`src/Share/WtKVCache.hpp`、`src/Share/DLLHelper.hpp`、`src/Share/ModuleHelper.hpp`、`src/WTSUtils/WtLMDB.hpp`、`src/WTSTools/WTSLogger.h`
本轮 `src/**` 全程只读，未改任何生产代码，也未改他人用例。
已避开的既有登记项（不重复登记、不复活）：`src/Share/charconv.hpp:560-582` 的 `isGBK` 死循环+越界读（隔离在 `l01_share_wtsutils/l01_converter_charconv_test.cpp` 的 `DISABLED_`）、`WTSBaseDataMgr::getTplIDByPID` 不含 `.` 的 pid 越界读。
`WTSValueArray::at()` 越界返回 `INVALID_DOUBLE` 按 `src/Includes/WTSDataDef.hpp:62-73` 的文档语义处理，不作为缺陷。

复核说明（2026-10-29）：本次只读对照源码、测试定义和在库调用点；没有运行测试或 sanitizer。`源码确认` 表示缺陷由源码控制流/接口约定支持，不代表动态复现已通过。`待验证` 保留给依赖未明确契约或缺少有效复现证据的条目；已撤回的误报在原编号下保留结论，避免重新登记。

---

## BUG-R16-01【中】TimeUtils::fasttime 对 1970 年以前的 epoch 用截断除法，日期算错

- 位置: `src/Share/TimeUtils.hpp:80-105`（`tm->tm_sec = unix_sec % 60`、`tm->tm_mday = (i + time_zone) / 24` 以及 `a = tm->tm_mday + kDaysFromUnixTime` 之后整段推导）
- 现象: 该算法直接对负 `unix_sec` 使用 `/`、`%`，C++ 对负数向零截断，因而秒/分及日期分解不符合 floor/Euclidean 分解；`tm_sec` 可为负，`time_zone` 使中间量为负时小时/日期也偏移。
- 证据: `src/TestUnits/coverage/r16_share_tools/r16_timeutils_test.cpp` 的 `DISABLED_fasttime_pre_epoch_date_is_wrong` 有负 epoch 输入和 `utcBreak` 对照，但本次未运行。用例注释把 `-86400 * 365` 误写成 1969-12-31；该时间戳对应 1969-01-01，应用例计算出的 `utcBreak` 为准。
- 修复方向: 先按 floor/Euclidean 规则规范化秒、分、小时及日偏移，再做年月日换算；覆盖负 epoch 和负时区边界。
- 状态: 源码确认；动态复现未执行

## BUG-R16-02【中】Time32 默认构造不初始化内部 tm，date()/time()/fmt() 读未初始化内存

- 位置: `src/Share/TimeUtils.hpp:374`（`Time32():_msec(0){}`），受影响读取 `:408-430`
- 现象: `struct tm t` 是裸 POD 成员，默认构造既不 memset 也不赋值；`Time32 t; t.date();` 读的是未初始化栈内存（`tm_year+1900` 可以是任意值）。`from_local_time()` 只有在被调用之后才安全。
- 证据: 启用用例 `r16_timeutils.time32_from_local_time_overwrites_previous_value` 先 `from_local_time()` 再读取，只证明重写后的路径；它没有读默认构造对象。直接读未初始化 `tm` 属 UB，普通断言不是可靠 oracle；本次未运行 MemorySanitizer。
- 修复方向: 在默认构造函数中值初始化 `t`（如 `t{}`），并为默认对象定义明确的日期/时间语义。
- 状态: 源码确认；动态复现未执行

## BUG-R16-03【高】StrUtil::replace 空 src 入参死循环并无限吃内存

- 位置: `src/Share/StrUtil.hpp:317-334`
- 现象: `str.find("")` 恒返回 0，`pos` 永远是 0、`lastPos = pos + srcLen` 永远是 0 → `while` 不推进，每轮还往 `ret` 追加一次 `des`：调用线程直接挂死 + 内存无限增长。
- 影响面: 空 `src` 会使 `find` 一直返回当前位置，循环不推进；`des` 非空时还会重复增长 `ret`。当前在库调用点（包括 `WTSLogger.cpp:150/159`）传入的查找串是非空字面量 `%s`，因此没有证据支持“当前配置可触发”的原影响描述；公共接口仍可被空串调用。
- 证据: `r16_strutil_stdutils_test.cpp` 已将原禁用用例改为受 CPU 和墙钟限制的子进程回归；以空 `src` 和空 `des` 调用可避免结果持续增长。调用前标记后子进程因 CPU 限额收到 signal 24，未正常返回；四条非空 `src` 对照（含空 `des`）PASS。有限时间测试只观测到本机超限，循环不会推进的结论来自源码；未对增长内存的非空 `des` 场景运行故障注入。
- 修复方向: 明确空 `src` 语义并在循环前短路（通常拒绝或原样返回）；同时决定是否允许空 `des`。
- 状态: 本机空 src、空 des 子进程超时；空 src 的返回/错误合同待明确

## BUG-R16-04【高】StrUtil::standardisePath 空串时用 `(size_t)-1` 下标读字符串尾部

- 位置: `src/Share/StrUtil.hpp:306-315`（`if (path[path.length() - 1] != '/' && bIsDir)`）
- 现象: `path.length()==0` 时 `path.length()-1` 下溢为 `SIZE_MAX`，`std::string::operator[]` 越界（libc++ 带 `_LIBCPP_ASSERT` 时 abort，release 下是未定义读），且读到的字节决定要不要补 `/`。
- 影响面: `path[path.length()-1]` 在 `path` 为空时先被求值，之后才检查 `bIsDir`，因此 `bIsDir=false` 也不能避免越界；调用者传入空文件名/路径会触发该访问。
- 证据: `r16_strutil_stdutils_test.cpp` 的 `DISABLED_standardise_path_empty_string_reads_before_begin`；本次未运行。该用例只能保留触发条件，不能把特定 libc++ abort 当作跨平台结果。
- 修复方向: 对空路径先返回/报错，再访问末字符；同时明确目录空串是否规范化为空串还是 `/`。
- 状态: 源码确认；动态复现未执行

## BUG-R16-05【中】IniHelper::load 吞掉解析异常后仍把对象标记为已加载

- 位置: `src/Share/IniHelper.hpp:73-86`（`try { read_ini(...) } catch(...) {}` 之后无条件 `_loaded = true;`）
- 现象: `read_ini` 异常被吞后 `_loaded` 仍无条件设为 true；这与同文件 `isLoaded()` 注释“成功加载”冲突。缺失/非法文件会留下空树并被当成已成功加载。
- 证据: 旧测试固定了缺文件时 `isLoaded()==true` 的错误行为；现合并为启用用例 `load_missing_file_does_not_mark_loaded`，本机运行 FAIL，实际为 true。`isLoaded` 注释明确表示是否成功加载。当前生产代码没有 `isLoaded()` 调用点，因此下游实际误判影响未由调用链证明。
- 修复方向: 仅在解析成功后置 `_loaded=true`；若缺失文件应作为“空配置”接受，应通过独立状态/返回值区分“允许空配置”和“解析成功”。
- 状态: 本机缺文件定向测试复现；生产影响待验证

## BUG-R16-06【高】IniHelper 用固定 64 字节 static 缓冲拼「节.键」路径，长名静默截断并互相覆盖

- 位置: `src/Share/IniHelper.hpp:59`（`MAX_KEY_LENGTH = 64`）与 `:183-184`、`:199-200`、`:215-216`、`:232-233`、`:248-249`、`:351-352`、`:367-368`、`:382-383`、`:400-401`、`:416-417`
- 现象: 所有读写都走 `static char path[64] = {0}; snprintf(path, 64, "%s.%s", szSec, szKey);`。
  1. 「节名+1+键名」超过 63 字符时**尾部被截掉**，两条只差尾字符的键会被折叠成同一条 ptree 路径，后写覆盖前写（静默数据丢失，`save()` 之后磁盘上也只剩一条）；
  2. 同一成员函数的 `path` 是函数级 `static`，不同实例并发调用该成员函数会竞争同一缓冲区；是否违反产品线程安全契约需另行确认，不影响第 1 点的确定性截断问题。
- 证据: 修正后的启用用例 `long_distinct_section_keys_keep_independent_values` 使用 30 字符节名和 33 字符键名，构造的组合路径为 64 字符；本机 FAIL，先后写入的两个键均读出 `valueB`，保存并重载后亦如此。内部存在 64 字节缓冲限制，外部调用方是否须自行限制路径长度仍需明确；静默覆盖不能由原测试的现状断言视为通过。
- 修复方向: 用局部 `std::string` 构造完整 ptree 路径，避免固定缓冲静默截断；若对象需支持并发读写，另外定义同步契约。
- 状态: 源码确认（截断）；并发契约待验证

## BUG-R16-07【高】WtKVCache::put 不校验 len，越过 64 字节字段写坏下一条目的键

- 位置: `src/Share/WtKVCache.hpp:211-232`（`put`，另 `:234-253` 的 `put_if_none` 同形）、字段定义 `:32-45`（`_key[64]`/`_val[64]`）、拷贝实现 `src/Includes/WTSMarcos.h:98-104`（`wt_strcpy` 直接 `memcpy(des, src, len); des[len]='\0';`）
- 现象: `wt_strcpy` 将指定 `len` 原样 memcpy 到 64 字节 `_val`，并写 `des[len]='\0'`；`len>63` 会越过 `_val`，覆盖后续映射内容。`len==64` 也会把终止符写到字段之外；当写入项是映射尾项时可越过映射范围。超长 key 经默认 `strlen` 路径也没有字段容量保护。该覆写不会自动把键加入 `_indice`，原“幽灵键”描述不成立。
- 证据: 原用例的 `cache.has("xxxxxx")` 断言不能证明覆写键进入索引：`has()` 只查 `_indice`，本次 `put` 仅插入 `k0`。现移除启用状态下的越界写，新增 `DISABLED_oversized_value_length_has_safe_defined_behavior`，待修复后验证相邻项和重开后的完整性；该危险路径本次 NOT_RUN。63 字节边界对照 `value_at_63_byte_limit_is_readable` 单独运行 PASS；越界结论由 `wt_strcpy` 和字段布局静态确认。
- 修复方向: 在拷贝前验证 key/value 最长 63 字节并拒绝或截断；给 `put` 增加可观测失败结果，避免无返回值 API 静默丢弃错误。
- 状态: 源码确认；原测试的幽灵键断言无效，动态复现未执行

## BUG-R16-08【高】WtKVCache::init 日期变更时先用文件头里的旧 capacity 清零，早于容量自愈 → 越界写映射区

- 位置: `src/Share/WtKVCache.hpp:144-152`（`memset(&_cache._block->_items, 0, sizeof(CacheItem)*_capacity)`）与 `:161-179`（按 `realSz` 回填 `_capacity/_size` 的自愈分支）
- 现象: 日期不同分支先按文件头旧 `_capacity` 清零，再校验真实映射大小；截短文件会让清零范围超过映射文件，存在越界写/SIGBUS 风险。同日期避免了这次 memset，但自愈分支把 `_size` 设为 `min(realCap, markedCap)` 而非 `min(持久化_size, realCap)`；测试展示原有 3 条记录被设成 size=10，重建索引还收进空槽，因此同日期对照也不是“正常自愈”。
- 证据: `DISABLED_truncated_file_other_date_memset_out_of_mapping` 的异日期映射越界路径本次 NOT_RUN，信号行为依平台而异。修正后的 `truncated_file_same_date_preserves_records_and_repairs_capacity` 先关闭映射并截短成 10 个槽位、保留 3 条记录；单独运行 FAIL：容量为 10 且三条原值仍可读，但 size 实际 10（预期 3）、空键被索引（预期不存在）。
- 修复方向: 在任何清零前先依据真实文件尺寸验证/修复头部容量；恢复 `_size=min(persisted_size, realCap)`，并仅索引有效条目。
- 状态: 源码确认；动态故障信号未执行

## BUG-R16-09【高】WtLMDBQuery::get 忽略传入的 key，恒返回库里的第一条记录

- 位置: `src/WTSUtils/WtLMDB.hpp:194-214`（`mKey` 只是被赋值给 `MDB_val`，随后 `mdb_cursor_get(cursor, &mKey, &mData, MDB_NEXT)` 用的是 `MDB_NEXT`，既不是 `MDB_SET` 也不是 `MDB_SET_KEY`）
- 现象: 输入 key 被放入 `MDB_val`，但新游标首次执行 `MDB_NEXT` 时 bundled LMDB 的 `mdb_cursor_next` 因游标未初始化转到 `mdb_cursor_first`；查询于是返回首条而非指定键。
- 证据: `src/WTSUtils/lmdb/mdb.c:5909-5910` 明确未初始化游标时走 first；现改正 `get_looks_up_the_requested_key` 并运行 FAIL：查询 `b` 和不存在的键均返回 `va`。源码检索未发现当前生产代码调用 `WtLMDBQuery::get`；公共接口注释仍声明读取指定 key。
- 修复方向: 改用 `mdb_get` 或 `MDB_SET` 精确定位，并在成功/失败路径都关闭 cursor。
- 状态: 本机定向测试复现；生产调用未确认

## BUG-R16-10【中】WtLMDB::open 用局部变量遮蔽成员 _errno，打开失败后错误状态全失真

- 位置: `src/WTSUtils/WtLMDB.hpp:73`、`:77`、`:81`（三处 `int _errno = ...`，成员是 `int _errno`(`:100`)）
- 现象: `mdb_env_create`、`mdb_env_open` 和 `mdb_env_set_mapsize` 的返回码写入局部 `_errno`，成员保持构造值；打开失败后 `has_error()`/`errmsg()` 仍报告成功。另，`mdb_env_set_mapsize` 的错误码未检查，失败时 `open()` 仍返回 true。LMDB 允许在 open 后、无活跃事务时设置 mapsize；此调用时点本身不是缺陷。
- 证据: 修正后的 `open_failure_updates_member_error` 对普通文件路径打开失败时，`has_error()` 实际仍为 false，定向测试 FAIL；正常只读环境对照 PASS。bundled `lmdb.h` 明确允许无活跃事务时在 open 后调用 set_mapsize。
- 修复方向: 用成员记录每个返回码并在失败时返回 false；检查 mapsize 结果，失败时关闭已创建环境并保留正确错误码。
- 状态: 本机打开失败路径定向复现；mapsize 失败未单独运行

## BUG-R16-11【高】WtLMDBQuery::get_all 多返回一条幽灵记录；空库时读未初始化 MDB_val；游标未关闭

- 位置: `src/WTSUtils/WtLMDB.hpp:371-390`
- 现象: 循环在 `MDB_NOTFOUND` 后仍无条件构造并追加结果；非空库会追加上一条记录副本。空库首次读取返回 NOTFOUND 时，`bKey/mData` 未初始化，随后用作 `std::string` 的指针和长度。函数也未关闭局部 cursor；只读事务的 bundled LMDB 清理路径不会替它关闭已跟踪 cursor，重复调用会泄漏 cursor。
- 证据: 修正后 `get_all_returns_each_record_once` 的两条记录本机返回计数及回调列表长度均为 3，正确断言 FAIL；空库用例仍禁用，未运行未初始化值路径。`mdb_cursor_next` 未命中提前返回见 `mdb.c:5945-5950`；只读事务结束见 `mdb.c:3012-3024`，未调用 `mdb_cursors_close`；本轮未做游标泄漏量测。
- 修复方向: 仅在 `mdb_cursor_get` 成功后读取/追加；对空库直接返回空结果；用 RAII 保证所有路径关闭 cursor。
- 状态: 非空库重复记录已本机复现；空库及泄漏未动态验证

## BUG-R16-12【中】WTSLogger::log_by_cat_prefix 的 format/args 形参被注释掉，消息正文丢失

- 位置: `src/WTSTools/WTSLogger.h:336-347`（`const char* /*format*/, const Args& .../*args*/`，函数体里只有 `strcpy(m_buffer+1, catName)`，没有任何 `fmtutil::format_to`）
- 现象: 文档和形参声明均承诺格式化消息，但函数没有使用 `format/args`；拼入 `]` 后也没有写新的终止符，传出的正文可能是同线程缓冲残留。另外未限制 `catName` 长度，可能越过 2048 字节缓冲。
- 证据: `r16_logger_dll_test.cpp` 的 `DISABLED_log_by_cat_prefix_should_format_the_message` 只做 `EXPECT_NO_THROW`，不能证明消息正文或残留内容；本次未运行。源码检索未发现当前生产调用点，公共方法仍可被调用。
- 修复方向: 先校验/有界写入分类前缀，再把格式化结果写入 `m_buffer+offset+2` 并显式终止；为长分类名定义拒绝或截断策略。
- 状态: 源码确认；动态输出未验证

## BUG-R16-13【高】DLLHelper::wrap_module 对不含字母的模块名越过字符串结尾继续扫描

- 位置: `src/Share/DLLHelper.hpp:76-78`（`std::size_t idx = 0; while (!isalpha(name[idx])) idx++;`）
- 现象: Unix 分支的扫描没有字符串终止边界；没有字母的名字会读过 `\0`。对非 ASCII UTF-8 字节，直接把可能为负的 `char` 传给 `isalpha` 也不满足其参数域，行为未定义。现有调用点多传 ASCII 模块名，但空/非 ASCII 输入仍未受保护。
- 附带主张撤回: `_sm` 前缀位置和已带 `lib` 时再次加前缀，现有方法注释/调用点未定义这两类输入的规范输出；测试只锁定当前输出，不能证明它违背契约，故不计缺陷。
- 证据: `r16_logger_dll_test.cpp` 的 `DISABLED_wrap_module_without_alpha_hangs` 是危险输入示例，但“必然挂死”不是确定结果；启用的前缀测试只记录当前输出。本次未运行这些测试。
- 修复方向: 先判空并限制扫描到 `name[idx] != '\0'`；转换为 `unsigned char` 后调用 `isalpha`；无字母时返回明确错误/空结果，并定义带前缀名称的规范。
- 状态: 源码确认（无边界扫描）；前缀规范主张已撤回

## BUG-R16-14【低】WtKVCache::init 忽略 BoostFile 建表/截断的返回值

- 位置: `src/Share/WtKVCache.hpp:124-133`（`bf.create_new_file(filename); bf.truncate_file(...); bf.close_file();` 三个返回值全部丢弃）
- 现象: 新文件分支丢弃 create/truncate 返回值；当前明确可见后果是失败原因只落成 map 阶段的通用日志，`init` 最终仍会在 map 失败时返回 false。尚无证据证明该路径会成功返回损坏 cache。原文“truncate 失败后 map 成功且字段在映射外”未由源码/用例支持，撤下。
- 证据: `r16_kvcache_test.cpp` 的 `init_on_unwritable_path_fails_with_log` 实际用的是不存在的父目录，并只断言通用日志；本次未运行。没有接口约定要求返回具体 OS 错误或分阶段日志。
- 修复方向: 若需要诊断分阶段错误，逐个检查返回值并记录失败阶段；要把它作为功能缺陷计入前，先明确错误诊断契约并验证 truncate 失败后的 map 行为。
- 状态: 已撤回（仅确认错误诊断粒度，未证实功能失败或违反诊断契约）

## BUG-R16-15【低】BoostFile::read_file_contents 向 const string 的缓冲区写入

- 位置: `src/Share/BoostFile.hpp:188-198`（`buffer.resize(filesize); return bf.read_file((void *)buffer.c_str(), filesize);`）
- 现象: C++17 目标中 `c_str()` 选择 const 访问器，却被 cast 后传给写入型 `read_file`；C++17 `basic_string` 对 const accessor 要求不得修改字符数组，应该使用非 const `data()`。另 `unsigned int filesize` 会把超过 4 GiB 的文件尺寸截断，返回内容可能只有低 32 位字节。
- 证据: `src/CMakeLists.txt` 与 coverage target 使用 C++17；`static_write_and_read_file_contents_roundtrip` 只覆盖小文件，不能排除 accessor 合约问题或大文件截断。本次未运行。
- 修复方向: 使用 `buffer.data()`（非 const overload）作为写缓冲，并用 `std::size_t`/文件尺寸宽类型保留完整长度；明确空文件的返回约定。
- 状态: 源码/接口合约确认；大文件行为未运行

## BUG-R16-16【低】WTSLogger 的级别守卫让 LL_NONE 成为未初始化态下唯一可达的日志入口

- 位置: `src/WTSTools/WTSLogger.h:163、189、215、241、267、294、316、338、364、387`（`if (m_logLevel > ll || m_bStopped) return;`）配合 `src/Includes/WTSTypes.h:129-137`（`LL_ALL=100`、DEBUG..FATAL=101..105、`LL_NONE=106`；`WTSLogger.cpp:39` 默认设为 `LL_NONE`）
- 复核结论: 原条目是误报。`m_logLevel` 是最低日志阈值，数值越大过滤越多；默认 `LL_NONE=106` 会过滤真实级别 100..105，符合“不输出”。把 `LL_NONE` 当日志事件传入时等于阈值而放行，但它是 sentinel，不是实际日志级别。原文把 LL_NONE 写成 105 也错了一位。
- 证据边界: 本次只按枚举和守卫静态判断，未运行日志测试；测试喂 `LL_NONE` 不能证明真实级别的过滤方向错误。
- 状态: 已撤回

## BUG-R16-17【低】IniHelper::save 对空文件名与写失败无任何防护

- 位置: `src/Share/IniHelper.hpp:95-101`
- 复核结论: 原条目是误报。`save()` 没有声明 `noexcept` 或保证以状态码吞掉写错误；`boost::property_tree::write_ini` 以异常报告无效/不可写目标符合其接口行为。对未 load 且未给目标路径的对象调用 `save()` 属于没有有效目标的调用，抛出本身不能证明功能违约。
- 证据边界: 成功路径用例不证明失败路径；本次未运行异常路径，也未发现 `IniHelper::save` 有不抛异常契约。
- 状态: 已撤回

## BUG-R16-18【中】WtLMDBQuery::get_range 用「当前键长度」做区间上界比较，变长二进制键判定失真

- 位置: `src/WTSUtils/WtLMDB.hpp:246`（比较长度取 `lKey.mv_size`）。`get_range` 的 lower/upper 参数类型为通用 `std::string`；当前生产存储调用点使用 `LMDBBarKey`/`LMDBHftKey` 固定 `sizeof` 的键。
- 现象: 对变长二进制键，该比较没有按两边长度进行字典序比较。例：upper 为 `std::string("ab")`，当前 key 为 `std::string("ab\\0", 3)`；比较长度为 3，正好把 upper 的终止符也比较掉，结果相等而放行；按字典序，upper 是当前 key 的严格前缀，当前 key 应大于上界。若当前 key 比 upper 多出更多字节，比较长度还会越过 upper 的有效字符数组范围。原例“当前 `ab`、上界 `abcd`”并不构成错收，因为 `ab` 本来就小于 `abcd`。
- 已撤回的附加主张: `:234-235` 的条件确属重复检查/死代码，但未证明有运行时错误，不作为独立缺陷。
- 证据: `r16_lmdb_test.cpp` 的 `put_then_get_range_returns_exact_window` 只测单字符等长键，未覆盖变长/嵌入 NUL；本次未运行。当前产品 `get_range` 调用点的 struct keys 为固定长度，实际产品影响面尚未确认。
- 修复方向: 使用长度感知的字典序比较（如比较共同前缀，再比较长度）；若接口只支持固定长度键，应显式校验并声明该前提。
- 状态: 源码确认（通用变长键）；当前产品触发面待验证
