# Card 8 (BUG-P02-005, slice1 final gaps): whole-lot providers reject non-integral
# quantities before the SDK instead of truncating through the integer SDK field.
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/'
def eol_of(b, i):
    j=b.find(b'\n', i)
    return b'\r\n' if j > 0 and b[j-1:j] == b'\r' else b'\n'
def helper(e):
    return (b'// Card 8 (BUG-P02-005, slice1 final gaps): this provider trades whole lots. A'+e+
            b'// quantity that is not finite, not positive, larger than the SDK int field or'+e+
            b'// fractional is rejected before any order identity is consumed: the strategy'+e+
            b'// rounds, the adapter never truncates through the integer SDK field.'+e+
            b'static bool isWholeLotVolume(double volume)'+e+b'{'+e+
            b'\treturn std::isfinite(volume) && volume >= 1 && volume <= (double)INT_MAX && std::floor(volume) == volume;'+e+
            b'}'+e+e)
def block(e, tag, sink, guard_null=False):
    cond=b'entrust == NULL || !isWholeLotVolume(entrust->getVolume())' if guard_null else b'!isWholeLotVolume(entrust->getVolume())'
    return (b'\tif ('+cond+b')'+e+b'\t{'+e+
            b'\t\twrite_log('+sink+b', LL_ERROR, "['+tag+b'] Order rejected: quantity {} is not a positive whole number of lots", entrust != NULL ? entrust->getVolume() : 0.0);'+e+
            b'\t\treturn -1;'+e+b'\t}'+e)
def patch(rel, func_markers, anchors, tag, sink, guard_null=False, insert_before=False):
    p=ROOT+rel; b=open(p,'rb').read()
    # helper before the first function marker
    fm=func_markers[0].encode(); assert b.count(fm)==1,(rel,fm)
    i=b.index(fm); e=eol_of(b,i)
    b=b[:i]+helper(e)+b[i:]
    for fmark, anchor in zip(func_markers, anchors):
        fm=fmark.encode(); assert b.count(fm)==1,(rel,fm)
        start=b.index(fm)
        a=anchor.encode(); k=b.find(a, start); assert k>start,(rel,anchor)
        e=eol_of(b,k)
        if insert_before:
            b=b[:k]+block(e,tag.encode(),sink.encode(),guard_null)+e+b[k:]
        else:
            # anchor is "...return -1;" : insert after that full line
            end=b.index(b'\n',k+len(a))+1
            b=b[:end]+e+block(e,tag.encode(),sink.encode(),guard_null)+b[end:]
    for inc in (b'#include <climits>', b'#include <cmath>'):
        if inc not in b:
            k=b.index(b'#include '); e=eol_of(b,k)
            b=b[:k]+inc+e+b[k:]
    open(p,'wb').write(b)
    print('patched',rel)

patch('TraderCTPMini/TraderCTPMini.cpp',
      ['int TraderCTPMini::orderInsert(WTSEntrust* entrust)'],
      ['\tif (ct == NULL)\n\t\treturn -1;'],
      'TraderCTPMini','currentSink()')
patch('TraderCTPOpt/TraderCTPOpt.cpp',
      ['int TraderCTPOpt::orderInsertOpt(WTSEntrust* entrust)','int TraderCTPOpt::orderInsert(WTSEntrust* entrust)'],
      ['\tif (entrust == NULL)\n\t\treturn -1;','\tif (ct == NULL)\n\t\treturn -1;'],
      'TraderCTPOpt','currentSink()')
patch('TraderFemas/TraderFemas.cpp',
      ['int TraderFemas::orderInsert(WTSEntrust* entrust)'],
      ['\tif (ct == NULL)\n\t\treturn -1;'],
      'TraderFemas','m_sink')
patch('TraderYD/TraderYD.cpp',
      ['int TraderYD::orderInsert(WTSEntrust* entrust)'],
      ['\tconst YDInstrument* pInst = m_pUserAPI->getInstrumentByID(entrust->getCode());'],
      'TraderYD','m_sink', guard_null=True, insert_before=True)
