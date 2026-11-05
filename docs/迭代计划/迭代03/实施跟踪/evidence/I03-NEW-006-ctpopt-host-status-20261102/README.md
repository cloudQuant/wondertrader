# I03-NEW-006：CTPOpt 结算确认错误与宿主状态（未修复）

2026-11-02，本机 macOS arm64。目标是审查此前 checked 时钟失败后仅写日志、`OnRspQrySettlementInfoConfirm` 不向宿主报告失败的问题。真实宿主 `WtCore::TraderAdapter::onLoginResult(false,...)` 会设置 `AS_LOGINFAILED` 并通知 notifier，UFT adapter 在登录中接收 false 后设置 `AS_LOGINFAILED`；但接口没有请求代次参数。

**本轮未修改 TraderCTPOpt 产品源码。** 当前 `WS_CONFIRM_QRYED` 同时覆盖尚未调用 SDK、确认请求已在途及同步调用返回失败后的状态；查询响应处理没有用请求 ID 关联代次，进入已确认日期分支时也不要求前置状态。只把 `confirm()` 的负返回当作一次确定未确认并通知宿主，会破坏在途/重入路径。SDK 在返回非零之前同步回调成功、重复查询或重排是本测试为审查状态机刻意构造的假设；本地假 API 不证明真实 CTPOpt 供应商保证这些时序一定存在或不存在。历史 callback/owner 生命周期和 SDK 释放屏障也不由这些结果证明。

## 当前代码的受控结果

新增 `src/TestUnits/fixtures/ctpopt_time_snapshot/confirm_snapshot.cpp` 四个 host 场景及一个在途场景，未修改旧测试断言；记录见 `final-results.json` / `current-final-*.log`。隔离产品 TU 使用本轮开始时工作树和相同 TimeUtils，`build.py` 的受控时钟/本地时间函数在快照中注入，真实 `OnRspQrySettlementInfoConfirm`、`OnRspSettlementInfoConfirm` 执行，fake sink 捕获登录回调：

- `host-clock-failure`：时间故障未发送 SDK 请求，但没有 `onLoginResult(false,...)`，期望失败报告的断言失败（exit 1）。
- `host-late-after-clock`：此前时间故障后，同一对象又收到一个确认日期已满足的查询响应，现有逻辑进入 ready，未见故障通知；检测断言失败（exit 1）。此场景没有构造新一次完整登录，不能单独证明跨次登录关联错误。
- `host-reentrant-notify`：假 sink 在收到失败回调后立即重入旧查询响应；现有代码根本未报告失败，断言失败（exit 1），所以此结果不验证现有失败回调的重入安全。
- `host-sdk-reentrant-negative`：假 SDK 在请求栈中先同步回调确认成功再返回 -5；当前代码产生一次成功结果（exit 0）。`host-pending-then-clock`：一次确认已在途后又收到第二个查询而本地时钟失败，先前在途确认仍可成功（exit 0）。这两个是受控边界，非真实 SDK 合同。
- `success` 和跨午夜单次采样 `boundary` 控制均 exit 0。原 checked 时间测试同一最终 fixture 重编后 **27/27** 独立场景通过，见 `compat-results.json`。

## 错误候选与关联风险

`naive-candidate.patch` 仅在临时源码中将两个 `confirm()` 调用改为负返回时设置 `WS_LOGINFAILED` 并调用 `onLoginResult(false,...)`；没有应用到工作区产品。该候选让 `host-clock-failure` 变绿，但：

- 重入确认成功后 SDK 返回 -5，候选又发布失败并覆盖 ready（`naive-final-host-sdk-reentrant-negative.log`，exit 1）。
- 早先确认已发送，后来的重复查询碰到本地时钟错误，候选错误报告失败且忽略先前请求的成功（`naive-final-host-pending-then-clock.log`，exit 1）。
- 失败通知回调内重入旧查询响应，候选又发布成功（`naive-final-host-reentrant-notify.log`，exit 1）；延迟旧查询亦可恢复 ready（`naive-final-host-late-after-clock.log`，exit 1）。

候选不是安全实现，不能据其绿色局部断言合并。要推进需要分别表达查询代次、确认尚未发出/调用中/已提交/结果不明、在途请求 ID 和最终状态；状态转移须在调用 SDK 前完成且不能持有会被同步回调重入的锁，callback 外部通知期间须保住对象和 sink。仅在确定未发送、无先前在途确认且当前登录代次匹配时才发布一次本地失败；被占用或无法确认身份的旧回报须隔离。调用者重登时尚需 owner/provider 生命周期和跨代回报协调，不能单靠 `WS_LOGINFAILED` 或时间戳猜测。具体 SDK 请求返回值、callback 时序与 Release/drain 需精确版本证据；不能据 fake 认定其合同。

## 复现与局限

`src/TestUnits/fixtures/ctpopt_time_snapshot/build.py` 用 Homebrew LLVM 22 将当前产品和 fixture 两个 TU 编译到实体目录，启用局部 ASan/UBSan，关闭 LSan；其余系统/Boost 与 fake SDK 依赖未全量插桩，使用动态符号解析，不是完整产品或真实 SDK 验证。没有创建符号链接，没有访问真实柜台。生成的实际构建命令见 `current-build.json` / `naive-build.json`；`hashes.sha256` 记录产品、头、fixture 和二进制。`fixture-before.cpp` 为本任务前未提交测试版本，不作为旧产品快照。

```sh
/Users/yunjinqi/opt/anaconda3/bin/conda run --no-capture-output -n base python src/TestUnits/fixtures/ctpopt_time_snapshot/build.py /tmp/wt-i03-ctpopt-host-status-20261102
```

`final-results.json` 记录两个二进制每个模式的完整 argv；`ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`，每项 12 秒进程超时。naive 副本位于独立 `/tmp/wt-i03-ctpopt-host-naive-20261102`，在已注入受控时钟的源码上应用本目录补丁后用 build.py 的 `reuse` 参数编译；它依赖该临时副本。没有重编全部依赖、运行 Windows/Linux 或真实 SDK，也未运行整合回归。现有 known-red host 模式为手动命令行模式，不加入正常的 27 场景通过汇总。
