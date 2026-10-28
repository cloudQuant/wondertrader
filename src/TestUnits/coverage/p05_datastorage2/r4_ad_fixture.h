// r4 / WtDataStorageAD 共用替身（不是用例本体，仅供 r4_*.cpp 引用）
//
// 约束（本轮硬规则）：
//  - 禁止 #define private public：只允许走 WtDataWriterAD / WtDataReaderAD 的公开接口
//    （init / release / writeTick / getCurTick / add_ext_dumper / readTickSlice /
//     readKlineSlice / onMinuteEnd），因此缓存块内容一律用「回读落盘文件字节」的方式断言；
//  - 禁止起线程：WtDataWriterAD::_async_task（WtDataWriterAD.h:245）在全仓库**没有任何赋值**
//    （见 bugs/R4_adstorage.md R4-BUG-01），构造后是堆内存残值。writeTick→pushTask 一旦读到
//    非 0 就会新建 StdThread（且 release() 的 notify_all 未持锁，存在丢唤醒死等）。
//    这里用「先申请同尺寸全 0 块再释放，随后 new 复用同一 size class」把残值压成 0，
//    并在每条用例里用同步副作用（sink 计数）证伪；
//  - 落盘只进各自 mkdtemp/temp_directory 下的唯一目录，不碰仓库。
#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
#include "Includes/IDataWriter.h"
#include "Includes/IDataReader.h"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/DataDefineAD.h"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtDataWriterAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

// 同 r21_ad_fixture.h：匿名命名空间会让 3 个包含本头的 TU 各得到一个「同名不同种」的
// R4WriterADTest，gtest 运行期直接拒绝整族用例（"must use the same test fixture class"），
// 实测连带 8 条 FAIL。具名 + using 指令保持非限定查找可用。
namespace r4fx {

//////////////////////////////////////////////////////////////////////////
// 常量：与生产实现保持一致的期望值（不要从生产代码里"抄算式"，这里全部是字面量）
//////////////////////////////////////////////////////////////////////////
// WtDataWriterAD.cpp:52 CACHE_SIZE_STEP_AD
static const uint32_t R4_STEP = 400;
// WtDataWriterAD.cpp:142/183 新建块的 capacity
static const uint32_t R4_INIT_CAP = 400;

[[maybe_unused]] static void r4_set_str(char* dst, size_t cap, const char* src)
{
	snprintf(dst, cap, "%s", src);
}

//////////////////////////////////////////////////////////////////////////
// IDataWriterSink 替身：只记录，不做任何真实广播
//////////////////////////////////////////////////////////////////////////
class R4WriterSink : public IDataWriterSink
{
public:
	explicit R4WriterSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~R4WriterSink() {}

	IBaseDataMgr* getBDMgr() override { return _bd; }

	bool canSessionReceive(const char* sid) override
	{
		_sid_seen = (sid == NULL) ? "" : sid;
		return _can_recv;
	}

	void broadcastTick(WTSTickData* curTick) override
	{
		_bcast_tick++;
		if(curTick != NULL) _last_bcast_price = curTick->price();
	}

	void broadcastOrdQue(WTSOrdQueData*) override { _bcast_oq++; }
	void broadcastOrdDtl(WTSOrdDtlData*) override { _bcast_od++; }
	void broadcastTrans(WTSTransData*) override { _bcast_td++; }

	CodeSet* getSessionComms(const char*) override { return &_comm_set; }
	uint32_t getTradingDate(const char*) override { return _tdate; }

	void outputLog(WTSLogLevel ll, const char* message) override
	{
		_logs[ll]++;
		_total_logs++;
		if(message != NULL)
		{
			_last[ll] = message;
			_all.push_back(message);
		}
	}

	void set_can_recv(bool v) { _can_recv = v; }
	const std::string& sid_seen() const { return _sid_seen; }
	uint32_t bcast_tick() const { return _bcast_tick; }
	double last_bcast_price() const { return _last_bcast_price; }
	uint32_t log_count(WTSLogLevel ll) const
	{
		auto it = _logs.find(ll);
		return it == _logs.end() ? 0u : it->second;
	}
	uint32_t total_logs() const { return _total_logs; }
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}
	// 全量日志里包含子串的条数：last_log 会被后续同级别日志覆盖
	// （如 updateTickCache L946 的"First tick of new tradingday"盖掉 L881 的扩容日志），
	// 一次性语句只能靠它断言。
	uint32_t count_containing(const char* sub) const
	{
		uint32_t ret = 0;
		const std::string s(sub == NULL ? "" : sub);
		for (const std::string& item : _all)
		{
			if (item.find(s) != std::string::npos)
				ret++;
		}
		return ret;
	}

private:
	IBaseDataMgr*	_bd;
	bool			_can_recv = true;
	std::string		_sid_seen;
	CodeSet			_comm_set;
	uint32_t		_tdate = 20230103;
	uint32_t		_bcast_tick = 0;
	uint32_t		_bcast_oq = 0;
	uint32_t		_bcast_od = 0;
	uint32_t		_bcast_td = 0;
	double			_last_bcast_price = 0.0;
	uint32_t		_total_logs = 0;
	std::vector<std::string> _all;
	wt_hashmap<WTSLogLevel, uint32_t> _logs;
	wt_hashmap<WTSLogLevel, std::string> _last;
};

//////////////////////////////////////////////////////////////////////////
// IBaseDataMgr 替身：一条交易时段 0900-1500（360 分钟），offset=0
//////////////////////////////////////////////////////////////////////////
class R4BaseDataMgr : public IBaseDataMgr
{
public:
	explicit R4BaseDataMgr(const char* sid = "R4_DAY",
		uint32_t sTime = 900, uint32_t eTime = 1500, int32_t offset = 0)
	{
		_session = WTSSessionInfo::create(sid, "R4 日盘", offset);
		_session->addTradingSection(sTime, eTime);
	}

	virtual ~R4BaseDataMgr()
	{
		// WTSContractInfo / WTSCommodityInfo 是 WTSObject（析构 protected），只能 release
		for (auto& kv : _contracts) kv.second->release();
		_contracts.clear();
		for (auto& kv : _comms) kv.second->release();
		_comms.clear();
		_session->release();
	}

	// 品种/合约注册表（返回裸指针，生命周期由本类托管）
	WTSCommodityInfo* add_commodity(const char* exchg, const char* pid, ContractCategory cat)
	{
		WTSCommodityInfo* comm = WTSCommodityInfo::create(pid, pid, exchg, _session->id(), "CHN");
		comm->setSessionInfo(_session);
		comm->setCategory(cat);
		_comms[std::string(exchg) + "." + pid] = comm;
		return comm;
	}

	WTSContractInfo* add_contract(const char* exchg, const char* code, WTSCommodityInfo* comm)
	{
		WTSContractInfo* ct = WTSContractInfo::create(code, code, exchg, comm->getProduct());
		ct->setCommInfo(comm);
		_contracts[std::string(exchg) + "." + code] = ct;
		return ct;
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		auto it = _comms.find(exchgpid == NULL ? "" : exchgpid);
		return it == _comms.end() ? NULL : it->second;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		std::string k = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		auto it = _comms.find(k);
		return it == _comms.end() ? NULL : it->second;
	}

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		std::string k = std::string(exchg == NULL ? "" : exchg) + "." + (code == NULL ? "" : code);
		auto it = _contracts.find(k);
		return it == _contracts.end() ? NULL : it->second;
	}

	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char*) override { return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	// 不做日历推导：交易日 == 自然日，便于用固定键值断言
	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override
	{
		return uDate;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	WTSSessionInfo* session() { return _session; }

private:
	WTSSessionInfo* _session;
	wt_hashmap<std::string, WTSCommodityInfo*> _comms;
	wt_hashmap<std::string, WTSContractInfo*> _contracts;
};

//////////////////////////////////////////////////////////////////////////
// tick 构造
//////////////////////////////////////////////////////////////////////////
[[maybe_unused]] static WTSTickStruct make_tick(const char* exchg, const char* code,
	uint32_t tdate, uint32_t adate, uint32_t atime, double price,
	double vol, double turnover, double oi, double diff,
	double total_vol = 0, double total_turnover = 0)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	r4_set_str(tick.exchg, sizeof(tick.exchg), exchg);
	r4_set_str(tick.code, sizeof(tick.code), code);
	tick.trading_date = tdate;
	tick.action_date = adate;
	tick.action_time = atime;
	tick.price = price;
	tick.open = price;
	tick.high = price;
	tick.low = price;
	tick.volume = vol;
	tick.turn_over = turnover;
	tick.open_interest = oi;
	tick.diff_interest = diff;
	tick.total_volume = (total_vol <= 0) ? vol : total_vol;
	tick.total_turnover = (total_turnover <= 0) ? turnover : total_turnover;
	return tick;
}

//////////////////////////////////////////////////////////////////////////
// 落盘块文件回读：不依赖私有成员，只读自己创建的临时文件
//////////////////////////////////////////////////////////////////////////
struct R4BlkHead
{
	char		_blk_flag[8];
	uint16_t	_type;
	uint16_t	_version;
	uint32_t	_size;
	uint32_t	_capacity;
};

[[maybe_unused]] static R4BlkHead read_head(const std::string& path)
{
	R4BlkHead head;
	memset(&head, 0, sizeof(head));
	std::ifstream fin(path, std::ios::binary);
	if (fin) fin.read((char*)&head, sizeof(head));
	return head;
}

[[maybe_unused]] static uint64_t file_size_or_zero(const std::string& path)
{
	std::error_code ec;
	if (!std::filesystem::exists(path, ec)) return 0;
	return (uint64_t)std::filesystem::file_size(path, ec);
}

// 把 RTBarCache 块文件里的全部 BarCacheItem 读出来（按 _exchg._code 定位）
[[maybe_unused]] static bool read_bar_item(const std::string& path, const char* exchg,
	const char* code, BarCacheItem& out)
{
	R4BlkHead head = read_head(path);
	if (head._capacity == 0) return false;
	std::ifstream fin(path, std::ios::binary);
	if (!fin) return false;
	fin.seekg((std::streampos)sizeof(RTBarCache), std::ios::beg);
	for (uint32_t i = 0; i < head._capacity; i++)
	{
		BarCacheItem item;
		memset(&item, 0, sizeof(item));
		fin.read((char*)&item, sizeof(item));
		if (!fin) return false;
		if (strcmp(item._exchg, exchg) == 0 && strcmp(item._code, code) == 0)
		{
			out = item;
			return true;
		}
	}
	return false;
}

[[maybe_unused]] static bool read_tick_item(const std::string& path, uint32_t idx, TickCacheItem& out)
{
	R4BlkHead head = read_head(path);
	if (idx >= head._capacity) return false;
	std::ifstream fin(path, std::ios::binary);
	if (!fin) return false;
	fin.seekg((std::streampos)(sizeof(RTTickCache) + (std::size_t)idx * sizeof(TickCacheItem)), std::ios::beg);
	fin.read((char*)&out, sizeof(out));
	return (bool)fin;
}

// 手写一个缓存块文件（用来构造"已有缓存"和"容量已满"两种前置状态）
[[maybe_unused]] static void write_block_file(const std::string& path, const char* flag,
	uint16_t type, uint16_t version, uint32_t size, uint32_t capacity,
	const std::string& payload)
{
	std::filesystem::path p(path);
	std::error_code ec;
	if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

	RTBlockHeader head;
	memset(&head, 0, sizeof(head));
	memcpy(head._blk_flag, flag, FLAG_SIZE);
	head._type = type;
	head._version = version;
	head._size = size;
	head._capacity = capacity;

	std::ofstream fout(path, std::ios::binary | std::ios::trunc);
	fout.write((const char*)&head, sizeof(head));
	fout.write(payload.data(), (std::streamsize)payload.size());
	fout.close();
}

[[maybe_unused]] static std::string bar_payload(const std::vector<BarCacheItem>& items)
{
	std::string buf;
	buf.resize(sizeof(BarCacheItem) * items.size());
	if (!items.empty()) memcpy((void*)buf.data(), items.data(), buf.size());
	return buf;
}

[[maybe_unused]] static std::string tick_payload(const std::vector<TickCacheItem>& items)
{
	std::string buf;
	buf.resize(sizeof(TickCacheItem) * items.size());
	if (!items.empty()) memcpy((void*)buf.data(), items.data(), buf.size());
	return buf;
}

[[maybe_unused]] static BarCacheItem make_bar_item(const char* exchg, const char* code,
	uint32_t date, uint64_t time, double px)
{
	BarCacheItem item;
	memset(&item, 0, sizeof(item));
	r4_set_str(item._exchg, sizeof(item._exchg), exchg);
	r4_set_str(item._code, sizeof(item._code), code);
	item._bar.date = date;
	item._bar.time = time;
	item._bar.open = px;
	item._bar.high = px;
	item._bar.low = px;
	item._bar.close = px;
	return item;
}

[[maybe_unused]] static TickCacheItem make_tick_item(const char* exchg, const char* code,
	uint32_t date, double px)
{
	TickCacheItem item;
	memset(&item, 0, sizeof(item));
	item._date = date;
	WTSTickStruct tick = make_tick(exchg, code, date, date, 93000000, px, 1, 1, 0, 0);
	item._tick = tick;
	return item;
}

//////////////////////////////////////////////////////////////////////////
// WTSVariant 构造：AD writer 的 init 只读这几个键
//////////////////////////////////////////////////////////////////////////
[[maybe_unused]] static WTSVariant* make_writer_cfg(const std::string& root, uint32_t groupsize,
	bool disabletick, bool disablemin1, bool disablemin5, bool disableday)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", root.c_str());
	cfg->append("groupsize", groupsize);
	cfg->append("disabletick", disabletick);
	cfg->append("disablemin1", disablemin1);
	cfg->append("disablemin5", disablemin5);
	cfg->append("disableday", disableday);
	return cfg;
}

//////////////////////////////////////////////////////////////////////////
// 见文件头 R4-BUG-01 说明：new 之前先把同尺寸堆块清零，压住 _async_task 残值
//////////////////////////////////////////////////////////////////////////
[[maybe_unused]] static WtDataWriterAD* new_writer_zeroed()
{
	const std::size_t sz = sizeof(WtDataWriterAD);
	void* probe = ::operator new(sz);
	memset(probe, 0, sz);
	::operator delete(probe);
	return new WtDataWriterAD();
}

//////////////////////////////////////////////////////////////////////////
// 夹具：唯一临时目录 + 替身 + 写入器工厂
// 注意：故意不调用 release()（release 在 _task_thrd 非空时会 join，见 R4-BUG-01），
// 只有「无异步线程」这一条分支由单独的用例显式验证。
//////////////////////////////////////////////////////////////////////////
class R4WriterADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("r4_wad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R4BaseDataMgr();
		_sink = new R4WriterSink(_bd);
	}

	void TearDown() override
	{
		_writer.reset();
		delete _sink;
		delete _bd;
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::string path_of(const char* name) const { return (_root / name).string(); }

	WtDataWriterAD* writer() { return _writer.get(); }

	void init_writer(WTSVariant* cfg)
	{
		_writer.reset(new_writer_zeroed());
		ASSERT_TRUE(_writer->init(cfg, _sink));
		cfg->release();
	}

	// 全禁用：只让 tick 缓存和 K 线缓存生效，避免创建任何 LMDB 环境
	void init_writer_all_disabled(uint32_t groupsize = 1000)
	{
		WTSVariant* cfg = make_writer_cfg(_root.string(), groupsize, true, true, true, true);
		init_writer(cfg);
	}

	void write_or_fail(WTSTickData* tick, uint32_t procFlag)
	{
		ASSERT_TRUE(tick != NULL);
		ASSERT_TRUE(_writer->writeTick(tick, procFlag));
		tick->release();
	}

	WTSTickData* tick_of(WTSContractInfo* ct, WTSTickStruct s)
	{
		// WTSTickData::create 需要可写的结构体副本（内部 memcpy 后即与实参脱钩）
		WTSTickData* t = WTSTickData::create(s);
		t->setContractInfo(ct);
		return t;
	}

	std::filesystem::path			_root;
	R4BaseDataMgr*					_bd = NULL;
	R4WriterSink*					_sink = NULL;
	std::unique_ptr<WtDataWriterAD>	_writer;
};

}  // namespace r4fx

using namespace r4fx;
