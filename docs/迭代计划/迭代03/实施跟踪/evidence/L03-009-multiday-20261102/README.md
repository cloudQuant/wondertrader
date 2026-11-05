# L03-009 多日 offset 与 VWAP 日期入口（局部修复，未闭合）

本轮开始前 `WTSSessionInfo.hpp` 与 l03 测试已有脏改动；`baseline-session.hpp` 和 `source-before.sha256` 保存本轮开始前工作树，包含这些改动。修改了 session 头及 l03 专属测试，并经主代理授权修改 `WtVWapExeUnit.cpp` 和 `r7_vwap_unit_test.cpp`。没有修改共享台账、TimeUtils 或其他产品路径。

`offsetTime` 左对齐以数学模日保持 `[0000,2400)`，右对齐的整日边界表示为 2400；非零偏移的 `originalTime` 使用逆变换模日，零偏移的旧直通行为保持。新 `tryGetOffsetDate` 以整数日历处理多日偏移，支持公历 1–9999 年，拒绝非法日期、时分与结果超域并保持输出不变；当日期为零时使用现有 checked 时钟快照。传统 `getOffsetDate` 对 checked 成功结果采用完整多日日期；为避免在其他旧调用方未迁移前引入未检查的 sentinel，当 checked 失败时**保留原有回退行为**。因此旧 API 对不可表示结果或时钟失败仍不可靠，不能称为完整失败传播。

VWAP 的 `validSessionDatePair` 使用 checked 方法，`on_tick` 在更新高水位及发单前先验证日期可表示；仍保留合法行情在非交易窗口推进高水位的原有行为。主动提交订单的假上下文测试覆盖本入口，没有触碰真实柜台。

## 红绿证据

- `range_oracle.cpp` 仅检查合法时分的左右对齐输出在合法 HHMM 范围；旧代码 +1441、-1441、+2880、-2880 共 18 处失败（`range-red.log`，exit 1）；新代码 `range-final.log` 为 failures=0，exit 0。旧版 `probe.log` 同时记录 +2880 的 09:00→3300、-2880→4294965796，以及旧日期仅偏移一天。相同初始日期按完整民用日历推进的候选计算保留在 `candidate-calendar.log`。
- 新增三项 session 测试覆盖 ±1441、±2880、INT32_MAX、左右 0000/2400、闰日跨月、逆映射和重复 setter；INT32_MIN 导致日期越过年 1、9999 年跨界、坏日期/时分时 checked 方法失败且不改输出。与原六项关联测试合计 **9/9 PASS**（`final.log`/`final.xml`），LLVM22 ASan/UBSan，UTC；纽约及上海各对应 8/8（`final-newyork.log`、`final-shanghai.log`）。既有极值旧测试只检无整数 UB，不是极值日期正确性证明。
- `vectors.cpp` / `check_vectors.py` 以固定种子生成 10000 个 int32 offset、0000–2400 和公历 1–9999 日期，使用 Python `datetime` 与 `divmod` 独立比较 checked 结果、不可表示标志、左右映射。当前 inline 产品代码用 ASan/UBSan 构建，`vectors.log` 为 10000 匹配，exit 0。该数学对照选择公历日语义，不证明所有外部业务合同、真实时区切换或 DST 行为。
- VWAP 新测试模拟 9999-12-31 21:00 经夜盘偏移落入年 10000；修改前工作树 header 与 HEAD 原 VWAP 源、相同新测试的基线 1 项失败（错误下单、更新高水位、无失败诊断），夜盘与日期冲突两项正控通过（`vwap-red.log`，2/3，exit 1）。当前源码对应 **3/3 PASS**（`vwap-green.log`，exit 0），包括先拒绝非法日期后接受正常夜盘行情。仅测试/产品 TU 局部 ASan/UBSan，其他静态依赖复用旧构建，LSan 对 VWAP 未启用。
- `date_days_ubsan.cpp` 对 `TimeUtils::getNextDate(20240228,1491308)` 的独立探针，记录 `days*86400` int 溢出（`date-days-red.log`，exit 134）；现有 session 旧实现并未传入该天数，不能把此探针称作其已执行的调用路径。

## 复现与限制

Session 测试与 `vectors.cpp` 用 `/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=address,undefined,signed-integer-overflow -fno-sanitize-recover=undefined -I/opt/homebrew/Cellar/boost/1.88.0/include -Isrc` 编译；GTest 测试另使用 `/opt/homebrew/Cellar/googletest/1.17.0/include` 及该版本 `libgtest_main.a`、`libgtest.a`。向量运行使用 `/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python .../check_vectors.py`，脚本依赖 `/tmp/wt-l03-multiday-20261102/current_vectors`。VWAP 编译分别执行 `zsh .../build-vwap.sh src /tmp/wt-l03-multiday-20261102/vwap-green` 和 `zsh .../build-vwap.sh /tmp/wt-l03-multiday-20261102/red/src /tmp/wt-l03-multiday-20261102/vwap-red`；red 源为本轮开始前 session 头与 HEAD VWAP 源、最终测试的实体复制，不是整个历史仓库版本。具体过滤器记录在对应 `*.log`；VWAP 运行 `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 TZ=UTC`。构建目录与实体 Boost/fmt/spdlog 头未新建符号链接；静态依赖及其他动态库未经全量插桩。`final.sha256` 记录最终相关源和二进制。

仍未闭合：旧 `getOffsetDate` 的失败回退会在超域或时钟故障时产生不可验证的日期，`WtDataStorage/WtDataWriter.cpp`、`WtDataStorageAD/WtDataWriterAD.cpp`、`WtDtCore/StateMonitor.cpp` 和 `WtStockVWapExeUnit.cpp` 尚未迁移 checked 方法；配置加载和 void `setOffsetMins` 不提供事务性失败反馈。年 1–9999 是新增 checked 接口的明确可表示范围，不是声称已获全部市场/外部 ABI 同意；公历日行为相对旧 `getNextDate` 的本地时区/DST 行为改变尚需完整业务兼容审查。其他平台、真实 provider、全部依赖重新编译和整体回归未执行。仍应保持 L03-009 阻塞。
