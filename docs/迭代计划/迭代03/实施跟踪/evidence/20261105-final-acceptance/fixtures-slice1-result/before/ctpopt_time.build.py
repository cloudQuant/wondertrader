import json
import pathlib
import re
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[4]
out = pathlib.Path(sys.argv[1]).resolve()
source = out / 'src'
source.mkdir(parents=True, exist_ok=True)
if len(sys.argv) < 3 or sys.argv[2] != 'reuse':
    for name in ('TraderCTPOpt', 'Includes', 'Share', 'WTSTools', 'WTSUtils', 'FasterLibs'):
        shutil.copytree(root / 'src' / name, source / name, dirs_exist_ok=True)
    shutil.copytree(root / 'src/API/CTPOpt3.5.8', source / 'API/CTPOpt3.5.8', dirs_exist_ok=True)
    if len(sys.argv) >= 3 and sys.argv[2] == 'baseline':
        saved = root / 'docs/迭代计划/迭代03/实施跟踪/evidence/I03-NEW-006-ctpopt/baseline'
        shutil.copyfile(saved/'TraderCTPOpt.cpp', source/'TraderCTPOpt/TraderCTPOpt.cpp')
        shutil.copyfile(saved/'TimeUtils.instrumented.hpp', source/'Share/TimeUtils.hpp')
    elif len(sys.argv) >= 3 and sys.argv[2] == 'checked-baseline':
        saved = root / 'docs/迭代计划/迭代03/实施跟踪/evidence/I03-NEW-006-localtime-20261102/baseline'
        shutil.copyfile(saved/'TimeUtils.hpp', source/'Share/TimeUtils.hpp')
    header = source / 'Share/TimeUtils.hpp'
    text = header.read_text(encoding='utf-8-sig')
    text = re.sub(r'(?<![A-Za-z0-9_])clock_gettime\(CLOCK_REALTIME, &now\)', 'wt_i03_clock_gettime(CLOCK_REALTIME, &now)', text)
    text = re.sub(r'(?<![A-Za-z0-9_])time\(NULL\)', 'wt_i03_time(NULL)', text)
    if 'extern "C" int wt_i03_clock_gettime' not in text:
        text = text.replace('#include <cmath>', '#include <cmath>\nextern "C" int wt_i03_clock_gettime(clockid_t, struct timespec*);\nextern "C" time_t wt_i03_time(time_t*);')
    header.write_text(text)
    header = source / 'TraderCTPOpt/TraderCTPOpt.h'
    text = header.read_text(encoding='utf-8-sig')
    old = 'private:\n\n\tint confirm();'
    if old not in text:
        raise SystemExit('private confirm marker missing')
    header.write_text(text.replace(old, 'public:\n\n\tint confirm();', 1))
header = source / 'Share/TimeUtils.hpp'
text = header.read_text(encoding='utf-8-sig')
text = text.replace('wt_i03_wt_i03_', 'wt_i03_')
text = re.sub(r'(?<![A-Za-z0-9_])gmtime_r\(&now, &tm_gtm\)', 'wt_i03_gmtime_r(&now, &tm_gtm)', text)
text = re.sub(r'(?<![A-Za-z0-9_])localtime_r\(&gt, &gtm2\)', 'wt_i03_localtime_r(&gt, &gtm2)', text)
text = re.sub(r'(?<![A-Za-z0-9_])localtime_r\(&clockSeconds, &snapshot\)', 'wt_i03_localtime_r(&clockSeconds, &snapshot)', text)
text = re.sub(r'(?<![A-Za-z0-9_])mktime\(&tm_gtm\)', 'wt_i03_mktime(&tm_gtm)', text)
if 'extern "C" struct tm* wt_i03_gmtime_r' not in text:
    text = text.replace('#include <cmath>', '#include <cmath>\nextern "C" struct tm* wt_i03_gmtime_r(const time_t*, struct tm*);\nextern "C" struct tm* wt_i03_localtime_r(const time_t*, struct tm*);')
if 'extern "C" time_t wt_i03_mktime' not in text:
    text = text.replace('#include <cmath>', '#include <cmath>\nextern "C" time_t wt_i03_mktime(struct tm*);')
header.write_text(text)
clang = '/opt/homebrew/Cellar/llvm/22.1.8/bin/clang++'
common = [clang, '-std=c++17', '-g', '-O1', '-pthread', '-fsanitize=address,undefined',
          '-fno-omit-frame-pointer', '-DFMT_HEADER_ONLY', '-DSPDLOG_FMT_EXTERNAL',
          '-I'+str(source), '-I'+str(source/'Includes'), '-I'+str(source/'Share'),
          '-I'+str(root/'src/TestUnits/coverage/t_TraderCTPOpt'),
          '-I/opt/homebrew/Cellar/boost/1.88.0/include', '-I/opt/homebrew/Cellar/fmt/11.2.0/include',
          '-I/opt/homebrew/Cellar/spdlog/1.15.3/include', '-include', 'stdlib.h', '-include', str(source/'Share/fmt_enum_compat.h')]
if 'tryGetDateTime(' in (source/'Share/TimeUtils.hpp').read_text():
    common.append('-DWT_TIME_SNAPSHOT_API')
if 'uint64_t& epochMillis' in (source/'Share/TimeUtils.hpp').read_text():
    common.append('-DWT_TIME_SNAPSHOT_EPOCH_API')
commands = [
    common + ['-c', str(source/'TraderCTPOpt/TraderCTPOpt.cpp'), '-o', str(out/'product.o')],
    common + ['-c', str(root/'src/TestUnits/fixtures/ctpopt_time_snapshot/confirm_snapshot.cpp'), '-o', str(out/'test.o')],
    [clang, '-g', '-pthread', '-fsanitize=address,undefined', str(out/'product.o'), str(out/'test.o'),
     '-o', str(out/'confirm-probe'), '-Wl,-undefined,dynamic_lookup',
     '-L/opt/homebrew/Cellar/boost/1.88.0/lib', '-Wl,-rpath,/opt/homebrew/Cellar/boost/1.88.0/lib',
     '-lboost_filesystem', '-lboost_thread', '-liconv'],
]
results = []
for i, argv in enumerate(commands):
    p = subprocess.run(argv, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out / ('build-%d.log' % i)).write_text(p.stdout)
    results.append({'argv': argv, 'exit': p.returncode})
    if p.returncode:
        (out / 'commands.json').write_text(json.dumps(results, indent=2) + '\n')
        raise SystemExit(p.returncode)
(out / 'commands.json').write_text(json.dumps(results, indent=2) + '\n')
