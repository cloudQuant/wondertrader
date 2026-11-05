# ParserShm lifecycle V2 local fixture

Run from the repository root on the macOS environment used for this evidence:

```sh
cmake -S src/TestUnits/fixtures/i03_parser_shm_lifecycle_v2 \
  -B /tmp/wt-i03-parser-shm-v2-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-shm-v2-20261103 -j2
(
  cd /tmp/wt-i03-parser-shm-v2-20261103
  LLVM_PROFILE_FILE=$PWD/profile.profraw perl -e 'alarm 30; exec @ARGV' -- \
    ./i03_parser_shm_v2 --gtest_output=xml:$PWD/results.xml > results.log 2>&1
  LLVM_PROFILE_FILE=$PWD/legacy.profraw perl -e 'alarm 30; exec @ARGV' -- \
    ./i03_parser_shm_legacy \
    --gtest_filter='R76ParserShmTest.*:ParserShmTest.*:shm_queue_layout.*:ParserShmZeroGlobalSize.*:shm_plugin_factory.*-R76ParserShmTest.InitIsRepeatableAndNeverConnects:ParserShmTest.init_accepts_full_and_partial_config' \
    --gtest_output=xml:$PWD/legacy.xml > legacy.log 2>&1
)
src/TestUnits/fixtures/i03_parser_shm_lifecycle_v2/baseline_reproduce.sh
```

The baseline script copies the saved source to a unique temporary directory and
rewrites only relative include paths; it expects the prior multisegment-code
unit test to fail. The current legacy run includes that unchanged test and
passes it for both subscribe and unsubscribe. The two explicitly excluded old
unit tests expect repeated initialization to succeed; the requested V2 rule now
rejects a second successful initialization. Another pre-existing disabled test
was not run.

The V2 fixture compiles the current `ParserShm.cpp` as an actual shared module,
loads it dynamically, and compiles current `ParserAdapter.cpp` for the real
adapter integration. The separate `ParserShmFault` module enables only
`WT_PARSERSHM_TEST_HOOKS` for worker creation and error-path observation; these
hooks are absent from the ordinary module. `WT_BUILD` provides pre-existing
WtCore/WTSTools/WTSUtils static libraries. Those dependencies were not rebuilt
or sanitizer-instrumented by this fixture; the CMake file specifies local
Homebrew and iconv paths.

The local file fixture creates a private `mkdtemp` directory and a new mapped
file only there. On this build, `CastQueue` is 4,259,872 bytes, each of 8,192
items is 520 bytes, and the synthetic producer only publishes the readable
index into that private file. Tests observe connection and tick callbacks,
including a tick delivered through the actual ParserAdapter and a callback
which requests stop. No user mapping, live provider or exchange is touched.

For ASan/UBSan and TSan, substitute build directories
`/tmp/wt-i03-parser-shm-v2-asan` and `/tmp/wt-i03-parser-shm-v2-tsan`, use
`-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++`, and add:

```sh
# ASan/UBSan
'-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -include stdlib.h'
'-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined'
'-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
# TSan, instead of the preceding three arguments
'-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -include stdlib.h'
'-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread'
'-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread'
```

Build only targets `ParserShm ParserShmFault i03_parser_shm_v2` with `-j2`.
Run the V2 executable from the relevant build directory with
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
or `TSAN_OPTIONS=halt_on_error=1`, respectively. Leak detection was not run.

Stop uses only atomics required at compile time to be always lock-free. It
closes host API admission and signals the worker without joining or waiting for
its callback/state lock. The worker checks the stop flag while waiting for a
file and while polling; sleeps are checked in at most 10ms chunks, without a
real-time scheduling guarantee. A zero-time V2 wait checks admitted host calls,
checks a future made ready only at worker thread exit, then tries the state lock
and clears host borrows; it never joins. Destroy joins and unmaps only after
DRAINED. A failed worker or unavailable proof returns ERROR/PENDING and requires
retaining the owner. Platforms without the required lock-free atomics fail to
build this V2 code.

The invalid-mapping test first uses a test-only hook to confirm synchronous
legacy worker join, then checks that V2 reports ERROR and leaves its handle
unchanged. It only disposes of that controlled instance after the independent
join proof; this is not general permission to destroy a provider after ERROR.
The caller must serialize destruction against future host API/lifecycle calls
and retain the adapter, module and borrowed graph on PENDING/ERROR. Direct
unsafe destruction from a callback and complete Runner graph shutdown remain
outside this fixture's guarantee.

The existing mapped queue uses volatile producer/consumer fields without a
portable cross-process synchronization contract. The local pwrite/mmap tick
observation and TSan run do not prove that protocol's data consistency or race
freedom (kernel writes and prebuilt dependencies are not fully instrumented).
No claim is made for concurrent production publishers, all interleavings,
other platforms, full regression, deployment or exchange acceptance.
