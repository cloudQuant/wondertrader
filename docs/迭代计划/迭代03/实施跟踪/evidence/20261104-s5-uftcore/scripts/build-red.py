#!/usr/bin/env python3
"""Red-control overlay build: compile the uftcore and m_WtUftRunner coverage targets
against the pre-change WtUftCore sources (evidence before/ copies) with the current
tests, reusing the exact compile/link commands of /tmp/wt-i03-s5-uftcore-cov."""
import os, shutil, subprocess, sys, shlex, concurrent.futures as cf
SRC='/Users/yunjinqi/Documents/new_projects/wondertrader/src'
BEFORE=sys.argv[1]  # evidence before dir (contains src/...)
BUILD='/tmp/wt-i03-s5-uftcore-cov'
RED=os.environ.get('RED_DIR','/tmp/wt-i03-s5-uftcore-red')
OV=RED+'/overlay'
shutil.rmtree(OV, ignore_errors=True)
shutil.copytree(SRC+'/WtUftCore', OV+'/WtUftCore', symlinks=False)
shutil.copytree(SRC+'/WtUftRunner', OV+'/WtUftRunner', symlinks=False)
for f in sorted(os.listdir(f'{BEFORE}/src/WtUftCore')):
    shutil.copy2(f'{BEFORE}/src/WtUftCore/{f}', f'{OV}/WtUftCore/{f}')
def q(target):
    out=subprocess.run(['ninja','-C',BUILD,'-t','query',target],capture_output=True,text=True).stdout
    return [l.strip() for l in out.splitlines() if l.strip().endswith('.o') and not l.strip().startswith('|')]
def cmd(obj):
    c=subprocess.run(['ninja','-C',BUILD,'-t','commands',obj],capture_output=True,text=True).stdout.strip().splitlines()[-1]
    return c
jobs=[]
targets={'uft':'build_x64/Debug/bin/TestUnits/CoverageTests_uftcore','runner':'build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner','lib':'build_x64/Debug/libs/libWtUftCore.a'}
objs={k:q(v) for k,v in targets.items()}
mapping={}
for k,lst in objs.items():
    for o in lst:
        c=shlex.split(cmd(o))
        src=c[c.index('-c')+1]
        if src.startswith(SRC+'/WtUftCore/'): src=OV+'/WtUftCore/'+src[len(SRC+'/WtUftCore/'):]
        if src.startswith(SRC+'/WtUftRunner/'): src=OV+'/WtUftRunner/'+src[len(SRC+'/WtUftRunner/'):]
        newo=RED+'/obj/'+o
        os.makedirs(os.path.dirname(newo),exist_ok=True)
        nc=[c[0],'-I'+OV,'-I'+SRC+'/WtUftCore']
        i=1
        while i<len(c):
            a=c[i]
            if a in ('-MD',): i+=1; continue
            if a in ('-MT','-MF'): i+=2; continue
            if a=='-o': nc+=['-o',newo]; i+=2; continue
            if a=='-c': nc+=['-c',src]; i+=2; continue
            nc.append(a); i+=1
        mapping[o]=newo
        jobs.append(nc)
def run(nc):
    r=subprocess.run(nc,cwd=BUILD,capture_output=True,text=True)
    return r.returncode,' '.join(nc[-1:]),r.stderr[-2000:]
fails=0
if os.environ.get('SKIP_COMPILE')!='1':
    with cf.ThreadPoolExecutor(3) as ex:
        for rc,src,err in ex.map(run,jobs):
            if rc!=0: fails+=1; print('COMPILE FAIL',src,err)
    print('compiled',len(jobs),'fail',fails)
# archive before-lib
lib=RED+'/libWtUftCore.before.a'
if os.path.exists(lib): os.remove(lib)
subprocess.run(['ar','qc',lib]+[mapping[o] for o in objs['lib']],check=True)
subprocess.run(['ranlib',lib],check=True)
def link(target,out,drop=()):
    c=shlex.split(cmd(targets[target]))
    if c and c[0]==':': c=c[2:]
    nc=[]
    i=0
    while i<len(c):
        a=c[i]
        if a in mapping: nc.append(mapping[a]); i+=1; continue
        if a=='build_x64/Debug/libs/libWtUftCore.a': nc.append(lib); i+=1; continue
        if a in drop: i+=1; continue
        if a=='-o': nc+=['-o',out]; i+=2; continue
        if a=='&&': break
        nc.append(a); i+=1
    r=subprocess.run(nc,cwd=BUILD,capture_output=True,text=True)
    print('link',target,r.returncode,r.stderr[-1500:])
link('uft',RED+'/CoverageTests_uftcore.red',drop=('build_x64/Debug/bin/WtDtPorter/libWtDtPorter.dylib','build_x64/Debug/bin/WtPorter/libWtPorter.dylib'))
link('runner',BUILD+'/build_x64/Debug/bin/TestUnits/CoverageTests_m_WtUftRunner'+os.environ.get('RED_SUFFIX','.red'))
