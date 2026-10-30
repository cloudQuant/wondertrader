# Replay / Storage / Match v4 历史证据（待最终重跑）

本目录保存 2026-10-30 macOS arm64 v4 focused 日志和当时的二进制/对象哈希。历史结果为 MatchEngine 15/15、P04 replay 1/1、R23 reader 16/16；**它们当前均不构成执行清单 TEST_PASS**。共享 `WTSDataDef.hpp` 在本批后继续修改，所有四项都依赖该公共头文件，必须以最终版本重编重跑后再更新状态。P04 的 `HisDataReplayer.cpp` 还发现同源越界路径，正在修复。

## 历史 v4 检查覆盖

- **BUG-L06-004：** `MatchEngine::match_orders` 现在在 `qty <= 0` 时跳过成交和账本更新。测试验证买/卖方向零对手量不产生合成成交、余量保持，并在后续出现正对手量时按真实盘口数量成交。旧缺陷是零量被强制改为一。
- **BUG-P04-009：** v4 用例先请求缺失的主 K 线并安全完成，再在同一实例补齐主 CSV、重新 prepare 并完成 5 根日线回放。缺陷根因是主 series 在加载和确认 cache 非空之前就发布 `_main_key`；当前源码延迟发布并在 run 路径做空 cache 防护。此 v4 运行不覆盖后来发现的 `HisDataReplayer.cpp` 越界路径，也受共享头文件后续修改影响。
- **BUG-R23-005：** RT 与历史 tick cutoff 应按 `(action_date, action_time)` 词典序一致处理；v4 suite 覆盖两边 `etime` 相等时保留边界 tick、跨 action date 的早日期晚时钟组合和历史块末条边界，并验证历史回补不越过命中日。
- **BUG-R23-007：** 空历史缓存将 `count` 归零；上层只有在 `hisCnt + rtCnt > 0` 时建 slice。v4 suite 覆盖无数据、header-only/空历史，以及空历史加 RT bar 时按实际数量和块数返回。

## R23 相邻测试纠偏与 harness 历史

R23-004 的 `CountSizeCheckFailureCanRecoverAfterFileRepair` 首读 oracle 已从“跳过短文件、回退前一天补数”更正为 `NULL` 或稳定空切片，并保留修复文件后同一 reader 重试成功的断言。R23 v4 的 16 项 suite 包含此纠偏用例。该项不在本次四条状态更新范围内。

早先两次手工 R23 focused link 因漏加 `WtDataReader.cpp.o`，未解析 `proc_block_data`，运行崩溃；这是 isolated harness 链接缺项，不是产品测试失败。v4 显式加入 `CoverageTests/.../WtDataReader.cpp.o` 后 16/16 通过。v4 脚本也保留此 helper 对象。

## 尚未覆盖与后续门槛

ASan/TSan、Windows 原生构建/运行、真实行情文件、完整回测和生产调用链均未验证。R23 range callback、长期/并发数据生命周期及其他平台边界没有从本次结果推断。待 `WTSDataDef.hpp` 和 P04 同源路径稳定后，应使用 `reproduce-v4-pre-final.sh` 的显式对象/链接结构更新为当前脚本，生成新的 build/run 日志和 binary SHA，再决定四条状态；不要把本目录历史 15/1/16 计入当前统计。
