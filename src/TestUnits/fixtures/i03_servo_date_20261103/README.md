# WtDtServo date-range failure propagation fixture

This fixture compiles the real `WtDtRunner`, `WtDataManager`, `WTSBaseDataMgr` and related source, then initializes the real runner with temporary metadata and a local dynamically loaded `IRdmDtReader` spy. The spy does not connect to a service. The fixture observes reader calls and the data manager's private cache; it never casts an arbitrary base-manager implementation to `WTSBaseDataMgr`. It confirms the runner's actual manager and data-manager sink pointers match.

A test-only executable-level `clock_gettime` interposer models a midnight rollover and clock failure in the calling thread. It verifies a zero-date request takes exactly one checked local snapshot, and that both boundaries correspond to that date. Other tests cover a valid day, invalid date, missing commodity/session/calendar, each single-sided range failure, inverted weekend boundaries, no reader calls or cache mutation for m1 and m2 failures, night and 24-hour sessions, missing/foreign dependencies, and the unchanged direct-range zero-end convention.

From the repository root, using an independent prebuilt tree with `WTSTools` and `WTSUtils` static libraries:

```sh
cmake -S src/TestUnits/fixtures/i03_servo_date_20261103 \
  -B /tmp/i03-servo-date-build-NEW -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DWT_BUILD=/tmp/i03-quarantine-build-20261103 -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-servo-date-build-NEW -j2
I03_DATE_SPY=/tmp/i03-servo-date-build-NEW/libDateSpy.dylib UBSAN_OPTIONS=halt_on_error=1 \
  /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python -c \
  'import os,subprocess; subprocess.run(["/tmp/i03-servo-date-build-NEW/servo_date"],cwd="/tmp/i03-servo-date-build-NEW",env=os.environ.copy(),timeout=30,check=True)'
```

The fixture and directly compiled production sources use UBSan; reused static dependencies and system libraries are not fully instrumented. This is a local targeted test, not a full-project regression, Windows test, real storage-reader validation or migration of other date consumers. The concrete manager normally returns a valid capability table; corrupted size/version/function-pointer table cases are checked in the implementation but are not injected in this fixture.
