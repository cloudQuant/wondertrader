# Iteration 03: date mapping V1

From the repository root on the fixture's macOS environment:

```sh
cmake -S src/TestUnits/fixtures/i03_date_mapping_20261103 \
  -B /tmp/wt-i03-date-mapping-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-date-mapping-20261103 -j2
LLVM_PROFILE_FILE=/tmp/wt-i03-date-mapping-20261103/profile.profraw \
  /tmp/wt-i03-date-mapping-20261103/i03_date_mapping \
  --gtest_output=xml:/tmp/wt-i03-date-mapping-20261103/results.xml \
  > /tmp/wt-i03-date-mapping-20261103/results.log
```

The target compiles the current `WTSBaseDataMgr.cpp` and uses current headers;
`WT_BUILD` supplies previously built WTSTools/WTSUtils static dependencies. It
requires the paths in the CMake file (Homebrew Boost and GTest, and iconv).
The dependency libraries are not independently rebuilt by this fixture.
`baseline/` holds the before snapshots and hashes; `evidence/` holds the
captured test results and hashes of tested current sources.

The interface is explicitly obtained from a concrete `WTSBaseDataMgr`; it is
not automatically negotiated from an arbitrary `IBaseDataMgr`. The manager
must outlive every call, and callers must not replace its context. Pass the V1
structure itself to its function pointers. Each function rejects a version or
size mismatch before using context. Failures always have value zero and must be
interpreted by status; zero is not a status. Legacy methods retain their prior
fallback/sentinel limitations and have not become checked APIs.

For calc, date 0 obtains one checked local date/time snapshot and replaces the
provided time; for boundary, date 0 uses one checked snapshot's date. For
getTradingDate, date 0 first returns a valid cached date if present, otherwise
uses one checked snapshot's date. Its time parameter is validated but, as in
the legacy method, otherwise ignored. Next/previous operations require an
explicit date and a positive day count. In common with legacy business logic,
a weekday holiday itself is not automatically moved by calc/getTradingDate;
weekend and requested next/previous transitions skip holidays. An empty
session has no usable mapping and returns MissingSession. Gregorian date range
is 1000-01-01 through 9999-12-31. No new consumer is migrated by this fixture.

The deterministic fixture covers local functionality and linked legacy
comparison only; it does not simulate clock failure or internal exceptions and
is not a full regression, provider test, deployment test, or real trade.
