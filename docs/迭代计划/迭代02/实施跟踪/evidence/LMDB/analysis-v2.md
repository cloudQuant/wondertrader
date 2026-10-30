# LMDB 批次 v2 证据与裁定

本批 macOS arm64 focused `r16_lmdb` suite 为 15/15 PASS，独立审查通过。`BUG-R16-10` 和 `BUG-R16-18` 均按有限本机范围更新为 `TEST_PASS`；完整验收仍受平台、sanitizer、aggregate build 和 callback 异常边界限制。

## BUG-R16-10 — LMDB open/mapsize 失败被吞

- **裁定 / 根因：** PRODUCT。打开路径中的错误状态曾由局部错误码遮蔽成员状态；`mdb_env_set_mapsize` 的返回也未可靠写入公开错误状态，调用方无法通过 `has_error()` / `errmsg()` 判断失败。
- **修复：** 当前 `open_impl` 将创建、打开和 mapsize 错误写入成员 `_errno`。mapsize 失败时关闭已创建的环境、清空 `_env`/`_dbi` 并返回 `false`。
- **本批覆盖：** 普通文件路径导致的 open 失败可观察到错误；通过 test seam 注入 `MDB_MAP_FULL`，验证失败返回、环境关闭、错误码/信息保留，并允许同一 wrapper 后续重试成功。
- **结果：** 15/15 PASS（combined focused suite）。

## BUG-R16-18 — 变长二进制 key 比较越界/区间误判

- **裁定 / 根因：** PRODUCT。公开区间接受任意长度的 `std::string` 键，旧逻辑按当前 key 长度比较上界，可能越过较短上界的有效内存或将更长的严格前缀键错收入闭区间。
- **修复：** 当前 `compare_lmdb_keys` 先用 `memcmp` 比较两键共同长度的字节，再按长度决定次序；`get_range` 比较时使用 LMDB key 的实际长度和上界的实际长度。
- **本批覆盖：** 变长键、共同前缀、嵌入 NUL 键和闭区间边界；原 `get_range("b", "c")` 测试错误期待 3 项、实际闭区间只有 `b`/`c` 两项，已把预期改为 2。此为测试 oracle 修正，不把它与上述产品缺陷混为一谈。
- **结果：** 15/15 PASS（combined focused suite）。

## 本机证据边界

证据包含旧版 3 项记录与本批 `build-v2.log`、`run-v2.log`、`run-v2.json`。`reproduce-v2.sh` 保存测试对象构建、链接和运行命令。链接复用现有 `libWTSUtils.a` 与系统 GoogleTest/Boost，仅构造 focused binary，不是 clean rebuild 或 aggregate `CoverageTests`。

ASan 未运行，没有通过证据；Windows/其他平台未构建或运行。`get_range` 的 callback 若抛异常，显式 cursor close 会被跳过；cursor 在 transaction end 前的持有及最终清理行为未验证。
