# Runner parser shutdown fixture

The filtered tests instantiate the actual `WtRunner`, real `ParserAdapter` and manager, and load the fake V2 parser module built from `i03_parser_lifecycle_v2/provider.cpp`. A fake `ITraderApi` observes downstream release. They cover stop-before-poll across a registered parser and a failed-init owner, pending and unsafe-wait retries, shutdown callback reentry, and repeated empty/completed shutdown. The fake module records the order of requests and checks waits use a zero timeout; no provider or trade is used.

On the configured macOS/Homebrew environment, run from the repository root, using a new absolute output directory:

```sh
src/TestUnits/fixtures/i03_runner_parser_drain_20261103/run.sh /tmp/i03-runner-parser-drain-replay
```

The script creates a physical source snapshot, builds with `-j2`, copies the test module into an isolated runtime directory, and writes build/test logs and JUnit XML there. It requires the Homebrew CMake, Ninja, Boost and GoogleTest paths encoded in the script and the user's conda base Python. The filter is `I03RunnerParserDrain.*`.

These tests do not prove behavior of real providers, a permanently unprovable legacy owner, or whole-owner-graph process-lifetime retention. The existing destructor behavior when shutdown cannot complete remains outside this fixture's scope. This is a targeted functional check, not full regression or production acceptance.
