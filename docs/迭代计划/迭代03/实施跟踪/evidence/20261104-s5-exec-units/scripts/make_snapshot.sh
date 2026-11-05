#!/bin/bash
# exec-units 包隔离快照：复制工作区 src/ 与 dist/common（实体复制，不跟随也不创建链接），
# 仅在快照内做两处与本包无关的构建隔离调整（均不回写工作区）：
#  1) （已撤销）早先曾以 HEAD 版 DataManager.h 绕开他包进行中的 WtDtCore 不一致；
#     17:45 后工作区头/源一致，快照直接使用工作区文件。
#  2) coverage CMake 的 wtcore、btcore 行收窄为执行器/撮合相关测试文件，绕开他包进行中的
#     q01_wtcore3/r15_hft_stra_context.cpp（HftStraBaseCtx 受保护成员）编译失败。
set -euo pipefail
REPO=/Users/yunjinqi/Documents/new_projects/wondertrader
SNAP=${1:?snapshot dir}
OVERLAY=${2:-}   # optional dir holding before/ product files to overlay (red build)
rm -rf "$SNAP"; mkdir -p "$SNAP/dist"
rsync -a --no-links "$REPO/src/" "$SNAP/src/"
rsync -a --no-links "$REPO/dist/common/" "$SNAP/dist/common/"
# (17:45 起工作区 DataManager.h/.cpp 已一致，不再替换 HEAD 版本)
if [ -n "$OVERLAY" ]; then rsync -a --no-links "$OVERLAY/src/" "$SNAP/src/"; fi
# R202-003 合同用例只在快照内并入 n01 lane（产品属 WP1，未改前为红控）
cp "$REPO/src/TestUnits/fixtures/i03_exec_units_20261104s5/r202_003_risk_unknown_code_test.cpp" \
   "$SNAP/src/TestUnits/coverage/n01_wtcore_tradepaths/r202_003_risk_unknown_code_test.cpp"
python3 - "$SNAP/src/TestUnits/coverage/CMakeLists.txt" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8').read()
old='"wtcore|WtCore|dir:n01_wtcore_tradepaths,dir:n02_wtcore_engines,dir:p01_wtcore_stractx,dir:p02_wtcore_engines2,dir:p11_wtcore_traderadapter,dir:q01_wtcore3"'
files=["n01_wtcore_tradepaths/r71_executers_test.cpp","n01_wtcore_tradepaths/r71_local_serialization_test.cpp",
"n01_wtcore_tradepaths/r190_exec_unit_retry_proof_test.cpp","n01_wtcore_tradepaths/r220_arbi_serialization_test.cpp",
"n01_wtcore_tradepaths/l00_exec_mgr_header_test.cpp","n02_wtcore_engines/r73_executers_test.cpp",
"n02_wtcore_engines/n02_diff_concurrency_test.cpp","n02_wtcore_engines/n02_exec_mgr_quiesce_test.cpp",
"n02_wtcore_engines/n02_executer_factory_test.cpp","n02_wtcore_engines/r120_diff_executer_unit_test.cpp",
"n02_wtcore_engines/n02_exec_mgr_test.cpp","p02_wtcore_engines2/p02_local_arbi_executer_test.cpp",
"p02_wtcore_engines2/p02_diff_executer_test.cpp","p11_wtcore_traderadapter/r59_exec_autoclear_test.cpp",
"q01_wtcore3/r10_wtcore_executers.cpp","q01_wtcore3/r90_executer_pool_test.cpp",
"q01_wtcore3/p02_005_account_net_limit_test.cpp","q01_wtcore3/r110_diff_executer_test.cpp",
"q01_wtcore3/r130_diff_executer_tail_test.cpp","q01_wtcore3/r15_mgr_executers.cpp",
"q01_wtcore3/r7_vwap_v2_local_integration_test.cpp","q01_wtcore3/r31_exe_fact_pluginload.cpp",
"q01_wtcore3/i03_new014_diff_config_init_test.cpp","n01_wtcore_tradepaths/r202_003_risk_unknown_code_test.cpp"]
import os
base=os.path.dirname(p)
files=[f for f in files if os.path.exists(os.path.join(base,f))]
new='"wtcore|WtCore|'+'+'.join(files)+'"'
assert s.count(old)==1
s=s.replace(old,new,1)
oldb='"btcore|WtBtCore|dir:l06_wtbtcore,dir:n04_btcore_mockers,dir:p03_btcore_mockers2,dir:p04_btcore_replayer,dir:p07_btcore_replayer2,dir:p08_btcore_mockers3,dir:q02_btcore3"'
newb='"btcore|WtBtCore|l06_wtbtcore/l06_match_engine_test.cpp+l06_wtbtcore/z15_match_engine_unsinked_test.cpp+n04_btcore_mockers/n04_exec_mocker_test.cpp+n04_btcore_mockers/r43_match_engine_flow_test.cpp+p08_btcore_mockers3/r20_exec_mocker_test.cpp+p08_btcore_mockers3/r121_exec_mocker_factory_test.cpp+p08_btcore_mockers3/r111_exec_mocker_factory_test.cpp"'
assert s.count(oldb)==1
s=s.replace(oldb,newb,1); open(p,'w',encoding='utf-8').write(s)
PY
test "$(find "$SNAP" -type l | wc -l | tr -d ' ')" = 0
echo "snapshot ok: $SNAP"
