# N05 timeout helper concurrency probe

The current `wtRunIfNoUnverifiedTimeout` releases the monitor lock before its
callback, so it requires caller serialization (as its header states). This
small probe deterministically pauses that callback before a simulated provider
action while another thread sets the timeout latch. It does not establish that
the real Local/Diff/Arbi hosts or a provider permit this schedule. It also does
not test the separate order-identity or terminal-recovery contract.

From the repository root, compile against the current production monitor:

```sh
c++ -std=c++17 -pthread src/TestUnits/fixtures/n05_timeout_guard_race/check.cpp src/WtExeFact/WtOrdMon.cpp -o /tmp/wt-n05-timeout-guard-probe
/tmp/wt-n05-timeout-guard-probe sequential
/tmp/wt-n05-timeout-guard-probe
```

The sequential control should exit 0 and show no callback or action. The second
process currently exits 1, showing that a callback admitted before the latch can
perform an action after it. A production fix needs an explicit serialization or
reservation contract compatible with provider callbacks and reentry; holding a
monitor mutex across an arbitrary provider call is not established as safe.

On 2026-11-02 the ordinary build and both processes produced those results on
macOS arm64. A build with `-fsanitize=address,undefined -fno-omit-frame-pointer`
compiled, but neither sanitized process produced output after more than 15 s;
both were interrupted, so sanitizer verification is `NOT_RUN`.
