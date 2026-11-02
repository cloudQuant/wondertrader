from pathlib import Path
p=Path('src/WtCore/HftStraBaseCtx.cpp'); data=p.read_bytes()
a='\t\tdouble curPos = stra_get_position(stdCode, true);//只读可用持仓\r\n\t\tif (!traderBindingUnchanged(trader, traderEpoch, "sell"))\r\n\t\t\treturn OrderIDs();\r\n'.encode()
b=b'\t\tdouble curPos = 0;\r\n\t\tif (!tryGetBoundPosition(stdCode, true, 3, trader, traderEpoch, curPos))\r\n\t\t\treturn OrderIDs();\r\n'
assert data.count(a)==1
data=data.replace(a,b)
needle=b'double HftStraBaseCtx::stra_get_position(const char* stdCode, bool bOnlyValid /* = false */, int flag /* = 3*/)\r\n'
assert data.count(needle)==1
helper=(
'b ool'.replace(' ','')+' HftStraBaseCtx::tryGetBoundPosition(const char* stdCode, bool onlyValid, int flag,\r\n'
'\tTraderAdapter* expected, uint64_t epoch, double& position)\r\n'
'{\r\n'
'\tCodeHelper::CodeInfo cInfo = CodeHelper::extractStdCode(stdCode, _engine->get_hot_mgr());\r\n'
'\tif (!traderBindingUnchanged(expected, epoch, "sell"))\r\n'
'\t\treturn false;\r\n'
'\tif (strlen(cInfo._ruletag) > 0)\r\n'
'\t{\r\n'
'\t\tstd::string code = _engine->get_hot_mgr()->getCustomRawCode(cInfo._ruletag, cInfo.stdCommID(), _engine->get_trading_date());\r\n'
'\t\tif (!traderBindingUnchanged(expected, epoch, "sell"))\r\n'
'\t\t\treturn false;\r\n'
'\t\tstd::string realCode = CodeHelper::rawMonthCodeToStdCode(code.c_str(), cInfo._exchg);\r\n'
'\t\t_code_map[realCode] = stdCode;\r\n'
'\t\tposition = expected->getPosition(realCode.c_str(), onlyValid, flag);\r\n'
'\t}\r\n'
'\telse\r\n'
'\t\tposition = expected->getPosition(stdCode, onlyValid, flag);\r\n'
'\treturn traderBindingUnchanged(expected, epoch, "sell");\r\n'
'}\r\n\r\n').encode()
p.write_bytes(data.replace(needle,helper+needle))
