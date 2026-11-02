from pathlib import Path
p=Path('src/WtCore/HftStraBaseCtx.cpp'); data=p.read_bytes()
a=b'void HftStraBaseCtx::setTrader(TraderAdapter* trader)\r\n{\r\n\t_trader = trader;\r\n}'
b=b'void HftStraBaseCtx::setTrader(TraderAdapter* trader)\r\n{\r\n\t_trader = trader;\r\n\t++_trader_binding_epoch;\r\n}'
assert data.count(a)==1
data=data.replace(a,b)
segments=[('bool','cancel','OrderIDs HftStraBaseCtx::stra_cancel','false'),('OrderIDs','cancel','const char* HftStraBaseCtx::get_inner_code','OrderIDs()'),('OrderIDs','buy','OrderIDs HftStraBaseCtx::stra_sell','OrderIDs()'),('OrderIDs','sell','uint32_t HftStraBaseCtx::stra_enter_long','OrderIDs()'),('uint32_t','enter_long','uint32_t HftStraBaseCtx::stra_exit_long','0'),('uint32_t','exit_long','uint32_t HftStraBaseCtx::stra_enter_short','0'),('uint32_t','enter_short','uint32_t HftStraBaseCtx::stra_exit_short','0'),('uint32_t','exit_short','WTSCommodityInfo* HftStraBaseCtx::stra_get_comminfo','0')]
for ret,name,nextfn,result in segments:
 start=data.index((ret+' HftStraBaseCtx::stra_'+name+'(').encode()); end=data.index(nextfn.encode(),start+1)
 s=data[start:end]
 old=b'\tif (_trader == nullptr)\r\n'
 assert s.count(old)==1,(name,s.count(old))
 capture=b'\tTraderAdapter* const trader = _trader;\r\n'
 if ret!='bool': capture+=b'\tconst uint64_t traderEpoch = _trader_binding_epoch;\r\n'
 s=s.replace(old,capture+b'\tif (trader == nullptr)\r\n',1)
 s=s.replace(b'_trader->',b'trader->').replace(b'_trader && ',b'trader && ')
 guard=(b'\tif (!traderBindingUnchanged(trader, traderEpoch, "'+name.encode()+b'"))\r\n\t\treturn '+result.encode()+b';\r\n')
 if name=='cancel' and ret=='OrderIDs':
  needle=b'\treturn trader->cancel(stdCode, isBuy, qty);'
  assert s.count(needle)==1
  s=s.replace(needle,guard+b'\r\n'+needle)
 if name in ('buy','sell','enter_long','exit_long','enter_short','exit_short'):
  needle=b'\tCodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());\r\n'
  assert s.count(needle)==1,(name,'extract')
  s=s.replace(needle,needle+guard,1)
  needle=b'\t\tstd::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());\r\n'
  assert s.count(needle)==1,(name,'custom')
  s=s.replace(needle,needle+b'\t'+guard.replace(b'\r\n\t\treturn',b'\r\n\t\t\treturn'),1)
 if name in ('buy','sell'):
  import re
  pattern=rb'(\t\tWTSContractInfo\* ct = _engine->get_basedata_mgr\(\)->getContract\([^\r\n]*\);\r\n)'
  s,count=re.subn(pattern,lambda m:m.group(1)+b'\t'+guard.replace(b'\r\n\t\treturn',b'\r\n\t\t\treturn'),s)
  assert count==2,(name,'contracts',count)
  needle=b'\t\tauto ids = trader->'+name.encode()+b'('
  assert s.count(needle)==2,(name,'action')
  s=s.replace(needle,b'\t'+guard.replace(b'\r\n\t\treturn',b'\r\n\t\t\treturn')+b'\r\n'+needle)
 if name=='sell':
  needle=b'\tWTSCommodityInfo* commInfo = _engine->get_basedata_mgr()->getCommodity(cInfo._exchg, cInfo._product);\r\n'
  assert s.count(needle)==1
  s=s.replace(needle,needle+guard,1)
  needle='\t\tdouble curPos = stra_get_position(stdCode, true);//只读可用持仓\r\n'.encode()
  assert s.count(needle)==1
  s=s.replace(needle,needle+b'\t'+guard.replace(b'\r\n\t\treturn',b'\r\n\t\t\treturn'),1)
 if name in ('enter_long','exit_long','enter_short','exit_short'):
  lookup={'enter_long':'openLong','exit_long':'closeLong','enter_short':'openShort','exit_short':'closeShort'}
  needle=b'\treturn trader->'+lookup[name].encode()+b'('
  assert s.count(needle)==1
  s=s.replace(needle,guard+b'\r\n'+needle)
 data=data[:start]+s+data[end:]
p.write_bytes(data)
