#!/usr/bin/env python3
# apply_enable.py <idx,...>  : remove DISABLED_ prefix of the given inventory cases and update stale comments.
import json, re, sys
S='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
REPO='/Users/yunjinqi/Documents/new_projects/wondertrader/'
inv=json.load(open(S+'disabled-inventory.json'))
N='迭代03 最终验收复核后启用'
K=lambda n: ('KEEP', n)
SPEC={
 2:[(328,329,['// '+N+'（原禁用：BUG-R56-002）。'])],
 4:[(473,478,['// '+N+'（原禁用：range 索引器下溢，BUG-P09-005 / BUG-P05-007）。'])],
 5:[(535,539,['// '+N+'（原禁用：extractKlineData seconds=0 除零，BUG-P12-002）。'])],
 7:[(165,168,['// '+N+'（原禁用：CMP_V2 历史读端 V2 头指针重分配风险，未运行）。'])],
 8:[(198,201,['// '+N+'（原禁用：CMP_V2 空载荷经 V2 头指针写入的风险，未运行）。'])],
 9:[(219,222,['// '+N+'（原禁用：CMP_V2 读端 V2 头指针重分配风险，R1_wtdatastorage.md §2）。'])],
 10:[(236,240,['// '+N+'（原禁用：CMP_V2 读端 V2 头指针重分配风险，未运行）。'])],
 11:[(242,247,['// '+N+'（原禁用：历史单次运行 signal 9 原因未明，CMP_V2 头指针风险）。'])],
 12:[(217,221,['/* '+N+'（原禁用：BUG-R175-002 V2 装载头指针失效）。 */'])],
 13:[(104,104,['// '+N+'（原禁用：同类 WtDataReader 路径 SIGTRAP，BUG-R74-001）。'])],
 14:[(175,175,['// '+N+'（原禁用：RDM 跨类型缓存缺陷 R1 §2 / R185 动态复现未确认）。'])],
 15:[(280,283,['// '+N+'（原禁用：BUG-R74-001，主记录 BUG-R132-002）。'])],
 16:[(74,78,['// '+N+'（原禁用：BUG-R94-001，主记录 BUG-P05-007）。'])],
 26:[(32,38,['//','// '+N+'（原禁用：BUG-R144-002）。'])],
 27:[(419,420,['// '+N+'（原禁用：BUG-R162-002 同日第二笔 tick 合并日线量额重复累加）。'])],
 28:[(200,206,['//','// '+N+'（原禁用：BUG-R32-001）。'])],
 29:[(233,241,['//','// '+N+'（原禁用：BUG-R144-002）。'])],
 30:[(277,283,['//','// '+N+'（原禁用：BUG-R144-002）。'])],
 31:[(125,129,['//','// '+N+'（原禁用：BUG-R215-002 / BUG-R32-001）。'])],
 32:[(204,207,['// '+N+'（原禁用：BUG-R212-001）。'])],
 34:[(352,358,['// '+N+'（原禁用：R21-BUG-06 空库 K 线读取）。'])],
 48:[(1746,1752,['/* '+N+'（原禁用：BUG-P04-004 / BUG-P04-005 空分钟 CSV）。 */'])],
 49:[(577,586,['/*',' * '+N+'（原禁用：BUG-P04-007 未知 session 模板）。',K(582),K(583),K(584),' */'])],
 54:[(234,236,['// '+N+'（原禁用：BUG-R191-001，主记录 BUG-R64-002）。'])],
 68:[(213,217,[' *   2) SelMocker::handle_replay_done 的 _emit_times 除零：'+N+'。'])],
 72:[(520,523,['/* '+N+'（原禁用：BUG-R205-003）。 */'])],
 85:[],
 96:[(485,486,['// '+N+'（原禁用：BUG-Q05-001 / BUG-R8-002）。'])],
 98:[(594,601,['/*! '+N+'（原禁用：BUG-R45-003，主记录 BUG-P05-002）。 */'])],
 99:[(186,193,['/*! '+N+'（原禁用：BUG-R8-003 默认模块名形态）。 */'])],
 100:[(204,211,['/*! '+N+'（原禁用：无 reader 时委托入口解引用）。 */'])],
 108:[(541,545,['// '+N+'（原禁用：BUG-R24-001 空串标准化代码越界）。'])],
 112:[],
 113:[(447,449,['\t * '+N+'（原禁用：D1 缺 basefiles 空指针，BUG-R104-001）。'])],
 125:[(1826,1826,['\t// '+N+'（原禁用：K2 未知长选项 SIGSEGV）。'])],
 126:[(1832,1832,['\t// '+N+'（原禁用：K3 缺 basefiles 空指针）。'])],
 150:[(128,129,['// '+N+'（原禁用：BUG-N05-002，主记录 BUG-R17-004）。'])],
 161:[(445,448,['/* '+N+'（原禁用：BUG-T4-008 构造未初始化 m_sink）。 */'])],
 166:[(356,356,['/// '+N+'（原禁用：未 registerSpi 时 _bd_mgr 未初始化）。'])],
 170:[(776,781,['/* '+N+'（原禁用：登录失败 post 的 handler 在 sink 摘除后解引用）。 */'])],
 175:[(2168,2174,['/* '+N+'（原禁用：BUG-T25-005 无终帧 disconnect 挂死）。 */'])],
 176:[(2188,2194,['/* '+N+'（原禁用：BUG-T25-001 构造未初始化 m_sink/m_bdMgr）。 */'])],
 183:[(1263,1266,['/* '+N+'（原禁用：OnRspOrderAction 空 pRspInfo 解引用）。 */'])],
 192:[(674,675,['// '+N+'（原禁用：lane「不起线程/不 sleep」约束；本例只起本地撮合线程，无端口）。'])],
 194:[(706,708,['// '+N+'（原禁用：splitVolume 零区间死循环）。'])],
 202:[(427,431,['// '+N+'（原禁用：BUG-R54-002）。'])],
 203:[(426,426,['// '+N+'（原禁用：缺 default 动作组，BUG-N01-001 同源）。'])],
 138:[(228,232,['/* '+N+'（原禁用：BUG-R6-003 未 config 时 createExtParser 解引用 _engine）。 */'])],
 140:[(277,284,['/* '+N+'（原禁用：未 config 引擎上的 cta 仓位变更入口崩溃）。 */'])],
 141:[(294,299,['/* '+N+'（原禁用：未 config 引擎上的 cta 历史切片入口崩溃）。 */'])],
 143:[(435,436,['/* '+N+'（原禁用：未 config 引擎上的 sel 仓位/订阅入口崩溃）。 */'])],
 214:[(129,130,['// '+N+'（原禁用：BUG-R101-001）。'])],
 216:[(208,212,['/* '+N+'（原禁用：BUG-R73-002）。 */'])],
 221:[(9,9,[' * 以及一个 WtSelEngine 后复权标志位用例（'+N+'）。']),(485,490,['// '+N+'（原禁用：BUG-R192-001，WtEngine::get_adjusting_flag 丢弃返回值）。',K(488),K(489)])],
 222:[(439,440,[' * 6. '+N+'（原禁用：BUG-R192-002/003 到期任务局部线程析构 abort、任务 id 序列）。'])],
 223:[(460,461,[' * 7. '+N+'（原禁用：BUG-R31-002；现按卡4b=A 拒绝异族插件，装载返回 true）。'])],
}
def strip_name(test):
    s,n=test.split('.',1)
    return s, n, (s[9:] if s.startswith('DISABLED_') else s), (n[9:] if n.startswith('DISABLED_') else n)
idxs=[int(x) for x in sys.argv[1].split(',')]
byfile={}
for i in idxs:
    it=inv[i-1]
    loc=[l for l in it['locs'] if '/coverage/' in l['file']][0]
    byfile.setdefault(loc['file'],[]).append(i)
for f, ids in byfile.items():
    raw=open(REPO+f,'rb').read()
    crlf=b'\r\n' in raw
    text=raw.decode('utf-8')
    eol='\r\n' if crlf else '\n'
    L=text.split(eol)
    orig=list(L)
    edits=[]
    for i in ids:
        s,n,s2,n2=strip_name(inv[i-1]['test'])
        pat=re.compile(r'(TEST(_F)?\(\s*)'+re.escape(s)+r'(\s*,\s*)'+re.escape(n)+r'(\s*\))')
        hits=[k for k,l in enumerate(orig) if pat.search(l)]
        assert len(hits)==1, (i,f,hits)
        t=hits[0]
        newl=pat.sub(lambda m: m.group(1)+s2+m.group(3)+n2+m.group(4), orig[t])
        edits.append((t+1,t+1,[newl],i))
        for (a,b,new) in SPEC[i]:
            assert a<=b and (b<t+1 or a>t+1), (i,a,b,t+1)
            edits.append((a,b,new,i))
    # check no overlap
    edits.sort(key=lambda e:e[0])
    for x,y in zip(edits,edits[1:]):
        assert x[1]<y[0], ('overlap',f,x[:2],y[:2])
    for (a,b,new,i) in sorted(edits,key=lambda e:-e[0]):
        rep=[orig[v[1]-1] if isinstance(v,tuple) else v for v in new]
        L[a-1:b]=rep
    out=eol.join(L)
    open(REPO+f,'wb').write(out.encode('utf-8'))
    print('edited',f,'crlf' if crlf else 'lf',ids)
