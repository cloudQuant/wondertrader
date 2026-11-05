#pragma once
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include <stdint.h>

#include "DataDefine.h"

#include "../Includes/FasterDefs.h"
#include "../Includes/IDataReader.h"
#include "../Includes/DateMappingBinding.h"

#include "../Share/BoostMappingFile.hpp"

NS_WTP_BEGIN

typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

class WtDataReader : public IDataReader
{
public:
	WtDataReader();
	virtual ~WtDataReader();

private:
	typedef struct _RTKBlockPair
	{
		RTKlineBlock*	_block;
		BoostMFPtr		_file;
		uint64_t		_last_cap;

		_RTKBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}

	} RTKlineBlockPair;
	typedef wt_hashmap<std::string, RTKlineBlockPair>	RTKBlockFilesMap;

	typedef struct _TBlockPair
	{
		RTTickBlock*	_block;
		BoostMFPtr		_file;
		uint64_t		_last_cap;

		_TBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} TickBlockPair;
	typedef wt_hashmap<std::string, TickBlockPair>	TBlockFilesMap;

	typedef struct _TransBlockPair
	{
		RTTransBlock*	_block;
		BoostMFPtr		_file;
		uint64_t		_last_cap;

		std::shared_ptr< std::ofstream>	_fstream;

		_TransBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} TransBlockPair;
	typedef wt_hashmap<std::string, TransBlockPair>	TransBlockFilesMap;

	typedef struct _OdrDtlBlockPair
	{
		RTOrdDtlBlock*	_block;
		BoostMFPtr		_file;
		uint64_t		_last_cap;

		std::shared_ptr< std::ofstream>	_fstream;

		_OdrDtlBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} OrdDtlBlockPair;
	typedef wt_hashmap<std::string, OrdDtlBlockPair>	OrdDtlBlockFilesMap;

	typedef struct _OdrQueBlockPair
	{
		RTOrdQueBlock*	_block;
		BoostMFPtr		_file;
		uint64_t		_last_cap;

		std::shared_ptr< std::ofstream>	_fstream;

		_OdrQueBlockPair()
		{
			_block = NULL;
			_file = NULL;
			_last_cap = 0;
		}
	} OrdQueBlockPair;
	typedef wt_hashmap<std::string, OrdQueBlockPair>	OrdQueBlockFilesMap;

	RTKBlockFilesMap	_rt_min1_map;
	RTKBlockFilesMap	_rt_min5_map;

	TBlockFilesMap		_rt_tick_map;
	TransBlockFilesMap	_rt_trans_map;
	OrdDtlBlockFilesMap	_rt_orddtl_map;
	OrdQueBlockFilesMap	_rt_ordque_map;

	typedef struct _HisTBlockPair
	{
		HisTickBlock*	_block;
		uint64_t		_date;
		std::string		_buffer;
		std::vector<WTSTickStruct> _items;  // R132-001: aligned records; _buffer keeps only the packed header

		_HisTBlockPair()
		{
			_block = NULL;
			_date = 0;
			_buffer.clear();
		}
	} HisTBlockPair;

	typedef wt_hashmap<std::string, HisTBlockPair>	HisTickBlockMap;

	typedef struct _HisTransBlockPair
	{
		bool			_loaded = false;
		uint64_t		_date = 0;
		std::vector<WTSTransStruct> _items;  // aligned, reader-owned; slices remain borrowed

	} HisTransBlockPair;

	typedef std::unordered_map<std::string, HisTransBlockPair>	HisTransBlockMap;

	typedef struct _HisOrdDtlBlockPair
	{
		bool			_loaded = false;
		uint64_t		_date = 0;
		std::vector<WTSOrdDtlStruct> _items;  // aligned, reader-owned; slices remain borrowed

	} HisOrdDtlBlockPair;

	typedef std::unordered_map<std::string, HisOrdDtlBlockPair>	HisOrdDtlBlockMap;

	typedef struct _HisOrdQueBlockPair
	{
		bool			_loaded = false;
		uint64_t		_date = 0;
		std::vector<WTSOrdQueStruct> _items;  // aligned, reader-owned; slices remain borrowed

	} HisOrdQueBlockPair;

	typedef std::unordered_map<std::string, HisOrdQueBlockPair>	HisOrdQueBlockMap;

	HisTickBlockMap		_his_tick_map;
	HisOrdDtlBlockMap	_his_orddtl_map;
	HisOrdQueBlockMap	_his_ordque_map;
	HisTransBlockMap	_his_trans_map;

private:
	RTKlineBlockPair* getRTKilneBlock(const char* exchg, const char* code, WTSKlinePeriod period);
	TickBlockPair* getRTTickBlock(const char* exchg, const char* code);
	OrdQueBlockPair* getRTOrdQueBlock(const char* exchg, const char* code);
	OrdDtlBlockPair* getRTOrdDtlBlock(const char* exchg, const char* code);
	TransBlockPair* getRTTransBlock(const char* exchg, const char* code);

	bool	cacheIntegratedBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
	bool	cacheAdjustedStkBars(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);

	/*
	 *	将历史数据放入缓存
	 */
	bool	cacheHisBarsFromFile(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);
	bool	cacheFinalBarsFromLoader(void* codeInfo, const std::string& key, const char* stdCode, WTSKlinePeriod period);


	bool	loadStkAdjFactorsFromFile(const char* adjfile);
	bool	loadStkAdjFactorsFromLoader();

public:
	/// K31=A：宿主注入 DateMappingV1 表（可选导出 bindDataReaderDateMappingV1 调用；须在 init 前，只接受一次）。
	bool bindDateMapping(const wtp::DateMappingV1* table) noexcept { return _date_binding.bind(table); }

	virtual void init(WTSVariant* cfg, IDataReaderSink* sink, IHisDataLoader* loader = NULL) override;

	virtual void onMinuteEnd(uint32_t uDate, uint32_t uTime, uint32_t endTDate = 0) override;

	virtual WTSTickSlice*	readTickSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	virtual WTSOrdDtlSlice*	readOrdDtlSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	virtual WTSOrdQueSlice*	readOrdQueSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	virtual WTSTransSlice*	readTransSlice(const char* stdCode, uint32_t count, uint64_t etime = 0) override;
	virtual WTSKlineSlice*	readKlineSlice(const char* stdCode, WTSKlinePeriod period, uint32_t count, uint64_t etime = 0) override;

	virtual double getAdjFactorByDate(const char* stdCode, uint32_t date = 0) override;

	virtual uint32_t getAdjustingFlag() override { return _adjust_flag; }

private:
	std::vector<std::string>	_rt_dir;
	std::vector<std::string>	_his_dir;
	IBaseDataMgr*	_base_data_mgr;
	wtp::DateMappingBinding	_date_binding;	// K31=A：已注入时走路径 A，否则路径 C
	IHotMgr*		_hot_mgr;

	//By Wesley @ 2022.08.15
	//复权标记，采用位运算表示，1|2|4,1表示成交量复权，2表示成交额复权，4表示总持复权，其他待定
	uint32_t		_adjust_flag;

	typedef struct _BarsList
	{
		std::string		_exchg;
		std::string		_code;
		WTSKlinePeriod	_period;
		uint32_t		_rt_cursor;
		std::string		_raw_code;

		std::vector<WTSBarStruct>	_bars;
		double			_factor;

		_BarsList() :_rt_cursor(UINT_MAX), _factor(DBL_MAX){}
	} BarsList;

	typedef wt_hashmap<std::string, BarsList> BarsCache;
	BarsCache	_bars_cache;

	uint64_t	_last_time;

	//除权因子
	typedef struct _AdjFactor
	{
		uint32_t	_date;
		double		_factor;
	} AdjFactor;
	typedef std::vector<AdjFactor> AdjFactorList;
	typedef wt_hashmap<std::string, AdjFactorList>	AdjFactorMap;
	AdjFactorMap	_adj_factors;
	// The loader may retain the key pointer after loadAdjFactors returns. Keep
	// one stable string per key until this reader is destroyed. Memory therefore
	// grows with the number of distinct factor keys requested by this reader.
	std::mutex		_adj_factor_loader_keys_mutex;
	std::set<std::string> _adj_factor_loader_keys;

	const char* getStableAdjFactorLoaderKey(const std::string& key);
	const AdjFactorList& getAdjFactors(const char* code, const char* exchg, const char* pid);
	
};

NS_WTP_END
