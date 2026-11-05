# ParserUDP lifecycle V2 local evidence

The ordinary dynamic module is built from the current `src/ParserUDP/ParserUDP.cpp`.
The fixture also builds a second module from the same source with
`WT_PARSERUDP_TEST_HOOKS` solely for thread-creation, timer-scheduling and
partial-connect failure injection. Those hooks are absent from the ordinary
module. The executable compiles the current `ParserAdapter.cpp` directly.

Run from the repository root on the macOS machine used for this evidence:

```sh
cmake -S src/TestUnits/fixtures/i03_parser_udp_lifecycle_v2 \
  -B /tmp/wt-i03-parser-udp-v2-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-udp-v2-20261103 -j2
(
  cd /tmp/wt-i03-parser-udp-v2-20261103
  LLVM_PROFILE_FILE=$PWD/profile.profraw perl -e 'alarm 30; exec @ARGV' -- \
    ./i03_parser_udp_v2 --gtest_output=xml:$PWD/results.xml > results.log 2>&1
  LLVM_PROFILE_FILE=$PWD/legacy.profraw perl -e 'alarm 30; exec @ARGV' -- \
    ./i03_parser_udp_legacy \
    --gtest_filter='R76ParserUDPTest.*:t6_parserudp_factory.*:t6_parserudp_init.valid*:t6_parserudp_init.explicit*:t6_parserudp_init.non_numeric*:t6_parserudp_init.missing*:t6_parserudp_init.repeated*:t6_parserudp_spi.*:t6_parserudp_subs.*:t6_parserudp_state.*:ParserUDPShutdownTest.ExternalReleaseWaitsForActiveSpiCallbackAndBlocksLaterDispatch:ParserUDPShutdownTest.ActiveUdpDisconnectEmitsOneCloseAndReconnectStartsNewEventEpoch' \
    --gtest_output=xml:$PWD/legacy.xml > legacy.log 2>&1
)
```

For the ASan/UBSan and TSan builds respectively, replace the build directory
above with `/tmp/wt-i03-parser-udp-v2-asan` or
`/tmp/wt-i03-parser-udp-v2-tsan`, use
`-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++`, and add:

```sh
# ASan/UBSan configure arguments
'-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -include stdlib.h'
'-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined'
'-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
# TSan configure arguments (instead of the three ASan/UBSan arguments)
'-DCMAKE_CXX_FLAGS=-fsanitize=thread -fno-omit-frame-pointer -include stdlib.h'
'-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread'
'-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread'
```

Build targets `ParserUDP ParserUDPFault i03_parser_udp_v2` with `-j2`. Run the
same V2 executable command from the respective build directory with
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
for ASan/UBSan, or `TSAN_OPTIONS=halt_on_error=1` for TSan. Leak detection was
not run. `WT_BUILD` supplies prebuilt WtCore/WTSTools/WTSUtils static libraries;
these supporting dependencies were not rebuilt or sanitizer-instrumented by
this fixture. Homebrew include and library paths and iconv are in CMakeLists.txt.

The V2 stop request uses only atomics required at compile time to be always
lock-free; it neither posts to Asio nor waits for a callback or worker lock.
An instance worker checks the stop signal on a 10ms timer, cancels sockets and
lets `io_context::run()` consume outstanding completions. The zero-time poll
requires closed API admission, no admitted host API operations and a future
made ready at thread exit after thread-local destruction; only destroy joins.
A timer or worker failure that prevents that proof yields ERROR or retains the
owner rather than reporting DRAINED. Timer delay is not a real-time guarantee.
Platforms without the required lock-free atomics fail to build this V2 code.

Normal checks use local loopback packets and the real dynamic module/adapter;
no exchange or provider service is contacted. The timer-failure test first
uses a test-only hook to prove the failed worker has been synchronously joined,
then verifies the V2 ERROR leaves the handle intact; it directly disposes of
that controlled test instance only after that independent cleanup proof. This
is not a general authority to destroy a provider after V2 ERROR.

The caller must serialize object destruction against all future host API and
lifecycle calls, retain the adapter, parser module and borrowed graph until
successful V2 destroy, and retain them on PENDING/ERROR. A provider cannot
protect a host call through a pointer after the host destroys that object.
The tests do not prove arbitrary external host use, all interleavings, platform
behavior beyond this machine, whole-Runner shutdown, production deployment,
full regression or provider/exchange acceptance. The previous
`t6_parserudp_init.wtcore_parser_adapter_init_ext_passes_null_and_releases_real_parser`
legacy oracle is not run: the separately changed adapter now retains initExt
owners lacking V2 proof. The two modified old init tests reflect the requested
new rejection of repeated successful initialization.
