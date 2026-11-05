# Data-kit parser host lifecycle (iteration 03, card 9) — s5 dt-parsers

Local fake only: no vendor SDK, front, network or market data is used.

Covers the host side of decision card 9 (9a=A+C, 9b=A, 9c=A, 9d=A) for the
`WtDtCore` and `WtDtServo` parser adapters, their managers and the WtDtServo
runner (BUG-R18-006, BUG-R18-012, BUG-R8-006, BUG-I02-NEW-009):

- `fake_provider.cpp` — a parser provider module with its own callback thread.
  Its legacy `release()` does not stop the thread (release is not a drain
  barrier); its legacy `deleteParser` refuses and counts deletion while the
  thread runs. The V2 table stops the thread (`request_stop`), reports
  `WOULD_DEADLOCK` on its own thread, joins in `wait_drained` and destroys in
  `destroy_api`. Built twice: `libI03DtFakeV2` and `libI03DtFakeLegacy`
  (`FAKE_NO_V2`, no getter).
- `dtcore_host_test.cpp` / `servo_host_test.cpp` — lifecycle matrix: legacy
  module rejected before `createParser`; failed provider init (false/throw)
  rolled back through V2 and config refcount back to baseline; success path
  drains before destroy; repeated init rejected (9c); release from the provider
  thread deferred to a safe point (9d); manager teardown; uncertain drain
  quarantined and never retried; pending drain retried; destructor drain;
  WtDtServo runner destruction/shutdown and failed-parser handling.
  Cases outside `WT_HOST_NEW` use only APIs common to the pre-change sources.
- Baselines (`*_base`) compile the pre-change copies passed as `WT_BEFORE`
  (red controls; they fail or crash with the provider thread calling a freed
  adapter).

```sh
src/TestUnits/fixtures/i03_dt_parser_host_20261104s5/run.sh \
  /tmp/wt-i03-s5-dt-parsers-cov /tmp/<new-build-dir> \
  docs/迭代计划/迭代03/实施跟踪/evidence/20261104-s5-dt-parsers/before [thread]
```

The quarantine cases deliberately leak the quarantined API and its thread
(process-lifetime owner); run TSan with `report_thread_leaks=0`. The archives
from the coverage build are not sanitizer-instrumented. This proves only the
local fake contract; real provider callback drain remains external (X).

Added later the same day:

- `dtporter_host_test.cpp` — real `WtDtPorter::WtDtRunner` with a running
  StateMonitor: destruction stops parsers before members (common case),
  `shutdown()` order/idempotence, an unprovable parser keeps the graph, and
  I03-NEW-047 (`fake_writer.cpp` flushes through registered external dumpers
  inside `release()`; the pre-change runner frees the dumpers first, which ASan
  reports as heap-use-after-free).
- `servo_host_test.cpp` — I03-NEW-044 (a repeated `start()` must not reconnect)
  and BUG-R123-003 (an in-flight runner callback during destruction).
- `fake_writer.cpp` + `monitor_writer_probe.cpp` — StateMonitor writer-callback
  modes (`self`, `self-external-join`, `exception`) against the current
  `DataManager` writer host; usage `monitor_writer_probe <mode> <build-dir>`.
- Sanitizers: configure with `-DWT_SANITIZE=address,undefined` or `thread` and
  the Homebrew LLVM compiler (see `run.sh`); quarantine cases leak by design.

## 2026-11-05 最终验收复核（slice2-result）

`run.sh` 普通变体在 macOS `/bin/bash` 3.2 下于 `cmake ... "${extra[@]}"` 中止（`set -u` 下空数组报 unbound variable），从未进入构建。改为 `${extra[@]+"${extra[@]}"}`（与同目录其他夹具一致）后，普通与 thread 变体在最终树上均通过（各 6/6）。baseline 红控需要改前副本（`before-dir`），最终验收按空串跳过。
