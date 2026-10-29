# 迭代 02 · 01_share 缺陷逐条复核

日期：2026-10-29  
复核范围：输入组 /tmp/wondertrader-review-20261029/groups/01_share.json 中 40 条候选，覆盖 5 个 lane。逐条对照原始记录、生产源码和测试前置条件/断言。  
执行边界：本轮只做静态审阅，没有运行测试、构建、进程、网络或 sanitizer；所有 JSON 条目均为 evidence_level=STATIC。原 lane 中的历史 PASS/FAIL、disabled 用例和此前报告只作为线索，不作为本轮新执行证据。

## 结果统计

| 分类 | 数量 | 说明 |
| --- | ---: | --- |
| PRODUCT | 28 | 有源码行为、有效生命周期/输入或明确接口契约支持 |
| TEST | 2 | 问题属于测试构建配置，不是产品运行缺陷 |
| DUPLICATE | 2 | 明确并入既有 canonical 编号 |
| UNDECIDED | 8 | 缺少输入合同、产品目标/链接映射或可达性证据 |
| NOT_BUG | 0 | 本组没有在现有证据下足以直接裁为非缺陷的行 |
| 合计 | 40 | 与输入组行数一致 |

lane 分布：R16_share_tools 15 条；R213 0 条候选；R55 3 条；l00_build_graph 7 条；l01_share_wtsutils 15 条。R213 历史条目 BUG-R213-001 已撤回，剩余 coverage inline 映射假设仍需隔离 profile 实验，见 docs/迭代计划/迭代01/bugs/R213.md:7-34；它不属于本组候选计数。

逐条裁定、原始源/测试行号、具体修复与验收标准见 [01_share.json](../triage/01_share.json)。

## 需要优先进入实现/验收的产品问题

1. **持久化内存安全和数据恢复**：BUG-R16-07 的 KVCache 超长字段写入越界；BUG-R16-08 的截短映射文件在日期变更时可能越界清零，且同日恢复把空槽计入 size。优先验证长度限制、文件尺寸校验、有效记录重建和重开完整性。
2. **长度边界和解析器安全**：BUG-R55-003 的 UTF-8 检测忽略逻辑长度；BUG-L01-005 的 GBK 检测失败分支不推进且可能读取 data[i+1]；BUG-L01-013 的无分隔符标准代码解析会把 npos 用作拷贝长度。相关 sanitizer/崩溃历史不是本轮运行证据。
3. **LMDB 查询语义与资源生命周期**：BUG-R16-09 按指定 key 查询却使用 MDB_NEXT；BUG-R16-11 在 NOTFOUND 后仍取 MDB_val、可能多报记录并不关闭 cursor；BUG-R16-10 的错误码写入局部变量。需以空库、缺失键和多记录的精确行为作门禁。
4. **并行冷构建依赖**：BUG-L00-001 的五个宿主通过硬编码路径 POST_BUILD 拷贝产物，没有显式依赖边。coverage.sh 的预构建是规避手段，热构建不能证明冷构建图正确。
5. **公开头和公共转换合同**：BUG-L00-004 的 TraderAdapter.h 缺少直接声明依赖；BUG-L00-008 的 ModuleHelper.hpp 头定义外部非 inline 函数；BUG-L01-011 的 std::string from_str 重载没有被现测试真正实例化；BUG-L01-017 的有符号解析把负号按数字累计。

上述均为静态审阅优先级，不代表本轮复现或生产影响强度已完成验收。

## 需要统一修正或补充的测试

- **BUG-L01-011**：src/TestUnits/coverage/l01_share_wtsutils/l01_converter_charconv_test.cpp:103-118 的测试名声称覆盖 std::string 重载不能编译，实际只调用 const char* 重载。应真实构造 std::string 并实例化 convert::from_str<int32_t>(s)；产品修复后先用 C++17 编译门禁，再断言成功、非法和溢出合同。
- **BUG-R55-001**：src/TestUnits/coverage/r16_share_tools/r55_codehelper_charconv_test.cpp:137-154 的空指针用例会执行 needFree 未初始化读取。其“delete[] nullptr 所以安全”的注释错误。产品修复前不能把普通断言当作析构安全证明；修复后保留用例并增加 MSan/等价检查。
- **BUG-L00-004**：src/TestUnits/coverage/n01_wtcore_tradepaths/n01_trader_adapter_test.cpp:22-25 在 TraderAdapter.h 前显式 include Boost noncopyable 头，遮住产品头的自包含问题。另加单头语法测试，或移除该预置 include 后定向编译。
- **BUG-R16-12**：src/TestUnits/coverage/r16_share_tools/r16_logger_dll_test.cpp:135-143 只断言 EXPECT_NO_THROW，不能验证日志正文；应建立可控 sink 并断言分类与格式化文本。
- **BUG-R16-07 / BUG-R16-08**：危险输入测试需在产品修复后以明确失败结果、guard 记录和受控 sanitizer/子进程覆盖。当前 BUG-R16-07 测试不应再以索引中的“幽灵键”作为越界信号；BUG-R16-08 异日期 mmap 越界用例不应直接在普通测试进程触发。
- **BUG-R16-04 / BUG-R16-13 / BUG-L01-006 / BUG-L01-009 / BUG-L01-012 / BUG-L01-014 / BUG-L01-018 / BUG-L01-019**：启用或新增测试前先确认空路径返回、无字母模块名、大时间偏移、fmt 缓冲期限、helper 前置条件、畸形期权码、平台时间失败和股票简码映射合同，避免让测试替产品 owner 作决定。
- **BUG-R55-003**：现有逻辑长度 canary 测试的物理前置条件安全且断言合理；精确物理边界越界仍需单独 ASan 证据。

明确测试修正点已发给主代理，交由其统一安排 worker；本复核未编辑任何测试文件。

## 待裁定项

- BUG-L00-005：确认具体目标是否把两个 archive 的同名强符号同时抽入同一链接单元，并保存最终 link line/成员映射；仅两份 archive 均含符号不足以证明目标冲突。
- BUG-L00-009：确认 ParserYD 是否属于支持构建目标，以及 -Werror,-Wreorder-ctor 是否为该目标的验收门槛。
- BUG-L01-006、BUG-L01-009、BUG-L01-012、BUG-L01-014、BUG-L01-018、BUG-L01-019：分别缺少偏移范围、指针有效期/线程安全、直接 helper 可达性、非法格式处理、平台可触发失败输入、两段股票代码规则等合同或可达性证明。

## 推荐工作拆分

1. **共享字符串/编码内存安全**：R16-03/04/13、R55-001/003、L01-005/013。先明确错误返回约定，再用边界/ASan/MSan 验收。
2. **缓存、INI 与 LMDB**：R16-05/06/07/08/09/10/11/18。分别处理状态/长度、映射恢复和游标/键语义，测试数据和资源清理各自独立。
3. **构建图、头文件与覆盖目标**：L00-001/004/005/006/007/008/009。将产品 ODR/自包含问题和测试专属配置问题分开验收；L00-005、009 先补目标级证据。
4. **时间/代码/数字转换契约**：R16-01/02/15、L01-003/006/007/011/016/017/019。先解决已有明确输入合同的问题；对未决项由接口 owner 定义范围后再写断言。

## 证据边界

本报告只确认代码结构、接口注释、当前测试定义与条件关系。报告中没有宣称本轮动态复现；此前记录的子进程超时、定向 FAIL、链接失败/成功、sanitizer 和旧构建目录结果都仍需由主代理按当前代码和指定模块安排新执行。
