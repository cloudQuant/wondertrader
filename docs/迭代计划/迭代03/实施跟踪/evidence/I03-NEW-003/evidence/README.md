# ParserCTPMini R105-003 initialization failure subfix

Product implementation: `src/ParserCTPMini/ParserCTPMini.cpp`; dedicated fixture: `src/TestUnits/fixtures/ctpmini_api_init/`. The pre-fix product copy at `red-src/ParserCTPMini.cpp` was saved before this change; `own.diff` contains only the current modification relative to that copy, including the pre-existing empty-module-path fix in both versions.

The implementation rejects null config and duplicate initialization, validates library, creator symbol and returned API before publishing members, closes the candidate module if no API was created, and gives stage-specific diagnostics. A successful module stays resident: neither `release` nor duplicate/reinitialization unloads or replaces it, because SDK callback quiescence is unproven. Failure permits retry. Successful init followed by release remains non-reinitializable on the same object; this deliberate lifecycle restriction requires owner review and is not proof of full lifecycle safety.

Using the isolated, locally rebuilt `/tmp/ctpm-i03-build` CMake target from the preceding CTPMini path check:

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpmini_api_init/run.py /tmp/ctpm-i03-build src/ParserCTPMini/ParserCTPMini.cpp /tmp/ctpm-i03-init/current-final
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python src/TestUnits/fixtures/ctpmini_api_init/run.py /tmp/ctpm-i03-build /tmp/ctpm-i03-init/red-src/ParserCTPMini.cpp /tmp/ctpm-i03-init/red-final2
```

Current: 6/6 individual subprocesses exit 0: missing library, missing symbol, null API, null config, successful API with duplicate-init and post-release rejection, and single successful API. Each failure verifies false, diagnostic, no API, exact loader/creator/close counts, repeated release, and a successful retry; success verifies SPI/front registration, fake connect and exactly one API release. Missing-symbol and null-API candidates close exactly once; a successful module is not unloaded. Prior source: all four failure controls terminate SIGSEGV (-11), duplicate-init test fails 21, and single-success positive passes. `current-final/commands.json` and `red-final2/commands.json` preserve exact argv/cwd/status and per-case outputs; compiler hashes and source check are in `inputs.sha256` and `current-hash-check.log`.

Production target rebuilt with `/opt/homebrew/bin/ninja -C /tmp/ctpm-i03-build -j 2 CoverageTests_t_ParserCTPMini` (build exit 0). In an isolated cwd only these related existing GTests ran: `R105MiniTest.injected_api_connect_login_and_do_subscribe`, `R105MiniTest.init_flowdir_creation_failure_returns_false_before_vendor_module`, `ParserCTPMiniFactory.lifecycle_calls_without_api_are_safe`: 3/3 PASS, `related-run/output.log` and XML. The preceding path fixture was rebuilt after the change and ran both CTPMini products' path cases 6/6 and two fake-API controls 2/2 (`path-retest.log`, `../path-retest/commands.json`). `symlinks.log` is empty.

Local macOS arm64 fake loader and current product TU only; no vendor SDK, real network, Windows/Linux, sanitizer, successful API callback drain or full suite verification. Candidate module close after a null-returning creator is safe only to the extent a failed creator has not left asynchronous work; that vendor contract is not established. Other linked targets may have been rebuilt concurrently with other work and do not prove an atomic whole-worktree snapshot. Earlier temp build compiled related target dependencies, but no full regression tests ran.
