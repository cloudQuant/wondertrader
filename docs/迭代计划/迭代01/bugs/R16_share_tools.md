# R16 Share / WTSUtils / WTSTools 头文件层 缺陷登记

登记人：lane R16　登记日期：2026-10-28
范围：`src/Share/TimeUtils.hpp`、`src/Share/StrUtil.hpp`、`src/Share/StdUtils.hpp`、`src/Share/IniHelper.hpp`、`src/Share/BoostFile.hpp`、`src/Share/WtKVCache.hpp`、`src/Share/DLLHelper.hpp`、`src/Share/ModuleHelper.hpp`、`src/WTSUtils/WtLMDB.hpp`、`src/WTSTools/WTSLogger.h`
本轮 `src/**` 全程只读，未改任何生产代码，也未改他人用例。
已避开的既有登记项（不重复登记、不复活）：`src/Share/charconv.hpp:560-582` 的 `isGBK` 死循环+越界读（隔离在 `l01_share_wtsutils/l01_converter_charconv_test.cpp` 的 `DISABLED_`）、`WTSBaseDataMgr::getTplIDByPID` 不含 `.` 的 pid 越界读。
`WTSValueArray::at()` 越界返回 `INVALID_DOUBLE` 按 `src/Includes/WTSDataDef.hpp:62-73` 的文档语义处理，不作为缺陷。

---

## BUG-R16-01【中】TimeUtils::fasttime 对 1970 年以前的 epoch 用截断除法，日期算错

- 位置: `src/Share/TimeUtils.hpp:80-105`（`tm->tm_sec = unix_sec % 60`、`tm->tm_mday = (i + time_zone) / 24` 以及 `a = tm->tm_mday + kDaysFromUnixTime` 之后整段推导）
- 现象: 该算法假设 `unix_sec >= 0`。C++ 的 `/`、`%` 对负数是**向零截断**，`unix_sec < 0` 时 `tm_sec` 直接变成负数、`tm_mday` 少算一天，后面的世纪/年月推导（`kMagicUnkonwnFirst/kMagicUnkonwnSec`）也随之偏移；`time_zone` 使 `i + time_zone < 0` 时 `tm_hour` 为负。
- 证据: `src/TestUnits/coverage/r16_share_tools/r16_timeutils_test.cpp` 的 `DISABLED_fasttime_pre_epoch_date_is_wrong`（启用用例只喂 `unix_sec >= 0` 的输入，并与 `gmtime_r` 对拍）。
- 状态: 待审核

## BUG-R16-02【中】Time32 默认构造不初始化内部 tm，date()/time()/fmt() 读未初始化内存

- 位置: `src/Share/TimeUtils.hpp:374`（`Time32():_msec(0){}`），受影响读取 `:408-430`
- 现象: `struct tm t` 是裸 POD 成员，默认构造既不 memset 也不赋值；`Time32 t; t.date();` 读的是未初始化栈内存（`tm_year+1900` 可以是任意值）。`from_local_time()` 只有在被调用之后才安全。
- 证据: 启用用例 `r16_timeutils.time32_from_local_time_overwrites_previous_value` 先 `from_local_time()` 再读取（直接读默认构造对象的 `date()` 是 UB，无法写成可证伪断言，故本条只登记）。
- 状态: 待审核

## BUG-R16-03【高】StrUtil::replace 空 src 入参死循环并无限吃内存

- 位置: `src/Share/StrUtil.hpp:317-334`
- 现象: `str.find("")` 恒返回 0，`pos` 永远是 0、`lastPos = pos + srcLen` 永远是 0 → `while` 不推进，每轮还往 `ret` 追加一次 `des`：调用线程直接挂死 + 内存无限增长。
- 影响面: 任何把外部配置值当作 `src` 传进来的调用（例如 `WTSLogger.cpp:150/159` 的 `StrUtil::replace(filename, "%s", catName)`，只要模板串为空即触发）。
- 证据: `r16_strutil_stdutils_test.cpp` 的 `DISABLED_replace_with_empty_source_hangs`（触发条件：`StrUtil::replace(s, "", "x")`）。
- 状态: 待审核

## BUG-R16-04【高】StrUtil::standardisePath 空串时用 `(size_t)-1` 下标读字符串尾部

- 位置: `src/Share/StrUtil.hpp:306-315`（`if (path[path.length() - 1] != '/' && bIsDir)`）
- 现象: `path.length()==0` 时 `path.length()-1` 下溢为 `SIZE_MAX`，`std::string::operator[]` 越界（libc++ 带 `_LIBCPP_ASSERT` 时 abort，release 下是未定义读），且读到的字节决定要不要补 `/`。
- 影响面: 该函数被 `WTSLogger.cpp:106 checkDirs`、`ModuleHelper.hpp:88`、各 Runner 的日志/数据目录拼接广泛调用，配置里给出空目录即触发。
- 证据: `r16_strutil_stdutils_test.cpp` 的 `DISABLED_standardise_path_empty_string_reads_before_begin`。
- 状态: 待审核

## BUG-R16-05【中】IniHelper::load 吞掉解析异常后仍把对象标记为已加载

- 位置: `src/Share/IniHelper.hpp:73-86`（`try { read_ini(...) } catch(...) {}` 之后无条件 `_loaded = true;`）
- 现象: 文件不存在/格式非法时异常被静默丢弃，`isLoaded()`(`:108`) 仍返回 true，调用方无法区分「加载成功但内容为空」与「根本没读到」；后续所有 `readXxx` 只会一路返回默认值，配置错误被吞干净。
- 证据: `r16_ini_boostfile_test.cpp` 的 `load_missing_file_still_marks_loaded`（断言实际行为）+ `DISABLED_load_missing_file_should_not_mark_loaded`（断言期望行为）。
- 状态: 待审核

## BUG-R16-06【高】IniHelper 用固定 64 字节 static 缓冲拼「节.键」路径，长名静默截断并互相覆盖

- 位置: `src/Share/IniHelper.hpp:59`（`MAX_KEY_LENGTH = 64`）与 `:183-184`、`:199-200`、`:215-216`、`:232-233`、`:248-249`、`:351-352`、`:367-368`、`:382-383`、`:400-401`、`:416-417`
- 现象: 所有读写都走 `static char path[64] = {0}; snprintf(path, 64, "%s.%s", szSec, szKey);`。
  1. 「节名+1+键名」超过 63 字符时**尾部被截掉**，两条只差尾字符的键会被折叠成同一条 ptree 路径，后写覆盖前写（静默数据丢失，`save()` 之后磁盘上也只剩一条）；
  2. `path` 是函数级 `static`，多个 IniHelper 实例共用同一块缓冲区，`static` 且非线程安全（并发读配置会互相踩）。
- 证据: `r16_ini_boostfile_test.cpp` 的 `long_section_and_key_are_silently_truncated_to_63_chars`（喂 30 字符节 + 33 字符键，两条只差末位 → `readString(keyA)` 返回 `valueB`）+ `DISABLED_long_section_and_key_should_not_collide`。
- 状态: 待审核

## BUG-R16-07【高】WtKVCache::put 不校验 len，越过 64 字节字段写坏下一条目的键

- 位置: `src/Share/WtKVCache.hpp:211-232`（`put`，另 `:234-253` 的 `put_if_none` 同形）、字段定义 `:32-45`（`_key[64]`/`_val[64]`）、拷贝实现 `src/Includes/WTSMarcos.h:98-104`（`wt_strcpy` 直接 `memcpy(des, src, len); des[len]='\0';`）
- 现象: `len` 由调用方传入且不做上界钳制。`len > 63` 时 `memcpy` 写穿 `_val[64]`，`des[len]` 的终止符落在**下一条 CacheItem 的 `_key` 里**：键空间被污染，`_indice` 里多出一个由数据填出来的幽灵键；`len == 64` 时终止符写到映射区末尾之外（缓存恰好写满时是映射区外的越界写）。键名本身超过 63 字符同理。
- 证据: `r16_kvcache_test.cpp` 的 `put_with_len_beyond_64_corrupts_next_item_key`（喂 len=70 → `has("xxxxxx")` 为真）+ `DISABLED_put_with_len_beyond_64_should_be_rejected`。
- 状态: 待审核

## BUG-R16-08【高】WtKVCache::init 日期变更时先用文件头里的旧 capacity 清零，早于容量自愈 → 越界写映射区

- 位置: `src/Share/WtKVCache.hpp:144-152`（`memset(&_cache._block->_items, 0, sizeof(CacheItem)*_capacity)`）与 `:161-179`（按 `realSz` 回填 `_capacity/_size` 的自愈分支）
- 现象: 清零用的是**文件头里标记的** `_capacity`，而「实际文件大小与 capacity 不一致就校正」的逻辑排在它后面。缓存文件被截短（旧版本写入、异常退出、磁盘满、手工 truncate）后，用**不同的 uDate** 再 init：`memset` 长度 = 旧 capacity×128，远大于 `mapped_region` 实际长度 → 写越过映射尾部 → SIGBUS/SIGSEGV。同日期分支反而是安全的（不 memset，直接走自愈），即缺陷只在「换日期」这条路径暴露。
- 证据: `r16_kvcache_test.cpp` 的 `DISABLED_truncated_file_other_date_memset_out_of_mapping`（触发条件三步写在用例注释里）；同文件的启用用例 `truncated_file_same_date_repairs_capacity_from_real_size` 给出对照（同日期 → 正常自愈，容量回填 10）。
- 状态: 待审核

## BUG-R16-09【高】WtLMDBQuery::get 忽略传入的 key，恒返回库里的第一条记录

- 位置: `src/WTSUtils/WtLMDB.hpp:194-214`（`mKey` 只是被赋值给 `MDB_val`，随后 `mdb_cursor_get(cursor, &mKey, &mData, MDB_NEXT)` 用的是 `MDB_NEXT`，既不是 `MDB_SET` 也不是 `MDB_SET_KEY`）
- 现象: 新打开的游标上 `MDB_NEXT` 等价于 `MDB_FIRST`，返回的是最小键所在记录；调用方传什么 key 都一样，**查不存在的键也会拿到一条数据**（非空串）。所有按 code/日期取记录的调用点拿到的都是错数据，且没有任何错误码。
- 证据: `r16_lmdb_test.cpp` 的 `get_ignores_the_key_and_returns_first_record`（库里 a→va、b→vb；查 `b`、查 `does_not_exist` 都拿到 `va`）+ `DISABLED_get_should_honour_the_key`。
- 状态: 待审核

## BUG-R16-10【中】WtLMDB::open 用局部变量遮蔽成员 _errno，打开失败后错误状态全失真

- 位置: `src/WTSUtils/WtLMDB.hpp:73`、`:77`、`:81`（三处 `int _errno = ...`，成员是 `int _errno`(`:100`)）
- 现象: `mdb_env_create`/`mdb_env_open`/`mdb_env_set_mapsize` 的返回码全部写进同名局部变量，成员 `_errno` 保持构造时的 0 → 之后 `has_error()`(`:88`) 恒 false、`errmsg()`(`:92-95`) 恒返回 `mdb_strerror(0)`＝"Successful return: 0"，调用方无从判断环境到底哪里没打开。另外 `:81` 的 `set_mapsize` 是**在 open 之后**才调，对已打开环境的扩容语义与注释期望不一致。
- 证据: `r16_lmdb_test.cpp` 的 `open_failure_leaves_member_errno_untouched`（对一个与目录同名的普通文件调 `open()` → 返回 false 但 `has_error()` 为 false、`errmsg()` 是 "Successful return: 0"）+ `DISABLED_open_failure_should_set_member_errno`。
- 状态: 待审核

## BUG-R16-11【高】WtLMDBQuery::get_all 多返回一条幽灵记录；空库时读未初始化 MDB_val；游标未关闭

- 位置: `src/WTSUtils/WtLMDB.hpp:371-390`
- 现象: 循环体是「先 `mdb_cursor_get` 再无条件 emplace」，`MDB_NOTFOUND` 那一轮同样被 emplace。`mdb_cursor_next` 在未命中时提前 return，不改写调用方的 `MDB_val`（见 `src/WTSUtils/lmdb/mdb.c:5945-5951` 的 `mc->mc_flags |= C_EOF; return rc;` 分支），于是末元素是上一条记录的**副本**，返回值 `ayVals.size()` 也被抬成 N+1。空库（一条都没写）时更糟：`_errno` 初始是 cursor_open 的 `MDB_SUCCESS`，第一轮就 NOTFOUND，`bKey/mData` 是**未初始化的栈变量** → `std::string(野指针, 随机长度)`，崩溃或巨量分配。此外 `get_all` 是唯一没有 `mdb_cursor_close` 的查询函数（依赖事务结束回收）。
- 证据: `r16_lmdb_test.cpp` 的 `get_all_appends_a_phantom_duplicate_of_the_last_record`（喂 x、y 两条 → 实际返回 3 条，第 3 条是 y 的副本）+ `DISABLED_get_all_on_empty_db_reads_uninitialised_mdb_val`。
- 状态: 待审核

## BUG-R16-12【中】WTSLogger::log_by_cat_prefix 的 format/args 形参被注释掉，消息正文丢失

- 位置: `src/WTSTools/WTSLogger.h:336-347`（`const char* /*format*/, const Args& .../*args*/`，函数体里只有 `strcpy(m_buffer+1, catName)`，没有任何 `fmtutil::format_to`）
- 现象: 与姊妹函数 `log_dyn_prefix`(`:385-397`，它确实 `fmtutil::format_to(s, format, args...)`) 相比，这里只拼了 `[分类]` 前缀就把 `m_buffer` 交给 `log_raw_by_cat`，正文是**同线程上一次日志的残留**（`m_buffer` 是 `thread_local` 静态，`:447`）。另外 `strcpy(m_buffer+1, catName)` 对 2048 字节缓冲区无长度检查，分类名超长即溢出写。
- 证据: `r16_logger_dll_test.cpp` 的 `DISABLED_log_by_cat_prefix_should_format_the_message`（未初始化路径直接 `fmt::print` 到 stdout，进程内拿不到文本，故只能放 DISABLED_）。
- 状态: 待审核

## BUG-R16-13【高】DLLHelper::wrap_module 对不含字母的模块名越过字符串结尾继续扫描

- 位置: `src/Share/DLLHelper.hpp:76-78`（`std::size_t idx = 0; while (!isalpha(name[idx])) idx++;`）
- 现象: 名字里只要没有字母（`""`、`"123"`、`"_.-"`、纯中文 UTF-8），`isalpha` 对 `'\0'` 也返回 false，循环**越过终止符**继续读堆/栈外内存直到撞上某个非 ASCII 字节：轻则把 `name + idx` 指到越界处再拼进文件名，重则死循环。且 `isalpha` 收的是 `char`（ARM/x86 上 signed），非 ASCII 字节传进去是 UB（断言/崩溃）。
- 附带: 正常路径也有排序问题——首个字母之前的字符被放到 `lib` 前缀**前面**（`"_sm"` → `"_libsm.dylib"`），该文件名不可能存在；已经带 `lib` 前缀的名字会被二次加前缀（`"libfoo"` → `"liblibfoo.dylib"`）。
- 证据: `r16_logger_dll_test.cpp` 的 `DISABLED_wrap_module_without_alpha_hangs`（触发条件：`DLLHelper::wrap_module("123")` 或 `wrap_module("")`）；上述两个排序后果由启用用例 `wrap_module_moves_leading_non_alpha_before_prefix`、`wrap_module_prefixes_lib_and_dylib` 实测断言。
- 状态: 待审核

## BUG-R16-14【低】WtKVCache::init 忽略 BoostFile 建表/截断的返回值

- 位置: `src/Share/WtKVCache.hpp:124-133`（`bf.create_new_file(filename); bf.truncate_file(...); bf.close_file();` 三个返回值全部丢弃）
- 现象: 目标路径不可写/父目录不存在时，`create_new_file`、`truncate_file` 都失败但流程继续，最终只在 `map()` 处得到一个笼统的 "Mapping cache file failed"，真实原因（权限/磁盘/路径）被抹掉；`truncate_file` 失败还会留下 0 字节文件，后续 `map` 成功而 `_capacity/_size` 全在映射之外。
- 证据: `r16_kvcache_test.cpp` 的 `init_on_unwritable_path_fails_with_log`（断言到的是 "Mapping cache file failed" 而不是建表失败）。
- 状态: 待审核

## BUG-R16-15【低】BoostFile::read_file_contents 向 const string 的缓冲区写入

- 位置: `src/Share/BoostFile.hpp:188-198`（`buffer.resize(filesize); return bf.read_file((void *)buffer.c_str(), filesize);`）
- 现象: 用 `(void*)` 抹掉 `const` 后把 `c_str()` 当写缓冲传给 `read(2)`。`std::string` 允许 SSO/自定义分配器实现把该区域视为只读，这是标准意义上的 UB（当前 libc++ 实测可工作，所以启用用例 `static_write_and_read_file_contents_roundtrip` 能过）；同时 `unsigned int filesize` 截断，>4GB 的文件会按低 32 位读。
- 状态: 待审核

## BUG-R16-16【低】WTSLogger 的级别守卫让 LL_NONE 成为未初始化态下唯一可达的日志入口

- 位置: `src/WTSTools/WTSLogger.h:163、189、215、241、267、294、316、338、364、387`（`if (m_logLevel > ll || m_bStopped) return;`）配合 `src/Includes/WTSTypes.h:129-137`（`LL_ALL=100 … LL_NONE=105`，`WTSLogger.cpp:39` 把 `m_logLevel` 初值设为 `LL_NONE`）
- 现象: 枚举里数值最大的反而是「不打日志」，于是默认态下 `105 > ll` 对一切真实级别成立（debug/info/warn/error/fatal 全被静默丢弃），而显式传 `LL_NONE` 反而放行。日志级别语义整体是反的：`level=debug`（101）时 `LL_NONE`(105) 也算放行。测试里为了在没有线程池的前提下覆盖这些模板，只能喂 `LL_NONE`。
- 状态: 待审核

## BUG-R16-17【低】IniHelper::save 对空文件名与写失败无任何防护

- 位置: `src/Share/IniHelper.hpp:95-101`
- 现象: 未 `load()` 或 `load()` 失败时 `_fname` 为空串，`write_ini("")` 直接把 boost 的异常抛给调用方（与 `load()` 吞异常的风格相反，同一对象的读写错误语义不对称）；目标目录不存在时同样抛出，且 `save()` 没有 `try`，调用方普遍不捕获 → 配置保存失败即终止流程。
- 证据: `r16_ini_boostfile_test.cpp` 的 `write_then_save_then_reload_roundtrip`、`save_without_filename_falls_back_to_load_path` 覆盖成功分支；异常分支需构造只读目录，本轮不落用例。
- 状态: 待审核

## BUG-R16-18【中】WtLMDBQuery::get_range 用「当前键长度」做区间上界比较，变长键判定失真；并有一处死代码

- 位置: `src/WTSUtils/WtLMDB.hpp:246`（`memcmp(lKey.mv_data, rKey.mv_data, lKey.mv_size) > 0`）、`:234-235`（`if (_errno != MDB_SUCCESS) return 0;` 在 `:223-225` 已经判过，永不为真）
- 现象: 比较长度取的是**游标命中的当前键**的字节数，而不是上边界 `upper_key` 的长度。定长键（bar/tick 的 yyyymmdd+… 主键）恰好看不出来，变长键则会被截短比较：例如上界 `"abcd"`、当前键 `"ab"` 时只比 2 字节 → 判定「未越界」而把不该返回的记录收进结果；反之当前键更长时会多读上界之外的字节。`get_uppers`(`:350`) 与 `get_lowers`(`:295、:302`) 取的是 `upper_key.size()/lower_key.size()`，同一个函数族两套边界比较口径。
- 证据: `r16_lmdb_test.cpp` 的 `put_then_get_range_returns_exact_window` 用定长单字符键锁定当前语义（变长键的偏差无法在不改产品代码的前提下写成稳定断言）。
- 状态: 待审核
