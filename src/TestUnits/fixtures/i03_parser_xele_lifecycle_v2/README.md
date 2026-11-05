# ParserXeleSkt lifecycle V2 local evidence (T2-XELE-02, R76-08)

Scope: `src/ParserXeleSkt/ParserXeleSkt.{h,cpp}` migrated to the Parser V2
stop/drain lifecycle (`Includes/IParserLifecycleV2.h`) following decision
card 9 (9a A+C, 9b A, 9c A, 9d A), in the same style as ParserUDP/ParserShm.
`ParserAdapter.cpp` and `Includes/*` were not changed. Pre-change source
(git HEAD `ccaad010`, identical to the working tree before this change) is
saved in `baseline/` with `SHA256SUMS`.

## Targets

- `ParserXeleSkt` (`parsers/`): ordinary module from the current source.
- `ParserXeleSktFault` (`fault/parsers/`): same source with
  `WT_PARSERXELE_TEST_HOOKS` (worker-start failure, stop-timer failure,
  legacy shutdown export). The hooks are absent from the ordinary module
  (`evidence/symbols.txt`).
- `ParserXeleSktBaseline` (`baseline/parsers/`): the saved pre-change source.
- `i03_parser_xele_v2`: `check.cpp` + current `ParserAdapter.cpp`, V2 table and
  real adapter two-phase stop.
- `i03_parser_xele_scenarios` / `i03_parser_xele_scenarios_baseline`: the same
  `scenarios.cpp` (legacy `IParserApi`/`createParser`/`deleteParser` only)
  against the current and the baseline module: green and red control.
- `i03_parser_xele_legacy`: current source + existing coverage tests
  `t_ParserXeleSkt/t2_parserxele_lifecycle_test.cpp` and
  `r76_xele_boundary_test.cpp`.
- `i03_parser_xele_prepare`: current source + `prepare_transaction.cpp`.

## Commands

Run from the repository root on the macOS machine used for this evidence:

```sh
cmake -S src/TestUnits/fixtures/i03_parser_xele_lifecycle_v2 \
  -B /tmp/wt-i03-parser-xele-v2-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-xele-v2-20261103 -j2
(
  cd /tmp/wt-i03-parser-xele-v2-20261103
  LLVM_PROFILE_FILE=$PWD/profile.profraw perl -e 'alarm 60; exec @ARGV' -- \
    ./i03_parser_xele_v2 --gtest_output=xml:$PWD/results.xml > results.log 2>&1
  LLVM_PROFILE_FILE=$PWD/scenarios.profraw perl -e 'alarm 60; exec @ARGV' -- \
    ./i03_parser_xele_scenarios --gtest_output=xml:$PWD/scenarios.xml > scenarios.log 2>&1
  LLVM_PROFILE_FILE=$PWD/prepare.profraw perl -e 'alarm 60; exec @ARGV' -- \
    ./i03_parser_xele_prepare --gtest_output=xml:$PWD/prepare.xml > prepare.log 2>&1
  LLVM_PROFILE_FILE=$PWD/legacy.profraw perl -e 'alarm 60; exec @ARGV' -- \
    ./i03_parser_xele_legacy \
    --gtest_filter='-R76ParserXeleSktLoopbackTest.PrepareCommitsOnlyCompleteTcpSnapshotBatches:R76ParserXeleSktLoopbackTest.FirstReconnectFailureLeavesRetryUnseeded:R76ParserXeleSktTest.InitIsRepeatableAndReleaseIsIdempotent' \
    --gtest_output=xml:$PWD/legacy.xml > legacy.log 2>&1
  LLVM_PROFILE_FILE=$PWD/legacy-all.profraw perl -e 'alarm 60; exec @ARGV' -- \
    ./i03_parser_xele_legacy --gtest_output=xml:$PWD/legacy-all.xml > legacy-all.log 2>&1
  # Red control: one process per scenario, because the baseline aborts.
  for t in ConnectDisconnectConnectResumesData ReleaseThenConnectIsRejected \
    ReleaseAndRemoverInsideWorkerCallbackDoNotSelfJoin \
    RepeatedReleaseAndDestructorJoinWorker DisconnectInsideCallbackIsQueuedAndConnectIsBusy; do
    LLVM_PROFILE_FILE=/dev/null perl -e 'alarm 20; exec @ARGV' -- \
      ./i03_parser_xele_scenarios_baseline --gtest_filter="XeleLifecycleScenario.$t" \
      --gtest_output=xml:$PWD/red-$t.xml > red-$t.log 2>&1
    echo "$t exit=$?" >> red-summary.txt
  done
)
```

For ASan/UBSan and TSan, use build directories
`/tmp/wt-i03-parser-xele-v2-20261103-asan` and
`/tmp/wt-i03-parser-xele-v2-20261103-tsan`,
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

Build only `ParserXeleSkt ParserXeleSktFault i03_parser_xele_v2
i03_parser_xele_scenarios i03_parser_xele_prepare` with `-j2`, then run the
three executables from that directory with
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
or `TSAN_OPTIONS=halt_on_error=1`. Leak detection was not run. `WT_BUILD`
supplies prebuilt WtCore/WTSTools/WTSUtils static libraries that were not
rebuilt or sanitizer-instrumented here. The product TU was also checked with
`/usr/bin/c++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror`.

## Results (2026-11-03, this machine)

| Run | Result | Evidence |
| --- | --- | --- |
| `i03_parser_xele_v2` | 11/11, exit 0 | `evidence/results.{log,xml}` |
| `i03_parser_xele_scenarios` (current) | 5/5, exit 0 | `evidence/scenarios.{log,xml}` |
| `i03_parser_xele_prepare` | 1/1, exit 0 | `evidence/prepare.{log,xml}` |
| legacy filtered | 30/30 (+1 pre-existing DISABLED), exit 0 | `evidence/legacy.{log,xml}` |
| legacy unfiltered | 30 pass, 3 fail, exit 1 (see below) | `evidence/legacy-all.{log,xml}` |
| red: baseline scenarios | 5/5 abort, exit 134 (`libc++abi: terminating`) | `evidence/red/` |
| ASan/UBSan | v2 11/11, scenarios 5/5, prepare 1/1 | `evidence/asan-*.{log,xml}` |
| TSan | v2 11/11, scenarios 5/5, prepare 1/1 | `evidence/tsan-*.{log,xml}` |
| repetition | normal v2 x30, scenarios x50; TSan v2 x10, scenarios x20; all pass | `evidence/repeat-summary.txt` |

The three unfiltered legacy failures all stop at the second `init()`
assertion (`r76_xele_boundary_test.cpp:613`, `:673`, `:743`). They encode the
former "repeated init succeeds" behavior, which card 9c (A) replaces. They
were not modified (not owned by this change). `prepare_transaction.cpp`
re-checks the same snapshot commit/rollback cases (complete batch, reset after
a full read, truncated tail, throwing lookup, later commit) with a single init
and one loopback port serving successive payloads.

The first TSan run reported a race inside Boost.Asio's kqueue reactor: the
socket's descriptor state was constructed on the connecting thread while the
running worker read it from the kernel event queue. The fix opens and
registers the multicast socket on the worker (`open_on_worker`); the
connecting thread waits on a future only while the worker is alive. The
TSan results above are after that change.

## Behavior covered

- connect -> disconnect -> connect (x3) on one instance: data resumes on the
  same persistent worker; one `WPE_Close` per active connection; the TCP
  snapshot is synchronized once per instance.
- `release()` then `connect()`: `connect` returns false; repeated release and
  repeated `deleteParser` are safe.
- `release()` and `deleteParser` inside the worker's quote callback: no
  self-join; the remover keeps the owner (pointer not cleared); a later remover
  call from the test thread joins and destroys it.
- Direct `delete` from a non-worker thread joins the worker.
- 9c: second init rejected, the original TCP endpoint is used.
- 9d: `connect()` from the worker callback returns false (busy);
  `disconnect()` there closes inline and the `WPE_Close` is queued.
- V2: table/preflight; request stop is idempotent and lock-free; wait/destroy
  from the callback return WOULD_DEADLOCK/ERROR; wait stays PENDING while a
  callback or a host API call is in progress; independent instances; worker
  creation and stop-timer failures; legacy release then V2 destroy; the real
  `ParserAdapter`/`ParserAdapterMgr` two-phase stop.

Local endpoints only: the snapshot server listens on `127.0.0.1`; the parser
joins `239.255.77.77` on the loopback interface (`local_host=127.0.0.1`) and
the tests send unicast datagrams to `127.0.0.1:<port>`. No multicast datagram
is sent and no vendor or exchange endpoint is contacted.

## Limits

- Direct `delete` of the parser from its own worker or an SPI callback cannot
  be made safe (the io_context it runs on would be destroyed); the remover
  refuses it, and a direct delete there terminates through the joinable thread
  member instead of using freed memory. It is not tested.
- `prepare()` is a blocking TCP read on the connecting thread; a stalled
  snapshot server blocks `connect()` (and V2 drain stays PENDING) until the
  socket fails. No synchronous network timeout was added or verified.
- The receive-error automatic retry now uses a cancellable 2s timer instead of
  a 2s sleep on the worker; that path was not exercised (no local way to inject
  a UDP receive error).
- Update 2026-11-03 (card 14 decided): Connect/Login are now emitted per
  connection epoch; see `../i03_parser_event_contract_20261103/README.md`.
  `check.cpp` `ReconnectCycleThenDrain` previously asserted zero Connect events
  (the then-undecided state); that assertion was replaced by the decided
  contract (3 Connect + 3 Login over three cycles). The results table above is
  the pre-card-14 run; the post-card-14 rerun (v2 11/11, scenarios 5/5,
  prepare 1/1, legacy 33/33 with the rewritten r76 oracles, ASan/UBSan and TSan
  each passing) is in
  `../i03_parser_event_contract_20261103/evidence/xele_lifecycle_rerun/`.
- Update 2026-11-03: the three r76 legacy tests that encoded repeated init were
  rewritten to the card-9c contract (single init, sequenced loopback server);
  the unfiltered legacy run now passes 33/33.
- Real Xele SDK feed, real multicast, Windows/MSVC, Runner-wide shutdown,
  production deployment and full regression were not run. The prebuilt static
  dependencies were not sanitizer-instrumented.
