# Adapter WTS trade quarantine fixture

The fixture uses the real `TraderAdapter`, a rebuilt `UnresolvedReportJournal`, an in-process fake trader and an independently compiled fake trader plugin. It uses only local temporary files and never connects to a trading service. The dynamic plugin path verifies successful `init` configuration; `initExt` supplies the in-process fake. `run.sh` compiles the adapter, journal, fixture and fake plugin with UBSan, links freshly built project static dependencies, and runs the eight tests with a 30-second timeout.

Example from the repository root (a fresh build is required after changing `WTSTradeDef.hpp`):

```sh
cmake -S src -B /tmp/i03-quarantine-build-NEW -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DWT_WARNINGS_AS_ERRORS=OFF -DWT_ENABLE_COVERAGE=OFF \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
  -DBoost_INCLUDE_DIR=/opt/homebrew/Cellar/boost/1.88.0/include \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_CXX_FLAGS='-include cstdlib'
cmake --build /tmp/i03-quarantine-build-NEW --target WtCore WtDtCore WTSTools WTSUtils -- -j2
src/TestUnits/fixtures/i03_report_quarantine_20261103/run.sh \
  /tmp/i03-quarantine-build-NEW /tmp/i03-quarantine-fixture-NEW
```

Tests cover initialized trade-object defaults and reuse, a normal trade and order, missing contract and commodity, invalid standard code, unchanged incoming WTS code, copied field values, absence of additional notifications and position changes, all six paused order entrances, successful journal append/reopen, missing and damaged journal, pending-on-restart, invalid configuration, reconfiguration, and a synchronous pause while the fake provider makes an entrust ID. The fixture does not verify other report paths, SDK raw messages, previously compiled providers, shared-account gates, reconciliation or actual exchanges. Initializing WTS defaults does not establish whether a provider supplied those fields. An adapter with a configured journal stays paused after startup because reconciliation/resume is outside this batch.

The recorded local run passed 8/8 cases using successful real `TraderAdapter::init` with the fake dynamic provider. This result does not close the original task's full acceptance. An in-flight call admitted before pause is not revoked if it already entered the provider. The existing split-order undone ledger and classification of unknown/negative provider submission outcomes remain separate unresolved dependencies; this fixture does not claim to repair either. Snapshot allocation failure and hard power loss before a durable append can still prevent retention. No manual reconciliation or resume path is wired here.
