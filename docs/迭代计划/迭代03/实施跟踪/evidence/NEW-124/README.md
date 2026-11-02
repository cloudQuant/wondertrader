# BUG-I02-NEW-124: synchronous fault fixture

Scope: macOS arm64, isolated process tests only. The change is limited to `src/TestUnits/coverage/q06_porter/v2_exec_status_api_test.cpp`. No production signal code was changed and no symlinks were created. The build uses the existing v8 target configuration and other prebuilt objects/libraries; this is not a full current-tree clean build. The test, WtRtRunner.cpp and WtPorter.cpp were independently recompiled from the current workspace. The Boost bridge is a physical directory, with no symlinks under it, and the dependency files show Boost headers selected from that directory; normal system libraries remain in use.

## Cause and correction

Before modification, the current source run of `SynchronousRunFailsFastOnSynchronousSegmentationFault` reached both existing ready and post-fault exit assertions; its sole failure was `WEXITSTATUS == 10` versus expected `SIGSEGV == 11`. That exit status by itself is ambiguous because configuration failure also uses exit 10, but the passed RUNNING handshake rules out that failure in this current run. A separate probe in the modified fixture resets both fault handlers to defaults, maps the same PROT_NONE page and reports reaching the write. It terminates by signal 10, macOS SIGBUS, as recorded in the fixed XML property `fault_probe_signal`. Therefore this platform delivers SIGBUS for this fault, and an unconditional SIGSEGV expectation is wrong here. The historical log alone may not establish whether its run-ready assertions existed or passed; this diagnosis is for the present source and run.

The fixture now checks that the probe genuinely dies from SIGBUS or SIGSEGV, requires the runner to report RUNNING (`R`) and completion of PROT_NONE mapping immediately before the fault (`F`), and requires normal exit with the probe's exact signal number and within the existing 3-second bounds. Setup failure or no fault cannot count as success. SIGTERM and SIGINT controls are unchanged.

## Commands and results

All scripts use the user's Anaconda base Python. Run from repository root:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new124/build.py
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new124/run-fixed.py
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-new124/red.py
```

The build script obtains exact compile/link argv from Ninja; `commands.json`, `build-0.log` through `build-3.log`, and `hashes.json` retain commands, output and input hashes. Compile/link cwd is `/tmp/wt-i02-fresh-20261101-v8-build`. `baseline-commands.json`, `baseline-hashes.json`, `baseline-build-*.log`, `baseline-results.json` and `baseline-*` case logs/XML preserve the original red and positive controls. Each test gets an independent cwd and TMPDIR and 30-second process timeout. `fixed-results.json` includes exact invocations and exit codes:

- `SynchronousRunFailsFastOnSynchronousSegmentationFault`: 1/1 PASS, XML reports actual fault signal 10;
- `SynchronousRunExitsOnSigterm`: 1/1 PASS;
- `SynchronousRunExitsOnSigint`: 1/1 PASS.

A test-only negative-control copy replaces the runner's actual faulting store after `F` with an ordinary exit(20); the probe is untouched. Its single case fails as expected (`expected_signal = 10`, observed exit 20). Exact compilation and invocation are `red-commands.json` and `red-result.json`, and output/XML are in `red-run/`. The copy is outside the workspace and is not a production change.

`git -c core.whitespace=cr-at-eol diff --check -- src/TestUnits/coverage/q06_porter/v2_exec_status_api_test.cpp` passed. Input hashes matched before and after build. No sanitizers, non-macOS platforms, real provider callbacks, whole-tree builds or whole-suite regressions were run. This is a test fixture correction and local evidence, not a claim of production signal safety.
