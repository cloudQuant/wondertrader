# L03-009：股票 VWAP 的不可表示 Session 日期

本项只处理股票 VWAP 行情日期入口，未关闭 L03-009。旧代码对 tick 日期直接调用 `getOffsetDate`，在 checked 日期超出 1–9999 年范围时仍执行旧回退。现对每个有效 tick 在警告、日期契约比较、high-water 更新与交易动作前调用 `tryGetOffsetDate`。失败时记录诊断，清除可能被后续 channel 回调用于下单的旧 tick，但不修改 high-water、日期契约锁存或目标仓位；已验证的当前 offset 日期用于后续比较。检测到的旧 high-water 日期也用 checked 接口验证，无法验证时拒绝本次行情而非锁存错误日期契约。

本轮修改仅有 `src/WtExeFact/WtStockVWapExeUnit.cpp` 与 `src/TestUnits/coverage/r7_exefact/r7_stock_vwap_unit_test.cpp`。`baseline/` 保存本轮编辑前的两个工作区文件，非完整历史源码树；`final.sha256` 记录相关源码、头和二进制。保留其他已有工作区修改。

## 定向测试

本机 macOS arm64，Homebrew LLVM 22，产品与测试 TU 使用 ASan/UBSan，LSan 关闭；其他 WTSTools/WTSUtils 静态库来自 `/tmp/ctpm-i03-build`，不能视为完整源码重编。Boost/fmt/spdlog、GoogleTest 使用实体 Cellar 路径，外部链接库与系统库没有全部插桩；没有创建符号链接，不接真实交易。具体编译脚本 `build.sh`：

```sh
zsh docs/迭代计划/迭代03/实施跟踪/evidence/L03-009-stock-vwap-20261102/build.sh src /tmp/wt-i03-stock-date-20261102/red-final docs/迭代计划/迭代03/实施跟踪/evidence/L03-009-stock-vwap-20261102/baseline/WtStockVWapExeUnit.cpp
zsh docs/迭代计划/迭代03/实施跟踪/evidence/L03-009-stock-vwap-20261102/build.sh src /tmp/wt-i03-stock-date-20261102/green-final
```

用两版各运行 `red-final.log`、`green-final.log` 中列出的 8 个关联 GoogleTest 过滤器，`TZ=UTC ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`。旧版 **3 项失败、5 项通过，exit 1**：未表示日期仍可能下单及污染 high-water；已有旧 tick 可在无效日期后被使用；无法验证旧 watermark 时错误锁存 rollover。新版 **8/8 PASS，exit 0**；包括无/有 trading_date 的日期失败、无效后有效行情恢复、原高水位保持和旧行情清除、正常夜盘、跨午夜重试、错误日期组合及无效时间。

另用 `green-lots.log` 的 4 个直接相关股票手数和 T+1 可卖仓用例验证合法交易约束，**4/4 PASS**。红绿原始日志和 XML 同目录。`git diff --check` 对本项文件无问题。

## 尚未验证

初始 tick 的 action date 为 0 会由既有 `validActionDate` 拒绝，本项未注入操作系统时钟故障，也未验证实际 SDK、真实交易、Windows/Linux、完整依赖和整合树。保留的旧 Session API 失败回退、其他 Session 调用方、Session 配置改变与并发合同及完整 L03-009 仍未闭合。数据时间错误触发清除旧 tick 是保护后续使用旧价格的现有约定，并非对订单/持仓所有业务状态的完整验收。

## 复核补充：执行器支持年份下界

`tryGetOffsetDate` 支持公历 1–9999 年，但股票 VWAP 既有 `validActionDate` 合同是 1900–9999；仅检查 checked 返回值会把 1900-01-01 经负偏移得到的 1899-12-31 当作可用日期。当前代码对当前和先前 session 日期均附加既有年份约束。`checked-only-control.cpp` 为受控旧候选（仅移除这两项年份判断）；新测试 `session_date_before_supported_year_is_not_used_for_orders` 在该候选上 exit 1，实际发送买单并更新 high-water（`red-checked-only.log`）。最终源码重编后该例和上文 8 项、4 项股票手数/T+1 对照合计 **13/13 PASS**，见 `green-latest.log/xml`。`final.sha256` 包含最终源码及各二进制。
