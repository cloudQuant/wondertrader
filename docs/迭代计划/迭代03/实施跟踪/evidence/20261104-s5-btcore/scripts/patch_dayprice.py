import sys
root=sys.argv[1]
p=root+'/src/WtBtCore/HisDataReplayer.cpp'
s=open(p).read()
old='''							curTS.price = newPx;
							curTS.volume = nextBar.vol;
							update_price(barsList->_code.c_str(), curTS.price);
							dispatch_sim_tick(realCode.c_str(), WTSTickData::create(curTS), cInfo.isExright(), barsList, barsList->_cursor);'''
new='''							curTS.price = newPx;
							curTS.volume = nextBar.vol;

							// BUG-I02-NEW-109: get_day_price (public "当日价格") reads _day_cache
							// when ticks are disabled. Accumulate the day's open/high/low/last
							// from the simulated OHLC sequence, as the minute arm does; the
							// dispatched simulated tick itself is unchanged.
							WTSTickStruct& dayTS = _day_cache[barsList->_code];
							strcpy(dayTS.code, barsList->_code.c_str());
							dayTS.action_date = _cur_date;
							dayTS.action_time = curTS.action_time;
							dayTS.price = newPx;
							dayTS.volume = nextBar.vol;
							if (decimal::eq(dayTS.open, 0))
								dayTS.open = newPx;
							dayTS.high = max(newPx, dayTS.high);
							if (decimal::eq(dayTS.low, 0))
								dayTS.low = newPx;
							else
								dayTS.low = min(newPx, dayTS.low);

							update_price(barsList->_code.c_str(), curTS.price);
							dispatch_sim_tick(realCode.c_str(), WTSTickData::create(curTS), cInfo.isExright(), barsList, barsList->_cursor);'''
assert s.count(old)==1, s.count(old)
s=s.replace(old,new,1)
open(p,'w').write(s)
print("dayprice patched")
