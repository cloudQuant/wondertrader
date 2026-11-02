# T3-012 current-source targeted red controls

Repository product and two pre-existing test files were verified against the recorded iteration-03 start worktree snapshot, preserving all prior dirty changes. `sha256.txt` records their current hashes. Candidate edits made during this investigation were reverted; no product/test code was changed.

`probe.cpp` creates an independently configured empty `Dumper`, sets the existing `WT_DUMPER_TEST_HOOKS` wait handshake, and only proceeds once the worker has entered the wait. `build.py` derives the link command from the existing v8 target without modifying the build; `rebuild_current.py` additionally recompiles the current worktree `Dumper.cpp` with the target's real Boost header bridge, then links the probe with that object and the remaining prebuilt v8 dependencies. Both commands, with arguments, are recorded in `current-commands.json`. Invocation:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-t3-012/build.py
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-t3-012/rebuild_current.py
(cd /tmp/wt-i03-t3-012/current-control && LLVM_PROFILE_FILE=profile.profraw perl -e 'alarm 10; exec @ARGV' -- /tmp/wt-i03-t3-012/probe-current control)
```

Run control, repeat_release, duplicate_run in separate independent directories with the same command and appropriate mode. `current-results.log` records results: control 0; repeat_release catches `std::system_error` from a second non-joinable `join` and exits 7; duplicate_run destroys a still-joinable thread and invokes the probe's `std::terminate` handler, exiting 88. All three confirm the worker reached its waiting point. Exit 88 is an explicit test terminate handler, not SIGABRT.

This uses current Dumper.cpp and header, but reuses v8 TraderAdapter, WtHelper and other linked objects/libraries. It is local fake/empty-manager evidence, not the full worktree, actual provider, shutdown drain, or Windows evidence.

An attempted lifetime state machine was not retained: a `void release()` that returns while startup or a prior join remains pending cannot give the caller an observable guarantee that owner destruction is safe, but waiting in a reentrant callback may deadlock. Safe full handling requires a provable owner/lifecycle protocol and potentially a compatible observable drain interface; provider callbacks and concurrent teardown remain BLOCKED.
