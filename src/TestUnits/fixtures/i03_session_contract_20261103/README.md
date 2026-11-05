# Iteration 03 Session contract fixture

Run from the repository root on the macOS environment used for this fixture:

```sh
cmake -S src/TestUnits/fixtures/i03_session_contract_20261103 \
  -B /tmp/wt-i03-session-contract-20261103 \
  -DWT_BUILD=/tmp/wt-i03-r58-20261102-build \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/wt-i03-session-contract-20261103 -j2
LLVM_PROFILE_FILE=/tmp/wt-i03-session-contract-20261103/session.profraw \
  /tmp/wt-i03-session-contract-20261103/i03_session_contract \
  --gtest_filter='I03SessionContract.*:l03_session.*:L03DaySessionTest.*:r12_session.*' \
  --gtest_output=xml:/tmp/wt-i03-session-contract-20261103/results.xml \
  > /tmp/wt-i03-session-contract-20261103/results.log
```

`WT_BUILD` must point to an already configured and built checkout containing
`build_x64/Debug/libs/libWTSTools.a` and `libWTSUtils.a`. This fixture compiles
`WTSBaseDataMgr.cpp` from the current source but links those pre-existing static
libraries; the result does not establish that those libraries were rebuilt from
the current source. The CMake file uses the machine's Homebrew include/library
paths, GTest static libraries, and `/usr/local/lib/libiconv.dylib`. The link
option for LLVM profile generation is needed by the pre-existing libraries.

`baseline/` contains the pre-implementation snapshots and hashes; `evidence/`
contains the captured results and hashes of the tested current files. These are
local deterministic Session and configuration checks. They do not cover live
providers, real trading, deployment, the complete regression suite, or a defined
`autoAdjust` policy for gaps around a section whose shifted endpoints wrap over
midnight. Commodity locale behavior is tested separately.
The fixture checks valid and invalid HHMM queries for ordinary and full-day
auction and trading sections; `2400` remains a valid endpoint label.
