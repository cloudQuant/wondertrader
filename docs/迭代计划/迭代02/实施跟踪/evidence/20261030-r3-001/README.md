# R3-001 evidence — IN_PROGRESS

This folder is an evidence snapshot, not acceptance. The local TraderMocker is installed under a temporary `traders/` directory; its config uses `udp_port:0`, and only synthetic orders are created. No login, broker credential, external network request, or real order is used.

## Current stable retest

Build Attempt D in `build-attempts.log` rebuilt the current posix_spawn-based test helper successfully. The parent test process launches a fresh filtered `CoverageTests_wtcore` process with `posix_spawn`; the child process executes the scenario directly, without a second spawn. The helper retains a 30-second child timeout, reports the true exit/signal status, and cleans PID-scoped mocker directories after the child exits.

- Order key test: 5 consecutive serial runs passed. Logs/XML: `order-key-posix-1` through `order-key-posix-5`.
- Cancel localid key test: 3 consecutive serial runs passed. Logs/XML: `cancel-key-posix-1` through `cancel-key-posix-3`.
- Inactive/unmapped localid safety test: 3 consecutive serial runs passed. Logs/XML: `cancel-safe-posix-1` through `cancel-safe-posix-3`.

The order key control compares the same standard contract and policy with independently fresh adapters; the caller prechecks the standard code while the API alt code changes between standardized and raw. The cancellation comparison waits up to 2 seconds for actual mocker order callbacks before cancelling.

## Preserved earlier failures

`order-key.log/xml` records SIGABRT 134 during concurrent independent processes; `order-key-serial.log/xml` records one later serial SIGABRT 134; `order-key-retry.log/xml` records the next serial pass under the older fork-based helper. The fork-based child crash cause was not proven. Replacing direct fork execution with `posix_spawn`/exec removed the flake in 11 consecutive serial scenario runs; this does not prove the old crash's exact cause.

Older `order-key-v2.xml` and `order-key-v3.xml` preserve invalid risk fixtures with the `module` field missing. An earlier manual-order cancellation case exited 139 because the adapter was seeded with a fake order absent from TraderMocker's internal `_awaits`; the test was changed to create local mocker orders. Those failures are test-setup evidence, not product evidence.

The mocker plugin itself leaves its UDP socket object allocated after `release()`. Each live-plugin scenario runs in its own process; process exit reclaims the local ephemeral socket. The test does not claim production-plugin or real broker lifecycle safety.

## R3-002 decision for this run

R3-002 was not modified. Boundary zero has no explicit user/config schema definition; code defaults and current fixtures suggest it disables rate limiting, but that remains an inference. The exact window endpoint is also undocumented; the old `lower_bound` suggests an inclusive left endpoint, but this is not authoritative. Wall-clock timestamps use `CLOCK_REALTIME`, so rollback safety needs a monotonic-clock decision. Per the user/parent instruction, R3-002 is ignored for this run until those semantics are confirmed. R3-003 was not modified.
