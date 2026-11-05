#!/usr/bin/env python3
"""build_red_overlay.py <green-build-dir> <red-out-dir> <before-dir> <repo-src>

Red control for the s5-runners product changes without touching the shared
work tree: a physical copy of src (no symlinks) gets the saved before/ product
files; only the changed product TUs (and the test TUs that #include a changed
product .cpp) are recompiled with the exact green compile commands (source and
object paths rewritten), and each affected test executable is relinked from the
green objects with the red objects substituted. Test TUs that need the new
C API (i03_s5_r42_capi_v2_test.cpp) are dropped from the red porter link.
"""
import os, shlex, shutil, subprocess, sys

green, out, before, src = [os.path.abspath(p) for p in sys.argv[1:5]]
reuse = os.environ.get('RED_REUSE') == '1'
if os.path.exists(out) and not reuse:
    sys.exit('output exists')
os.makedirs(out, exist_ok=True)
red_src = os.path.join(out, 'src')
if not (reuse and os.path.isdir(red_src)):
    subprocess.check_call(['rsync', '-a', '--no-links', src + '/', red_src + '/'])
overlay = [
    'WtPorter/WtRtRunner.cpp', 'WtPorter/WtPorter.cpp', 'WtExecMon/WtExecRunner.cpp',
    'WtHftStraFact/WtHftStraDemo.cpp', 'WtUftStraFact/WtUftStraDemo.cpp',
    'WtRunner/main.cpp', 'WtRunner/WtRunner.cpp', 'WtRunner/WtRunner.h',
    'WtUftRunner/main.cpp', 'WtUftRunner/WtUftRunner.cpp', 'WtUftRunner/WtUftRunner.h',
]
# Headers whose changes are additive and shared with green objects linked into
# the same red executable: keep the current (green) version so layouts match.
shared_headers = ['WtPorter/WtRtRunner.h', 'WtPorter/WtPorter.h', 'WtPorter/ExpHftContext.h']
for rel in overlay:
    shutil.copyfile(os.path.join(before, 'src', rel), os.path.join(red_src, rel))
for rel in shared_headers:
    shutil.copyfile(os.path.join(src, rel), os.path.join(red_src, rel))
links = subprocess.run(['find', red_src, '-type', 'l'], capture_output=True, text=True).stdout.strip()
if links:
    sys.exit('red snapshot contains symlinks')

# target -> (product TUs to rebuild from red src, test TUs to drop from the link)
plan = {
    'CoverageWtHftStraFactPlugin': (['WtHftStraDemo.cpp'], []),
    'CoverageTests_porter': (['WtPorter/WtRtRunner.cpp', 'WtPorter/WtPorter.cpp',
                              'TestUnits/coverage/q06_porter/i03_s5_hft_demo_params_test.cpp'],
                             ['i03_s5_r42_capi_v2_test.cpp.o']),
    'CoverageTests_exefact': (['WtExecMon/WtExecRunner.cpp'], []),
    'CoverageTests_m_WtUftRunner': (['TestUnits/coverage/m_WtUftRunner/i03_s5_uft_demo_params_test.cpp'], []),
    'WtRunner': (['WtRunner/main.cpp', 'WtRunner/WtRunner.cpp'], []),
    'WtUftRunner': (['WtUftRunner/main.cpp', 'WtUftRunner/WtUftRunner.cpp'], []),
}
red_plugin = os.path.join(out, 'bin', 'libWtHftStraFact.dylib')
only = os.environ.get('RED_TARGETS')
if only:
    plan = {k: v for k, v in plan.items() if k in only.split(',')}
log = open(os.path.join(out, 'red-build.log'), 'a')
for target, (tus, drop) in plan.items():
    cmds = subprocess.run(['ninja', '-C', green, '-t', 'commands', target],
                          capture_output=True, text=True, check=True).stdout.splitlines()
    compile_cmds = [c for c in cmds if ' -c ' in c]
    link_cmd = cmds[-1]
    link_objs = set(shlex.split([seg.strip() for seg in link_cmd.split('&&') if ' -o ' in seg][0]))
    # Only compile commands whose object is linked into this target (a TU may
    # also be compiled into a product library with the same source path).
    compile_cmds = [c for c in compile_cmds
                    if shlex.split(c)[shlex.split(c).index('-o') + 1] in link_objs]
    substitutions = {}
    for tu in tus:
        matches = [c for c in compile_cmds if c.rstrip().endswith('/' + tu) or ('/' + tu + ' ') in c]
        # the HFT plugin compiles a configure_file copy from the build tree
        if not matches:
            sys.exit('no compile command for %s in %s' % (tu, target))
        cmd = matches[-1]
        args = shlex.split(cmd)
        o_index = args.index('-o') + 1
        green_obj = args[o_index]
        red_obj = os.path.join(out, target, os.path.basename(green_obj))
        os.makedirs(os.path.dirname(red_obj), exist_ok=True)
        args[o_index] = red_obj
        c_index = args.index('-c') + 1
        source = args[c_index]
        if tu == 'WtHftStraDemo.cpp':
            red_source = os.path.join(out, 'hft_plugin', 'WtHftStraDemo.cpp')
            os.makedirs(os.path.dirname(red_source), exist_ok=True)
            shutil.copyfile(os.path.join(red_src, 'WtHftStraFact', 'WtHftStraDemo.cpp'), red_source)
        else:
            red_source = os.path.join(red_src, tu)
        args[c_index] = red_source
        if tu.endswith('i03_s5_hft_demo_params_test.cpp'):
            args = ['-DWT_R6_HFT_FACTORY_PLUGIN="%s"' % red_plugin if a.startswith('-DWT_R6_HFT_FACTORY_PLUGIN=') else a
                    for a in args]
        args = [a for a in args if a not in ('-MD',)]
        if '-MF' in args:
            i = args.index('-MF'); del args[i:i + 2]
        if '-MT' in args:
            i = args.index('-MT'); del args[i:i + 2]
        log.write('$ ' + ' '.join(shlex.quote(a) for a in args) + '\n'); log.flush()
        subprocess.check_call(args, cwd=green, stdout=log, stderr=log)
        substitutions[green_obj] = red_obj
    segments = [seg.strip() for seg in link_cmd.split('&&') if ' -o ' in seg]
    largs = shlex.split(segments[0])
    new_largs = []
    for a in largs:
        if a in substitutions:
            new_largs.append(substitutions[a]); continue
        if any(a.endswith(d) for d in drop):
            continue
        new_largs.append(a)
    o_index = new_largs.index('-o') + 1
    red_out = os.path.join(out, 'bin', os.path.basename(new_largs[o_index]))
    os.makedirs(os.path.dirname(red_out), exist_ok=True)
    new_largs[o_index] = red_out
    log.write('$ ' + ' '.join(shlex.quote(a) for a in new_largs) + '\n'); log.flush()
    subprocess.check_call(new_largs, cwd=green, stdout=log, stderr=log)
    print(target, '->', red_out)
