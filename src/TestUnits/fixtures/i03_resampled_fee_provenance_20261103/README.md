# Verified provenance for resampled continuous bars

The resampler records half-open input index ranges in its existing merge loops. A result inherits an identity only when the complete range is mapped, every constituent has the same nonempty source and that source resolves to a registered physical contract. Physical metadata is checked again when the synthetic tick is dispatched. Mixed contracts, unknown or inconsistent sources and invalid registration leave that result unknown and its fee request fails closed. The original virtual interface and protected nonvirtual extraction symbols remain present; a derived factory override uses its normal virtual method and returns no ranges because its grouping cannot be verified. The range-carrying path uses per-call storage and RTTI, without new factory state.

The 21-test executable includes the 13 earlier monthly-bar provenance tests (one updated per-result source assertion), factory range tests, unknown/invalid-source controls and a two-day actual CTA fill test. It checks m1 and m5, hour and half-hour, section alignment, overnight grouping, partial final bars, day bars, nested calls and derived overrides. The CTA test has two physical contracts with different fee rates and multipliers and reenters through the loader and sink. The existing daily resampler includes an incomplete last daily bar even when `bIncludeOpen` is false; this change preserves that legacy behavior.

The external raw loader is trusted to return data belonging to the physical code requested. Synthetic local callbacks satisfy that contract but do not verify vendor data. DSB tests use the prebuilt local `libWtDataStorage.dylib`. Other prebuilt static libraries are reused; current Replayer, DataFactory, BaseDataMgr and the CTA test's CtaMocker are compiled in this fixture. The existing monthly fixture README predates this change and its statement that all resampled provenance is unknown is superseded by these per-result rules.

## Commands

```sh
cmake -S src/TestUnits/fixtures/i03_resampled_fee_provenance_20261103 \
  -B /tmp/i03-resampled-fee-provenance-build-20261103 \
  -DCMAKE_CXX_COMPILER=/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++ \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/i03-resampled-fee-provenance-build-20261103 -j2
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-resampled-main-%p.profraw \
  /tmp/i03-resampled-fee-provenance-build-20261103/i03_resampled_fee_provenance --gtest_color=no
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-resampled-status-%p.profraw \
  /tmp/i03-resampled-fee-provenance-build-20261103/i03_resampled_status --gtest_color=no
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  LLVM_PROFILE_FILE=/tmp/i03-resampled-fee-%p.profraw \
  /tmp/i03-resampled-fee-provenance-build-20261103/i03_resampled_fee --gtest_color=no \
  --gtest_filter='FeeIdentityTest.*:HisDataReplayerRuntimeTest.calcFeeForUnconfiguredCode:HisDataReplayerRuntimeTest.amountBasedFeeRequiresCommodityAtInit:HisDataReplayerRuntimeTest.amountBasedFeeUsesPhysicalContractVolumeScale:HisDataReplayerRuntimeTest.volumeBasedFeeInitAllowsMissingCommodityButFillRequiresMetadata:HisDataReplayerRuntimeTest.volumeBasedFeeUsesVerifiedPhysicalContract'
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/i03-resampled-fee-provenance-build-20261103/i03_factory_baseline > /tmp/i03-factory-before.txt
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/i03-resampled-fee-provenance-build-20261103/i03_factory_current > /tmp/i03-factory-after.txt
diff -u /tmp/i03-factory-before.txt /tmp/i03-factory-after.txt
```

The saved runs show 21/21, 16/16 and 13/13 passing, respectively. Eleven legacy factory cases produce identical before/after textual numeric results, and all seven baseline extraction symbols appear in the new executable's symbol list. This is a local symbol comparison, not a cross-platform binary compatibility certification. ASan/UBSan cover newly built translation units only; leak detection was disabled. No full regression, other platforms, concurrency sanitizer, real providers or live trading were run.

`evidence/` holds logs, outputs, symbol lists, incremental patches, hashes and the pre-change DataFactory source used as the numerical baseline. All four product snapshots were saved before edits. The prior fixture's pre-edit test file was reconstructed by reversing the one authorized assertion; its hash matches the previously recorded pre-edit hash `ea10e4d760e1c28aeaba1b9e5a63b5c3aa228596a3adbf62317a9de1e6b626a2`.
