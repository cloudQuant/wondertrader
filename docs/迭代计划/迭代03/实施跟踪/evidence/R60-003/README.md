# BUG-R60-003 same-instance lifecycle: focused reproductions, not a fix

Workspace: `/Users/yunjinqi/Documents/new_projects/wondertrader`, macOS arm64. Product files were not edited during this task. The only workspace edit is three new tests in `src/TestUnits/coverage/t_TraderMocker/r60_tradermocker_async_test.cpp`. Original dirty source/test copies and hashes were retained in `baseline/` and `baseline.sha256`; `source-before-final.sha256` and `source-after-final.sha256` match. Output and physical Boost bridge contain no new symlink (`symlinks.log`).

Build current product/test translation units via:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-r60-lifecycle/build.py
```

`commands.json` has exact compile/link argv and working directory. Each TU was recompiled using the entity Boost bridge in `/tmp/ctpm-i03-build`; other static/dynamic libraries came from that earlier build, whose complete inputs were not revalidated. A signedness warning in the first version of the added test caused a `-Werror` build failure; its command/output are in `commands-initial-failure.json` and `test-build-initial-failure.log`, and the test variable was corrected before all final runs. No product warning was suppressed.

Each selected test was run in a distinct temp cwd via `perl -e 'alarm 30; exec @ARGV' -- /tmp/wt-i03-r60-lifecycle/focused --gtest_filter=R60MockerTest.<name> --gtest_output=xml:<cwd>/result.xml`, with an isolated LLVM profile path. `final-results.log` and `final-runs/<name>/` retain statuses, logs and XML:

- `same_instance_reinitializes_and_delivers_each_connect`: **FAIL**, exit 1: first init/connect sends WTE_Connect, release frees the UDP port, second init/connect on the *same instance* binds but no second connect event is delivered within 3 s. It checks process FD stability after the first warmed cycle and return to baseline after destruction. io_context retains three internal descriptors between release and object destruction; no FD mismatch was reported.
- `released_instance_does_not_deliver_old_queued_query_after_reinit`: **FAIL**, exit 1, because the second connect event never arrives in the current implementation. The old query was posted after release, not while a callback was actively executing; this is a public API call that presently returns 0 while stopped, and its intended return contract remains to be settled.
- `release_reentered_from_connect_callback_does_not_terminate`: **FAIL**, exit 1: a fork-isolated fake SPI synchronously calls `release()` from WTE_Connect on the io worker. The child's custom terminate handler exits 88. The child has an 8-second alarm; no real provider was contacted.
- `release_and_destructor_free_bound_udp_port`: **PASS**, exit 0: existing single-lifecycle port cleanup control.

## Why a minimal restart is insufficient

`make-naive.py` creates `/tmp/wt-i03-r60-lifecycle/naive.cpp` from the saved original product copy, changing only `connect()` to call `_io_service.restart()` when stopped and reset `_terminated=false`; no workspace product was changed. `build-naive.py` recompiles this variant and current test using the same linked dependencies. In `naive-run/output-final.log` / `result-final.xml`, `same_instance...` **PASS** but `released_instance_does_not_deliver_old_queued_query_after_reinit` **FAIL**: the query posted after release executes in the next lifecycle and invokes `onRspAccount` once. This deterministic counterexample demonstrates that restarting the context alone revives queued operations; it does not reproduce an operation posted before release or establish a general ordering guarantee for all calls.

`orderInsert` and `orderAction` retain raw WTSEntrust/WTSEntrustAction pointers before posting and release them only inside their handlers. A generation check that merely skips an old handler would leak those references, while unconditionally executing them can place or cancel stale orders. `release` can be synchronously reentered on the worker, as tested; returning before the worker is quiescent could allow a caller to destroy the object while handlers still capture `this`. A correct fix requires an agreed owner/lifetime and callback quiescence protocol, generation-scoped admission and cleanup of retained work, serialized connect/release, and a defined treatment of positions/orders across re-init. `_terminated` and `_io_service` currently do not supply these guarantees. No detach, unsafe restart or suppression has been committed.

All results are local fake SPI/UDP tests on macOS. No real broker or provider, sanitizer, cross-platform execution, full tree rebuild, or overall regression was performed. The GTest failures above remain intentionally failing pending the full fix; they are not counted as passes.
