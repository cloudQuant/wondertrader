#!/bin/sh
set -eu
cd /Users/yunjinqi/Documents/new_projects/wondertrader
out=/private/tmp/wt-cta-review
plugin_dir=/private/tmp/wt-i03-cta-quarantine/review3/run.8W1N9zDN
shasum -a 256 src/WtCore/CtaStrategyMgr.cpp src/WtCore/CtaStrategyMgr.h src/WTSTools/WTSLogger.h src/Share/fmtlib.h src/TestUnits/fixtures/cta_factory_quarantine/logger_stub.cpp "$out/repeated.cpp" "$out/repeated_different.cpp" /private/tmp/wt-cta-review-long/long_factory.cpp /private/tmp/wt-cta-review-long/logger.cpp /private/tmp/wt-cta-review-long/main.cpp "$plugin_dir/plugins/fixture.dylib" "$plugin_dir/plugins2/fixture.dylib" > "$out/source-before-final.sha256"
set -x
/usr/bin/c++ -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -Isrc -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -include src/Share/fmt_enum_compat.h src/WtCore/CtaStrategyMgr.cpp src/TestUnits/fixtures/cta_factory_quarantine/logger_stub.cpp "$out/repeated.cpp" -o "$out/repeated-final"
/usr/bin/c++ -std=c++17 -g -O0 -pthread -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -Isrc -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -include src/Share/fmt_enum_compat.h src/WtCore/CtaStrategyMgr.cpp src/TestUnits/fixtures/cta_factory_quarantine/logger_stub.cpp "$out/repeated_different.cpp" -o "$out/repeated-different-final"
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -fsanitize=address -fno-omit-frame-pointer -fPIC -dynamiclib /private/tmp/wt-cta-review-long/long_factory.cpp -o /private/tmp/wt-cta-review-long/plugins/fixture.dylib
/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ -std=c++17 -g -O1 -pthread -fsanitize=address -fno-omit-frame-pointer -DFMT_HEADER_ONLY -DSPDLOG_FMT_EXTERNAL -I/opt/homebrew/Cellar/boost/1.88.0/include -I/opt/homebrew/Cellar/fmt/11.2.0/include -I/opt/homebrew/Cellar/spdlog/1.15.3/include -include stdlib.h -include src/Share/fmt_enum_compat.h src/WtCore/CtaStrategyMgr.cpp /private/tmp/wt-cta-review-long/logger.cpp /private/tmp/wt-cta-review-long/main.cpp -o /private/tmp/wt-cta-review-long/main-final
WT_CTA_FACTORY_MODE=good WT_CTA_MARKER="$out/marker-final" perl -e 'alarm 20; exec @ARGV' -- "$out/repeated-final" "$plugin_dir/plugins" > "$out/repeated-final.log" 2>&1
WT_CTA_FACTORY_MODE=good WT_CTA_MARKER="$out/marker-final2" perl -e 'alarm 20; exec @ARGV' -- "$out/repeated-different-final" "$plugin_dir/plugins" "$plugin_dir/plugins2" > "$out/repeated-different-final.log" 2>&1
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 perl -e 'alarm 20; exec @ARGV' -- /private/tmp/wt-cta-review-long/main-final /private/tmp/wt-cta-review-long/plugins > "$out/long-final.log" 2>&1
shasum -a 256 src/WtCore/CtaStrategyMgr.cpp src/WtCore/CtaStrategyMgr.h src/WTSTools/WTSLogger.h src/Share/fmtlib.h src/TestUnits/fixtures/cta_factory_quarantine/logger_stub.cpp "$out/repeated.cpp" "$out/repeated_different.cpp" /private/tmp/wt-cta-review-long/long_factory.cpp /private/tmp/wt-cta-review-long/logger.cpp /private/tmp/wt-cta-review-long/main.cpp "$plugin_dir/plugins/fixture.dylib" "$plugin_dir/plugins2/fixture.dylib" > "$out/source-after-final.sha256"
cmp "$out/source-before-final.sha256" "$out/source-after-final.sha256"
cat "$out/repeated-final.log" "$out/repeated-different-final.log" "$out/long-final.log"
