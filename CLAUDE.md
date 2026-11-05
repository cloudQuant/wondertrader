# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

WonderTrader C++ core (v0.9.9): a multi-market, multi-instrument quantitative trading framework built around an **M+1+N** design — M market-data parsers feed one strategy engine/portfolio, whose target positions are executed through N traders. `src/` is the C++ codebase. `dist/` contains runtime configs and base data. `docs/` contains Chinese architecture docs plus the active, evidence-tracked repair iterations. The Python layer (`wtpy`) is a separate repository that consumes the C ports (`WtPorter`, `WtBtPorter`, `WtDtPorter`, `WtExecMon`).

Requirements: C++17, CMake >= 3.16, Boost (system/filesystem/thread), rapidjson, fmt, spdlog, nanomsg, libiconv; tests need GTest. Vendor SDK headers and static libraries (CTP, CTPMini, CTPOpt, Femas, XTP, etc.) are vendored under `src/API/`.

## Build

```bash
# Release (same as src/build_release.sh, but portable)
cmake -S src -B src/build_all -DCMAKE_BUILD_TYPE=Release
cmake --build src/build_all -j "$(sysctl -n hw.ncpu 2>/dev/null || nproc)"

# Debug (same as src/build_debug.sh)
cmake -S src -B src/build_debug -DCMAKE_BUILD_TYPE=Debug
cmake --build src/build_debug -j 4

# One target
cmake --build src/build_all --target WtRunner -j
```

On macOS, install dependencies with Homebrew: `brew install cmake ninja boost rapidjson fmt spdlog nanomsg googletest`. The shell scripts are Linux-oriented (`build_release.sh` calls `nproc`); use the commands above on macOS. Output layout (PLATFORM is always `x64` outside MSVC, including Apple Silicon):

- binaries: `src/build_all/build_x64/Release/bin/<Module>/`
- static libraries: `src/build_all/build_x64/Release/libs/`
- `TestUnits` and coverage executables: `.../bin/TestUnits/`

Build options in `src/CMakeLists.txt`:

- `-DWT_ENABLE_COVERAGE=ON` adds LLVM source-based instrumentation and builds the coverage test targets (only when enabled).
- `-DWT_WARNINGS_AS_ERRORS` defaults to **ON on APPLE**, OFF elsewhere. Non-MSVC builds always use `-Wall -Wextra`. New product code must compile cleanly under this policy; pass `-DWT_WARNINGS_AS_ERRORS=OFF` only as a deliberate local workaround, not in committed changes.
- `xmake.lua` / `make_release.sh` are legacy; the CMake build is authoritative.
- There is no separate lint target — compile warnings are the lint.

## Tests

Two independent harnesses exist:

1. **`src/TestUnits`** — always built. Uses the vendored gtest under `src/TestUnits/gtest/` and the root `test_*.cpp` files; produces `.../bin/TestUnits/TestUnits`.
2. **`src/TestUnits/coverage`** — only built with `-DWT_ENABLE_COVERAGE=ON`. This is the main suite for this fork. Lanes are directories such as `l01_share_wtsutils`, `n02_wtcore_engines`, `p04_btcore_replayer`, `q04_uftcore3`, `r7_exefact`, `t_ParserCTP`, `t_TraderCTP`, `t_CTPLoader`.

Coverage tests are **not registered with CTest**; run the binaries directly. Coverage configure/build/run:

```bash
cmake -S src -B build-cov -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWT_ENABLE_COVERAGE=ON \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_INCLUDE_DIR="$(brew --prefix boost)/include"

cmake --build build-cov --target CoverageTests_wtcore -j 6

# one case / one suite / whole target
build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore --gtest_filter='Suite.Case'
build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore --gtest_filter='Suite.*'
build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore

# evidence output and coverage report
build-cov/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore \
  --gtest_filter='Suite.Case' --gtest_output=xml:/tmp/result.xml
llvm-profdata merge -sparse *.profraw -o merged.profdata
llvm-cov report <test-binary> -instr-profile=merged.profdata
```

Target families are declared in `src/TestUnits/coverage/CMakeLists.txt`: `CoverageTests_wtcore`, `_uftcore`, `_btcore`, `_dtcore`, `_dtservo`, `_porter`, `_btporter`, `_adstorage`, `_idxfactory`, `_exefact`, `_riskmon`, `_mqshare`, plus one `_t_<AdapterModule>` per `Parser*`/`Trader*`/`*Loader` directory. A target is created only when its lane has test sources. An executable contains production TUs for its own family and deliberately does **not** link sibling core static libraries (see “Duplicate-symbol traps” below).

Adding tests: put `.cpp` files in the lane that maps to the target. Files added to an existing `dir:` lane are picked up automatically; a new `t_<AdapterModule>/` directory gets a target automatically; other new lanes require adding a row to `WT_COV_SPLIT_TARGETS`. Loadable fakes/plugins belong in `src/TestUnits/fixtures/`, outside `coverage/`, so their exported factory symbols are not linked into another coverage binary. Some lanes depend on `dist/common` base files through compile definitions such as `WT_COV_P07_FIXTURE_ROOT`.

The repository-root `tests/` is a separate, older CMake project (`wtTest`, C++20, `find_package(GTest)`) that is **not** part of the `src` build; do not confuse it with `TestUnits`.

## Architecture

### Runtime data and order flow

```
Parser* plugin ──(C factory export)──> ParserAdapter ──> WtEngine
                                                          ├─ CtaEngine (synchronous, event+time driven)
                                                          ├─ SelEngine (asynchronous, scheduled recompute)
                                                          └─ HftEngine (event driven, high frequency)
                                                                  │ target positions
                                                                  v
                                                   WtExecMgr / IExecuterStub
                                                                  │ orders
                                                                  v
                   TraderAdapter ──(C factory export)──> Trader* plugin ──> broker
```

`WtUftCore` mirrors this flow as a fully independent ultra-low-latency core (no Python/application interface, separate from `WtCore`).

Backtesting (`WtBtCore`) replaces live adapters with `HisDataReplayer` plus `CtaMocker`/`SelMocker`/`HftMocker`/`UftMocker`/`ExecMocker`; `WtBtPorter` and `WtBtRunner` expose it. All strategy types share this engine.

Market data path: parser → `WtDtCore` (ingest, UDP/SHM broadcast, `StateMonitor`) → `WtDataStorageAD` writer; `WtDataStorage`/`WtDataStorageAD` readers serve `WtDtServo`/`WtDtMgr`; `QuoteFactory` is the standalone dump program; `LoaderRunner` loads contract lists.

### Module layers

- `src/Includes/` — every cross-module interface and shared domain type: `IParserApi`/`IParserSpi`, `ITraderApi`/`ITraderSpi`, `ICtaStraCtx`/`ISelStraCtx`/`IHftStraCtx`/`IUftStraCtx`, `IExecPluginV2`, `IDataReader`/`IDataWriter`/`IRdmDtReader`, `IDataManager`, `IBaseDataMgr`, `ISessionMgr`, `WTSContractInfo`, `WTSTickData`, `WTSTradeDef`, `WTSVariant`.
- `src/Share/` — core object model and utilities: `WTSVariant` config tree, object/containers, `TimeUtils`/`WTSSessionInfo`, `DLLHelper`/`ModuleHelper`, `ObjectPool`, `SpinMutex`, `cppcli`, threadpool.
- `src/WTSUtils/` — configuration loading (`WTSCfgLoader`: JSON/YAML → `WTSVariant`), yaml-cpp, signal hooks, and other third-party wrappers.
- `src/WTSTools/` — `WTSLogger`, `WTSBaseDataMgr` (holidays/sessions/commodities/contracts/hot lists), `WTSHotMgr`, `WTSDataFactory`.
- `src/WtCore/` — live trading core: `WtEngine` and Cta/Sel/Hft engines, `ParserAdapter`, `TraderAdapter`, `WtExecMgr`, risk integration. Static library.
- `src/WtUftCore/` — independent UFT engine and its own adapters. Static library; do not link it together with `WtCore`.
- `src/WtBtCore/` — backtest engine, replayer, mockers. Static library.
- Data kit: `WtDtCore`, `WtDataStorage`/`WtDataStorageAD` (mmap-backed binary data; AD adds LMDB metadata/keys; readers handle ZSTD-compressed blocks), `WtDtHelper`, `WtDtServo`.
- Adapters: `Parser*` (market data), `Trader*` (orders), `*Loader` (contract loading). Each is a shared library with hidden visibility and `extern "C"` factory exports (`createParser`/`deleteParser`, `createTrader`/`deleteTrader`, loader-specific names); the host resolves them by module name through `DLLHelper` from the executable/module directory (see `Share/ModuleHelper.hpp::getBinDir()`).
- Plugin factories: `WtCtaStraFact`, `WtSelStraFact`, `WtHftStraFact`, `WtUftStraFact`, `WtExeFact` (VWAP/TWAP/stock-VWAP/min-impact/diff/arbi units), `WtRiskMonFact`.
- C ports and runners: `WtPorter` (live C API), `WtBtPorter`, `WtDtPorter`, `WtExecMon`, `WtShareHelper` (shared-memory helper), `WtMsgQue` (nanomsg C wrapper); executables `WtRunner`, `WtBtRunner`, `WtUftRunner`, `QuoteFactory`, `LoaderRunner`, `TraderDumper`.

### Plugin and configuration contracts

- Execution units have the legacy C++ v1 interfaces **and** the versioned C ABI in `Includes/IExecPluginV2.h` (`getExecPluginApiV2`, ABI 2.2). Preserve the old vtable/ABI when extending it. Under v2: creation/initialization is synchronous; a unit that fails initialization must be destroyed immediately and never registered, called, or quiesced; only successfully initialized units may own asynchronous work, and that work must be drained through the quiesce callbacks before unload. Old-ABI plugins must still load, but unverified initialization must fail closed.
- Configs are YAML or JSON, loaded into `WTSVariant` by `WTSCfgLoader`. Base data lives in `dist/common/` (`holidays.json`, `sessions.json`, `commodities.json`, `contracts.json`, `hots.json`, `fees.json`, `fees_stk.json`, `stocks.json`, `stk_comms.json`).
- Runner invocations default to files in the current directory:
  - `WtRunner` / `WtUftRunner`: `-c config.yaml -l logcfg.yaml`
  - `WtBtRunner`: `-c configbt.yaml -l logcfgbt.yaml`
  - `QuoteFactory`: `-c dtcfg.yaml -l logcfgdt.yaml`
  - `LoaderRunner`: `-m <loader module> -c config.ini`
  Copy the matching `dist/<Runner>/` directory (and `dist/common/` next to it) before running.

## Non-obvious constraints that bite

- **Duplicate-symbol traps.** `WtCore` and `WtUftCore` both define `wtp::ActionPolicyMgr::init`; `WtDataStorage` and `WtDataStorageAD` export identical C factory names; `WtHelper.cpp` is copied into many modules; `WtDtCore` and `WtDtServo` both define a `ParserAdapter` with different shapes. Static-archive lazy extraction silently picks one implementation. Never link two sibling cores or two storage modules into one test binary; follow the one-family-per-executable rule in the coverage CMake.
- `ModuleHelper.hpp` uses a translation-unit-local anchor. Coverage builds rewrite `inst_hlp` per source file. Do not define a global `inst_hlp` symbol; it will collide.
- **Vendor SDK availability is platform-specific.** Many `Parser*`/`Trader*` modules have no macOS vendor library; their coverage targets link with `-Wl,-undefined,dynamic_lookup` so tests can exercise local fake paths. Tests must never call real vendor APIs or perform real orders/settlement.
- **Dual-Boost macOS setup.** Coverage CMake builds a physical `cov_boost_include/boost` bridge because Homebrew/GTest adds `-isystem /opt/homebrew/include` and can silently select a different Boost than the one linked. If Boost path/algorithm symbols go undefined, fix `Boost_INCLUDE_DIR`/the bridge rather than adding ad-hoc `-I` flags.
- **No symlinks anywhere** (iteration constraint). The coverage Boost bridge verifies this and refuses to operate on symlinked trees. The only exception (user decision K63, 2026-11-06) is `UnresolvedReportJournal.SymlinkAndVisiblePathReplacementFailClosed`: when run explicitly with `WT_ALLOW_SYMLINK_TESTS=1`, it may create a symlink in the OS temp directory (outside the repo) and must delete it afterwards.
- Test TUs may relax a small named set of warnings (`-Wno-error=unused-parameter`, `-Wno-error=reorder-ctor`, `-Wno-error=return-type`, `-Wno-error=unused-variable`) only for legacy vendor adapters and renamed-`main` runner TUs. Do not broaden this; fix new code instead.
- The working tree intentionally carries many uncommitted iteration changes. Never run `git checkout --`, `git reset --hard`, `git clean`, or overwrite files you did not create without checking. Record HEAD, file hashes, and snapshots when producing evidence.
- Do not weaken assertions or skip tests to get green results.
- Many coverage tests write fixture configs, logs, `Logs/`, `mocker_*` dirs and `*.profraw` into the **current working directory**. The repository root already holds hundreds of untracked artifacts like this from earlier runs (`r28wt_*.yaml`, `r104qf_*`, `*.log`, ...). Some may be referenced as evidence, so do not bulk-delete them. When running test binaries, `cd` into a scratch/evidence directory first so you don't add more.
- `.gitignore` excludes `dist/*`, but some `dist/` files are already tracked. New files under `dist/` need `git add -f`.

## Repair-iteration workflow

The fork is currently driven by documented repair iterations under `docs/迭代计划/迭代NN/`; at the time of writing the active one is **迭代03** (2026-11-02). Before doing repair work, read:

- `docs/迭代计划/迭代03/迭代计划.md` — phases, work packages, closure rules.
- `docs/迭代计划/迭代03/213项任务清单.md` — the frozen scope (task count must not be changed to show progress).
- `docs/迭代计划/迭代03/执行跟踪.json` + `实施跟踪/执行记录.md` — current status and rolling log.
- `实施跟踪/evidence/<BUG-ID>/` — commands, logs, XML, hashes for each item.

Rules that matter during implementation:

- During the focused-fix phase, run **only the tests directly related to the items being changed**, not the full suite. Full regression is a gated phase and is currently `NOT_RUN`; never describe targeted passes as a full regression or overall acceptance.
- Every change needs traceable evidence: exact command, exit code, output/log/XML path, source and binary hashes, and a before/after (red-green) control for product fixes. Static analysis must be labeled as such, not presented as a failing run.
- Status vocabulary: `待复核` → `实施中` → `待验证` → `本地验证通过` → `已验收`; use `阻塞` with an executable unblock condition when contracts, environment, or external evidence are missing. Local fake-based success does not cover Windows, real vendors, production, or a final integrated tree.
- Newly discovered issues get new IDs and are linked to the current iteration; they must not alter the frozen 213-item denominator.
- These documents and execution records are written in Chinese; keep new entries in Chinese and match the surrounding format.
- The local machine is macOS; Windows/MSVC and real vendor acceptance remain external and must be reported as unverified unless actually executed.
