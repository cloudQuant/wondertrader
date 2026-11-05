#!/bin/bash
# I03-NEW-015 红控：把 r20_mq_fixture 的 notifier_check.cpp/fake_mq.cpp 按主目标编译参数编入，
# 用主目标原链接命令重链为 CoverageTests_red，复现自带 main 取代 gtest_main（旧 GLOB_RECURSE 行为）。
# usage: red_main_relink.sh <已配置并构建 CoverageTests 的构建目录> <输出目录>
set -eu
B=$1; O=$2; mkdir -p "$O"
D="$(cd "$(dirname "$0")/../../coverage/p08_btcore_mockers3/r20_mq_fixture" && pwd)"
python3 - "$B" "$O" "$D" <<'P'
import json,re,sys
B,O,D=sys.argv[1:4]
cc=json.load(open(B+'/compile_commands.json'))
e=[x for x in cc if x['file'].endswith('l05_dt_servo_porter/l05_dt_helper_bar_test.cpp')][0]
with open(O+'/compile.sh','w') as f:
    for n in ['notifier_check','fake_mq']:
        c=e['command'].replace(e['file'],D+'/'+n+'.cpp')
        c=re.sub(r'-o \S+','-o '+O+'/'+n+'.o',c); c=re.sub(r'-MD -MT \S+ -MF \S+','',c); c=re.sub(r'-Dinst_hlp=\S+','',c)
        f.write(c+'\n')
P
(cd "$B" && bash "$O/compile.sh")
L=$(cd "$B" && ninja -t commands build_x64/Debug/bin/TestUnits/CoverageTests | tail -1)
L=${L/-o build_x64\/Debug\/bin\/TestUnits\/CoverageTests/-o $O/CoverageTests_red $O/notifier_check.o $O/fake_mq.o}
(cd "$B" && eval "$L")
