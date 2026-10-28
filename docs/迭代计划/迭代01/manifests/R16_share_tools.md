# R16 用例清单（用例 → 生产文件:行段）

lane: R16　用例目录: `src/TestUnits/coverage/r16_share_tools/`（由主目标 `CoverageTests` 的 `GLOB_RECURSE` 自动收编，未改 `src/TestUnits/coverage/CMakeLists.txt`，未碰 `r12_headers/`）
门禁口径（真实构建 flags，**6/6 文件 exit 0**，含 `-Werror`）：

```
cd build-cov && ninja -t commands TestUnits/coverage/CMakeFiles/CoverageTests.dir/l01_share_wtsutils/l01_codehelper_test.cpp.o | tail -1
# 把 -o X.o -c Y 换成 -fsyntax-only <r16_share_tools/xxx.cpp>
```

统计：5 个 TU + 1 个内部头（`r16_tmp.h`），115 个 TEST 块 = **103 条启用 + 12 条 DISABLED**。
本轮范围（api-brief-R16 列出的 12 个文件/约 1049 行 UNCOVERED）里实际可测面：`TimeUtils.hpp`、`StrUtil.hpp`、`StdUtils.hpp`、`IniHelper.hpp`、`BoostFile.hpp`、`WtKVCache.hpp`、`WtLMDB.hpp`、`WTSLogger.h`、`DLLHelper.hpp`、`ModuleHelper.hpp`。
与既有 lane 的分工：`l01_share_wtsutils/`（getNextMinute/getNextMonth/minBar/makeTime/timeToString 正常分支/getNextDate/getWeekDay/isWeekends/Time32::fmt/Ticker/挂钟入口）与 `ws01_share_core/ws01_strutil_test.cpp`（trim/split/大小写/startsWith/endsWith/standardisePath/replace/isEmpty 的正常分支）已覆盖的接口本轮不重复；`charconv.hpp` 的 `isGBK`、`WTSBaseDataMgr::getTplIDByPID` 两条已登记项未复活。
行号均取自本轮实际 Read 到的生产源码。无 `#define private public`、无线程/真实 socket/真实 shm、无挂钟断言；文件 IO 只落 `/tmp/wondertrader_r16_<tag>_<pid>/`（用例间可并行）。

## 1. `r16_timeutils_test.cpp`（15 启用 + 1 DISABLED）→ `src/Share/TimeUtils.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_timeutils.fasttime_epoch_zero_utc | TimeUtils.hpp:80-105（epoch=0/tz=0 全字段，与 `gmtime_r` 对拍） |
| r16_timeutils.fasttime_matches_libc_for_2023_epoch_with_plus8_zone | TimeUtils.hpp:88-104（跨日进位 + `time_zone` 加法 `:92-93`） |
| r16_timeutils.fasttime_handles_minute_and_second_fields | TimeUtils.hpp:88-90（sec/min 取模）+ `:100-103`（世纪/年月推导，2000-02-29T12:34:56Z） |
| r16_timeutils.fasttime_negative_timezone_offset | TimeUtils.hpp:92-93（`i + time_zone` 为负方向的回卷） |
| r16_timeutils.fasttime_does_not_fill_week_or_dst_fields | TimeUtils.hpp:88-104（只写 6 个字段；wday/yday/isdst 保持调用方值） |
| r16_timeutils.DISABLED_fasttime_pre_epoch_date_is_wrong | TimeUtils.hpp:88-94（负 epoch 截断除法 → BUG-R16-01） |
| r16_timeutils.time32_date_and_time_from_local_epoch | TimeUtils.hpp:376-384（`Time32(time_t, msecs)`）、`:408-421`（date/time/time_ms） |
| r16_timeutils.time32_ctor_from_milliseconds_splits_msec | TimeUtils.hpp:386-395（毫秒入参拆分 `_msec`） |
| r16_timeutils.time32_ctor_without_msecs_defaults_to_zero | TimeUtils.hpp:376-384（`msecs` 默认实参） |
| r16_timeutils.time32_from_local_time_overwrites_previous_value | TimeUtils.hpp:374（默认构造，见 BUG-R16-02）、`:397-406`（from_local_time）、`:408-421` |
| r16_timeutils.time32_time_ms_is_time_times_1000_plus_msec | TimeUtils.hpp:413-421（time_ms 与 time 的自洽关系） |
| r16_timeutils.time_to_string_returns_empty_for_small_negative_input | TimeUtils.hpp:262-265（`msec < 0` 提前返回分支） |
| r16_timeutils.time_to_string_returns_empty_for_zero | TimeUtils.hpp:262（`mytime == 0`） |
| r16_timeutils.time_to_string_without_millisec_has_no_dot | TimeUtils.hpp:277-280（无毫秒格式化分支，长度 14） |
| r16_timeutils.time_to_string_with_millisec_appends_dot_msec | TimeUtils.hpp:274-276（有毫秒分支，`.007` 后缀） |
| r16_timeutils.tz_offset_is_cached_and_in_whole_hours | TimeUtils.hpp:61-78（static 缓存 + 整小时抹平） |

## 2. `r16_strutil_stdutils_test.cpp`（33 启用 + 2 DISABLED）→ `src/Share/StrUtil.hpp`、`src/Share/StdUtils.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_strutil.trim_inplace_left_only_keeps_right_ws | StrUtil.hpp:33-39（`right=false` 分支） |
| r16_strutil.trim_inplace_right_only_keeps_left_ws | StrUtil.hpp:33-39（`left=false` 分支） |
| r16_strutil.trim_inplace_custom_delims | StrUtil.hpp:33-39（自定义 delims `"#"`） |
| r16_strutil.trim_cstr_only_right | StrUtil.hpp:41-50（cstr 重载 + 单向裁剪） |
| r16_strutil.trim_all_chars_removed_yields_empty | StrUtil.hpp:35-38（全裁剪 → 空串，`npos+1` 边界） |
| r16_strutil.trimAll_chained_filter_matches_doc | StrUtil.hpp:53-62（`pattern(chars)` + `remove_if`，文档示例 `A12`） |
| r16_strutil.trimAll_default_only_whitespace_family | StrUtil.hpp:53-62（默认 `" \t\n\r"`） |
| r16_strutil.find_first_of_nul_returns_length | StrUtil.hpp:64-79（查 `'\0'` 命中终止符） |
| r16_strutil.find_first_not_found_is_npos | StrUtil.hpp:72-78（未命中 → npos） |
| r16_strutil.find_last_at_end_and_absent | StrUtil.hpp:81-92（末位命中/未命中/长度 1） |
| r16_strutil.split_respects_max_splits | StrUtil.hpp:111-116（`maxSplits` 命中 → 余串整体入列） |
| r16_strutil.split2_respects_max_splits | StrUtil.hpp:183-188（split2 的 maxSplits 分支） |
| r16_strutil.split2_advances_by_delim_length | StrUtil.hpp:176-194（`start = pos + delims.size()`，双字符分隔符） |
| r16_strutil.split_leading_delim_yields_empty_first | StrUtil.hpp:105-110（`pos == start` 空项分支） |
| r16_strutil.split_output_version_appends_to_existing_vector | StrUtil.hpp:131-164（出参版本向已有数组追加） |
| r16_strutil.split_empty_string_yields_one_empty_item | StrUtil.hpp:111-116（空串输入） |
| r16_strutil.replace_all_occurrences_and_longer_target | StrUtil.hpp:317-334（多处命中 + 目标更长） |
| r16_strutil.replace_no_match_keeps_string | StrUtil.hpp:322-331（未命中路径） |
| r16_strutil.replace_shrinks_target | StrUtil.hpp:325-331（目标更短的收缩） |
| r16_strutil.DISABLED_replace_with_empty_source_hangs | StrUtil.hpp:322-330（空 src → 死循环，BUG-R16-03） |
| r16_strutil.standardise_path_non_dir_no_trailing_slash | StrUtil.hpp:306-315（`bIsDir=false` + `\\`→`/` 替换 `:310`） |
| r16_strutil.DISABLED_standardise_path_empty_string_reads_before_begin | StrUtil.hpp:311（空串下标下溢越界读，BUG-R16-04） |
| r16_stdutils.stdlocker_locks_and_releases_on_scope_exit | StdUtils.hpp:34-50（`std::mutex` + RAII 解锁，`try_lock` 验证） |
| r16_stdutils.stdlocker_locks_once_and_unlocks_once_on_destroy | StdUtils.hpp:38-46（自计数假锁：构造 1 次 lock、析构 1 次 unlock） |
| r16_stdutils.stdlocker_nested_scopes_lock_twice_unlock_twice | StdUtils.hpp:38-46（嵌套作用域 2/2 计数） |
| r16_stdutils.stdfile_write_then_read_whole_file | StdUtils.hpp:115-124（string 版 write）、`:57-113`（length=0 读全量 `:75-78`） |
| r16_stdutils.stdfile_read_missing_file_throws | StdUtils.hpp:60-63（`File does not exist`） |
| r16_stdutils.stdfile_read_offset_beyond_size_throws | StdUtils.hpp:69-72（`Offset exceeds file size`） |
| r16_stdutils.stdfile_read_offset_equal_size_returns_zero | StdUtils.hpp:69-78（offset==size → length 0 → 返回 0） |
| r16_stdutils.stdfile_read_clamps_length_to_file_end | StdUtils.hpp:81-84（offset+length 越界夹回） |
| r16_stdutils.stdfile_read_partial_window | StdUtils.hpp:94-109（seekg + 定长窗口读取） |
| r16_stdutils.stdfile_write_raw_buffer_overwrites_and_reads_back | StdUtils.hpp:126-135（void* 版 write） |
| r16_stdutils.stdfile_write_to_missing_directory_throws | StdUtils.hpp:117-121（`Failed to open file for writing`） |
| r16_stdutils.stdfile_exists_true_for_file_false_for_missing | StdUtils.hpp:137-146（exists 两个返回值） |
| r16_stdutils.stdfile_empty_file_read_returns_zero | StdUtils.hpp:75-78、`:100-109`（length==0 跳过 read） |

## 3. `r16_ini_boostfile_test.cpp`（22 启用 + 2 DISABLED）→ `src/Share/IniHelper.hpp`、`src/Share/BoostFile.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_inihelper.load_sets_loaded_flag | IniHelper.hpp:66（构造 `_loaded(false)`）、`:73-86`（load/`isLoaded` `:108`） |
| r16_inihelper.read_string_int_double_bool_from_loaded_file | IniHelper.hpp:159-170、`:181-186`、`:197-202`、`:213-218`、`:230-235`、`:246-251` |
| r16_inihelper.read_returns_default_when_missing_or_unparsable | IniHelper.hpp:162-169（`get<T>` 未命中/转换异常 → 默认值，5 种类型） |
| r16_inihelper.read_sections_and_key_arrays | IniHelper.hpp:260-268（readSections）、`:278-295`（readSecKeyArray + catch）、`:307-324`（readSecKeyValArray + catch） |
| r16_inihelper.write_then_save_then_reload_roundtrip | IniHelper.hpp:95-101（`save(filename)` `:98`）、`:334-338`、`:349-419`（五种类型写入） |
| r16_inihelper.save_without_filename_falls_back_to_load_path | IniHelper.hpp:99-100（`_fname` 回落分支） |
| r16_inihelper.remove_value_and_remove_section | IniHelper.hpp:118-129（removeValue，含节/键缺失的 catch）、`:137-147`（removeSection） |
| r16_inihelper.write_overwrites_existing_value | IniHelper.hpp:334-338、`:365-370`（`ptree::put` 覆盖语义） |
| r16_inihelper.load_missing_file_still_marks_loaded | IniHelper.hpp:76-86（`catch(...) {}` 后仍 `_loaded = true`，BUG-R16-05 实际行为） |
| r16_inihelper.DISABLED_load_missing_file_should_not_mark_loaded | 同上（期望行为，BUG-R16-05） |
| r16_inihelper.long_section_and_key_are_silently_truncated_to_63_chars | IniHelper.hpp:59（`MAX_KEY_LENGTH`）、`:351-352`、`:183-184`（snprintf 截断 → 键折叠，BUG-R16-06） |
| r16_inihelper.DISABLED_long_section_and_key_should_not_collide | 同上（期望行为，BUG-R16-06） |
| r16_boostfile.fresh_handle_is_invalid_and_close_is_idempotent | BoostFile.hpp:35-42（ctor/dtor）、`:66-83`（is_invalid_file/valid/close_file） |
| r16_boostfile.create_new_file_truncates_to_zero | BoostFile.hpp:44-51（create_new_file + `truncate_file(0)`）、`:85-101`（get_file_size 两版） |
| r16_boostfile.create_new_file_on_existing_clears_content | BoostFile.hpp:44-51（对已存在文件重新截零） |
| r16_boostfile.open_existing_missing_returns_false | BoostFile.hpp:60-64（open 失败）、`:103-112`（静态 get_file_size 缺失 → 0） |
| r16_boostfile.seek_begin_current_end_and_pointer | BoostFile.hpp:114-145（set_file_pointer/seek_to_begin/seek_current/seek_to_end/get_file_pointer 两版） |
| r16_boostfile.write_after_truncate_shrinks_file | BoostFile.hpp:85-93（truncate_file + 尺寸回读） |
| r16_boostfile.read_file_exact_length_true_short_read_false | BoostFile.hpp:157-177（read_file 的全长判定 + read_file_length 的实际字节数） |
| r16_boostfile.write_to_closed_handle_fails | BoostFile.hpp:76-83、`:85-88`、`:147-150`（无效句柄上的写/截断返回 false） |
| r16_boostfile.static_write_and_read_file_contents_roundtrip | BoostFile.hpp:188-206（read_file_contents/write_file_contents 静态入口） |
| r16_boostfile.read_file_contents_false_for_missing_or_empty | BoostFile.hpp:191-195（打开失败 / filesize==0 两个 false 分支） |
| r16_boostfile.delete_file_true_then_false | BoostFile.hpp:183-186（delete_file 命中/未命中） |
| r16_boostfile.create_or_open_keeps_existing_size | BoostFile.hpp:53-58（create_or_open 不截断） |

## 4. `r16_kvcache_test.cpp`（11 启用 + 2 DISABLED）→ `src/Share/WtKVCache.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_kvcache.uninitialized_object_reports_empty | WtKVCache.hpp:202-209（get 未命中）、`:255-274`（has/size/capacity 的 `_block==NULL` 分支） |
| r16_kvcache.init_creates_file_with_initial_capacity | WtKVCache.hpp:120-142（新建文件 + map）、`:154-160`（`_capacity=SIZE_STEP`/`_blk_flag`）、`:8`（SIZE_STEP=200） |
| r16_kvcache.put_get_has_and_overwrite | WtKVCache.hpp:211-232（新增分支 + 命中 `_indice` 分支）、`:202-209`、`:255-258` |
| r16_kvcache.put_if_none_keeps_existing_value | WtKVCache.hpp:234-253（命中直接 return + 未命中插入） |
| r16_kvcache.put_with_len_beyond_64_corrupts_next_item_key | WtKVCache.hpp:32-45（`_key[64]/_val[64]`）、`:211-232`（len 不钳制）＋ `src/Includes/WTSMarcos.h:98-104`（BUG-R16-07 实际后果） |
| r16_kvcache.DISABLED_put_with_len_beyond_64_should_be_rejected | 同上（期望行为，BUG-R16-07） |
| r16_kvcache.put_fills_capacity_then_resizes_to_double | WtKVCache.hpp:220-225（写满触发 resize）＋ `:65-117`（写文件扩尾 + 重新 map + 回填 `_capacity`） |
| r16_kvcache.clear_empties_entries_but_keeps_capacity | WtKVCache.hpp:188-200（clear：`_indice.clear()` + memset + `_size=0`） |
| r16_kvcache.reopen_with_same_date_restores_entries | WtKVCache.hpp:144（日期相同 → 不重置）、`:161-179`（尺寸一致 → 不改）、`:181-185`（重建索引） |
| r16_kvcache.reopen_with_other_date_resets_and_logs | WtKVCache.hpp:144-152（日期不同重置 + logger 文案）、`:62`（CacheLogger） |
| r16_kvcache.init_on_unwritable_path_fails_with_log | WtKVCache.hpp:124-141（create/truncate 返回值被忽略 + map 失败 → logger，BUG-R16-14） |
| r16_kvcache.truncated_file_same_date_repairs_capacity_from_real_size | WtKVCache.hpp:164-179（按真实文件大小回填 `realCap/size`）、`:181-185`（空键被收进索引的可观测后果） |
| r16_kvcache.DISABLED_truncated_file_other_date_memset_out_of_mapping | WtKVCache.hpp:144-152 vs `:161-179`（顺序颠倒 → 越界写映射区，BUG-R16-08） |

## 5. `r16_lmdb_test.cpp`（12 启用 + 3 DISABLED）→ `src/WTSUtils/WtLMDB.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_lmdb.open_fresh_env_sets_handle_and_no_error | WtLMDB.hpp:28-33（ctor 初值）、`:45-46`（env/dbi）、`:57-84`（open 建目录 + env 三步）、`:88`（has_error） |
| r16_lmdb.put_then_get_range_returns_exact_window | WtLMDB.hpp:107-115（query 构造 + begin + update_dbi）、`:144-147`、`:219-260`（get_range 命中窗口 + 回调 `:257`） |
| r16_lmdb.get_uppers_walks_forward_until_count_or_upper | WtLMDB.hpp:333-369（SET_RANGE 前向扫、`:350` 上界比较、`:359-360` count 截断） |
| r16_lmdb.get_lowers_walks_backward_then_reverses | WtLMDB.hpp:269-324（`:283` SET_RANGE、`:286-290` NOTFOUND→MDB_LAST、`:295-302` 边界比较、`:319-320` reverse） |
| r16_lmdb.put_and_commit_variant_persists_without_destructor_commit | WtLMDB.hpp:149-152、`:167-184`（put_and_commit + `_commited=true`）、`:117-123`（析构不再重复 commit） |
| r16_lmdb.rollback_drops_uncommitted_writes | WtLMDB.hpp:126-133（rollback 及 `_commited` 短路）、`:117-123`（只读/已回滚分支） |
| r16_lmdb.commit_is_idempotent_and_reuses_dbi | WtLMDB.hpp:48-55（update_dbi 的 `_dbi != 0` 短路）、`:135-142`（commit 幂等） |
| r16_lmdb.error_accessors_reflect_manual_errno_update | WtLMDB.hpp:86-95（update_errno/has_error/is_readonly/errmsg） |
| r16_lmdb.readonly_env_reports_readonly_and_rejects_put | WtLMDB.hpp:28-32（`_readonly`）、`:90`、`:112-113`（MDB_RDONLY 事务）、`:154-165`（put 失败返回 false + 写回 errno） |
| r16_lmdb.get_ignores_the_key_and_returns_first_record | WtLMDB.hpp:186-214（`:206` 用 MDB_NEXT，入参 key 未被使用，BUG-R16-09 实证） |
| r16_lmdb.DISABLED_get_should_honour_the_key | 同上（期望行为，BUG-R16-09） |
| r16_lmdb.get_all_appends_a_phantom_duplicate_of_the_last_record | WtLMDB.hpp:371-390（`:380-387` NOTFOUND 轮仍 emplace；`mdb.c:5945-5951` 不改写出参，BUG-R16-11） |
| r16_lmdb.DISABLED_get_all_on_empty_db_reads_uninitialised_mdb_val | WtLMDB.hpp:378-387（空库首轮即 NOTFOUND → 读未初始化 MDB_val，BUG-R16-11） |
| r16_lmdb.open_failure_leaves_member_errno_untouched | WtLMDB.hpp:73/77/81（局部 `_errno` 遮蔽成员 `:100`，BUG-R16-10 实证） |
| r16_lmdb.DISABLED_open_failure_should_set_member_errno | 同上（期望行为，BUG-R16-10） |

## 6. `r16_logger_dll_test.cpp`（10 启用 + 2 DISABLED）→ `src/WTSTools/WTSLogger.h`、`src/WTSTools/WTSLogger.cpp`、`src/Share/DLLHelper.hpp`、`src/Share/ModuleHelper.hpp`

| 用例 | 生产行段 |
| --- | --- |
| r16_wtslogger.init_returns_silently_when_config_file_is_missing | WTSLogger.cpp:204-210（`isFile && !exists` 直接 return）、WTSLogger.h:409 |
| r16_wtslogger.init_registers_logger_then_throws_without_root | WTSLogger.cpp:212-235（解析 + 遍历分类）、`:136-202`（initLogger：console_sink `:165-170`、非 async `:179-184`）、`:237-241`（root 缺失 → runtime_error）；观测面是 `spdlog::get/drop` |
| r16_wtslogger.register_handler_accepts_null_and_real_handler | WTSLogger.cpp:250-253、WTSLogger.h:418 |
| r16_wtslogger.log_at_none_level_reaches_uninited_print_path | WTSLogger.h:291-300（log）、`:313-322`（log_by_cat）、`:160-175/186-201/212-227/238-253/264-279`（五级模板的 `m_logLevel > ll` 早退分支）、WTSLogger.cpp:323-332（log_raw 未初始化 → print_message）、`:129-134` |
| r16_wtslogger.log_dyn_creates_dynamic_logger_and_free_all_drops_it | WTSLogger.h:361-370（log_dyn）、WTSLogger.cpp:398-405（log_dyn_raw → getLogger）、`:438-459`（getLogger 的动态模板分支 `:444-455`）、`:216-231`（dyn_pattern 装入 m_mapPatterns）、`:461-471`（freeAllDynLoggers） |
| r16_wtslogger.log_dyn_prefix_builds_bracket_prefix | WTSLogger.h:384-397（`m_buffer[0]='['`、`strcpy(m_buffer+1, catName)`、`fmtutil::format_to(s, ...)`） |
| r16_wtslogger.DISABLED_log_by_cat_prefix_should_format_the_message | WTSLogger.h:336-347（format/args 形参被注释，正文丢失，BUG-R16-12） |
| r16_dllhelper.wrap_module_prefixes_lib_and_dylib | DLLHelper.hpp:69-89（`std::string ret(name, idx)` `:79` + `unixPrefix` + `.dylib` 分支 `:82-86`） |
| r16_dllhelper.wrap_module_moves_leading_non_alpha_before_prefix | DLLHelper.hpp:76-81（首个字母前缀字符被排到 `lib` 之前） |
| r16_dllhelper.DISABLED_wrap_module_without_alpha_hangs | DLLHelper.hpp:77-78（`while (!isalpha(name[idx])) idx++;` 越过 `\0`，BUG-R16-13） |
| r16_dllhelper.load_missing_library_returns_null_and_helpers_tolerate_it | DLLHelper.hpp:26-43（dlopen 失败 + dlerror 打印）、`:45-48`（free_library 判空）、`:57-60`（get_symbol 判空） |
| r16_modulehelper.get_bin_dir_returns_existing_directory_with_trailing_slash | ModuleHelper.hpp:48（inst_hlp）、`:55-66`（getInstPath/dladdr 缓存）、`:78-97`（getBinDir 取目录 + 尾斜杠 + 缓存指针） |

## 留白面（本轮不测 / 不可达，附估计行数）

| 面 | 原因 | 估计未覆盖行 |
| --- | --- | --- |
| `TimeUtils.hpp:110-199、223-232`（getLocalTimeNow/getLocalTime/getYYYYMMDDhhmmss/getDateTime/getCurDate/getCurMin）、`:37-57`（KUSER_SHARED_DATA/SharedUserData 等 `_MSC_VER` 分支） | 断言必须依赖挂钟（硬规则禁止）；Windows 分支在本机不可达 | ≈85 |
| `WTSLogger.h:160-279、313-347` 的 **已初始化** 分支（`*_imp` 落 spdlog logger） | 要置 `m_logLevel` 只能走成功 `init()`，而成功路径含 `spdlog::flush_every`（后台 flush 线程）与文件 sink，硬规则禁止起线程 → seam-blocked，不做 hack | ≈65（含 `WTSLogger.cpp:263-321、334-353、373-395` 的 inited 分支） |
| `WTSLogger.cpp:147-164、186-196`（daily_file_sink/basic_file_sink 建日志文件；async → `spdlog::init_thread_pool`） | 前者要往工作目录写日志文件（与真实构建产物面重叠），后者直接起线程 | ≈40 |
| `IniHelper.hpp:95-101` 的 `save()` 异常路径 | 需要只读目录/空 `_fname`，会抛 boost 异常且行为依赖 umask，暂不落断言（已登记 BUG-R16-17） | ≈6 |
| `StdUtils.hpp:105-108`（`gcount != length` 抛出） | 前置条件是「打开成功但读取变短」，进程内无法稳定构造 | ≈4 |
| `BoostFile.hpp:160-162、171-173`（`_WIN32 ReadFile` 分支） | 本机不可达 | ≈8 |
| `WtKVCache.hpp:88-92、100-110、116`（resize 过程中 logger 记录的三类失败：写扩尾异常 / map 失败 / 二次 map 异常） | 需要让 `BoostFile`/`mmap` 在已存在且可写的文件上精确失败，进程内没有稳定注入口；`_size==_capacity 且 resize 失败后仍继续写`（`:220-231`）的越界写风险因此也无法实测，已在 BUG-R16-07/08 的影响面里说明 | ≈22 |
| `WtLMDB.hpp:234-235`（get_range 死代码）、`:246` 的变长键比较偏差 | 死代码永不执行；变长键偏差属于 BUG-R16-18 的描述面，断言会锁定错误语义 | ≈4 |
| `ModuleHelper.hpp:12-41`（Windows `DllMain`/`g_dllModule` 分支） | 本机不可达 | ≈25 |
