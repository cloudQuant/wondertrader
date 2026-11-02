# BUG-P01-004 focused evidence (not acceptance)

Workspace: /Users/yunjinqi/Documents/new_projects/wondertrader. Final compiled input hashes and exact command arrays are in last-build.json. The initial complete build used final-build.py; after test-only fixture changes, the rebuild used:

    /Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-p01-004/final-p01-rebuild.py

The build reuses the existing v8 build tree and its objects and libraries. The ASan build instruments only the context and two relevant test translation units; other objects and libraries are not instrumented and use mixed toolchains. The ASan linker output with old-DWARF warnings is last-asan-link.log.

Test outputs and XML are under last2-tests; result lists and individual test names are last2-normal-results.log (8/8) and last2-asan-results.log (4/4). Each ran alone in an isolated working directory, with a 30-second process alarm. ASan used ASAN_OPTIONS=detect_leaks=0:halt_on_error=1. Red-control output with the baseline ctx is under last2-red and last2-red-results.log; the root assertion aborts, and the precommit lookup failure and mixed-condition rejection expectations fail on baseline.

Two earlier strict ASan executions reported container-overflow in fixture std::vector<std::string> operations. Their original failure output is preserved in initial-asan-container-overflow.log and second-asan-container-overflow.log. The fixtures subsequently used scalar/stack storage with the relevant diagnostics still asserted. The cause of the earlier reports is not established; a later pass does not erase them. Full same-version rebuild/link-order investigation, LSan, full-tree build, Windows, external provider behavior and whole-suite regression were not performed.

Candidate stages validated snapshot state before publication; tick subscriptions occur after publication. Failure or reentrancy in those external calls remains unresolved, as does the complete historic schema contract. The item remains blocked pending review and verification.

## Follow-up: bounded code display

The condition diagnostic concatenates its key only after validating it against CondEntrust::_code (32 bytes), so at most 31 key bytes reach the diagnostic. The direct malformed-snapshot test now checks lengths 31, 32 and 4096, plus a 4096-byte signal key and a sub-512-byte diagnostic. Normal and ASan outputs are in display-tests/{normal,asan}/MalformedSnapshotsDoNotPublishAnyStateOrSubscriptions.

Reachability probes on the pre-display candidate produced fmt::v11::format_error on 4096-byte expired position and signal codes; original outputs are probe-position/output.log and probe-signal/output.log. probe-stack-position/output.log and probe-stack-signal/output.log record terminate backtraces through load_data -> log_info. These reports are exceptions and aborts, not ASan buffer-overflow reports.

The follow-up bounds only code text in position/signal recovery logs and the stra_sub_ticks log; lookups, map keys and subscriptions receive the entire code. display-check.py links the prior ctx object with the final tests as red controls and runs normal, ASan and red binaries in isolated directories with a 30-second timeout. display-results.log records six normal and six ASan passing, and five cases aborting on both prior binaries. Two distinct 4096-byte identifiers with identical 128-byte prefixes verify map and subscription identity in position, signal and direct subscription tests, using a fake contract manager. display-controls-results.log records five additional normal legacy/recovery controls passing. Builds are recorded in final-build.json and last-build.json; earlier build and test evidence is retained in pre-code-display, prior-build, boundary-previous and previous-display-check.

This bounded logging change does not establish real provider acceptance of long codes or a supported historical code-length contract. Earlier unexplained ASan reports, other external call paths, and postcommit subscription failure/reentrancy remain unresolved; P01 is not accepted.
