// q03_storage3 / 共用夹具（非用例 TU）
//
// 提供三块能力，供 q03_*.cpp 复用：
//  1) 最小宿主替身：IBaseDataMgr / IHotMgr / IDataReaderSink / IRdmDtReaderSink
//     （只 include Includes/*.h 与 Share/*，绝不引用 WtCore/WtBtCore/WtUftCore/WtDtCore 的类）
//  2) 落盘构造器：按 WtDataStorage/DataDefine.h 的块头布局拼 .dsb / .dmb 文件
//     （RAW / CMP / RAW_V2 / CMP_V2 / 未知版本位、尾部残缺记录、压缩载荷）
//  3) CWD 沙箱：WtRdmDtReader::_base_dir 是 private 且只有 init(cfg!=NULL) 才赋值
//     （那条路径会起 5 秒轮询线程，本波纪律禁止），故保持默认空串
//     —— 生产代码里的路径退化为 "his/xxx/..." 相对路径，用 chdir 进 temp 目录即可完全隔离，
//     不需要 #define private public，也不碰仓库 dist/。
//
// 白盒手法：只用「派生类/替身」，没有任何访问权限宏。
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "Includes/WTSMarcos.h"
#include "Includes/WTSTypes.h"
#include "Includes/WTSStruct.h"
#include "Includes/WTSVariant.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSSessionInfo.hpp"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/IHotMgr.h"
#include "Includes/IDataReader.h"
#include "Includes/IRdmDtReader.h"
#include "Includes/FasterDefs.h"
#include "WTSUtils/WTSCmpHelper.hpp"
#include "WTSUtils/WTSCfgLoader.h"
#include "Share/BoostMappingFile.hpp"
#include "Share/StdUtils.hpp"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "Share/fmtlib.h"
#include "WtDataStorage/DataDefine.h"

namespace q03 {

using std::string;
using std::vector;

// ---------------------------------------------------------------- 落盘沙箱
class Sandbox
{
public:
	explicit Sandbox(const char* tag)
	{
		std::error_code ec;
		_backup = std::filesystem::current_path(ec);

		static uint32_t seq = 0;
		seq++;
		_root = std::filesystem::temp_directory_path()
			/ fmtutil::format("q03_{}_{}_{}", tag, (uint32_t)::getpid(), seq);
		_data = _root / "data";
		std::filesystem::create_directories(_data, ec);
		ASSERT_CHDIR(_data.string().c_str());
	}

	~Sandbox()
	{
		std::error_code ec;
		if(!_backup.empty())
			::chdir(_backup.c_str());
		std::filesystem::remove_all(_root, ec);
	}

	// 绝对根目录（带结尾分隔符），给 WtDataReader::init 的 cfg.path 用
	string root_abs() const
	{
		return _data.string() + "/";
	}

	const std::filesystem::path& path() const { return _data; }

private:
	void ASSERT_CHDIR(const char* p)
	{
		int ret = ::chdir(p);
		ASSERT_EQ(ret, 0) << "chdir(" << p << ") failed";
	}

	std::filesystem::path	_backup;
	std::filesystem::path	_root;
	std::filesystem::path	_data;
};

inline bool make_parent_dirs(const string& file)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
	return !ec;
}

inline bool write_bytes(const string& file, const string& bytes)
{
	make_parent_dirs(file);
	std::ofstream ofs(file.c_str(), std::ios::binary | std::ios::trunc);
	if(!ofs.is_open())
		return false;
	ofs.write(bytes.data(), (std::streamsize)bytes.size());
	ofs.flush();
	return ofs.good();
}

inline string read_bytes(const string& file)
{
	std::ifstream ifs(file.c_str(), std::ios::binary);
	std::stringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

// ---------------------------------------------------------------- 块头拼装
// payload 是「已经按目标结构体序列化好的字节」（新/老结构体都可能）
inline string make_block(uint16_t blk_type, uint16_t version, const string& payload)
{
	if(version == BLOCK_VERSION_CMP || version == BLOCK_VERSION_CMP_V2)
	{
		string comp = WTSCmpHelper::compress_data(payload.data(), payload.size());
		BlockHeaderV2 head;
		memset(&head, 0, sizeof(head));
		memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
		head._type = blk_type;
		head._version = version;
		head._size = comp.size();

		string ret;
		ret.append((const char*)&head, sizeof(head));
		ret.append(comp);
		return ret;
	}

	BlockHeader head;
	memset(&head, 0, sizeof(head));
	memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
	head._type = blk_type;
	head._version = version;

	string ret;
	ret.append((const char*)&head, sizeof(head));
	ret.append(payload);
	return ret;
}

// 实时块：RTDayBlockHeader(24) + capacity 个 item（writer 侧的真实形态，见 DataDefine.h:139-205）
template<typename T>
inline string make_rt_block(uint16_t blk_type, uint32_t date, uint32_t size, uint32_t capacity, const vector<T>& items)
{
	// sizeof(RTDayBlockHeader) == FLAG_SIZE + 2 + 2 + 4 + 4 + 4 == 20（#pragma pack(1)）
	const std::size_t head_size = sizeof(RTDayBlockHeader);
	string ret(head_size, '\0');
	RTDayBlockHeader* head = (RTDayBlockHeader*)ret.data();
	memcpy(head->_blk_flag, BLK_FLAG, FLAG_SIZE);
	head->_type = blk_type;
	head->_version = BLOCK_VERSION_RAW_V2;
	head->_size = size;
	head->_capacity = capacity;
	head->_date = date;

	string payload;
	payload.resize(items.size() * sizeof(T));
	if(!items.empty())
		memcpy(payload.data(), items.data(), payload.size());
	ret.append(payload);

	// 容量补齐，保证 mmap 后 _ticks[0.._size-1] 一定在映射区内
	if((std::size_t)capacity * sizeof(T) > items.size() * sizeof(T))
		ret.append(string(((std::size_t)capacity - items.size()) * sizeof(T), '\0'));

	return ret;
}

// ---------------------------------------------------------------- 记录填充
inline WTSBarStruct make_bar(uint32_t date, uint64_t time, uint32_t seq)
{
	WTSBarStruct bar;
	bar.date = date;
	bar.time = time;
	bar.open = 10.0 + seq;
	bar.high = 11.0 + seq;
	bar.low = 9.0 + seq;
	bar.close = 10.5 + seq;
	bar.settle = 10.25 + seq;
	bar.money = 1000.0 + seq;
	bar.vol = 2000.0 + seq;
	bar.hold = 3000.0 + seq;
	bar.add = 100.0 + seq;
	return bar;
}

inline string pack_bars(const vector<WTSBarStruct>& bars)
{
	string payload;
	payload.resize(bars.size() * sizeof(WTSBarStruct));
	if(!bars.empty())
		memcpy(payload.data(), bars.data(), payload.size());
	return payload;
}

inline WTSBarStructOld make_old_bar(uint32_t date, uint32_t time, uint32_t seq)
{
	WTSBarStructOld bar;
	bar.date = date;
	bar.time = time;
	bar.open = 20.0 + seq;
	bar.high = 21.0 + seq;
	bar.low = 19.0 + seq;
	bar.close = 20.5 + seq;
	bar.settle = 20.25 + seq;
	bar.money = 5000.0 + seq;
	bar.vol = 5000 + seq;
	bar.hold = 6000 + seq;
	bar.add = 11 + seq;
	return bar;
}

inline string pack_old_bars(const vector<WTSBarStructOld>& bars)
{
	string payload;
	payload.resize(bars.size() * sizeof(WTSBarStructOld));
	if(!bars.empty())
		memcpy(payload.data(), bars.data(), payload.size());
	return payload;
}

inline WTSTickStruct make_tick(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSTickStruct tick;
	snprintf(tick.exchg, sizeof(tick.exchg), "%s", "SSE");
	snprintf(tick.code, sizeof(tick.code), "%u", 600000 + seq);
	tick.price = 30.0 + seq;
	tick.open = 29.0 + seq;
	tick.high = 31.0 + seq;
	tick.low = 28.0 + seq;
	tick.settle_price = 30.25 + seq;
	tick.upper_limit = 33.0 + seq;
	tick.lower_limit = 27.0 + seq;
	tick.total_volume = 4000.0 + seq;
	tick.volume = 40.0 + seq;
	tick.total_turnover = 120000.0 + seq;
	tick.turn_over = 1200.0 + seq;
	tick.open_interest = 9000.0 + seq;
	tick.diff_interest = 12.0 + seq;
	tick.trading_date = tdate;
	tick.action_date = adate;
	tick.action_time = atime;
	tick.pre_close = 29.5 + seq;
	tick.pre_settle = 29.75 + seq;
	tick.pre_interest = 8000.0 + seq;
	for(int i = 0; i < 10; i++)
	{
		tick.bid_prices[i] = 30.0 + i * 0.01 + seq;
		tick.ask_prices[i] = 30.1 + i * 0.01 + seq;
		tick.bid_qty[i] = 100.0 + i + seq;
		tick.ask_qty[i] = 200.0 + i + seq;
	}
	return tick;
}

inline string pack_ticks(const vector<WTSTickStruct>& ticks)
{
	string payload;
	payload.resize(ticks.size() * sizeof(WTSTickStruct));
	if(!ticks.empty())
		memcpy(payload.data(), ticks.data(), payload.size());
	return payload;
}

inline WTSTickStructOld make_old_tick(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSTickStructOld tick;
	snprintf(tick.exchg, sizeof(tick.exchg), "%s", "SSE");
	snprintf(tick.code, sizeof(tick.code), "%u", 600000 + seq);
	tick.price = 40.0 + seq;
	tick.open = 39.0 + seq;
	tick.high = 41.0 + seq;
	tick.low = 38.0 + seq;
	tick.settle_price = 40.25 + seq;
	tick.upper_limit = 44.0 + seq;
	tick.lower_limit = 36.0 + seq;
	tick.total_volume = 7000 + seq;
	tick.volume = 70 + seq;
	tick.total_turnover = 210000.0 + seq;
	tick.turn_over = 2100.0 + seq;
	tick.open_interest = 12000 + seq;
	tick.diff_interest = 21 + seq;
	tick.trading_date = tdate;
	tick.action_date = adate;
	tick.action_time = atime;
	tick.pre_close = 39.5 + seq;
	tick.pre_settle = 39.75 + seq;
	tick.pre_interest = 11000 + seq;
	for(int i = 0; i < 10; i++)
	{
		tick.bid_prices[i] = 40.0 + i * 0.01 + seq;
		tick.ask_prices[i] = 40.1 + i * 0.01 + seq;
		tick.bid_qty[i] = 700 + i + seq;
		tick.ask_qty[i] = 800 + i + seq;
	}
	return tick;
}

inline string pack_old_ticks(const vector<WTSTickStructOld>& ticks)
{
	string payload;
	payload.resize(ticks.size() * sizeof(WTSTickStructOld));
	if(!ticks.empty())
		memcpy(payload.data(), ticks.data(), payload.size());
	return payload;
}

inline WTSOrdQueStruct make_ordque(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdQueStruct item;
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%u", 600000 + seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.side = seq % 2 == 0 ? BDT_Buy : BDT_Sell;
	item.price = 50.0 + seq;
	item.order_items = 3 + seq;
	item.qsize = 5;
	for(uint32_t i = 0; i < 50; i++)
		item.volumes[i] = 10 + i + seq;
	return item;
}

inline WTSOrdDtlStruct make_orddtl(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSOrdDtlStruct item;
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%u", 600000 + seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.index = 1000 + seq;
	item.price = 51.0 + seq;
	item.volume = 100 + seq;
	item.side = seq % 2 == 0 ? BDT_Buy : BDT_Sell;
	item.otype = ODT_LimitPrice;
	item.orderid = 700000 + seq;
	return item;
}

inline WTSTransStruct make_trans(uint32_t tdate, uint32_t adate, uint32_t atime, uint32_t seq)
{
	WTSTransStruct item;
	snprintf(item.exchg, sizeof(item.exchg), "%s", "SSE");
	snprintf(item.code, sizeof(item.code), "%u", 600000 + seq);
	item.trading_date = tdate;
	item.action_date = adate;
	item.action_time = atime;
	item.index = 2000 + seq;
	item.ttype = TT_Match;
	item.side = seq % 2 == 0 ? BDT_Buy : BDT_Sell;
	item.price = 52.0 + seq;
	item.volume = 200 + seq;
	item.askorder = 300000 + seq;
	item.bidorder = 400000 + seq;
	return item;
}

template<typename T>
inline string pack_items(const vector<T>& items)
{
	string payload;
	payload.resize(items.size() * sizeof(T));
	if(!items.empty())
		memcpy(payload.data(), items.data(), payload.size());
	return payload;
}

// ---------------------------------------------------------------- 宿主替身
class BaseDataMgr : public IBaseDataMgr
{
public:
	BaseDataMgr()
	{
		// 日盘 09:00-11:30 / 13:00-15:00，getCloseTime() == 1500
		_session = WTSSessionInfo::create("SSE_DAY", "日盘", 0);
		_session->addTradingSection(900, 1130);
		_session->addTradingSection(1300, 1500);

		_stock = WTSCommodityInfo::create("STK", "股票", "SSE", "SSE_DAY", "CHN");
		_stock->setSessionInfo(_session);
		_stock->setCategory(ContractCategory::CC_Stock);

		_fut = WTSCommodityInfo::create("rb", "螺纹钢", "SHFE", "SSE_DAY", "CHN");
		_fut->setSessionInfo(_session);
		_fut->setCategory(ContractCategory::CC_Future);
	}

	virtual ~BaseDataMgr()
	{
		_stock->release();
		_fut->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _stock;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return strcmp(exchg, "SSE") == 0 ? _stock : _fut;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	// uDate==0 走「当前交易日」，其余原样返回（可被 _override 改写）
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override
	{
		if(uDate == 0)
			return _cur_tdate;

		auto it = _override.find(uDate);
		return it == _override.end() ? uDate : it->second;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	void set_cur_tdate(uint32_t d) { _cur_tdate = d; }
	uint32_t cur_tdate() const { return _cur_tdate; }
	void set_override(uint32_t from, uint32_t to) { _override[from] = to; }
	uint32_t commodity_calls() const { return _calls; }
	void touch() { _calls++; }

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_stock = NULL;
	WTSCommodityInfo*	_fut = NULL;
	string		_last_pid;
	string		_last_sid;
	uint32_t	_cur_tdate = 20240110;
	uint32_t	_calls = 0;
	std::map<uint32_t, uint32_t>	_override;
};

class HotMgr : public IHotMgr
{
public:
	virtual ~HotMgr() {}

	const char* getRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isHot(const char*, const char*, uint32_t) override { return false; }
	bool splitHotSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevSecondRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isSecond(const char*, const char*, uint32_t) override { return false; }
	bool splitSecondSecions(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	const char* getPrevCustomRawCode(const char*, const char*, uint32_t) override { return ""; }
	bool isCustomHot(const char*, const char*, uint32_t) override { return false; }
	bool splitCustomSections(const char*, const char*, uint32_t, uint32_t, HotSections&) override { return false; }
	const char* getRuleTag(const char*) override { return ""; }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }
};

// IDataReaderSink：三个时间源固定，避免任何墙钟依赖
class ReaderSink : public IDataReaderSink
{
public:
	ReaderSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~ReaderSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void on_bar(const char* stdCode, WTSKlinePeriod period, WTSBarStruct*) override
	{
		_bars[stdCode] = (uint32_t)period;
	}
	void on_all_bar_updated(uint32_t) override { _updated++; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		_last_log = message == NULL ? "" : message;
	}
	uint32_t get_date() override { return _date; }
	uint32_t get_min_time() override { return _min_time; }
	uint32_t get_secs() override { return _secs; }

	void set_clock(uint32_t d, uint32_t m, uint32_t s) { _date = d; _min_time = m; _secs = s; }
	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	const string& last_log() const { return _last_log; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	std::map<string, uint32_t>	_bars;
	std::map<WTSLogLevel, uint32_t>	_logs;
	string		_last_log;
	uint32_t	_total = 0;
	uint32_t	_updated = 0;
	uint32_t	_date = 20240110;
	uint32_t	_min_time = 1500;
	uint32_t	_secs = 0;
};

class RdmSink : public IRdmDtReaderSink
{
public:
	RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~RdmSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total++;
		if(message != NULL)
		{
			_last_log = message;
		}
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	const string& last_log() const { return _last_log; }

private:
	IBaseDataMgr*	_bd;
	IHotMgr*		_hot;
	std::map<WTSLogLevel, uint32_t>	_logs;
	string		_last_log;
	uint32_t	_total = 0;
};

// ---------------------------------------------------------------- 断言辅助
inline void expect_bar_eq(const WTSBarStruct& a, const WTSBarStruct& b)
{
	EXPECT_EQ(a.date, b.date);
	EXPECT_EQ(a.time, b.time);
	EXPECT_DOUBLE_EQ(a.open, b.open);
	EXPECT_DOUBLE_EQ(a.high, b.high);
	EXPECT_DOUBLE_EQ(a.low, b.low);
	EXPECT_DOUBLE_EQ(a.close, b.close);
	EXPECT_DOUBLE_EQ(a.settle, b.settle);
	EXPECT_DOUBLE_EQ(a.money, b.money);
	EXPECT_DOUBLE_EQ(a.vol, b.vol);
	EXPECT_DOUBLE_EQ(a.hold, b.hold);
	EXPECT_DOUBLE_EQ(a.add, b.add);
}

inline void expect_bar_upgraded(const WTSBarStruct& a, const WTSBarStructOld& b)
{
	EXPECT_EQ(a.date, b.date);
	EXPECT_EQ(a.time, (uint64_t)b.time);
	EXPECT_DOUBLE_EQ(a.open, b.open);
	EXPECT_DOUBLE_EQ(a.high, b.high);
	EXPECT_DOUBLE_EQ(a.low, b.low);
	EXPECT_DOUBLE_EQ(a.close, b.close);
	EXPECT_DOUBLE_EQ(a.settle, b.settle);
	EXPECT_DOUBLE_EQ(a.money, b.money);
	EXPECT_DOUBLE_EQ(a.vol, (double)b.vol);
	EXPECT_DOUBLE_EQ(a.hold, (double)b.hold);
	EXPECT_DOUBLE_EQ(a.add, (double)b.add);
}

inline void expect_tick_eq(const WTSTickStruct& a, const WTSTickStruct& b)
{
	EXPECT_STREQ(a.exchg, b.exchg);
	EXPECT_STREQ(a.code, b.code);
	EXPECT_DOUBLE_EQ(a.price, b.price);
	EXPECT_DOUBLE_EQ(a.open, b.open);
	EXPECT_DOUBLE_EQ(a.high, b.high);
	EXPECT_DOUBLE_EQ(a.low, b.low);
	EXPECT_DOUBLE_EQ(a.settle_price, b.settle_price);
	EXPECT_DOUBLE_EQ(a.upper_limit, b.upper_limit);
	EXPECT_DOUBLE_EQ(a.lower_limit, b.lower_limit);
	EXPECT_DOUBLE_EQ(a.total_volume, b.total_volume);
	EXPECT_DOUBLE_EQ(a.volume, b.volume);
	EXPECT_DOUBLE_EQ(a.total_turnover, b.total_turnover);
	EXPECT_DOUBLE_EQ(a.turn_over, b.turn_over);
	EXPECT_DOUBLE_EQ(a.open_interest, b.open_interest);
	EXPECT_DOUBLE_EQ(a.diff_interest, b.diff_interest);
	EXPECT_EQ(a.trading_date, b.trading_date);
	EXPECT_EQ(a.action_date, b.action_date);
	EXPECT_EQ(a.action_time, b.action_time);
	EXPECT_DOUBLE_EQ(a.pre_close, b.pre_close);
	EXPECT_DOUBLE_EQ(a.pre_settle, b.pre_settle);
	EXPECT_DOUBLE_EQ(a.pre_interest, b.pre_interest);
	for(int i = 0; i < 10; i++)
	{
		EXPECT_DOUBLE_EQ(a.bid_prices[i], b.bid_prices[i]);
		EXPECT_DOUBLE_EQ(a.ask_prices[i], b.ask_prices[i]);
		EXPECT_DOUBLE_EQ(a.bid_qty[i], b.bid_qty[i]);
		EXPECT_DOUBLE_EQ(a.ask_qty[i], b.ask_qty[i]);
	}
}

inline void expect_tick_upgraded(const WTSTickStruct& a, const WTSTickStructOld& b)
{
	EXPECT_STREQ(a.exchg, b.exchg);
	EXPECT_STREQ(a.code, b.code);
	EXPECT_DOUBLE_EQ(a.price, b.price);
	EXPECT_DOUBLE_EQ(a.open, b.open);
	EXPECT_DOUBLE_EQ(a.high, b.high);
	EXPECT_DOUBLE_EQ(a.low, b.low);
	EXPECT_DOUBLE_EQ(a.settle_price, b.settle_price);
	EXPECT_DOUBLE_EQ(a.upper_limit, b.upper_limit);
	EXPECT_DOUBLE_EQ(a.lower_limit, b.lower_limit);
	EXPECT_DOUBLE_EQ(a.total_volume, (double)b.total_volume);
	EXPECT_DOUBLE_EQ(a.volume, (double)b.volume);
	EXPECT_DOUBLE_EQ(a.total_turnover, b.total_turnover);
	EXPECT_DOUBLE_EQ(a.turn_over, b.turn_over);
	EXPECT_DOUBLE_EQ(a.open_interest, (double)b.open_interest);
	EXPECT_DOUBLE_EQ(a.diff_interest, (double)b.diff_interest);
	EXPECT_EQ(a.trading_date, b.trading_date);
	EXPECT_EQ(a.action_date, b.action_date);
	EXPECT_EQ(a.action_time, b.action_time);
	EXPECT_DOUBLE_EQ(a.pre_close, b.pre_close);
	EXPECT_DOUBLE_EQ(a.pre_settle, b.pre_settle);
	EXPECT_DOUBLE_EQ(a.pre_interest, (double)b.pre_interest);
	for(int i = 0; i < 10; i++)
	{
		EXPECT_DOUBLE_EQ(a.bid_prices[i], b.bid_prices[i]);
		EXPECT_DOUBLE_EQ(a.ask_prices[i], b.ask_prices[i]);
		EXPECT_DOUBLE_EQ(a.bid_qty[i], (double)b.bid_qty[i]);
		EXPECT_DOUBLE_EQ(a.ask_qty[i], (double)b.ask_qty[i]);
	}
}

} // namespace q03
