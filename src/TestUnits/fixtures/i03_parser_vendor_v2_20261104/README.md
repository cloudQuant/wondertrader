# Vendor parser lifecycle V2 local evidence (2026-11-04, iteration 03, card 9)

Scope: `ParserCTP`, `ParserCTPMini`, `ParserCTPOpt`, `ParserFemas` and
`ParserXTP` export `getParserLifecycleApiV2` (`Includes/IParserLifecycleV2.h`)
under decision card 9 (9a=A+C, 9b=A, 9c=A, 9d=A). The shared implementation is
`src/ParserCTP/VendorMdLifecycleV2.hpp` (header-only, included by all five).
`src/WtCore` (including `ParserAdapter.cpp`) and `src/Includes` were not
changed. `baseline/` holds the pre-change working-tree sources (card-3
`ActionDayNormalizer` and card-14 `subbatch` changes included; git HEAD
`ccaad010`) with `SHA256SUMS`; `evidence/*.diff` are the increments.

## Capability reported, and why

Every vendor SDK callback and every provider-posted task (XTP worker) enters a
callback admission gate; every host `IParserApi` call enters an API gate.
`request_stop` closes both with lock-free atomics. `wait_drained` waits for
both gates to be empty. That proves no `IParserSpi` call and no use of host
objects can start or still be running from this parser.

It does not prove that SDK-owned threads are gone. The vendored headers give no
release/join guarantee:

| Parser | Header (lines) | What it documents |
| --- | --- | --- |
| CTP | `API/CTP6.6.9/ThostFtdcMdApi.h:93-103,127-129` (`API/CTP6.3.15/...:90-100,124-126`) | `Release` "删除接口对象本身"; `Join` "等待接口线程结束运行"; `RegisterSpi` "注册回调接口" |
| CTPMini | `API/CTPMini1.5.8/ThostFtdcMdApi.h:90-100,113-115` | same text |
| CTPOpt | `API/CTPOpt3.5.8/ThostFtdcMdApi.h:92-102,126-128` | same text |
| Femas | `API/Femas3.02/USTPFtdcMduserApi.h:98-108,141-143` | same text |
| XTP | `API/XTP2.2.32.2/xtp_quote_api.h:234-236,258-260,390-393` | `Release` "删除接口对象本身"; `RegisterSpi` "请在登录之前设定"; `Logout` "同步阻塞式" (nothing about later callbacks) |

None of them says that `Release()` joins the callback threads or that no
callback can start after it returns, and `Join()` cannot be used after
`Release()` because the object is gone. So the parsers report:

- **DRAINED** (and `destroy_api` deletes the parser) only if the instance never
  created an SDK API object: never initialized, or initialization failed before
  the creator returned an object (missing module or creator symbol, creator
  returned NULL).
- **ERROR** ("drain state uncertain") once an SDK API object has existed. First
  the gates must be empty (PENDING until then). Then, on the waiting thread
  that `can_wait` has checked, the parser calls `RegisterSpi(NULL)` +
  `Release()` once (XTP also joins its own worker), and still returns ERROR.
  The real `ParserAdapter` then keeps the handle, module and borrowed objects
  (`_release_uncertain`). That is quarantine until process exit (9a).
  `destroy_api` returns ERROR and leaves the handle as it is.
- The vendor library is never unloaded once an SDK object existed. This is a
  change for Femas (`disconnect` used to `free_library`) and CTPOpt (`release`
  used to `unloadApiLibrary`).

Legacy paths:

- `release()`/`disconnect()` called inside one of the parser's own callbacks,
  inside an API call, or (XTP) on its worker only close callback admission.
  They defer the vendor release to the next lifecycle call from a safe thread,
  and `disconnect` returns false there (9d). On a safe thread they wait for
  admitted callbacks and API calls to leave before the vendor release.
- `connect()` re-entered from a callback (or the XTP worker) returns busy/false (9d).
- A second `init` is rejected and the original is kept (9c). `init`/`connect`
  are rejected after shutdown, and the CTP-family SDK `Init()` is issued only once.
- `deleteParser` deletes only a parser that never created an SDK object. In any
  other case it keeps the pointer (does not clear it) so that the host
  quarantines it (9b: there is no SDK thread-exit confirmation).
- A direct `delete` from inside the parser's own frame calls `std::terminate`
  (fail-stop). On other threads the destructor releases the SDK first.
- XTP: the posted login-event handlers check the gate and `m_sink` again. A
  sink unregistered after posting is no longer dereferenced (product half of
  BUG-T27-004). The relogin wait is interruptible.

## Fixture

For each family, `CMakeLists.txt` builds the following:

- the current module (`parsers/`) and the baseline module (`baseline/parsers/`);
- a local fake vendor SDK next to each module (`fake_ctp_family.cpp`,
  `fake_femas.cpp`, `fake_xtp.cpp`). The fake runs an SDK-owned callback thread,
  and its `Release()` joins that thread. A `Release()` on the SDK thread itself
  (which a real SDK would deadlock on) is counted and refused. The fake can
  also deliver one late callback through the stale SPI pointer;
- `scenarios_<f>_cur` and `scenarios_<f>_base`, legacy API only (9 cases);
- `v2_<f>`, which checks the V2 table and runs the real `ParserAdapter`/`ParserAdapterMgr`
  (11 cases), and `v2_<f>_base`, its red control run with cwd=`baseline`.

```sh
cmake -S src/TestUnits/fixtures/i03_parser_vendor_v2_20261104 \
  -B /tmp/wt-i03-parser-vendor-v2-20261104 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-vendor-v2-20261104 -j4
src/TestUnits/fixtures/i03_parser_vendor_v2_20261104/run.sh \
  /tmp/wt-i03-parser-vendor-v2-20261104 /tmp/wt-i03-parser-vendor-v2-20261104/out all
```

For the sanitizers, use `-asan` or `-tsan` build directories with
`-DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++` and
`-DCMAKE_{CXX_FLAGS,SHARED_LINKER_FLAGS,EXE_LINKER_FLAGS}` set to
`-fsanitize=address,undefined` or `-fsanitize=thread` (add
`-fno-omit-frame-pointer -include stdlib.h` to the C++ flags). Build only the
`scenarios_<f>_cur` and `v2_<f>` targets, then run
`run.sh <dir> <out> green` with
`ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1`
or `TSAN_OPTIONS=halt_on_error=1`. Leak detection is off on purpose, because
quarantined parsers are leaked by design. `WT_BUILD` provides prebuilt,
coverage-instrumented WtCore/WTSTools/WTSUtils archives that are not
sanitizer-instrumented.

Results, logs and XML are in `evidence/` (see `evidence/RESULTS.md`).

## Limits

- The real SDK callback drain is unverified (X). Only local fakes were used: no
  vendor library, front or exchange was loaded or contacted. Whatever the real
  SDK does inside `Release()` is outside this evidence. The parsers treat it as
  unprovable and report it that way.
- macOS dyld never unmaps an image that has thread-local variables. Every parser
  here has them, the baseline included (`write_log` buffers). So "module
  unloaded" cannot be observed on this machine. The adapter test records it as
  a property and does not assert it.
- Waits are unbounded:
  - A legacy release on a host thread waits without limit for an admitted
    callback. If that callback blocks on the releasing thread, the result is a
    deadlock (the inherent drain contract).
  - In V2, `wait_drained(0)` performs a one-time blocking vendor `Release()`
    (and the XTP worker join) on the waiting thread. `timeout_ms` does not bound it.
- `registerSpi` must not race SDK callbacks. The host calls it before `init`,
  as `ParserAdapter` does. The pointer swap is not synchronized against callbacks.
- Not covered: the `ParserAdapter` destructor on an adapter that was never
  stopped (it does not request stop, see the report), the WtDtServo/WtDtCore
  legacy adapters, WtDtRunner/WtRunner whole-graph teardown, Windows/MSVC,
  production deployment and full regression.
