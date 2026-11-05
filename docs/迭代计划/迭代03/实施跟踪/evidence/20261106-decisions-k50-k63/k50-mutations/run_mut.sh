#!/bin/bash
set -u
M=/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/k50mut
T=$M/tree/src
B=$M/build/build_x64/Debug/bin/TestUnits/CoverageTests_wtcore
cp $T/WtCore/TraderAdapter.cpp $M/TraderAdapter.cpp.orig
cp $T/WtCore/AccountTradingGate.h $M/AccountTradingGate.h.orig
runcase() { name=$1; mkdir -p $M/$name; for t in AuditedCommandReleasesTheManualClaimForExecuters LiveManualOrderBlocksTheRelease ReleaseWithoutDurableAuditKeepsTheClaim; do (cd $M/$name && LLVM_PROFILE_FILE=$M/$name/%p.profraw $B --gtest_filter="I03K50ManualClaimRelease.$t" --gtest_output=xml:$M/$name/$t.xml > $t.log 2>&1; echo "$name $t exit=$?"); done; (cd $M/$name && LLVM_PROFILE_FILE=$M/$name/%p.profraw $B --gtest_filter='I03K50AccountGate.*' --gtest_output=xml:$M/$name/gate.xml > gate.log 2>&1; echo "$name gate exit=$?"); }
# M1: live manual orders are ignored
python3 - $T/WtCore/TraderAdapter.cpp <<'PY'
import sys; p=sys.argv[1]; s=open(p,'rb').read()
old=b'_account_gate->releaseContractClaim(kManualOrderOwner, request, liveManualOrders, &text);'
assert s.count(old)==1; s=s.replace(old,b'_account_gate->releaseContractClaim(kManualOrderOwner, request, false /*K50 mutation M1*/, &text);'); open(p,'wb').write(s)
PY
cmake --build $M/build --target CoverageTests_wtcore -j 3 > $M/logs/build-m1.log 2>&1; echo "build-m1=$?"
runcase m1-ignore-live
cp $M/TraderAdapter.cpp.orig $T/WtCore/TraderAdapter.cpp
# M2: release first, audit failures ignored
python3 - $T/WtCore/AccountTradingGate.h <<'PY'
import sys; p=sys.argv[1]; s=open(p,'rb').read()
old=b'''		if (_audit_fd < 0)
			return finish(ClaimReleaseStatus::AuditUnavailable, "account audit log is not configured");
		const std::string detail = "owner=" + owner + " code=" + request.code;'''
assert s.count(old)==1; s=s.replace(old,b'''		const std::string detail = "owner=" + owner + " code=" + request.code; /*K50 mutation M2*/''')
old=b'''		if (!appendAuditLocked(auditLine("claim_released", _generation, request.operator_id,
			detail, request.reason, request.reference), error))
			return finish(ClaimReleaseStatus::AuditWriteFailed, error);'''
assert s.count(old)==1; s=s.replace(old,b'''		(void)appendAuditLocked(auditLine("claim_released", _generation, request.operator_id,
			detail, request.reason, request.reference), error);''')
open(p,'wb').write(s)
PY
cmake --build $M/build --target CoverageTests_wtcore -j 3 > $M/logs/build-m2.log 2>&1; echo "build-m2=$?"
runcase m2-release-before-audit
cp $M/AccountTradingGate.h.orig $T/WtCore/AccountTradingGate.h
cmp $T/WtCore/TraderAdapter.cpp /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/g7/tree/src/WtCore/TraderAdapter.cpp && cmp $T/WtCore/AccountTradingGate.h /private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/g7/tree/src/WtCore/AccountTradingGate.h && echo restored-identical
