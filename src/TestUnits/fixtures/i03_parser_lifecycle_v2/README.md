# Parser lifecycle V2 local fixture

From the repository root on the macOS environment used for these checks:

```sh
cmake -S src/TestUnits/fixtures/i03_parser_lifecycle_v2 \
  -B /tmp/wt-i03-parser-v2-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-parser-v2-20261103 -j2
(
  cd /tmp/wt-i03-parser-v2-20261103
  LLVM_PROFILE_FILE=/tmp/wt-i03-parser-v2-20261103/profile.profraw \
    ./i03_parser_v2 \
    --gtest_output=xml:/tmp/wt-i03-parser-v2-20261103/results.xml \
    > /tmp/wt-i03-parser-v2-20261103/results.log 2>&1
)
```

The target directly compiles the current `ParserAdapter.cpp` and creates real
shared modules in the build directory's `parsers/` folder. `WT_BUILD` supplies
previously built `libWtCore.a`, `libWTSTools.a`, and `libWTSUtils.a`; those
supporting static libraries are not rebuilt by this fixture. The CMake file
uses the machine's Homebrew Boost/GTest and iconv paths. `baseline/` holds the
pre-change source snapshots; `evidence/` contains results, hashes and the
incremental source diff. The fixture has no live providers or market traffic.

Dynamic providers without a valid V2 table are now rejected before
`createParser`; they must migrate to the V2 contract before they can be used
by this adapter. This test does not establish that existing providers have
migrated or that all are compatible. Legacy `initExt` can still initialize an
API, but the adapter has no V2 drain proof and retains it without destroying
it. Duplicate contended submissions can be recorded more than once; none of
these legacy retained pointers is released or destroyed by the adapter.

Adapter `requestStopLifecycle()` and `finishReleaseLifecycle()` are separate
nonblocking calls returning success as `true`. Manager methods
`requestStopLifecycleAll()` and `finishReleaseLifecycleAll()` implement the
corresponding phases; `release()` performs both phases. `false` or a remaining
quarantined adapter means the caller must retain the owner and borrowed graph.
The fixture covers retryable PENDING, unsafe waiting thread, and synchronous
reentry; it does not validate concurrent manager-map mutation, real providers,
full graph ownership or the Runner. Existing external synchronization of the
manager and graph remains required. In particular, the current Runner destructor
may still terminate if its graph cannot be safely drained; it is not made safe
by this adapter-only change. The caller remains responsible for preserving the
entire owner graph and module lifetime until teardown is proven complete.

## Final acceptance (2026-11-05)

`ParserV2.ExternalWithoutV2IsNotDestroyed` followed the pre-decision behavior (zero
`release()` calls). Decision K15=A (2026-11-04): an `initExt` API belongs to its caller and
has no V2 drain proof; an explicit release notifies it to stop exactly once, which is not
quiescence, and the adapter never destroys it. The case now asserts exactly one
`release()` call (also after a second stop request), `hasApi()` and no destruction.
