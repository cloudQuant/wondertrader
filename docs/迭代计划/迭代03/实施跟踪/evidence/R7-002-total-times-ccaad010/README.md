# R7-002：TWAP total_times 局部验证

源码固定为 `ccaad010` 的 `src` Git archive；源代码和测试哈希见 `source-hashes.txt`。旧代码在 `getUInt32` 中将浮点数转换为无符号整数；新代码先按 double 读值，拒绝非有限、低于 1 和大于 `uint32_t` 上限的值，之后转换。保留原先有效非整数取整行为。本项不代表 R7-002 全项完成。

## 旧路径组件反例

`old-conversion.cpp` 只调用同一 WTSVariant 旧转换路径，并不是旧版完整插件。以本机 Clang 编译并实际运行：

```sh
/usr/bin/c++ -std=c++17 -fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all -I src old-conversion.cpp -o old-conversion
./old-conversion
```

编译退出 0，运行退出 134；`old-conversion-run.log` 在 WTSVariant.hpp:377 报 `-1 is outside the range of representable values of type 'unsigned int'`。

## 快照和构建

```sh
git archive ccaad010 src | tar -xf - -C /tmp/wt-i03-r7-times-frozen
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/bin/cmake -S /tmp/wt-i03-r7-times-frozen/src -B /tmp/wt-i03-r7-times-frozen-build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build /tmp/wt-i03-r7-times-frozen-build --target CoverageTests_exefact -j 6
```

配置退出 0；首次构建退出 1：无关 `r24_exec_runner_test.cpp` 在 GTest 实例化中遇到 signed/unsigned 比较，`-Werror` 拒绝；见 `frozen-build.log`。随后仅在**隔离快照**中按 `CMakeLists.focus.patch` 把 exefact 测试源限定到 `r7_twap_unit_test.cpp`，保留原产品 TU，重新构建退出 0（`focused-build.log`）。这是裁剪目标，不能称为完整 exefact 目标通过。常规二进制 SHA-256 `1a4f7bd2dd5333f2dbbb8e0287af83408b648d2ce2e3bc7bd4279edc870e65bd`。

再按 `CMakeLists.ubsan.patch` 对该裁剪目标 TU 增加 `-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all`，重新构建退出 0；哈希见 `ubsan-binary-hash.txt`。它的外部库及其他依赖未插桩。全程对快照、构建、运行目录执行 `find ... -type l` 无输出，未创建符号链接。

## 定向运行

常规及局部 UBSan 构建分别在独立临时工作目录运行下列筛选；后者增加 `UBSAN_OPTIONS=halt_on_error=1`，二者各退出 0、5/5 PASS；日志和 XML 分别见 `focused-test.log`/`focused.xml`、`ubsan-test.log`/`ubsan.xml`。

```sh
CoverageTests_exefact '--gtest_filter=r7_twap.total_times_out_of_range_is_rejected_before_unsigned_conversion:r7_twap.positive_total_times_one_and_n_derive_expected_spans:r7_twap.init_zero_total_times_bounded_child_probe:r7_twap.init_positive_total_times_control_returns:r7_twap.invalid_schedule_configs_fail_closed_without_orders' --gtest_output=xml:RESULT.xml
```

覆盖缺项/零、负整数和负数字符串、超过 uint32 上限、NaN/Inf 的受控拒绝以及 1、10 次的间隔正控。`invalid_schedule_configs_fail_closed_without_orders` 还含同一测试函数内的其他配置拒绝检查。未运行全量回归或真实供应商；未复验 Local/Diff/Arbi/回测宿主。Diff 初始化失败后安全重试与目标落账竞态、跨平台和生命周期仍未闭合。当前证据只证明上述本机快照的局部行为。

## 检查点后 Windows 宏兼容复核

`TimeUtils.hpp` 在 Windows 包含 `windows.h`，`numeric_limits::max()` 可能被 `max` 宏展开。检查点后仅将它改为 `(std::numeric_limits<uint32_t>::max)()`，具体差异见 `source-max-after-checkpoint.patch`。将此单一修改复制到上述隔离快照后，产品 TU SHA-256 为 `59200d69aa16d0735d8b72051df1f7cb3d0b1a552a1a4132c17dfbcaa2f2126f`，其余源和测试仍是 `ccaad010`。局部 UBSan 目标重新编译退出 0（`ubsan-max-build.log`），上述同一筛选重新运行 5/5 PASS、退出 0（`ubsan-max-test.log` 和 `ubsan-max.xml`），新二进制哈希见 `ubsan-max-hash.txt`。这不代替 Windows 实际编译。
本机另用 `max-macro-probe.cpp` 人工定义同形函数式 `max` 宏后，`/usr/bin/c++ -std=c++17 -Werror max-macro-probe.cpp -o max-macro-probe` 编译及执行均退出 0，验证该表达式不被宏替换；该探针不证明 Windows/MSVC 行为。
