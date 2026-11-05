# BUG-R10-004 product fixes: engine run() guards (CTA/SEL/HFT) and SEL tick isolation.
import sys
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/'
def edit(rel, pairs):
    p=ROOT+rel
    raw=open(p,'rb').read()
    bom=raw.startswith(b'\xef\xbb\xbf')
    s=raw.decode('utf-8-sig')
    for old,new in pairs:
        assert s.count(old)==1,(rel,old[:80])
        s=s.replace(old,new)
    out=s.encode('utf-8')
    if bom: out=b'\xef\xbb\xbf'+out
    open(p,'wb').write(out)
def guard(engine):
    return ('\t// BUG-R10-004 (slice1 final gaps): a repeated run() is rejected. Creating a\r\n'
            '\t// second ticker would overwrite _tm_ticker and leak the running ticker and\r\n'
            '\t// its worker, which would keep driving this engine.\r\n'
            '\tif (_tm_ticker != NULL)\r\n\t{\r\n'
            '\t\tWTSLogger::error("'+engine+' engine is already running; repeated run() rejected");\r\n'
            '\t\treturn;\r\n\t}\r\n\r\n')
edit('WtCore/WtSelEngine.cpp', [
 ('void WtSelEngine::run()\r\n{\r\n\tif (!fee_config_valid())', 'void WtSelEngine::run()\r\n{\r\n'+guard('SEL')+'\tif (!fee_config_valid())'),
 ('\t\t\t\t\tSelContextPtr& ctx = (SelContextPtr&)cit->second;\r\n\t\t\t\t\tuint32_t opt = it->second.second;\r\n\r\n\t\t\t\t\tif (opt == 0)\r\n',
  '\t\t\t\t\tSelContextPtr& ctx = (SelContextPtr&)cit->second;\r\n\t\t\t\t\tuint32_t opt = it->second.second;\r\n\r\n'
  '\t\t\t\t\t// BUG-R10-004 / K25=A (slice1 final gaps): a strategy whose tick handler\r\n'
  '\t\t\t\t\t// throws is isolated and reported; the other subscribers still get the tick\r\n'
  '\t\t\t\t\t// and the exception never reaches the ticker strand or the market-data thread.\r\n'
  '\t\t\t\t\ttry\r\n\t\t\t\t\t{\r\n'
  '\t\t\t\t\tif (opt == 0)\r\n'),
 ('\t\t\t\t\t\t\tWTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());\r\n',
  '\t\t\t\t\t\t\tWTSTickData* newTick = WTSTickData::create(curTick->getTickStruct());\r\n'
  '\t\t\t\t\t\t\tstruct TickRelease { WTSTickData* tick; ~TickRelease() { if (tick) tick->release(); } } releaseNewTick{ newTick };\r\n'),
 ('\t\t\t\t\t\t\tctx->on_tick(wCode.c_str(), newTick);\r\n\t\t\t\t\t\t\tnewTick->release();\r\n\t\t\t\t\t\t}\r\n\t\t\t\t\t}\r\n\r\n\t\t\t\t}\r\n',
  '\t\t\t\t\t\t\tctx->on_tick(wCode.c_str(), newTick);\r\n\t\t\t\t\t\t}\r\n\t\t\t\t\t}\r\n'
  '\t\t\t\t\t}\r\n'
  '\t\t\t\t\tcatch (const std::exception& e)\r\n\t\t\t\t\t{\r\n'
  '\t\t\t\t\t\tWTSLogger::error("Tick handler of strategy {} threw on {}: {}", ctx->name(), stdCode, e.what());\r\n'
  '\t\t\t\t\t}\r\n'
  '\t\t\t\t\tcatch (...)\r\n\t\t\t\t\t{\r\n'
  '\t\t\t\t\t\tWTSLogger::error("Tick handler of strategy {} threw an unknown exception on {}", ctx->name(), stdCode);\r\n'
  '\t\t\t\t\t}\r\n\r\n\t\t\t\t}\r\n'),
])
edit('WtCore/WtCtaEngine.cpp', [
 ('void WtCtaEngine::run()\r\n{\r\n\tif (!fee_config_valid())', 'void WtCtaEngine::run()\r\n{\r\n'+guard('CTA')+'\tif (!fee_config_valid())'),
])
edit('WtCore/WtHftEngine.cpp', [
 ('void WtHftEngine::run()\r\n{\r\n\tif (!fee_config_valid())', 'void WtHftEngine::run()\r\n{\r\n'+guard('HFT')+'\tif (!fee_config_valid())'),
])
print('applied r10004')
