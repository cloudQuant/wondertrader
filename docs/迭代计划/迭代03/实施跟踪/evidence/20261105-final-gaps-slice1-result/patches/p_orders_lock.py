# S1-NEW-1 (to be I03-NEW-117): every _orders access under _mtx_orders.
import os
ROOT='/Users/yunjinqi/Documents/new_projects/wondertrader/src/'
SC='/private/tmp/claude-501/-Users-yunjinqi-Documents-new-projects-wondertrader/6eec4fa0-1d68-45f7-92f0-284edb551a79/scratchpad/'
p=ROOT+'WtCore/TraderAdapter.cpp'; raw=open(p,'rb').read(); bom=raw.startswith(b'\xef\xbb\xbf')
s=raw.decode('utf-8-sig'); assert '\n' not in s.replace('\r\n','')
s=s.replace('\r\n','\n')
pairs=[
('''OrderMap* TraderAdapter::getOrders(const char* stdCode)
{
	if (_orders == NULL)
		return NULL;

	bool isAll = strlen(stdCode) == 0;

	SpinLock lock(_mtx_orders);
	OrderMap* ret = OrderMap::create();
	for (auto it = _orders->begin(); it != _orders->end(); it++)
	{
		uint32_t localid = it->first;
		WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;

		bool matchesCode''','''OrderMap* TraderAdapter::getOrders(const char* stdCode)
{
	// S1-NEW-1 (TSan, slice1 final gaps): SPI threads create and mutate _orders
	// under _mtx_orders, so the NULL check and the iteration happen under the
	// lock too. Entries are retained into a snapshot and filtered after the
	// lock is released: filtering may consult the base-data manager, and the
	// non-reentrant spin lock must not be held across such calls.
	std::vector<std::pair<uint32_t, WTSOrderInfo*>> snapshot;
	{
		SpinLock lock(_mtx_orders);
		if (_orders == NULL)
			return NULL;
		snapshot.reserve(_orders->size());
		for (auto it = _orders->begin(); it != _orders->end(); it++)
		{
			WTSOrderInfo* ordInfo = (WTSOrderInfo*)it->second;
			ordInfo->retain();
			snapshot.emplace_back(it->first, ordInfo);
		}
	}

	bool isAll = strlen(stdCode) == 0;

	OrderMap* ret = OrderMap::create();
	for (const auto& item : snapshot)
	{
		uint32_t localid = item.first;
		WTSOrderInfo* ordInfo = item.second;

		bool matchesCode'''),
('''		if (matchesCode)
			ret->add(localid, ordInfo);
	}
	return ret;
}
''','''		if (matchesCode)
			ret->add(localid, ordInfo);
		ordInfo->release();
	}
	return ret;
}
'''),
('''bool TraderAdapter::cancelOne(uint32_t localid, const std::string* owner)
{
	if (_orders == NULL || _orders->size() == 0)
		return false;
''','''bool TraderAdapter::cancelOne(uint32_t localid, const std::string* owner)
{
	{
		// S1-NEW-1: the order map is checked under the same lock as its writers.
		SpinLock lock(_mtx_orders);
		if (_orders == NULL || _orders->size() == 0)
			return false;
	}
'''),
('''	WTSOrderInfo* ordInfo = NULL;
	{
		SpinLock lock(_mtx_orders);
		ordInfo = (WTSOrderInfo*)_orders->grab(localid);
		if (ordInfo == NULL)
			return false;
	}
	if (!ordInfo->isAlive())
''','''	WTSOrderInfo* ordInfo = NULL;
	{
		SpinLock lock(_mtx_orders);
		if (_orders != NULL)
			ordInfo = (WTSOrderInfo*)_orders->grab(localid);
		if (ordInfo == NULL)
			return false;
	}
	if (!ordInfo->isAlive())
'''),
('''	if (ayOrders)
	{
		if (_orders == NULL)
			_orders = OrderMap::create();

		uint64_t snapshotEpoch = 0;''','''	if (ayOrders)
	{
		{
			// S1-NEW-1: lazy creation under the lock that guards every other access.
			SpinLock lock(_mtx_orders);
			if (_orders == NULL)
				_orders = OrderMap::create();
		}

		uint64_t snapshotEpoch = 0;'''),
('''			if (keepActive)
			{
				SpinLock lock(_mtx_orders);
				_orders->add(localid, orderInfo);
			}''','''			if (keepActive)
			{
				SpinLock lock(_mtx_orders);
				if (_orders != NULL)
					_orders->add(localid, orderInfo);
			}'''),
('''			{
				SpinLock lock(_mtx_orders);
				_orders->remove(localid);
			}
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Order {} absent from order snapshot epoch {}; marked as absent tombstone"''','''			{
				SpinLock lock(_mtx_orders);
				if (_orders != NULL)
					_orders->remove(localid);
			}
			WTSLogger::log_dyn("trader", _id.c_str(), LL_WARN,
				"[{}] Order {} absent from order snapshot epoch {}; marked as absent tombstone"'''),
]
for old,new in pairs:
    assert s.count(old)==1,(old[:80]); s=s.replace(old,new)
out=(b'\xef\xbb\xbf' if bom else b'')+s.replace('\n','\r\n').encode('utf-8')
tmp=SC+'slice1-patches/TraderAdapter.cpp.new'
open(tmp,'wb').write(out)
os.chmod(tmp, 0o644)
os.replace(tmp, p)
print('applied orders lock')
