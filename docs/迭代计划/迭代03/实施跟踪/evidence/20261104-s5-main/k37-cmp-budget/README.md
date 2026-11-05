# K37=B：CMP_V2 解压资源预算（BUG-R132-001 剩余项）

- 产品：`src/WTSUtils/WTSCmpHelper.hpp` `uncompress_data` 在分配解压缓冲前按帧头声明的内容大小检查：未声明/非法大小拒绝；默认绝对上限 1 GiB、压缩比上限 1000:1，超限抛 `std::runtime_error`（读端已在 helper 外捕获）；新增 `set_uncompress_budget(maxBytes, maxRatio)`（0 表示不启用该项）与查询接口，按进程（模块）生效。行尾保持 CRLF。
- 用例：`src/TestUnits/coverage/l01_share_wtsutils/s5_main_k37_cmp_budget_test.cpp`（第一组只用改前接口：默认比率超限拒绝、未声明大小拒绝、普通帧往返；第二组：默认值、绝对上限边界含等号、比率可配置且 0 关闭）。
- 红控：以改前头文件（副本，无链接）与构建产物 `libWTSUtils.a` 单独编译第一组（`-DS5K37_LEGACY_HEADER_ONLY`），`red.log`：2 例失败；改后同法 `green.log` 3/3；覆盖率主二进制 `coverage-green.log` 6/6。
- 回归：CoverageTests、btcore、dtcore、dtservo、adstorage、porter、btporter、TestUnits 逐例 3042 通过，仅既有 Q05（BUG-I02-NEW-104）与 R222（I03-NEW-081）失败（`regression-summary.json`）。
- 未覆盖：预算尚未接入各读端/回放器的配置项（仅进程内接口可配）；Windows/真实大数据未验。
