import sys
root=sys.argv[1]
# ---- SelMocker.h
p=root+'/src/WtBtCore/SelMocker.h'
s=open(p).read()
old='''	/// @brief 当前交易日，记录当前的交易日期，格式为YYYYMMDD
	uint32_t		_cur_tdate;
'''
new='''	/// @brief 当前交易日，记录当前的交易日期，格式为YYYYMMDD
	uint32_t		_cur_tdate;

	/// @brief 冻结量所属交易日（卡3 3d=C：session-begin 仅在交易日不同时清零冻结量）
	uint32_t		_frozen_tdate = 0;
'''
assert s.count(old)==1; s=s.replace(old,new,1); open(p,'w').write(s)
# ---- SelMocker.cpp
p=root+'/src/WtBtCore/SelMocker.cpp'
s=open(p).read()
old='''			pItem.AddMember("lastexittime", pInfo._last_exittime, allocator);

			rj::Value details(rj::kArrayType);'''
new='''			pItem.AddMember("lastexittime", pInfo._last_exittime, allocator);
			pItem.AddMember("frozen", pInfo._frozen, allocator);	//卡3 3d=C：保存冻结量，归属交易日为 fund.tdate

			rj::Value details(rj::kArrayType);'''
assert s.count(old)==1; s=s.replace(old,new,1)
old='''void SelMocker::on_session_begin(uint32_t curTDate)
{
	_cur_tdate = curTDate;
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (!decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
			pInfo._frozen = 0;
		}
	}
}'''
new='''void SelMocker::on_session_begin(uint32_t curTDate)
{
	_cur_tdate = curTDate;
	//每个交易日开始，要把冻结持仓置零
	//卡3 3d=C：冻结量归属交易日（上一个 session）与本交易日相同（同日重复 session-begin）时保留
	if (_frozen_tdate != curTDate)
	{
		for (auto& it : _pos_map)
		{
			const char* stdCode = it.first.c_str();
			PosInfo& pInfo = (PosInfo&)it.second;
			if (!decimal::eq(pInfo._frozen, 0))
			{
				log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
				pInfo._frozen = 0;
			}
		}
	}
	else
	{
		log_debug("Frozen positions kept on same-tradingday session begin {}", curTDate);
	}
	_frozen_tdate = curTDate;
}'''
assert s.count(old)==1; s=s.replace(old,new,1); open(p,'w').write(s)
# ---- HftMocker.h
p=root+'/src/WtBtCore/HftMocker.h'
s=open(p).read()
old='''	PositionMap		_pos_map;
'''
assert s.count(old)==1
new='''	PositionMap		_pos_map;

	/**
	 * @brief 冻结量所属交易日
	 * @details 卡3 3d=C：session-begin 仅在交易日与冻结量所属交易日不同时清零冻结量
	 */
	uint32_t		_frozen_tdate = 0;
'''
s=s.replace(old,new,1); open(p,'w').write(s)
# ---- HftMocker.cpp
p=root+'/src/WtBtCore/HftMocker.cpp'
s=open(p).read()
old='''void HftMocker::on_session_begin(uint32_t curTDate)
{
	//每个交易日开始，要把冻结持仓置零
	for (auto& it : _pos_map)
	{
		const char* stdCode = it.first.c_str();
		PosInfo& pInfo = (PosInfo&)it.second;
		if (!decimal::eq(pInfo._frozen, 0))
		{
			log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
			pInfo._frozen = 0;
		}
	}
'''
new='''void HftMocker::on_session_begin(uint32_t curTDate)
{
	//每个交易日开始，要把冻结持仓置零
	//卡3 3d=C：冻结量归属交易日（上一个 session）与本交易日相同（同日重复 session-begin）时保留
	if (_frozen_tdate != curTDate)
	{
		for (auto& it : _pos_map)
		{
			const char* stdCode = it.first.c_str();
			PosInfo& pInfo = (PosInfo&)it.second;
			if (!decimal::eq(pInfo._frozen, 0))
			{
				log_debug("{} of {} frozen released on {}", pInfo._frozen, stdCode, curTDate);
				pInfo._frozen = 0;
			}
		}
	}
	else
	{
		log_debug("Frozen positions kept on same-tradingday session begin {}", curTDate);
	}
	_frozen_tdate = curTDate;
'''
assert s.count(old)==1; s=s.replace(old,new,1); open(p,'w').write(s)
print("mockers patched")
