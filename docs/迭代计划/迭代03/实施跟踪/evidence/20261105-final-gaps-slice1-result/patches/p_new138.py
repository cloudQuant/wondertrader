# I03-NEW-138: _stat_map and every WTSTradeStateInfo counter under one leaf lock (_mtx_stat).
import os, re, sys
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/WtCore/'
def load(p):
    raw=open(p,'rb').read(); bom=raw.startswith(b'\xef\xbb\xbf'); s=raw.decode('utf-8-sig')
    crlf=s.count('\r\n'); lf=s.count('\n')-crlf
    return bom, s.replace('\r\n','\n'), crlf, lf
def save(p,bom,s,mode):
    if mode=='crlf': s=s.replace('\n','\r\n')
    out=(b'\xef\xbb\xbf' if bom else b'')+s.encode('utf-8')
    tmp=p+'.s1tmp'; open(tmp,'wb').write(out); os.chmod(tmp,0o644); os.replace(tmp,p)
def rep(s,old,new,label):
    n=s.count(old); assert n==1,(label,n); return s.replace(old,new)

# ---------------- header (mixed endings: keep untouched lines byte-identical)
hp=ROOT+'TraderAdapter.h'; hraw=open(hp,'rb').read()
old=b'\tTradeStatMap*\t_stat_map;\t///< \xe7\xbb\x9f\xe8\xae\xa1\xe6\x95\xb0\xe6\x8d\xae'
assert hraw.count(old)==1
i=hraw.index(old); j=hraw.index(b'\n',i); eol=b'\r\n' if hraw[j-1:j]==b'\r' else b'\n'
ins=(eol+b'\t// I03-NEW-138: guards _stat_map (lookup, insert, iteration) and every'+eol+
     b'\t// WTSTradeStateInfo counter. Leaf lock: nothing else is locked and no'+eol+
     b'\t// external code is called while it is held.'+eol+
     b'\tmutable SpinMutex\t_mtx_stat;')
hraw=hraw[:j]+ins+hraw[j:]
tmp=hp+'.s1tmp'; open(tmp,'wb').write(hraw); os.chmod(tmp,0o644); os.replace(tmp,hp)

# ---------------- cpp
p=ROOT+'TraderAdapter.cpp'; bom,s,crlf,lf=load(p); assert lf==0
GETC=lambda code: ('WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(%s);' % code)

# helper after makeLocalOrderID block: put before "inline const char* formatAction"
s=rep(s,'inline const char* formatAction(WTSDirectionType dType, WTSOffsetType oType)\n',
'''// I03-NEW-138: find-or-create the statistics entry of a code. The caller holds
// TraderAdapter::_mtx_stat, which guards the map and every counter of its entries.
static WTSTradeStateInfo* statInfoLocked(WTSHashMap<std::string>* statMap, const std::string& stdCode)
{
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)statMap->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode.c_str());
		statMap->add(stdCode, statInfo, false);
	}
	return statInfo;
}

inline const char* formatAction(WTSDirectionType dType, WTSOffsetType oType)
''','helper')

# destructor
s=rep(s,'''	if (_stat_map)
		_stat_map->release();
''','''	TradeStatMap* statMap = NULL;
	{
		SpinLock statLock(_mtx_stat);
		statMap = _stat_map;
		_stat_map = NULL;
	}
	if (statMap)
		statMap->release();
''','dtor')

# run()
s=rep(s,'''	if (_stat_map == NULL)
		_stat_map = TradeStatMap::create();
''','''	{
		SpinLock statLock(_mtx_stat);
		if (_stat_map == NULL)
			_stat_map = TradeStatMap::create();
	}
''','run')

# checkCancelLimits / checkOrderLimits
for kind in ('cancel','order'):
    old='''		WTSTradeStateInfo* statInfo = _stat_map ? (WTSTradeStateInfo*)_stat_map->get(stdCode) : NULL;
		if (riskPara && statInfo && riskPara->_%s_total_limits != 0 &&
			statInfo->total_%ss() >= riskPara->_%s_total_limits)
''' % (kind,kind,kind)
    new='''		bool hasStat = false;
		uint32_t total = 0;
		{
			// I03-NEW-138: SPI threads insert into _stat_map and update the counters.
			SpinLock statLock(_mtx_stat);
			WTSTradeStateInfo* statInfo = _stat_map ? (WTSTradeStateInfo*)_stat_map->get(stdCode) : NULL;
			if (statInfo != NULL)
			{
				hasStat = true;
				total = statInfo->total_%ss();
			}
		}
		if (riskPara && hasStat && riskPara->_%s_total_limits != 0 &&
			total >= riskPara->_%s_total_limits)
''' % (kind,kind,kind)
    s=rep(s,old,new,'limits-'+kind)

# buy / sell: snapshot at the start of each open-rule check
old_bs='''	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode);
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode);
		_stat_map->add(stdCode, statInfo, false);
	}
	TradeStatInfo& statItem = statInfo->statInfo();
'''
assert s.count(old_bs)==2
new_bs='''	// I03-NEW-138: the statistics entry is created here as before; its counters are
	// read as a locked snapshot at each limit check (orders submitted below may
	// update them synchronously from the provider callback).
	{
		SpinLock statLock(_mtx_stat);
		statInfoLocked(_stat_map, stdCode);
	}
	auto statSnapshot = [this, stdCode]() {
		SpinLock statLock(_mtx_stat);
		return statInfoLocked(_stat_map, stdCode)->statInfo();
	};
'''
s=s.replace(old_bs,new_bs)
for side in ('l','s'):
    old='''			//先检查是否已经到了限额
			//%s
			double maxQty = left;
''' 
# insert snapshot at the start of each AT_Open branch body: anchor 'double maxQty = left;' within open branches using statItem
cnt=0
out=[]; pos=0
for m in re.finditer(r'\n(\t\t\tdouble maxQty = left;\n)', s):
    # only branches whose following ~1200 chars use statItem.*openvol
    seg=s[m.end():m.end()+1500]
    nxt=seg.find('double maxQty = left;')
    body=seg if nxt<0 else seg[:nxt]
    if 'statItem.' in body:
        out.append(s[pos:m.start()]+'\n\t\t\tconst TradeStatInfo statItem = statSnapshot();\n'+m.group(1)); pos=m.end(); cnt+=1
out.append(s[pos:]); s=''.join(out)
assert cnt==2,cnt

# reconcile orders
old='''			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())'''
new='''			{
			// I03-NEW-138: statistics under their own leaf lock.
			SpinLock statLock(_mtx_stat);
			TradeStatInfo& statItem = statInfoLocked(_stat_map, stdCode)->statInfo();
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();

				if (orderInfo->isError())'''
s=rep(s,old,new,'rec-orders-open')
old='''					else
					{
						statItem.s_auto_cancels++;
						statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
				}
			}			

			if (taggedId == 0)
				continue;'''
new='''					else
					{
						statItem.s_auto_cancels++;
						statItem.s_auto_canclqty += orderInfo->getVolume() - orderInfo->getVolTraded();
					}
				}
			}			
			}

			if (taggedId == 0)
				continue;'''
s=rep(s,old,new,'rec-orders-close')

# reconcile trades
old='''			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if(statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();

			bool isLong'''
new='''			bool isLong'''
s=rep(s,old,new,'rec-trades-open')
old='''			if (isLong)
			{
				if (isOpen)
					statItem.l_openvol += qty;'''
new='''			{
			// I03-NEW-138: statistics under their own leaf lock (taken after the
			// ledger lock above has been released).
			SpinLock statLock(_mtx_stat);
			TradeStatInfo& statItem = statInfoLocked(_stat_map, stdCode)->statInfo();
			if (isLong)
			{
				if (isOpen)
					statItem.l_openvol += qty;'''
s=rep(s,old,new,'rec-trades-mid')
old='''					statItem.s_closevol += qty;
			}

			checkSelfMatch(stdCode.c_str(), tInfo);'''
new='''					statItem.s_closevol += qty;
			}
			}

			checkSelfMatch(stdCode.c_str(), tInfo);'''
s=rep(s,old,new,'rec-trades-close')

# stats log loop
old='''		for (auto it = _stat_map->begin(); it != _stat_map->end(); it++)
		{
			const char* stdCode = it->first.c_str();
			WTSTradeStateInfo* pItem = (WTSTradeStateInfo*)it->second;
			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
				"[{}] {} action stats updated, long opened: {}, long closed: {}, new long closed: {}, short opened: {}, short closed: {}, new short closed: {}",
				_id.c_str(), stdCode, pItem->open_volume_long(), pItem->close_volume_long(), pItem->closet_volume_long(),
				pItem->open_volume_short(), pItem->close_volume_short(), pItem->closet_volume_short());
		}'''
new='''		// I03-NEW-138: copy the statistics under their lock, log after releasing it.
		std::vector<std::pair<std::string, TradeStatInfo>> statsSnapshot;
		{
			SpinLock statLock(_mtx_stat);
			if (_stat_map != NULL)
			{
				statsSnapshot.reserve(_stat_map->size());
				for (auto it = _stat_map->begin(); it != _stat_map->end(); it++)
					statsSnapshot.emplace_back(it->first, ((WTSTradeStateInfo*)it->second)->statInfo());
			}
		}
		for (const auto& item : statsSnapshot)
		{
			const char* stdCode = item.first.c_str();
			const TradeStatInfo& pItem = item.second;
			WTSLogger::log_dyn("trader", _id.c_str(), LL_INFO, 
				"[{}] {} action stats updated, long opened: {}, long closed: {}, new long closed: {}, short opened: {}, short closed: {}, new short closed: {}",
				_id.c_str(), stdCode, pItem.l_openvol, pItem.l_closevol, pItem.l_closetvol,
				pItem.s_openvol, pItem.s_closevol, pItem.s_closetvol);
		}'''
s=rep(s,old,new,'stats-log')

# onPushOrder canceled
old='''	if (orderInfo->getOrderState() == WOS_Canceled)
	{
		WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
		if (statInfo == NULL)
		{
			statInfo = WTSTradeStateInfo::create(stdCode.c_str());
			_stat_map->add(stdCode, statInfo, false);
		}
		TradeStatInfo& statItem = statInfo->statInfo();
'''
new='''	if (orderInfo->getOrderState() == WOS_Canceled)
	{
		// I03-NEW-138: statistics under their own leaf lock (only counters below).
		SpinLock statLock(_mtx_stat);
		TradeStatInfo& statItem = statInfoLocked(_stat_map, stdCode)->statInfo();
'''
s=rep(s,old,new,'push-cancel')

# onPushOrder first push
old='''			WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
			if (statInfo == NULL)
			{
				statInfo = WTSTradeStateInfo::create(stdCode.c_str());
				_stat_map->add(stdCode, statInfo, false);
			}
			TradeStatInfo& statItem = statInfo->statInfo();
			if (isBuy)
			{
				statItem.b_orders++;
				statItem.b_ordqty += orderInfo->getVolume();
			}
			else
			{
				statItem.s_orders++;
				statItem.s_ordqty += orderInfo->getVolume();
			}
'''
new='''			{
				// I03-NEW-138: statistics under their own leaf lock.
				SpinLock statLock(_mtx_stat);
				TradeStatInfo& statItem = statInfoLocked(_stat_map, stdCode)->statInfo();
				if (isBuy)
				{
					statItem.b_orders++;
					statItem.b_ordqty += orderInfo->getVolume();
				}
				else
				{
					statItem.s_orders++;
					statItem.s_ordqty += orderInfo->getVolume();
				}
			}
'''
s=rep(s,old,new,'push-first')

# onPushTrade: accumulate into a local delta, apply under the lock afterwards
old='''	PosItem& pItem = _positions[stdCode];
	WTSTradeStateInfo* statInfo = (WTSTradeStateInfo*)_stat_map->get(stdCode.c_str());
	if (statInfo == NULL)
	{
		statInfo = WTSTradeStateInfo::create(stdCode.c_str());
		_stat_map->add(stdCode, statInfo, false);
	}

	// A trade already counted by the trade query keeps its position effect but
	// is not counted in the statistics twice.
	TradeStatInfo uncountedStats;
	TradeStatInfo& statItem = countStats ? statInfo->statInfo() : uncountedStats;
'''
new='''	PosItem& pItem = _positions[stdCode];

	// A trade already counted by the trade query keeps its position effect but
	// is not counted in the statistics twice.
	// I03-NEW-138: the volumes are accumulated locally and applied to the shared
	// statistics entry under _mtx_stat once the position update is done.
	TradeStatInfo statItem;
'''
s=rep(s,old,new,'trade-open')
old='''		pItem.l_newavail = pItem.l_newvol;
		pItem.l_preavail = pItem.l_prevol;
		pItem.s_newavail = pItem.s_newvol;
		pItem.s_preavail = pItem.s_prevol;
	}
	
	printPosition(stdCode.c_str(), pItem);
'''
new='''		pItem.l_newavail = pItem.l_newvol;
		pItem.l_preavail = pItem.l_prevol;
		pItem.s_newavail = pItem.s_newvol;
		pItem.s_preavail = pItem.s_prevol;
	}
	{
		SpinLock statLock(_mtx_stat);
		TradeStatInfo& shared = statInfoLocked(_stat_map, stdCode)->statInfo();
		if (countStats)
		{
			shared.l_openvol += statItem.l_openvol;
			shared.l_closevol += statItem.l_closevol;
			shared.l_closetvol += statItem.l_closetvol;
			shared.s_openvol += statItem.s_openvol;
			shared.s_closevol += statItem.s_closevol;
			shared.s_closetvol += statItem.s_closetvol;
		}
	}
	
	printPosition(stdCode.c_str(), pItem);
'''
s=rep(s,old,new,'trade-apply')
assert '_stat_map->get(' not in s.replace('(WTSTradeStateInfo*)_stat_map->get(stdCode) : NULL','').replace('statMap->get(stdCode)','') , 'unconverted site'
save(p,bom,s,'crlf')
print('patched')
