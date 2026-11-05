import sys
root=sys.argv[1]
p=root+'/src/WtBtCore/HisDataReplayer.cpp'
s=open(p).read()
old='''		const auto validEnd = tickList._items.begin() + std::min(tickList._count, tickList._items.size());
		auto tit = std::lower_bound(tickList._items.begin(), validEnd, curTick, [](const WTSTickStruct& a, const WTSTickStruct& b){
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		});
		if (tit == validEnd)
			return NULL;
		if (tit->action_date > uDate || (tit->action_date == uDate && tit->action_time > uTime))
		{
			if (tit == tickList._items.begin())
				return NULL;
			--tit;
		}
		tickList._query_cursor = static_cast<std::size_t>(tit - tickList._items.begin()) + 1;'''
new='''		// BUG-R103-002 (E): each record owns the left-closed, right-open interval
		// [t_i, t_{i+1}); the visible prefix is every record with time <= now and
		// the query returns its last element. Same-time records keep their stable
		// load order, so the last inserted one is visible at that time. Before the
		// first record and strictly after the last record the result is empty.
		const auto validBegin = tickList._items.begin();
		const auto validEnd = validBegin + std::min(tickList._count, tickList._items.size());
		if (validBegin == validEnd)
			return NULL;
		auto before = [](const WTSTickStruct& a, const WTSTickStruct& b) {
			if (a.action_date != b.action_date)
				return a.action_date < b.action_date;
			else
				return a.action_time < b.action_time;
		};
		const WTSTickStruct& tail = *(validEnd - 1);
		if (before(tail, curTick))
			return NULL;
		auto tit = std::upper_bound(validBegin, validEnd, curTick, before);
		if (tit == validBegin)
			return NULL;
		tickList._query_cursor = static_cast<std::size_t>(tit - validBegin);'''
assert s.count(old)==1; s=s.replace(old,new,1)
old2='''	if (_replay_finishing) return;
	if (_replay_failure_code == WT_REPLAY_ERROR_NONE)
	{
		_replay_failure_code = errorCode == WT_REPLAY_ERROR_NONE ? WT_REPLAY_ERROR_INTERNAL : errorCode;
		_replay_failure_message = message.empty() ? "Replay failed" : message;
	}
	_terminated.store(true);
}'''
new2='''	if (_replay_finishing) return;
	if (_replay_failure_code == WT_REPLAY_ERROR_NONE)
	{
		_replay_failure_code = errorCode == WT_REPLAY_ERROR_NONE ? WT_REPLAY_ERROR_INTERNAL : errorCode;
		_replay_failure_message = message.empty() ? "Replay failed" : message;
		// I03-NEW-019: make the first recorded failure observable in the log.
		WTSLogger::error("Replay failed with replay error {}: {}", _replay_failure_code, _replay_failure_message);
	}
	_terminated.store(true);
}'''
assert s.count(old2)==1; s=s.replace(old2,new2,1)
open(p,'w').write(s)
print("replayer patched")
