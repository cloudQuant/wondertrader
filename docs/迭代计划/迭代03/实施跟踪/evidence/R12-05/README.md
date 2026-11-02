# BUG-R12-05 WTSMap 空值生命周期定向结果

在 macOS arm64 使用 LLVM 22 AddressSanitizer，复现 `WTSMap::clear` 对空值调用 `release()`；修复前两条新增用例分别退出 134，日志为 `red-*.log`。修复后两条 WTSMap 用例及原 WTSHashMap 空值用例共 3/3 PASS，见 `green-focused.xml`。

构建命令（红控在产品修改之前、绿控在修改之后执行，末尾输出分别为 `map-l02-asan-red` 和 `map-l02-asan-green`）：

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -fsanitize=address -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I /opt/homebrew/Cellar/boost/1.88.0/include -I /opt/homebrew/Cellar/googletest/1.17.0/include -I /opt/homebrew/Cellar/fmt/11.2.0/include -I src -I src/Includes -I src/Share -I src/WTSTools -I src/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h src/TestUnits/coverage/l02_wtstools_mq/l02_variant_collection_test.cpp /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a -lpthread -o /tmp/wt-i03-r12-map/map-l02-asan-green
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r12-map/map-l02-asan-green '--gtest_filter=L02VariantFixture.WtsMapNullAndOwnedValuesAcrossClear:L02VariantFixture.WtsMapFinalReleaseSkipsNullAndReleasesTransferredValue:L02VariantFixture.WtsHashMapNullValuesRemainSafeAcrossLifecycle' --gtest_output=xml:/tmp/wt-i03-r12-map/green-focused.xml
```

红控逐例使用相同的 ASAN_OPTIONS 和 `perl` 有界执行，过滤上述两个 WTSMap 测试。哈希见 `sha256.txt`。ASan 仅插桩测试翻译单元和内联容器；GTest 是预编译库，LeakSanitizer 未运行。Windows、完整工作树构建和整体回归未验证。
