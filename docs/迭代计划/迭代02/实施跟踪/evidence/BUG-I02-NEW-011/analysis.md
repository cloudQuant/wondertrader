# BUG-I02-NEW-011：WtKVCache 扩容失败后仍写入

## 当前状态（v12）

`PRODUCT_BUG`，当前为有限范围 `TEST_PASS`。主审与独立复审均已完成；macOS arm64 focused suite 30/30 PASS，覆盖扩容故障注入、容量溢出、失败 rollback 与重启恢复。归档记录见 [`WtKVCache-v12/run-v12.json`](../WtKVCache-v12/run-v12.json)，原始编译/运行日志和重放命令也在同目录。

**实际修复：** resize 前验证映射长度、容量及 backing file identity；扩展文件后验证长度和身份 marker，再单独建立/校验候选 mapping，全部通过后才切换当前 mapping。失败时保留旧 mapping，并尽力通过原文件 handle rollback 和复核长度。容量翻倍溢出会先拒绝。插入路径只有 resize 成功后才更新索引和 size；新增 `try_put`/`try_put_if_none` 返回 bool，旧 `void put`/`put_if_none` 为兼容性保留并记录日志。init 和 resize 的 `noexcept` 路径会将异常转为安全失败。

**未验收边界：** Windows 未实际编译或运行；本次 focused link 复用已有库，不是 clean rebuild 或完整调用方联动。rollback 失败不能保证原文件长度恢复；旧 mapping 在当前进程仍可用，重启时完整槽布局可以按实际容量恢复，非整槽尾部会 fail closed 并需人工恢复文件。旧 void 插入 API 不直接报告失败。没有 ASan PASS 证据（sanitizer 在启动阶段挂起后被终止），真实跨进程 init/resize 竞争也未验证。

## 修复前裁定与静态状态

修复前曾为 `PRODUCT_BUG` / `IN_PROGRESS`；以下为实施前的静态根因与验收分析，当前状态以本页 v12 更新及修复执行清单为准。

## 修复前根因证据

- `src/Share/WtKVCache.hpp:99-150`：`resize()` 的文件打开、seek 和写入返回值均未检查。随后先释放当前 mapping，再创建新 mapping；映射失败会返回 `false`，但对象可能已失去可用 mapping。函数声明为 `noexcept`，内部字符串扩容/分配与映射构造仍可能抛异常。
- `src/Share/WtKVCache.hpp:282-324` 和 `327-367`：`put()`、`put_if_none()` 都忽略 `resize()` 的返回值，继续递增 `_size` 并访问新槽。文件扩容或重新映射失败时可能越过旧映射边界写入。
- 两处以 `_capacity * 2` 计算新容量，乘法在 `uint32_t` 中进行；容量达到高位区间时可回绕到更小值，`resize()` 的“当前容量已足够”分支可能把回绕值误判为成功。
- `init()` 在 `noexcept` 边界内创建并调用映射对象（`src/Share/WtKVCache.hpp:154-193`），没有把构造/映射异常转换为可观察的失败返回，异常逃逸会触发 `std::terminate`。

## 初始修复方案

将增长作为可失败事务处理：检查文件操作与映射返回结果；在新映射和容量确认成功前保留旧 mapping/容量/索引；失败时不得递增 `_size`、改变 `_indice` 或写槽。为新容量增长增加 `uint32_t` 溢出及映射字节数检查。统一 `put` / `put_if_none` 的失败可观察合同（错误返回，或在兼容限制下提供可靠的错误状态/logger），并处理 `noexcept` 内所有可能抛出的分配与映射异常，保证 `init` 返回失败而非终止进程。

## 计划故障注入与验收

1. 增加可控文件扩容/映射 seam，分别注入 open、seek、write、remap 失败；填满初始容量后调用 `put` 和 `put_if_none`。
2. 每个失败点均验证：调用失败可观察；`size/capacity/index` 与既有记录保持一致；无新增槽写入和越映射边界访问；对象仍可安全销毁，后续按明确策略可重试或继续使用。
3. 在 `capacity > UINT32_MAX / 2` 注入/构造边界，验证扩容先检测溢出并失败，不访问映射外地址；不得依靠实际创建超大缓存文件才能覆盖该分支。
4. 注入 init 中 mapping 构造/映射异常，验证不终止进程、返回 false 且对象处于安全未初始化状态；成功扩容的现有路径仍保留 200→400 正向控制。
5. 在 ASan/UBSan 下运行上述故障矩阵；如声称并发安全，再另行提供 TSan/并发合同证据。

## 既有覆盖边界

`BUG-R16-07` 处理键和值字段长度；`BUG-R16-08` 处理截断/损坏缓存文件恢复；`R4-BUG-11` 针对 `WtDataWriterAD` 的扩容失败合同。它们都没有验证 `WtKVCache::resize()` 返回失败后 `put`/`put_if_none` 是否继续写，也没有覆盖容量翻倍溢出或 `init noexcept` 映射异常，因此本条保持独立。

## v12 更新前的覆盖边界

`src/TestUnits/coverage/r16_share_tools/r16_kvcache_test.cpp:198-224` 当时只覆盖成功扩容（填满 200 项后变为 400 项），不是失败注入证据。该陈述仅描述 v12 前的覆盖；当前 v12 故障用例、ASan 未通过状态和失败可观察性边界见上文。
