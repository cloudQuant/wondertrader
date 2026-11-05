import sys
R=sys.argv[1]
def rep(path, pairs):
    p=R+'/'+path
    b=open(p,'rb').read()
    for old,new,cnt in pairs:
        n=b.count(old)
        assert n==cnt, (path, old[:70], n)
        b=b.replace(old,new)
    open(p,'wb').write(b)
    print('patched',path)
# K27 (BUG-R13-004) live-engine changes reverted, K25 kept.
rep('WtCore/WtSelEngine.cpp',[
 (b"if (tInfo->_period != TPT_Minute && tInfo->_time != nextTime)", b"if (tInfo->_time != nextTime) /* S3 red: K27 reverted */",1),
 (b"if (sInfo != NULL && tInfo->_time != 0)", b"if (true) /* S3 red: K27 reverted */",1),
 (b"if (_cur_date % 100 == tInfo->_day)", b"if (_cur_date % 1000000 == tInfo->_day)",1),
 (b"(preTDate % 10000 / 100 < _cur_date % 10000 / 100) && _cur_date % 100 > tInfo->_day)", b"(preTDate % 10000 / 100 < _cur_date % 10000 / 100) && _cur_date % 1000000 > tInfo->_day)",1),
 (b"else if (preTDate % 100 < tInfo->_day && _cur_date % 100 > tInfo->_day)", b"else if (preTDate % 1000000 < tInfo->_day && _cur_date % 1000000 > tInfo->_day)",1),
])
# K27 entry validation in createSelContext reverted (HHMMSS etc. accepted again).
rep('WtPorter/WtRtRunner.cpp',[
 (b"\tif (const char* why = wt_sel_schedule::invalidReason(static_cast<uint32_t>(ptype), date, time))", b"\tif (const char* why = (false ? wt_sel_schedule::invalidReason(static_cast<uint32_t>(ptype), date, time) : (const char*)NULL)) /* S3 red: K27 reverted */",1),
])
# I03-NEW-063: SEL open fee charged on the target again.
rep('WtCore/SelStraBaseCtx.cpp',[
 (b"double fee = _engine->calc_fee(stdCode, trdPx, abs(diff), 0);", b"double fee = _engine->calc_fee(stdCode, trdPx, abs(qty), 0); /* S3 red: I03-NEW-063 reverted */",1),
])
# K41 load-time diagnostic removed.
rep('WtCore/WtExecuterFactory.cpp',[
 (b"\t\tif ((plugin->_api.capabilities & WT_EXEC_PLUGIN_V2_CAP_HOST_ORDER_IDENTITY) == 0)\n\t\t\tWTSLogger::warn(", b"\t\tif (false) /* S3 red: K41 diagnostic removed */\n\t\t\tWTSLogger::warn(",1),
])
