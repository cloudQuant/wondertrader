# BUG-L01-018：weekday helper 本机定向复验

2026-11-02，macOS arm64，Apple clang 17.0.0。测试和头文件从检查点 `ccaad010e9169f610aded4d8dc0f5647f70a5cff` 导出到独立实体目录；运行时工作区的对应两个文件与快照的 SHA-256 一致。没有修改产品代码、创建符号链接或运行整体回归。

```sh
mkdir -p /tmp/wt-i03-l01-018-20261102/snapshot
git archive ccaad010 src/Share/TimeUtils.hpp src/TestUnits/coverage/l01_share_wtsutils/l01_timeutils_test.cpp | tar -x -C /tmp/wt-i03-l01-018-20261102/snapshot
clang++ -std=c++17 -Wall -Wextra \
  -I/tmp/wt-i03-l01-018-20261102/snapshot/src \
  -I/opt/homebrew/Cellar/googletest/1.17.0/include \
  /tmp/wt-i03-l01-018-20261102/snapshot/src/TestUnits/coverage/l01_share_wtsutils/l01_timeutils_test.cpp \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  -pthread -o /tmp/wt-i03-l01-018-20261102/weekday_tests
/tmp/wt-i03-l01-018-20261102/weekday_tests \
  '--gtest_filter=l01_timeutils.get_week_day:l01_timeutils.try_get_week_day_rejects_invalid_gregorian_dates:l01_timeutils.is_weekends' \
  --gtest_output=xml:/tmp/wt-i03-l01-018-20261102/results.xml
```

编译退出 0；三个指定用例实际运行 3/3 PASS、0 skip，进程退出 0，见 [run.log](run.log) 和 [results.xml](results.xml)。断言包含 `19691231 -> 3`、`20240101 -> 1`、有效闰日及非法年月日返回 `INVALID_WEEKDAY` 并拒绝识别为周末。仅这三个用例运行；无 sanitizer 结果。

SHA-256：`TimeUtils.hpp` 为 `4a6491595dd9ec46d2489d441e06186d0dea352d6b0296c62b8f188c6da7018f`；测试 TU 为 `690bd1a5827fd35d2eb6dd5e9cd16336a235342101f30c95cb4c0553bde1ec52`；测试二进制为 `6d6e209a2c8629d416d1ba679f552b36be3e7d5c8cbcc7b8338cdaf427c49485`。GTest 静态库来自 Homebrew Cellar 1.17.0；没有重编或插桩 GTest。

本次仅证明上述 macOS helper 范围；Windows/MSVC、`isHoliday` 和其他调用者的失败传播、BUG-R12-13 类型化日期映射、最终整合树均未验证，本条不能据此关闭。
