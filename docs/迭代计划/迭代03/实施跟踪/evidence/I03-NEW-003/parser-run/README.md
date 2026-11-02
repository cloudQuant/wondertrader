# I03-NEW-003: parser-adapter connection command result

`IParserApi::connect` returns whether the connection command was sent, not whether a vendor connection subsequently succeeds. `WtCore`, `WtDtCore`, and `WtDtServo` `ParserAdapter::run` now return that result directly after their existing uninitialized/stopped checks. Their manager/runner APIs are unchanged; this result is **not yet propagated end to end**.

## Reproduction

Before editing, the three source files were copied into `baseline/`; `baseline.sha256` records hashes of the dirty worktree and test files prior to this work. Three new focused tests use real adapters and fake parser APIs. For each, they verify fresh/uninitialized false, initialized fake connect=false propagates false and is called exactly once, connect=true returns true and is called exactly once, and stopped/released false. The WtDtServo case uses separate fake instances for false and true. No real provider was contacted.

```
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-run/build.py red
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-run/run.py red
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-run/build.py green
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-run/run.py green
/Users/yunjinqi/opt/anaconda3/bin/conda run -n base python /tmp/wt-i03-parser-run/controls.py
```

Each build extracts the existing configured Ninja compile/link argv, compiles only the selected product and test TU into this independent output directory, removes other test objects from the link, and links existing `/tmp/ctpm-i03-build` libraries. Exact commands/exit codes are in each `commands.json`. All 3 red builds and links succeeded; each of the 3 red tests failed specifically because run returned true when connect returned false. All 3 green builds and links succeeded; the 3 direct tests and 3 adjacent init/subscription controls passed individually (6/6, no skips). `xml-summary.log`, `red/*-run` and `green/*-run`/`*-control` preserve XML and output. `green-before.sha256` and `green-after.sha256` match for all six selected source/test TUs. `dependency-libs.sha256` records four reused archives after the run; they and other prebuilt libraries were not fully rebuilt or compared with the complete worktree. The six new depfiles use the physical Boost bridge under `/tmp/ctpm-i03-build/TestUnits/coverage/cov_boost_include/boost`; the output directory contains no created symlinks. No sanitizer, Windows/Linux, real SDK, full-current-tree build or whole regression was performed.

## Remaining propagation

Each `ParserAdapterMgr::run` is a public nonvirtual `void` method (`src/WtCore/ParserAdapter.h`, `src/WtDtCore/ParserAdapter.h`, `src/WtDtServo/ParserAdapter.h`). All three manager implementations discard each adapter result and log the total count as started, even when one failed. `WtRunner::run` (`src/WtRunner/WtRunner.cpp:1174`), `WtRtRunner::run` (`src/WtPorter/WtRtRunner.cpp:2108`), `WtExecRunner::run` (`src/WtExecMon/WtExecRunner.cpp:462`) and `WtDtServo::WtDtRunner::start` (`src/WtDtServo/WtDtRunner.cpp:577`) call those void managers and cannot currently react to false. `WtDtPorter::WtDtRunner::start` (`src/WtDtPorter/WtDtRunner.cpp:255`) independently iterates snapshots, now receives false and logs it but continues startup. These are static observations only; no end-to-end runner test has been run.

A compatible direction is to add a separate result-bearing manager operation while retaining the existing void method, have updated runner owners explicitly consume an aggregate/per-parser result, and define their public C/API startup failure/reporting and partial-start rollback policy. Neither changing a public return signature in place nor merely changing the log establishes ABI-compatible end-to-end failure handling. The three adapter fixes do not close the broader issue.
