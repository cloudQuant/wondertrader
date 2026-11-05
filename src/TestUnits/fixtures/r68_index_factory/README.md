# R68-003 index factory conflict probes

These fixtures exercise a local subcase, not the full factory, worker and
module shutdown contract. Two separately loaded modules deliberately expose
the same factory name. The current manager keeps the first factory, retains
the rejected candidate and its module, and rejects subsequent scans on that
manager. A candidate whose creator throws, or whose name is null, empty or
throws, is also quarantined. A synchronous `create_index_factory` reentry
probe checks that a nested scan is refused while creation is in progress.

On macOS arm64 with Homebrew Boost 1.88.0, GoogleTest 1.17.0, CMake and Ninja,
an isolated copy of `src` and `dist/common` was configured with
`WT_ENABLE_COVERAGE=ON`. In that copy only, the `dtcore` test group in
`src/TestUnits/coverage/CMakeLists.txt` was restricted from `dir:q05_dtsrv`
to `q05_dtsrv/r68_idxworker_factory_test.cpp`; the repository build file was
not changed for this test. The existing build is
`/tmp/wt-i03-r68-conflict-20261102/build`, using its snapshot directory.

To reproduce against that existing configured build, first copy the current
`IndexWorkerMgr.cpp`/`.h`, `r68_idxworker_factory_test.cpp`, and both fixture
files into the corresponding locations in the snapshot, then run:

```sh
PATH=/opt/homebrew/bin:/usr/bin:/bin /opt/homebrew/bin/cmake --build \
  /tmp/wt-i03-r68-conflict-20261102/build --target CoverageTests_dtcore -j 6
work=$(mktemp -d /tmp/wt-r68-review.XXXXXX)
cd "$work"
LLVM_PROFILE_FILE="$work/profile-%p.profraw" perl -e 'alarm 45; exec @ARGV' -- \
  /tmp/wt-i03-r68-conflict-20261102/build/build_x64/Debug/bin/TestUnits/CoverageTests_dtcore \
  '--gtest_filter=R68IndexWorkerFactoryTest.DifferentModulesWithSameFactoryNameSealWithoutReplacingOwner:R68IndexWorkerFactoryTest.RepeatedSameModuleDoesNotRecreateFactoryOrInvalidateWorker:R68IndexWorkerFactoryTest.BadFactoryNamesAndThrowingCreatorAreQuarantined:R68IndexWorkerFactoryTest.CreatorReentryCannotStartAnotherScan'
```

The final ordinary build passed 4/4 cases. Final source SHA-256 values were
`f371f0a75e03e94f242afb7d9e5f09aa69da7722abdf06c4d80af03fc1846601`
for `IndexWorkerMgr.cpp` and
`d8d1aa2b970a9e9d5408f15fe971516bd6528f3b169b3b37650f42e2ba172ac4`
for `IndexWorkerMgr.h`; both matched the compiled snapshot. With the `HEAD` manager source in
the same snapshot, the two-module conflict case failed (exit 1): duplicate
load returned success and further modules were loaded. With the interrupted
candidate that checked duplicates but did not validate names, the bad-name
case exited 139 when a fixture returned null. These are separate red controls,
not sanitizer results.

The test uses local fake modules; it does not establish real provider callback
drain, orderly worker/factory/module destruction, Windows behavior, concurrent
scans, allocation-failure recovery or reentry during the platform loader's
own constructors. A scan can still register earlier modules before finding a
later duplicate, so failure is not a transaction rollback. Quarantine is
limited to one candidate per manager but
repeatedly creating managers can still retain modules and objects indefinitely.
The manager layout changes; all binary consumers must be rebuilt. No ASan,
UBSan, TSan, full-tree or provider acceptance is claimed by this test.
