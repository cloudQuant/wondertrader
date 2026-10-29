# L04（WtDataStorage / WtDataStorageAD）缺陷记录

> 本迭代只记录缺陷，不修改生产代码；条目状态以各条为准。

> 复核注记（2026-10-29）：以下仅做源码静态核查，未运行这些测试。测试文件中的夹具或 DISABLED 用例不代表已经运行通过/复现；本机已有 `build-cov` 测试二进制，可供后续定向复核。

## BUG-L04-001

- 状态: 已撤回（迭代02：NOT_BUG，原产品主张不成立）
- 严重级别: 待定（私有函数当前无仓库内调用点）
- 模块/文件: `src/WtDataStorageAD/WtDataReaderAD.cpp:220-227`（`WtDataReaderAD::read_bars_to_buffer`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_ad_lmdb_store_test.cpp`（`BarsCommittedAndReadableBack` 只验证 LMDB 的正确逐条读取语义，未调用本私有函数）
- 输入/前置条件: 直接调用私有 `read_bars_to_buffer(exchg, code, period)` 且 LMDB 中有 K 线记录；当前没有仓库内调用点。
- 期望行为: 把回调收到的每条 value（一个 `WTSBarStruct`）依次拷进 `buffer`，得到 `N * sizeof(WTSBarStruct)` 字节的连续 bar 数组
- 实际行为: `memcpy((void*)buffer.data(), ayVals.data(), sizeof(WTSBarStruct)*ayVals.size())` 的源是 `std::vector<std::string>` 的对象数组首地址（`ayVals.data()`），不是各 value 的 `item.data()`；拷贝的是 string 对象表示而非 K 线 value 字节。
- 复现步骤: 本轮未运行复现；若未来提供可直接驱动私有函数的测试入口，可用多条 LMDB value 对照输出验证对象头误拷贝和查询范围错误。
- 复核裁定: 若被调用，源指针和查询上界确有错误：`WtLMDBQuery::get_range` 回调参数是 `vector<string>`（`src/WTSUtils/WtLMDB.hpp:239-253`），此处把 string 对象表示当作 bar bytes 拷贝，并重复传 `lKey`。但该方法在头文件中为 private，当前仓库 `rg read_bars_to_buffer` 只有声明/定义、没有调用点；当前产品可达性未证，原“回测/导出调用方”和 Blocker 级影响不成立。
- 修复方向: 若该方法已无用途，删除死代码；若后续要启用，传 `rKey` 作为上界，逐个校验 `item.size()` 后从 `item.data()` 拷贝，并新增直接驱动该方法的测试。当前证据不足以按已发生产品故障定级。


- 迭代02复核: NOT_BUG。函数内部确实将 std::vector<std::string> 对象区误当作 WTSBarStruct payload，并把 range 的右 key 写成左 key；但这是 private helper，仓库调用搜索只有声明/定义，没有生产调用路径。现有 LMDB test 不调用它，当前不能把不可达私有函数的问题计为产品运行缺陷。
- 迭代02依据: [逐条裁定与主代理修订](../../迭代02/缺陷裁定总表.md)。原记录保留作为历史，未声称产品已修复。
- 迭代02前状态（历史）: - 状态: 待审核
## BUG-L04-002

- 状态: 待审核
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtDataReader.cpp:50-52, 89`（自由函数 `proc_block_data`）
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_proc_block_data_test.cpp`（仅覆盖长度合法的路径；越界路径未写成测试以免整轮用例被 ASAN 打断）
- 输入/前置条件: `content` 长度小于 `sizeof(BlockHeader)`（12 字节），例如被截断 / 0 字节的 `.dsb` 文件
- 期望行为: 校验长度后返回 `false`，不触碰缓冲区之外的内存
- 实际行为: 直接 `BlockHeader* header = (BlockHeader*)content.data()` 读取 `_version`；老版本未压缩分支还会执行 `content.size() - BLOCK_HEADER_SIZE`，长度不足时 `size_t` 下溢为巨大值传给 `buffer.append`
- 复现步骤: 源码可确认边界路径；本轮未运行越界复现。尤其 CMP/CMP_V2 至少要在读取 `_size` 前验证 20 字节头部，旧版 RAW 则需防止小于 12 字节时 `size()-12` 下溢。
- 影响范围: 所有 `read_raw_*` / `WtRdmDtReader` / `WtDataWriter` 的数据文件解析入口，损坏文件可导致崩溃或 OOM
- 修复方向: 在读取头字段前按版本校验最小长度：普通头 12 字节，压缩 V2 头 20 字节；长度不够时返回 false 且不改写 `content`。现有 `l04_proc_block_data_test.cpp` 仅覆盖合法长度路径，本缺陷尚无已运行复现证据。

## BUG-L04-003

- 状态: 已合并（BUG-L04-002）
- 严重级别: Major
- 模块/文件: `src/WtDataStorage/WtBtDtReader.cpp:151, 182, 212, 242, 272`
- 相关测试: 同上（`CorruptedBlockReturnsFalse` 覆盖压缩块声明长度不一致，不等于覆盖空文件或不足 12 字节）
- 输入/前置条件: `.dsb` 文件存在但为空或长度不足对应头部大小；入口自身不预检。
- 期望行为: 读取侧对 `StdFile::read_file_content` 的结果做最小长度校验，直接返回 `false`
- 实际行为: 未校验即调用 `proc_block_data`，把非法长度继续下传（与 BUG-L04-002 叠加）
- 复现步骤: 在 `his/<period>/<exchg>/<code>.dsb` 放一个空文件后调用 `read_raw_bars`
- 影响范围: 回测数据读取全部 5 个 `read_raw_*` 接口
- 复核裁定: 与 BUG-L04-002 是同一根因的入口表现，不单独计为第二个缺陷。`WtBtDtReader` 的五个接口确实把读取内容直接传给 `proc_block_data` 并返回其结果（见 `src/WtDataStorage/WtBtDtReader.cpp:151-156` 及后续四个 `read_raw_*`），中心解析器增加版本相关 guard 即可兜底；单独给五个调用点重复登记/修复属于 defense-in-depth。
- 修复方向: 并入 BUG-L04-002 统一修解析器；若仍保留调用侧防线，明确其为可选二次校验，不另计根因。

## BUG-L04-004

- 状态: 待审核
- 严重级别: 待定（超长上游输入是否允许尚未确认）
- 模块/文件: `src/WtDataStorageAD/LMDBKeys.h:92-93, 134-135`
- 相关测试: `src/TestUnits/coverage/l04_datastorage/l04_lmdb_key_layout_test.cpp`（`MaxLengthFieldsFitExactly` 验证 15/31 字符边界内安全；越界场景未写测试以免破坏套件）
- 输入/前置条件: 只有调用方可把长度至少为目标数组容量（包括终止 NUL 空间）的字符串传入 key 构造器时才触发；这些构造器本身不校验输入长度。
- 期望行为: 使用 `wt_strcpy`/`snprintf` 截断并返回错误，或静态断言长度
- 实际行为: 裸 `strcpy(_exchg, exchg)`、`strcpy(_code, code)`，越界写入 packed 结构体后续字段（含 `_date`/`_time`/`_bartime`），栈上键对象被破坏
- 复现步骤: 当前仅由 `strcpy` 和固定数组容量静态证明条件性越界；本轮未执行越界复现。现有测试只覆盖 15/31 字符的恰好安全边界，未证明生产上游会提供超长交易所/合约字符串。
- 影响范围: 所有传入超长字符串的 key 构造调用；上游输入约束未在本轮确认，故生产触发性待验证。
- 修复方向: 明确长度契约并在构造前拒绝超长输入，或安全截断且保证 NUL；为长度等于容量与超过容量分别补拒绝/安全行为测试，不要把尚未运行的越界路径写成已复现。

## BUG-L04-005

- 状态: 待审核
- 严重级别: Minor
- 模块/文件: `src/WtDataStorageAD/WtDataReaderAD.cpp:280-302`（`update_cache_from_lmdb`）
- 相关测试: `l04_ad_lmdb_store_test.cpp`（`GetUppersSkipsBarEqualToLastBarTime` 复刻其取数/去重语义）
- 输入/前置条件: LMDB 中存在长度小于 `sizeof(WTSBarStruct)` 的损坏/非规范 value；空列表路径须另外证明可从正常调用点到达。
- 期望行为: 对 `item.size()` 做校验；日志行不依赖容器非空
- 实际行为: 直接 `*(WTSBarStruct*)item.data()`，短 value 会被按完整结构体读。末尾日志确有无条件 `.back()`，但已知公开调用链在空 bars 时会先于此函数返回（`WtDataReaderAD.cpp:419`）；`onMinuteEnd` 对空缓存的行为也会更早在 `:511` 访问 back。因此“回调未调用且空 vector 到达本函数”尚未证明。
- 复现步骤: 现有测试复刻去重语义，未调用该私有函数；本轮未运行短 value 复现。需经真实调用点确认 LMDB 可写入非规范短 value。
- 影响范围: 分钟线/日线增量更新路径（`on_minute_end`、`readKlineSlice`）
- 修复方向: 在转换前验证每项 `item.size() == sizeof(WTSBarStruct)`，按产品策略记录并拒绝/跳过坏项；为正常调用点补短 value 测试。若保留空列表日志，先守卫 empty；该空列表风险当前标为待验证。

## BUG-L04-006

- 状态: 待审核
- 严重级别: Minor（归属 WTSUtils，但直接影响本车道 AD 存储）
- 模块/文件: `src/WTSUtils/WtLMDB.hpp:57-84`（`WtLMDB::open`）、`src/Includes/WTSStruct.h:204`（`WTSTickStruct::operator=(const WTSTickStructOld&)`）
- 相关测试: `l04_ad_lmdb_store_test.cpp`（显式传 8MB mapsize 并在 open 后写入 20 条记录）、`l04_proc_block_data_test.cpp`（`OldTicksUpgraded`）
- 期望行为: `mdb_env_set_mapsize` 的失败需使 `open` 失败；老→新 tick 字符串转换需在固定数组边界内完成并保证终止。
- 实际行为: `WtLMDB::open` 在 `mdb_env_open` 后调用 `mdb_env_set_mapsize`，忽略返回值且仍返回 true。顺序本身不是缺陷：仓库 LMDB `mdb.c:4086-4088` 允许 open 后在无活动事务时设置；当前调用点刚 open，尚未开启事务。旧 tick 转换 `strncpy(exchg, tick.exchg, MAX_EXCHANGE_LENGTH)` 把 `char[10]` 源（`WTSStruct.h:48-50`）交给 16 字节拷贝；只有损坏/无 NUL 的旧格式字段会跨出 `exchg` 子数组，正常短字符串遇 NUL 会停。现有 fixture 默认零填充，未覆盖损坏字段。
- 复核裁定: 拆为两个条件性问题；“必须在 open 前”与“任意 >1MB 即失败”均无源码依据。未运行 mapsize 错误返回或未终止旧 tick 的复现。
- 修复方向: 检查 `mdb_env_set_mapsize` 返回码并在失败时关闭环境、返回 false；旧字段按源数组长度有界查找 NUL，拒绝损坏值或复制到目标后显式 NUL 终止。补对应错误配置/损坏输入测试后再定级。

## 跳过（不测）说明

- `WtDataWriter`/`WtDataWriterAD` 的 `proc_loop`/`check_loop` 线程与 `BoostMappingFile` 共享内存块：依赖 `IHotMgr`/`IBaseDataMgr`/`IDataWriterSink` 全套宿主环境与后台线程（含 `sleep_for(10s/300s)` 的清理循环），单测内起线程会引入不确定性。
- `WtDataReader::init`/`onMinuteEnd`、`WtRdmDtReader` 的行情订阅与复权因子加载：依赖 CTP Loader / 真实 `hisdatadir` 与网络。
- `WtBtDtReaderAD`、`WtRdmDtReaderAD` 的 `get_k_db`/`get_t_db`：需要先有 WtDataWriterAD 落库（其写入依赖共享内存 RT 块），本车道改为直接对 `LMDBKeys + WtLMDB` 组合做等价的区间语义测试。
