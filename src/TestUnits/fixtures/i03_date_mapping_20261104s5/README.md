# i03_date_mapping_20261104s5：日期映射调用链、Session 段与时钟失败（S5 date-session 包）

关联：BUG-R12-13、BUG-L03-009、BUG-L03-010、BUG-L01-018（本机部分）、I03-NEW-006、I03-NEW-011。合同依据：决策卡 1（A+C）、卡 2（仅 0–0 表示全天，其他相同起止拒绝）。

只使用本地 fake / loopback，不调用供应商 API、不连接柜台、不发组播数据。

| 可执行文件 | 内容 | 预期 |
|---|---|---|
| `date_client` | `DateMappingClient` 旧接口守卫（非法输入不进虚函数、缺商品/会话/日历名、非法返回值与异常）、date=0 单次时钟快照与时钟失败、V1 表路径与 checked 管理器一致、缺日历只有 V1 可见、`tryGetNextDate` 矩阵（含 19691231、闰年、四位年边界、8 万天星期一致性）、交易段重叠/混合 0–0 拒绝及配置整批不发布 | 通过 |
| `reader_chain` / `_baseline` | `WtRdmDtReader`、`WtDataReader` 调用链：非法端点、缺会话、时钟失败不再把非法/0 日期传给旧虚函数且不返回切片；合法区间用显式日期映射 | 当前通过；改前源码失败（红控） |
| `xele_chain` / `_baseline` | `ParserXeleSkt`：映射失败（缺商品、旧接口返回 0/非法日期、抛异常）不改写缓存行情、不投递；`ActionDay/UpdateTime` 无 NUL 结尾或含非数字时清零而不越界 | 当前通过；改前源码投递错误行情并在 UBSan 下 `index 10 out of bounds for type 'char[10]'` |
| `xele_tsan` / `_baseline` | 两线程并发 `prepare()` 解析快照时钟字段 | 当前无 TSan 报告；改前源码 `static char str[10]` 数据竞争 |
| `monitor_chain` / `_baseline` | `StateMonitor::advanceOnce` 对非法日期、date=0、非法时间、越界年份不做任何状态转换；合法工作日与节假日对照 | 当前通过；改前源码在非法快照上转换状态 |
| `tz_legacy` / `_baseline` | 旧 `TimeUtils` 时钟函数：时区查询失败（`mktime` 注入失败）不把哨兵 99 当小时偏移；时钟失败发布非法日期 19000100 而不是貌似合法的日期；合法日期运算不依赖 `mktime` | 当前通过；改前头文件得到 +4 天的日期、`19700101` 等 |

运行（仓库根目录；`BEFORE` 为保存改前副本的目录，其中 `src/...` 保留相对路径）：

```sh
sh src/TestUnits/fixtures/i03_date_mapping_20261104s5/run.sh \
  /tmp/wt-i03-s5-date-session-fixture /tmp/wt-i03-s5-date-session-run/fixture2 \
  /tmp/wt-i03-s5-date-session-cov \
  "$PWD/docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-date-session/before"
```

编译器为 Homebrew LLVM 22.1.8，ASan+UBSan（TSan 变体单列）；链接的 `libWTSTools.a`/`libWTSUtils.a` 来自覆盖率构建，未做 sanitizer 插桩。时钟、`mktime` 失败由可执行文件内的符号插桩注入，只影响本进程。

限制：旧接口守卫路径（卡 1 C）无法通过 `IBaseDataMgr` 判断节假日日历是否已加载（见 `MissingCalendarIsOnlyVisibleThroughTheTable`）；插件侧取得 V1 表需宿主协商（见证据 README 的决策卡草稿）。不覆盖 Windows/MSVC、真实行情/柜台、DST 实地切换、AD 读端（另由 adstorage 目标的既有用例回归）和整体回归。
