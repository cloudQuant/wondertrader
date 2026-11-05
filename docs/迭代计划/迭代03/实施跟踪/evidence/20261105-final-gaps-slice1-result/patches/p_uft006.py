# I03-NEW-006 UFT part. mode: 'header' | 'cpp' | 'redvariant OUT'
import os, sys
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/WtUftCore/'
def load(p):
    raw=open(p,'rb').read(); bom=raw.startswith(b'\xef\xbb\xbf'); s=raw.decode('utf-8-sig')
    assert '\n' not in s.replace('\r\n',''), p
    return bom, s.replace('\r\n','\n')
def save(p,bom,s):
    out=(b'\xef\xbb\xbf' if bom else b'')+s.replace('\n','\r\n').encode('utf-8')
    tmp=p+'.s1tmp'; open(tmp,'wb').write(out); os.chmod(tmp,0o644); os.replace(tmp,p)
def rep(s,old,new):
    assert s.count(old)==1,(old[:90]); return s.replace(old,new)
HDR_PUBLIC_OLD='''	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

'''
HDR_PUBLIC_NEW='''	bool initExt(const char* id, ITraderApi* api, IBaseDataMgr* bdMgr, ActionPolicyMgr* policyMgr);

	// I03-NEW-006 (slice1 final gaps): checked clock used for the local order id.
	// UFT has no identity journal, so every entry takes one snapshot and a failed
	// snapshot rejects the entry before the provider call. Test seam: NULL restores
	// the default clock.
	typedef bool (*OrderClock)(uint32_t& date, uint32_t& time, uint64_t& epochMillis);
	void setOrderClockForTest(OrderClock clock);

'''
HDR_PRIV_OLD='''	RiskParamsMap	_risk_params_map;
	bool			_risk_mon_enabled;
};
'''
HDR_PRIV_NEW='''	RiskParamsMap	_risk_params_map;
	bool			_risk_mon_enabled;

	OrderClock		_order_clock = &TraderAdapter::defaultOrderClock;
	static bool defaultOrderClock(uint32_t& date, uint32_t& time, uint64_t& epochMillis);
};
'''
SEAM_OLD='''TraderAdapter::TraderAdapter()
	: _cfg(NULL)'''
SEAM_NEW='''bool TraderAdapter::defaultOrderClock(uint32_t& date, uint32_t& time, uint64_t& epochMillis)
{
	return TimeUtils::tryGetDateTime(date, time, epochMillis);
}

void TraderAdapter::setOrderClockForTest(OrderClock clock)
{
	_order_clock = clock != NULL ? clock : &TraderAdapter::defaultOrderClock;
}

TraderAdapter::TraderAdapter()
	: _cfg(NULL)'''
ID_OLD='''inline uint32_t makeLocalOrderID()
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	// Local ID 0 is reserved as the no-order sentinel; keep initialization state separate.
	static std::once_flag _auto_order_id_once;
	std::call_once(_auto_order_id_once, []() {
		uint32_t curYear = TimeUtils::getCurDate() / 10000 * 10000 + 101;
		uint32_t seed = (uint32_t)((TimeUtils::getLocalTimeNow() - TimeUtils::makeTime(curYear, 0)) / 1000 * 50);
		if (seed == 0)
			seed = 1;
		_auto_order_id.store(seed, std::memory_order_relaxed);
	});

	return _auto_order_id.fetch_add(1, std::memory_order_relaxed);
}
'''
ID_NEW='''// I03-NEW-006 (slice1 final gaps), same rule as WtCore: the process-wide id seed
// comes from the checked clock snapshot of the first entry that has one. Until a
// seed exists no id is allocated (0 is the no-order sentinel), so a failed clock
// never freezes a seed derived from failed values and a later entry retries. The
// sequence skips 0 on wrap.
static uint32_t makeLocalOrderID(uint32_t curDate, uint64_t epochMillis)
{
	static std::atomic<uint32_t> _auto_order_id{ 0 };
	static std::atomic<bool> _seeded{ false };
	static std::mutex _seed_mutex;
	if (!_seeded.load(std::memory_order_acquire))
	{
		std::lock_guard<std::mutex> lock(_seed_mutex);
		if (!_seeded.load(std::memory_order_relaxed))
		{
			const int64_t yearStart = TimeUtils::makeTime((long)(curDate / 10000 * 10000 + 101), 0);
			if (yearStart <= 0 || (int64_t)epochMillis < yearStart)
				return 0;
			uint32_t seed = (uint32_t)(((int64_t)epochMillis - yearStart) / 1000 * 50);
			if (seed == 0)
				seed = 1;
			_auto_order_id.store(seed, std::memory_order_relaxed);
			_seeded.store(true, std::memory_order_release);
		}
	}

	uint32_t id = _auto_order_id.fetch_add(1, std::memory_order_relaxed);
	if (id == 0)
		id = _auto_order_id.fetch_add(1, std::memory_order_relaxed);
	return id;
}
'''
GATE_OLD='''	uint32_t localid = makeLocalOrderID();
	char* usertag = entrust->getUserTag();'''
GATE_NEW='''	// I03-NEW-006 (slice1 final gaps): UFT has no identity journal, so the local id
	// always needs one checked clock snapshot; a clock or time-zone failure rejects
	// the entry before anything is reserved or sent (fail-closed), and a later entry
	// retries once the clock is available.
	uint32_t clockDate = 0;
	uint32_t clockTime = 0;
	uint64_t clockMillis = 0;
	uint32_t localid = 0;
	if (!_order_clock(clockDate, clockTime, clockMillis) || (localid = makeLocalOrderID(clockDate, clockMillis)) == 0)
	{
		WTSLogger::log_dyn("trader", _id.c_str(), LL_ERROR,
			"[{}] entrust of {} rejected: the local clock or time zone is unavailable, no order id allocated",
			_id.c_str(), stdCode.c_str());
		return UINT_MAX;
	}
	char* usertag = entrust->getUserTag();'''
mode=sys.argv[1]
if mode=='header':
    p=ROOT+'TraderAdapter.h'; bom,s=load(p)
    s=rep(s,HDR_PUBLIC_OLD,HDR_PUBLIC_NEW); s=rep(s,HDR_PRIV_OLD,HDR_PRIV_NEW); save(p,bom,s); print('header ok')
elif mode=='cpp':
    p=ROOT+'TraderAdapter.cpp'; bom,s=load(p)
    s=rep(s,SEAM_OLD,SEAM_NEW); s=rep(s,ID_OLD,ID_NEW); s=rep(s,GATE_OLD,GATE_NEW); save(p,bom,s); print('cpp ok')
elif mode=='redvariant':
    src=sys.argv[2]; out=sys.argv[3]; bom,s=load(src)
    s=rep(s,SEAM_OLD,SEAM_NEW)   # seam only: old id generator, no gate
    s=s.replace('TraderAdapter::TraderAdapter()\n','// RED VARIANT (I03-NEW-006 UFT): seam added, clock gate absent\nTraderAdapter::TraderAdapter()\n',1)
    open(out,'wb').write((b'\xef\xbb\xbf' if bom else b'')+s.replace('\n','\r\n').encode('utf-8')); print('variant ok')
