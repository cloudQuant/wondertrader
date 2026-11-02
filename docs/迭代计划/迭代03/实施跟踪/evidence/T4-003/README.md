# TraderFemas BUG-T4-003 isolated fake probes

Scope: current workspace TraderFemas source, isolated fake API, no real provider and no repository edits by this probe. These are controlled failure demonstrations, not acceptance and not proof of real SDK callback/drain semantics. Other test and product changes present in the workspace are outside this probe's ownership.

Build from repository root using the user's conda base Python:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-t4-003-femas/build.py
```

`build.py` derives flags and link dependencies from the existing `/tmp/wt-i02-fresh-20261101-v8-build` Ninja build. `commands.json` records each exact argument vector, working directory and exit status; all three compilation/link steps exited 0. Logs are `build-0.log`, `build-1.log`, `build-2.log`.

Run each mode independently with a 10-second process limit (directories already exist):

```
for mode in worker idlequery inflight; do
  runpath="/tmp/wt-i03-t4-003-femas/$mode"
  (cd "$runpath" && LLVM_PROFILE_FILE="$runpath/profile-final.profraw" perl -e 'alarm 10; exec @ARGV' -- /tmp/wt-i03-t4-003-femas/probe "$mode" "$runpath/") > "$runpath/output-final.log" 2>&1
  result=$?; printf '%s exit=%s\n' "$mode" "$result"
done
```

- `worker`, exit 86: after `release`, worker remained joinable; deletion invoked a custom terminate handler (86) through destruction of the joinable thread.
- `idlequery`, exit 0: after 100 ms idle, `queryAccount` returned 0; fake request did not enter within 300 ms. The worker lacks a persistent work guard and repeatedly invokes `run_one` after the context can stop; the timed observation does not establish scheduler-independent latency.
- `inflight`, exit 0: fake `Init` synchronously queues a query; the fake request blocks at a test barrier. `release` returned and called fake `Release` before the request completed; the request then continued. The fake remains alive deliberately and does not model actual provider destruction or callback-drain guarantees.

No ASan/TSan, real provider, comprehensive concurrency verification or full regression was performed. The fake query state setup is an intentional controlled hook, not a full simulated login. A correct fix also needs owner-side semantics for callbacks and in-flight operations, shutdown admission, external coordination of worker/reentrancy, and safe join before deletion/module unload.

SHA-256 of input and executable at time of run:

```
9d0ff837936eb23755855f794fb82e4b829a7955384aa70a8941116479175ade  src/TraderFemas/TraderFemas.cpp
f40375e78aa451180e4dae79b932631d36e8fa7ed607ef4edb1b597524e1107d  src/TraderFemas/TraderFemas.h
f200898e07cf3805b32dc2c561170a6361a0f76f04f1ad4ba3fc71718f945950  src/TestUnits/coverage/t_TraderFemas/r93_femas_api_stub.h
1a2427dd488f4e44a6e49fb5a60b0e05da02ccdd54001b45b286ca896931b288  /tmp/wt-i03-t4-003-femas/probe.cpp
4c951104b9e217e3bb3d90b0f7842c7a39338f589c1e7d4975d36bf7e53e01a2  /tmp/wt-i03-t4-003-femas/probe
```
