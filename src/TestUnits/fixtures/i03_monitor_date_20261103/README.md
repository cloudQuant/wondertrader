# StateMonitor initialization and data-runner gate

The fixture compiles the real `StateMonitor`, `WTSBaseDataMgr` and `WtDtPorter` runner with a local fake parser shared library. A test-only calling-thread `clock_gettime` interposer supplies a deterministic local UTC clock or failure; it does not alter the production interfaces. The active-caster failure checks do not open sockets or create shared memory. The actual runner tests exercise missing-calendar, offset-date overflow and clock failure before caster/writer/parser initialization, and verify no fake parser was created or connected and no UDP thread or shared-memory file was created.

The tests also verify one clock snapshot, weekday/holiday, weekend/holiday, night and 24-hour sessions, shared-template ordering, previous state/manager preservation, and a valid commodity staged before a later missing-calendar commodity fails. A separate real manager checks that pure weekend `nextTradingDate(pid,...,false)` agrees with `checkedGetTradingDate(pid,...,false)` for the configured calendar. A valid-state runner and all-day bypass are gate controls; their intentionally absent writer is not evidence of a working writer or completed startup. The existing Q05 test fixture was corrected to supply valid session metadata under the current all-or-nothing `loadSessions` contract and to expect no partial monitor state after a later session lookup exception.

Build in an independent directory, using an existing build providing WtDtCore, WTSTools and WTSUtils static libraries:

```sh
cmake -S src/TestUnits/fixtures/i03_monitor_date_20261103 \
  -B /tmp/i03-monitor-date-build-NEW -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DWT_BUILD=/tmp/i03-quarantine-build-20261103 -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-monitor-date-build-NEW -j2
I03_MONITOR_SPY=/tmp/i03-monitor-date-build-NEW/libMonitorSpy.dylib UBSAN_OPTIONS=halt_on_error=1 \
  /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python -c \
  'import os,subprocess; subprocess.run(["/tmp/i03-monitor-date-build-NEW/monitor_date"],cwd="/tmp/i03-monitor-date-build-NEW",env=os.environ.copy(),timeout=30,check=True)'
UBSAN_OPTIONS=halt_on_error=1 /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python -c \
  'import os,subprocess; subprocess.run(["/tmp/i03-monitor-date-build-NEW/q05_monitor_control","--gtest_filter=Q05StateMonitorTest.*"],cwd="/tmp/i03-monitor-date-build-NEW",env=os.environ.copy(),timeout=30,check=True)'
```

Both executables directly compile the changed monitor and base-data manager; the runner test directly compiles the changed data runner. Those sources and test code use UBSan, but reused static dependencies and system libraries are not fully instrumented. Before the commit loop, all dates and states are staged without changing the shared calendar cache. Existing `setTradingDate` has no rollback/noexcept/transaction guarantee: an exception during the commit loop can leave a subset of valid dates written, and concurrent initialization or concurrent calendar mutation is not atomic or supported. Neither condition is fault-injected here. Unknown session IDs retain the existing skip behavior. `advanceOnce`, other date consumers, actual providers, Windows and full regression are outside this fixture.
