# Charconv 批次 v1 证据与裁定

本批以三份已重编测试对象链接 macOS arm64 focused binary，运行 22/22 PASS。主审和独立复审均未发现当前范围的逻辑 blocker。记录为本机定向 `TEST_PASS`，不代表 sanitizer、跨平台或完整构建验收。

## BUG-R55-001 — 转换器所有权标志未初始化

- **裁定 / 根因：** PRODUCT。`UTF8toChar`、`ChartoUTF8` 的空指针路径会让输出指针为空，但析构仍读取未初始化的 `needFree`，构成未定义行为。
- **修复：** 当前源码将两处 `needFree` 默认初始化为 `false`，只有取得转换缓冲区后才标记为拥有内存。
- **本批覆盖：** 空指针构造与析构、空串、ASCII 保留原指针、固定中文转换往返。
- **结果：** 本机 focused 22/22 PASS。该结果没有 ASan/MSan 通过证据。
- **剩余：** 同一 converter 已分配缓冲后再次调用公开 `init()`，旧缓冲是否释放仍需单独审查；ASan/MSan、其他平台及完整消费者联动未验收。

## BUG-R55-003 — UTF-8 续字节读取越过逻辑长度

- **裁定 / 根因：** PRODUCT。旧路径未在读续字节前按 `len` 检查完整序列，逻辑长度截断时可越界读取。
- **当前修复：** `isUtf8` 当前实现已在读取前验证剩余长度，并限制合法 UTF-8 为 2 至 4 字节，拒绝 overlong、surrogate 和大于 U+10FFFF 的码点。本批只增加/补齐测试，不能将本批描述为新增产品代码修复。
- **本批覆盖：** canary 缩短逻辑长度；物理数组恰好终止在截断边界；合法码点上下界和非法标量值。
- **结果：** 本机 focused 22/22 PASS。用例有物理边界断言，但本批没有 ASan/MSan 通过证据。
- **剩余：** ASan/MSan、其他平台、完整消费者联动未验收。

## BUG-L01-005 — GBK 检查越界且畸形输入不前进

- **裁定 / 根因：** PRODUCT。旧路径可能在末尾 lead byte 读取 `i+1` 越界，非法双字节失败路径也可能不推进扫描下标而卡住。
- **修复：** 当前实现先检查 `i+1 < len`，无效字节推进一个字节，确保循环终止。
- **本批覆盖：** 合法 GBK 双字节、ASCII、非法首字节、禁用的 `0xF7` 次字节、低次字节、末尾单 lead byte、空/null 输入及“坏序列后跟合法序列”。
- **结果：** 本机 focused 22/22 PASS。
- **语义边界：** `isGBK` 按当前头文件文档只要发现一个合法双字节对就返回 `true`；例如坏字节后跟合法 GBK 字节对仍为 `true`。它检测“包含合法 GBK 序列”，不是要求整个输入均为严格 GBK。
- **剩余：** ASan/MSan、其他平台及完整消费者联动未验收；严格整串校验如有需要应另行定义 API 合同。

## 构建与重放范围

本记录重放三个测试对象的 Ninja 构建和单独 focused link/run。链接使用现有静态库及系统 GoogleTest/Boost 库，并带 `-Wl,-undefined,dynamic_lookup`；允许动态未解析符号，不能替代 clean build 或 aggregate `CoverageTests`。完整引擎/生产调用链未由此验证。

`build-v1.log`、`run-v1.log` 为本批原始日志副本；`run-v1.json` 记录 binary、日志和源码哈希。`reproduce.sh` 保存可重放命令，本批未重新执行该脚本。
