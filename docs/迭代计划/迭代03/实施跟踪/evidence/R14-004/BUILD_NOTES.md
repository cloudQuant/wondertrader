# BUG-R14-004 isolated rebuild after restart

Input: a physical `src` + `dist` copy at `/tmp/wt-r14-resume/source`. Source-tree `find -type l` before copying returned no symlinks. No repository file is edited. The key workspace/snapshot SHA-256 values are captured in `input-before.json`; they matched at capture.

CMake configure:

```
PATH=/opt/homebrew/bin:/usr/bin:/bin MyDeps=/opt/homebrew /opt/homebrew/Cellar/cmake/4.0.2/bin/cmake -S /tmp/wt-r14-resume/source/src -B /tmp/wt-r14-resume/build -G Ninja -DCMAKE_MAKE_PROGRAM=/opt/homebrew/Cellar/ninja/1.12.1/bin/ninja -DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DCMAKE_C_COMPILER=/usr/bin/cc -DCMAKE_CXX_COMPILER=/usr/bin/c++ -DCMAKE_PREFIX_PATH=/opt/homebrew -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include -DGTest_DIR=/opt/homebrew/Cellar/googletest/1.17.0/lib/cmake/GTest -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

An initial build used the existing `/opt/homebrew/include/boost` symlink. It was deliberately interrupted, exit 130, and `build-symlink-include-stopped.log` preserves its output. In the isolated copy only, `src/CMakeLists.txt` was then modified by adding `include_directories(BEFORE /opt/homebrew/Cellar/boost/1.88.0/include)` after the Boost include line. The source file before that change is `snapshot-CMakeLists.before.txt`; no behavior/source tests were changed. Reconfigured with `-DCMAKE_CXX_FLAGS=-I/opt/homebrew/Cellar/boost/1.88.0/include -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`; `reconfigure-physical.log` contains the result. `compile_commands.json` shows the physical Boost include before `/opt/homebrew/include`; `ninja -t deps` confirms WtDataWriter's Boost headers are physically sourced from the Cellar path.

Build command, output in `build-physical.log`:

```
PATH=/opt/homebrew/bin:/usr/bin:/bin nice -n 10 /opt/homebrew/Cellar/cmake/4.0.2/bin/cmake --build /tmp/wt-r14-resume/build --parallel 2 --target CoverageTests_m_QuoteFactory CoverageTests_m_WtBtRunner QuoteFactory WtBtRunner WtDataStorage
```

## Build and scoped tests

The corrected build exited 0. It compiled the current-at-snapshot product entry points, HisDataReplayer, WtDataStorage, both R14 test TUs and all their CMake dependencies; it is not a full repository build. WtStraDtSel.cpp emitted two existing unused-variable warnings under target-specific warning options; see build-physical.log. `binary-hashes.txt`, `writer-deps.txt`, and `symlinks.log` record binaries, actual writer dependencies, and no symlink in the isolated output tree.

`/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-r14-resume/run_tests.py` ran the 13 specified cases individually, in separate cwd/TMPDIR with 35-second bounds and saved `runs/<case>/output.log`, `result.xml`, and `results.json`. Each was 1/1 PASS, exit 0, no skip: eight QuoteFactory cases (help variants, explicit/default missing config, config initialization, and actual writer startup + SIGINT) and five WtBtRunner cases (help, explicit/default missing config and valid SEL configuration). No other cases ran. The real-writer case copies a physical module into a unique temporary directory, uses empty parser list, verifies writer initialization/cache, waits for readiness and sends SIGINT; it does not contact a provider.

`input-before.json` and `input-after.json` record matching workspace/snapshot hashes for the explicitly listed relevant source files and test TUs, including HisDataReplayer. The isolated snapshot CMakeLists has the intentional physical-Boost include addition described above. At test completion `workspace-snapshot-drift.txt` records other files changed concurrently during the build: WtCore's CtaStrategyMgr, EventNotifier and HftStraBaseCtx, as well as unrelated test/fixture additions, differ; these differences are not proof of current full-tree compatibility. QuoteFactory test target links libraries that can include WtCore dependencies; no subsequent full integration/build against those changed files was performed. Other platforms, real vendor data, sanitizers and overall regression remain unverified.
