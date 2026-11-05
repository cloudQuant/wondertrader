# I03-NEW-006：订单 ID 时间故障与链接身份审计

2026-11-02，macOS arm64。本项只在隔离快照中修改源码并运行进程内 fake，不连接真实交易；未修改生产源码或仓库测试。不能据此关闭 I03-NEW-006、NEW-008、N05-003 或 L01-014。

## 时间故障红控

隔离副本使用当时 `TimeUtils.hpp` SHA256 `3b603ba7a58806bed3cc39c933cc9106fc28e2bcc7e6d7eda95e6d5569c070ff`，将首次 `time(NULL)` 受控设为 `-1`（使原 getTZOffset 返回 99），将读取当前墙钟设为 UTC 2024-12-30 00:00:00；保留其余订单代码，加入 stderr 标记。此为受控替代，不是操作系统故障实测；快照时间头早于随后新增的 checked 三参数 API。`time-controlled.patch`、`core-controlled.patch` 和 `uft-controlled.patch` 记录注入，后两者还含下面讨论的临时错误候选；`baseline/` 为本次开始时的产品文件，哈希 `snapshot-hashes.json`。

- `core-red2.log` / XML：WtCore fake buy 在时区失败时仍提交一笔订单，返回非空 ID 集合，undone 为 1；预期“不发送、无 ID、undone 不变”的测试 exit 1。标记显示计算基准为 2025-01-01，而受控当前时间为 2024-12-30。
- `uft-red2.log` / XML：UFT fake openLong 仍提交 2 手，返回 4286327296，undone 为 2；同一不发送/不改变账本预期 exit 1。为了确保调用的是本次注入的 UFT 实现，**只在隔离副本**将其内部同名生成函数改名为 `i03_uft_makeLocalOrderID`；不以此证明未改名生产二进制的具体绑定。
- 受控错误候选只在 `doEntrust` 准入处对 99 返回 `UINT_MAX`，没有应用到工作树。`core-candidate.log`、`uft-candidate.log` 均 exit 1：两者不再发单，但 Core 返回的 ID 集合仍非空且 undone 为 1，UFT undone 为 2。这证明仅在 doEntrust 失败返回不足以实现无副作用；两个候选都不是最终修复。
- 使用原时钟、相同隔离构建和 UFT 函数改名的正常组装及 Core 拆单正控分别 exit 0（`uft-positive.log`、`core-positive.log`）。这些正控不证明时间故障可安全恢复。

新增两个断言仅保存在 `uft-test.patch`、`core-test.patch`，未直接加入常规测试目标。`build.py` 和 `build-commands.json` 记录从既存 Ninja 编译命令重编两个产品 TU 与各自测试 TU，链接复用 `/tmp/wt-i03-r78-20261102/build`、`/tmp/wt-i03-r42-003-20261102/build` 的其他对象和库；Boost 头使用实体目录。本测试不代表干净完整树或全依赖插桩。

## 实际链接和 ABI 边界

WtCore `TraderAdapter.cpp` 与 WtBtCore `HftMocker.cpp` 均提供同名强外部 `makeLocalOrderID()`，UFT `TraderAdapter.cpp` 提供同名弱外部定义；WtBtCore 的 MatchEngine/UftMocker 等及仓内测试通过 extern 调用该签名。macOS 旧隔离构建的 `libWtPorter.dylib` 和 `libWtBtPorter.dylib` 均实际导出 mangled 符号，分别见 `porter-symbol.txt`、`btporter-symbol.txt`。仓内未找到公开头声明，不能仅凭导出断定厂商承诺，也不能排除仓外插件依赖。

未改名的 UFT 定向可执行文件同时链接多个旧快照动态库，其 UFT 源注入标记并未执行；`uft-dyld-bindings.log` 中 dyld 明确将该弱调用绑定到 `libWtBtPorter.dylib` 的非弱同名实现。因此未隔离版本的红控 `uft-unisolated-red.log` 不能用于证明 UFT 自身在所注入时钟下的行为。`uft-unisolated-symbol.txt` 表示文件静态弱定义，不单独决定运行时绑定。此本机混链取决于具体加载集和顺序，不能推广到所有平台或宿主。

## 安全迁移仍需设计和验证

原种子使用 `getCurDate()` 得到的本地年份，以及另一实时时钟 epoch 与 `makeTime(年初)` 的差。仅由本地日期时间倒推出 epoch 在夏令时回拨时不唯一：`America/New_York` 的 epoch 1730611800 和 1730615400 均为 2024-11-03 01:30，但原种子差 180000。后续新增的 checked 三参数接口可提供同一次采样的 epoch，尚需真实接入并验证年初 `mktime` 的失败、范围、跨年和原 `tm_isdst=0` 合同。

预检须在 WtCore buy/sell 的聚合未完成量更新前、四个公开直接开平仓入口以及 UFT 四个入口的 `updateUndone` 前完成，并保证调用者收到明确失败、拆单结果不包含失败 ID；失败不得触发 provider `makeEntrustID` 或 `orderInsert`。初始化失败须保留可重试状态，不能把错误日期固化进 `once_flag`。同时，预检与随后分配必须是**同一解析结果、同一生成器状态**；单独给每个 TU 增私有预检会在上述 weak/strong 混链下失效。不能仅额外分配并丢弃 ID：这改变序列、加速回绕，也不能保证并发拆单后续分配。

需要在保持旧导出 wrapper 与现有可观测合同的前提下，明确 WtCore/WtUftCore/WtBtCore 各自或跨模块共享的 ID 命名空间；用三个宿主独立及实际混合加载、顺序、并发首次初始化/失败重试、回绕、回报与已接受订单覆盖验证，并与 NEW-008/N05-003/L01-014 的账户身份及跨重启去重协调。现有信息不足以证明对未知外部 ABI 或既有订单完全兼容；本项没有引入未经证明的局部产品补丁。Windows/Linux、真实 SDK、进程重启/恢复与完整树均未验证。
