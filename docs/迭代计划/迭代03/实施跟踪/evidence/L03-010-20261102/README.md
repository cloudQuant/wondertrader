# L03-010：单一 0–0 全天段的累计分钟视图

本轮限定单一 raw `0–0` 段的聚合视图，不修改时间映射或其他零宽、混合时段合同。新增用例在构造偏移 `0/+300/-300` 时注册该段，检查三个聚合 getter，再依次设置 `+300/-300/0` 并检查缓存更新。旧实现当非零偏移映射的两个端点相同时，时长 getter 返回 1440 分钟而 `getSecMinList()` 返回 0；新增用例旧版 exit 1，八个断言失败（`red.log`、`red.xml`）。

新实现仅在单段 raw `0–0` 且偏移端点相同时，将该段的累计分钟数设为 1440，不改变其他段。普通构建在仓库根目录执行：

```sh
clang++ -std=c++17 -Isrc -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/googletest/1.17.0/include src/TestUnits/coverage/l03_includes/l03_session_info_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -pthread -o /tmp/wt-i03-l03-010/session-tests
/tmp/wt-i03-l03-010/session-tests --gtest_filter='l03_session.all_day_section_aggregate_views_survive_offset_changes:l03_session.minute_to_time_24h_uses_2400_convention:l03_session.empty_sections_fallback_branches:L03DaySessionTest.trading_minutes_and_section_list:l03_session.set_offset_mins_keeps_registered_sections_consistent:l03_session.night_section_raw_vs_offseted' --gtest_output=xml:/tmp/wt-i03-l03-010/green.xml
```

修复后 exit 0、6/6 PASS、0 skip（`green.log`、`green.xml`），包括空会话、零偏移全天、合法跨夜、普通分段和 offset 更新对照。Boost 和 GoogleTest 使用上述实体 Cellar 路径；源码与二进制 SHA-256 见 `sha256.txt`。未运行 sanitizer、跨平台或整体回归。

重要限制：非零偏移的 `timeToMinutes`、`minuteToTime` 等时间映射仍会遇到相等端点；此处只修聚合列表，不能声明全天映射已修复。空会话映射、多个时段混合 `0–0`、其他零宽段的拒绝和日期归属均未确定，原条目保持阻塞。
