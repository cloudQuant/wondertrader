# BUG-L03-002：身份字符串初始化局部验证

日期：2026-11-02。此记录仅覆盖 `WTSCommodityInfo` 身份字符数组子项，整项仍阻塞，未进行整体回归。

构造时将七个身份字符数组清零；`create` 随后仍写入所给身份和完整品种码。新增定向用例检查每个数组字符串后的剩余字节，既有身份、显式数值和分类用例作为对照。未保存修复前红控，不称红绿闭环。

## 本机定向验证

macOS arm64，Apple clang 17；使用以下实体 Cellar 目录的 Boost 1.88.0 与 GoogleTest 1.17.0；本次未创建符号链接：

```sh
clang++ -std=c++17 -Isrc \
  -I/opt/homebrew/Cellar/boost/1.88.0/include \
  -I/opt/homebrew/Cellar/googletest/1.17.0/include \
  src/TestUnits/coverage/l03_includes/l03_commodity_info_test.cpp \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest_main.a \
  /opt/homebrew/Cellar/googletest/1.17.0/lib/libgtest.a \
  -pthread -o /tmp/wt-i03-l03-002/commodity_physical

/tmp/wt-i03-l03-002/commodity_physical \
  --gtest_filter='l03_commodity.create_initializes_identity_buffers:l03_commodity.create_fills_identity_fields:l03_commodity.scalar_accessors_round_trip:l03_commodity.category_predicates'
```

编译及运行均退出 0，定向 4/4 PASS，运行原始输出见 [physical.log](physical.log)。`git diff --check -- src/Includes/WTSContractInfo.hpp src/TestUnits/coverage/l03_includes/l03_commodity_info_test.cpp` 退出 0。

SHA-256：

```text
7cf68896ea679ba2ffc1df000ca11a8496bf5472cf1e546ede5613e2d7c1db52  src/Includes/WTSContractInfo.hpp
7296464838a1b27801b39feff88a1567959b3d200283d7e6705738a3e69a6454  src/TestUnits/coverage/l03_includes/l03_commodity_info_test.cpp
80512b9c67181b483fac8c87d9e535762f6d3057ab9eb9d4431451397fca1e03  /tmp/wt-i03-l03-002/commodity_physical
237ea603c89f0ceef569ef564eab8c9835fe3331bb9c466ccfee80474dd1d054  physical.log
```

## Sanitizer 及局限

使用上述源和实体依赖，附加 `-fsanitize=address,undefined -fno-omit-frame-pointer` 的目标编译成功；相同过滤器运行约 32 秒无输出后人工中断，退出 130。空程序以同一编译器和 sanitizer 构建，运行 `perl -e 'alarm 5; exec @ARGV' /tmp/wt-i03-l03-002/asan-probe` 无输出、退出 142。故 ASan/UBSan 没有有效运行结论，不计通过。

`clang++ -std=c++17 -fsanitize=memory -x c++ -o /tmp/wt-i03-l03-002/msan-probe - <<<'int main(){ return 0; }'` 在该目标不受支持，重新执行的退出码及诊断见 [msan-compile.exit](msan-compile.exit) 和 [msan-compile.log](msan-compile.log)；MSan 未运行。

创建后 `volScale`、`priceTick`、`lotTick`、`minLots` 和四个枚举仍未初始化。零枚举分别可能表示 `CC_Stock`、`TM_Both` 等实际交易含义；数值字段存在被用作除数的调用。配置加载路径会设置这些字段，但直接 `create` 不保证已配置。现有 getter 没有失败返回，添加状态成员涉及布局兼容及所有调用方的准入语义；本次没有猜测默认值，也未声称解决整项。完整加载路径、跨平台和最终集成树未验证。
