# Parser success-event contract (card 14) for ParserXeleSkt and ParserUDP

Items: R76-03 (ParserXeleSkt), R76-02 (ParserUDP). Card 14 was decided by the
user on 2026-11-03: 14a=C (separate "sent/transport" and "confirmed" events;
transport success is never reported as business success), 14c=C (without an
SDK contract, fail closed).

## Mapping onto the existing event codes

`Includes/WTSTypes.h` defines `WPE_Connect`, `WPE_Close`, `WPE_Login`, `WPE_Logout`.
No core consumer overrides `IParserSpi::handleEvent` (`WtCore/ParserAdapter`
has no override; only Trader adapters handle trader events), so no interface
change is needed. The two-stage contract maps onto the existing codes:

| Code | Meaning in both parsers |
| --- | --- |
| `WPE_Connect` | Transport established for one connection epoch. Xele: TCP snapshot fully synchronized (EOF on a complete batch) and the multicast socket bound, joined and its receive armed. UDP: broadcast and subscription sockets open, receive armed, subscription request dispatched (send completion is *not* awaited and not treated as confirmation). |
| `WPE_Login` | Confirmed: the first complete, recognized message from the feed in that epoch (Xele: correct version, known type, full frame inside the datagram; UDP: known message type with a full payload). Never emitted from local success alone (fail-closed). |
| `WPE_Close` | Unchanged: once per active connection, on `disconnect()` (queued) or a transport error. |

Each of Connect and Login is emitted at most once per epoch, Connect strictly
before Login and before that epoch's Close. A reconnect (explicit or automatic
recovery) starts a new epoch. A failed or stopped connect emits neither. Events
are serialized with all other SPI callbacks on the parser's I/O mutex; Connect
is queued on the worker strand, and the first confirmed frame emits a pending
Connect inline before Login. `disconnect()` now consumes the close token
immediately, so a queued Close can no longer be attributed to a later connect
(applies to both parsers).

The legacy test-only helper `ParserUDP::doOnConnected` (still reachable through
the coverage test peer) is unchanged; production code no longer calls either
parser's `doOnConnected`/`doOnDisconnected`.

## Targets

- `XeleCurrent`, `UdpCurrent`, `UdpCurrentFault` (`parsers/`, `fault/parsers/`):
  current sources; the fault module only adds `WT_PARSERUDP_TEST_HOOKS`.
- `XeleBefore`, `UdpBefore`, `UdpBeforeFault` (`before/...`): sources before this
  change, saved in `baseline/` (`baseline/SHA256SUMS`). The Xele baseline is the
  lifecycle-V2 version from `i03_parser_xele_lifecycle_v2`; the UDP baseline
  equals the hash recorded by `i03_parser_udp_lifecycle_v2/evidence/SHA256SUMS`.
- `i03_parser_events` / `i03_parser_events_before`: same `events.cpp` against
  current/before modules (green/red).
- `i03_xele_oracle_head`: the updated coverage tests
  `t_ParserXeleSkt/{t2_parserxele_lifecycle_test,r76_xele_boundary_test}.cpp`
  compiled against git HEAD (`ccaad010`) ParserXeleSkt source in
  `baseline_head/` (pre-card-9c), to show the rewritten oracles detect the
  old repeated-init behavior.

## Commands

```sh
cmake -S src/TestUnits/fixtures/i03_parser_event_contract_20261103 \
  -B /tmp/wt-i03-parser-events-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-events-20261103 -j2
(
  cd /tmp/wt-i03-parser-events-20261103
  LLVM_PROFILE_FILE=$PWD/events.profraw perl -e 'alarm 90; exec @ARGV' -- \
    ./i03_parser_events --gtest_output=xml:$PWD/events.xml > events.log 2>&1
  LLVM_PROFILE_FILE=/dev/null perl -e 'alarm 120; exec @ARGV' -- \
    ./i03_parser_events_before --gtest_output=xml:$PWD/events-before.xml > events-before.log 2>&1
  LLVM_PROFILE_FILE=/dev/null perl -e 'alarm 120; exec @ARGV' -- \
    ./i03_xele_oracle_head --gtest_output=xml:$PWD/oracle-head.xml > oracle-head.log 2>&1
)
```

ASan/UBSan and TSan: build directories `/tmp/wt-i03-parser-events-20261103-asan`
and `-tsan` with `-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++`
and the same sanitizer flags as `i03_parser_xele_lifecycle_v2/README.md`; build
`XeleCurrent UdpCurrent UdpCurrentFault i03_parser_events` with `-j2`; run
`./i03_parser_events` with `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1
UBSAN_OPTIONS=halt_on_error=1` or `TSAN_OPTIONS=halt_on_error=1`.

## Results (2026-11-03, this machine)

| Run | Result | Evidence |
| --- | --- | --- |
| `i03_parser_events` (current) | 13/13, exit 0 | `evidence/events.{log,xml}` |
| `i03_parser_events_before` (red) | 6 fail / 7 pass, exit 1 | `evidence/events-before.{log,xml}` |
| `i03_xele_oracle_head` (rewritten oracles vs HEAD source, red) | 2 fail / 31 pass, exit 1 | `evidence/oracle-head.{log,xml}` |
| ASan/UBSan `i03_parser_events` | 13/13 | `evidence/asan-events.{log,xml}` |
| TSan `i03_parser_events` | 13/13 | `evidence/tsan-events.{log,xml}` |
| repetition | normal x20, TSan x5, all pass | `evidence/repeat-summary.txt` |
| `i03_parser_udp_lifecycle_v2` rerun (own build dir) | v2 8/8, legacy filter 20/20, ASan 8/8, TSan 8/8 | `evidence/udp_lifecycle_rerun/` |
| `i03_parser_xele_lifecycle_v2` rerun | v2 11/11, scenarios 5/5, prepare 1/1, legacy 33/33; ASan and TSan v2/scenarios/prepare pass | `evidence/xele_lifecycle_rerun/` |

Red detail. Before this change, the six success-contract tests fail for both
parsers: no `WPE_Connect` after a successful connect, and no Login on the first
valid frame (`ConnectThenLogin...`, `InvalidFrames/MessagesDoNotConfirm`,
`ReconnectStartsNewEventEpoch`). The seven failure/stop-path tests pass on both
versions; they guard against emitting success events on failure, release or V2
stop, and the old code emitted none. On HEAD source, the rewritten oracles
`PrepareCommitsOnlyCompleteTcpSnapshotBatches` and
`SecondInitIsRejectedKeepsOriginalAndReleaseIsIdempotent` fail at their 9c
assertions (second init returns true). `FirstReconnectFailureLeavesRetryUnseeded`
was only restructured to a single init and passes on both versions.

The UDP lifecycle rerun used `/tmp/wt-i03-parser-udp-v2-rerun-20261103[-asan|-tsan]`
with the commands of `../i03_parser_udp_lifecycle_v2/README.md`; that fixture's
files were not modified.

## Coverage lanes (CLAUDE.md procedure)

`CoverageTests_t_ParserXeleSkt` and `CoverageTests_t_ParserUDP` were built with
`cmake -S src -B /tmp/wt-i03-cov-parsers-nowerror-20261103 -G Ninja
-DCMAKE_BUILD_TYPE=Debug -DWT_ENABLE_COVERAGE=ON -DWT_WARNINGS_AS_ERRORS=OFF
-DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBoost_INCLUDE_DIR=$(brew --prefix boost)/include`
and `cmake --build ... --target CoverageTests_t_ParserXeleSkt CoverageTests_t_ParserUDP -j 2`.
`-DWT_WARNINGS_AS_ERRORS=OFF` is a local workaround only: the default
(`-Werror`) configure `/tmp/wt-i03-cov-parsers-20261103` stopped in
`src/WTSTools/WTSDataFactory.cpp:1334` (`-Wunused-parameter`, `bIncludeOpen`),
an uncommitted change outside this work (sha256 `93c1bdc4…a7b7` at the time).
The changed parser TUs were separately checked with
`c++ -std=c++17 -fsyntax-only -Wall -Wextra -Werror`. The binaries were run
from a scratch cwd with `--gtest_output=xml:`.

- `CoverageTests_t_ParserXeleSkt`: 34/34 pass (+1 pre-existing DISABLED), exit 0.
- `CoverageTests_t_ParserUDP`: 43 pass, 2 fail, exit 1:
  - `t6_parserudp_init.wtcore_parser_adapter_init_ext_passes_null_and_releases_real_parser`
    fails identically with the pre-change UDP source (`i03_udp_legacy_before`).
    The UDP lifecycle README already records it as obsolete, because the adapter
    now retains initExt owners that lack V2 proof.
  - `ParserUDPShutdownTest.OldTwoChannelReadCompletionsBelongToOneReconnectEpoch`
    is intermittent and pre-existing: over 300 repeats it failed 63 times with
    the pre-change source and 42 times with the current source
    (`evidence/coverage_lanes/flaky-OldTwoChannel-*-x300.txt`). The failure is
    a second Close after one injected read error, while the real worker is
    also running. Not fixed here; it is reported as a new issue.

## Limits

- "Confirmed" means a structurally valid message from the feed. Neither
  protocol has an SDK-level login/ack, so this is not business authentication;
  for UDP, a broadcast datagram from any local sender with a valid type counts.
- The automatic receive-error recovery path (new epoch after a transport error)
  is implemented through the same helper but was not exercised for Xele (no
  local way to inject a UDP receive error). The UDP recovery tests in
  `t_ParserUDP` exercise Close cycles, not Login.
- The real Xele/UDP feed, Windows/MSVC, consumer behavior (no core consumer
  exists), and full regression were not run.
