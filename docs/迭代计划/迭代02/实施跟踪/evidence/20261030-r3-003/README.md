# R3-003 targeted evidence (IN_PROGRESS)

Scope: `cancel(localid)` records a cancel timestamp only when `doCancel()` reports a non-negative synchronous API submission. The new risk-rejection case uses successful `TraderAdapter::init()` with isolated local `TraderMocker` and active risk configuration, then forces the already-established excluded-code short circuit. It retries a still-live order 100 times and checks that the existing one-entry cache does not grow. A separate P11 `FakeTraderApi` case returns `-1` synchronously and verifies no timestamp is added, then returns `0` as a positive control.

Build command:

```sh
/opt/homebrew/bin/cmake --build build-cov --target WtCore CoverageTests_wtcore -j4
```

Build exit status: 0. See `build.log`.

Targeted test commands and results:

- `CoverageTests_wtcore --gtest_filter=R3TraderTest.cancel_localid_risk_rejection_retries_do_not_grow_cache --gtest_output=xml:/tmp/wt-r3-003/evidence/risk-rejection.xml` — passed; see `risk-rejection.log` and XML.
- `CoverageTests_wtcore --gtest_filter=R3TraderTest.cancel_localid_synchronous_api_failure_is_not_cached --gtest_output=xml:/tmp/wt-r3-003/evidence/api-failure.xml` — passed; see `api-failure.log` and XML.
- `CoverageTests_wtcore --gtest_filter=R3TraderTest.cancel_localid_frequency_cache_uses_standardized_contract_code --gtest_output=xml:/tmp/wt-r3-003/evidence/standard-code-regression.xml` — passed; see `standard-code-regression.log` and XML.

This evidence does not establish a global bound for timestamps from successfully submitted cancellations or resolve zero-boundary, window-endpoint, or wall-clock rollback semantics. No such semantics were changed or assumed. Concurrent/reentrant API callback behavior and cache synchronization remain unverified. R3-003 remains IN_PROGRESS; no acceptance claim is made.
