# Monthly-bar fee provenance: local evidence

This fixture exercises current `HisDataReplayer.cpp`, `WTSBaseDataMgr.cpp` and `CtaMocker.cpp` through actual replay and CTA fill processing. Two historical physical contracts have different rates and volume scales. It also exercises local DSB input, external raw callbacks, adjustment, clipping, loader and sink reentry, minute bars, missing or invalid provenance, aggregate data, resampling, and retry after unordered input. The DSB case uses the prebuilt local `libWtDataStorage.dylib`; remaining static libraries also come from the existing local build and were not rebuilt or instrumented here. Paths below are machine-specific.

The external raw callback contract is trusted to return bars belonging to the requested physical code. The fixture's callback does so; this does not verify that a vendor or other loader supplies truthful data. Direct continuous aggregate DSB, CSV and external final bars have no verified physical identity and fail fee preparation. Continuous bars resampled with `times > 1` also retain unknown provenance, including bars whose constituents happen to share one physical contract; cross-contract aggregation is rejected. Only unresampled (`times = 1`) continuous monthly segments carry this provenance in this change. Explicit physical subscriptions are separately controlled.

## Commands

```sh
cmake -S src/TestUnits/fixtures/i03_bar_fee_provenance_20261103 \
  -B /tmp/i03-bar-fee-provenance-build-20261103 \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-bar-fee-provenance-build-20261103 -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-bar-fee-%p.profraw \
  /tmp/i03-bar-fee-provenance-build-20261103/i03_bar_fee_provenance --gtest_color=no

cmake --build /tmp/i03-replay-status-build-20261103 -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-bar-status-%p.profraw \
  /tmp/i03-replay-status-build-20261103/i03_replay_status --gtest_color=no

cmake --build /tmp/i03-replay-fee-identity-build-20261103 -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-bar-oldfee-%p.profraw \
  /tmp/i03-replay-fee-identity-build-20261103/i03_replay_fee_identity --gtest_color=no \
  --gtest_filter='FeeIdentityTest.*:HisDataReplayerRuntimeTest.calcFeeForUnconfiguredCode:HisDataReplayerRuntimeTest.amountBasedFeeRequiresCommodityAtInit:HisDataReplayerRuntimeTest.amountBasedFeeUsesPhysicalContractVolumeScale:HisDataReplayerRuntimeTest.volumeBasedFeeInitAllowsMissingCommodityButFillRequiresMetadata:HisDataReplayerRuntimeTest.volumeBasedFeeUsesVerifiedPhysicalContract'
```

After the aligned-object copy change, the corresponding saved logs report 13/13 provenance, 16/16 replay status and 13/13 fee controls passing. ASan and UBSan apply to the fixture's newly compiled translation units; prebuilt dependencies are not covered. Leak detection was disabled. Full regression, other platforms, concurrency sanitizer, real providers and live trading were not run. The patches in `evidence/` compare the final two Replayer files to the pre-task snapshots, which included prior dirty work; hashes identify snapshots, source, dependencies and logs.
