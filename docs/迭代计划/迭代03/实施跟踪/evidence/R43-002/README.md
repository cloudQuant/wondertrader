# BUG-R43-002 空簿买卖 UBSan 探针

当前 MatchEngine.cpp 和本目录 probe.cpp 独立编译；买卖两方向 latest=10、bid=ask=0 分别在第 359/406 行触发浮点除零并 exit 134；每个方向的 bid-only、ask-only、双边对照合计 6/6 exit 0。对应日志及哈希见本目录。

```sh
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -O1 -g -fsanitize=undefined,float-divide-by-zero,float-cast-overflow -fno-sanitize-recover=all -fprofile-instr-generate -fcoverage-mapping -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/tmp/wt-i02-fresh-20261101-v8-build/TestUnits/coverage/cov_boost_include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/include -Isrc -Isrc/Includes -Isrc/Share -Isrc/WTSTools -Isrc/WTSUtils -include stdlib.h -include src/Share/fmt_enum_compat.h /tmp/wt-i03-r43-probe/probe.cpp src/WtBtCore/MatchEngine.cpp /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSTools.a /tmp/wt-i03-r58-build/build_x64/Debug/libs/libWTSUtils.a -L/opt/homebrew/lib -L/usr/local/lib -lboost_filesystem -lboost_thread -liconv -ldl -lpthread -o /tmp/wt-i03-r43-probe/probe
UBSAN_OPTIONS=print_stacktrace=1 LLVM_PROFILE_FILE=/tmp/wt-i03-r43-probe/<side>-<book>.profraw /tmp/wt-i03-r43-probe/probe <side> <book>
```

以上 side=buy/sell，book=empty/bid/ask/both；其他 WTSTools/WTSUtils 静态库复用隔离构建且未全插桩。现有 VWAP 对空订单返回仍记录触发次数，不能未经合同分析直接拒单。未知排队量和订单恢复语义待明确；未修复，保持阻塞。
