# lane N06 收口清单（WtCtaTwoMaFact / WtCtaStraFact / WtUftStraFact / WtSelStraFact / WtHftStraFact / WtShareHelper / WtMsgQue）

## 新增文件（src/TestUnits/coverage/n06_strategies_mq_share/）
| 文件 | 行数 | 用例数（含 DISABLED_） |
|---|---|---|
| n06_cta_stra_test.cpp | ~190 | 8（1 DISABLED_） |
| n06_cta_ctx_mock_test.cpp | ~330 | 7（1 DISABLED_） |
| n06_sel_uft_hft_test.cpp | ~230 | 8（1 DISABLED_） |
| n06_mq_share_test.cpp | ~230 | 5（1 DISABLED_） |

共 4 个 .cpp、28 条 TEST（4 条 DISABLED_ 收口崩溃/数值缺陷类）。

## 覆盖符号（均出自本轮简报或工具结果）
- WtCtaTwoMaFact.cpp / WtCtaStraFact.cpp：`WtStraFact::getName/createStrategy/deleteStrategy/enumStrategy`、`FACT_NAME`（ODR 竞争见 BUG-WSN06-003）
- WtStraTwoMa.cpp：`getName/getFactName/init/on_init/on_session_begin/on_schedule/on_tick`（空仓金叉、持多死叉路径经 MockCtaCtx + 真实 WTSKlineSlice 驱动）
- WtStraDualThrust.cpp：`init/on_init/on_session_begin/on_schedule`（upper/lower 计算、突破进场、平多）
- WtSelStraFact.cpp：`WtSelStraFact::createStrategy/enumStrategy/deleteStrategy/getName`
- WtStraDtSel.cpp：`init`（含 codes 逗号分割、空/重复/纯分隔符配置）
- WtUftStraFact.cpp / WtUftStraDemo.cpp：`createStrategy/deleteStrategy/enumStrategy`、`init`、`on_entrust`
- WtHftStraFact.cpp / WtHftStraDemo.cpp：`createStrategy/deleteStrategy`、`init`、`on_entrust`
- WtShareHelper/ShareBlocks.cpp（namespace shareblock::ShareBlocks）：`one/init_master/init_slave/update_slave/get_sections/get_keys/get_section_updatetime/commit_section/delete_section/allocate_{int32,uint64,double,string}/set_{int32,uint64,double,string}/get_{int32,uint64,double,string}`（文件映射模式，落盘全部在 temp_directory_path() 的 n06_ 前缀唯一目录并 remove_all 清理）；`init_cmder/add_cmd/get_cmd` 走宽松不崩溃路径
- WtMsgQue（仅导出 C API，经链接 dylib）：`regiter_callbacks/create_server/destroy_server/publish_message/create_client/destroy_client/subscribe_topic/start_client`；含 inproc:// 端点对一次真实收发；未触碰 MQManager/MQClient 内部 C++ 符号

## 跳过目标及原因（供排除清单评审）
- WtStraDtSel::on_init/on_schedule/on_tick/on_bar：需完整 ISelStraCtx mock（38 个纯虚）+ WTSSessionInfo 交易时段对象，预算内不展开
- WtUftStraDemo/WtHftStraDemo 的 on_tick/on_bar/on_trade/on_order/on_position/on_channel_*：依赖真实 UFT/HFT 交易上下文与回报流水，纯逻辑部分仅 init/on_entrust
- CTA 两工厂的 extern "C" `createStrategyFact/deleteStrategyFact` 导出：同名 extern "C" 符号在两个 TU 重复（BUG-WSN06-003），调用结果依赖链接顺序，未纳入断言
- ShareBlocks 多进程/真实 POSIX shm 场景：单进程文件映射已覆盖，跨进程发布订阅跳过
- 简报中提到的 `shareblock_helper.h`/`CSplitStringPool`：仓库不存在（BUG-WSN06-006 勘误）

## 是否需要主控为 WtMsgQue 增加 TU
不需要。导出 C API（extern "C" + EXPORT_FLAG visibility default）经 TARGET_LINK_LIBRARIES 里的 WtMsgQue dylib 即可调用与覆盖（覆盖率归属取决于主控的 profraw 采集口径；若要求 WtMsgQue.cpp 行级归属第一方覆盖率，则需把 WtMsgQue/*.cpp 编入 CoverageTests 并补 libnanomsg 链接——本 lane 测试代码两种形态均无需改动）。

## 门禁结果
4/4 文件通过（仓库根执行的指定 clang++ -fsyntax-only -Wall -Wextra -Werror 命令，0 error 0 warning，最后一轮输出 `N06_GATE_ALL_PASS`、循环无 FAIL）。未跑 cmake/ninja/测试二进制。

## 待审核缺陷
docs/迭代计划/迭代01/bugs/n06_strategies_mq_share.md 共 6 条（BUG-WSN06-001 ~ 006），状态一律“待审核”，未修改任何生产代码。
