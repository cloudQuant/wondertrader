// r23_storage4 / WtDataWriter 公共夹具（非用例 TU）
//
// 提供「走通 WtDataWriter::init 的公开入口」所需的最小宿主：
//  1) WriterBDMgr —— IBaseDataMgr 替身：注册品种/合约，getContractSize 可调（决定 cache.dmb 初始容量），
//     forget_contract() 用来模拟「合约已过期」，从而驱动 proc_loop 的缓存重建分支
//  2) WriterSink —— IDataWriterSink 替身：canSessionReceive/broadcast*/getSessionComms/getTradingDate
//     全部固定返回，outputLog 按级别计数并留存文本（断言靠它，不靠日志文件）
//  3) R23Writer  —— 只是 `class : public WtDataWriter` 的一层薄派生，用来把 IDataWriter 的
//     **protected** 成员 `_dumpers` 开放给用例（IHisDataDumper 没有任何公开的注册入口，
//     生产里只有 WtDtServo 之类的兄弟类在内部塞它）。派生访问 protected 是语言允许的正常手段，
//     **没有使用 #define private public**，没有改任何生产代码。
//  4) 落盘读回辅助：writer 的 rt/*.dmb 与 his/*.dsb 都是普通文件 + MAP_SHARED 映射，
//     用 ifstream 直接按 DataDefine.h 的头部布局读回比对。
//
// ⚠ 线程纪律（本车道的核心风险点，评审请重点看这段）
//  - `init()` 结尾无条件 `_proc_chk.reset(new StdThread(check_loop))`，check_loop 每轮先
//    `sleep_for(10s)`（WtDataWriter.cpp:1621），而 `release()` 只 join `_proc_thrd`、**不 join `_proc_chk`**；
//    `~WtDataWriter()` 为空，成员 `StdThreadPtr`（= std::shared_ptr<std::thread>）析构时若线程仍 joinable
//    会直接 `std::terminate()`。所以**凡是调过 init() 的 writer 一律不 delete**（只 release），
//    由 case_runner「一条用例一个进程」兜住，泄漏有界。
//  - `_terminated` 是普通 bool（非原子），check_loop 只在 10s 边界读它；本车道用例都在 1s 内结束，
//    进程退出时该线程还睡在 sleep_for 里，不会碰到主线程刚改过的块表。
//  - `transHisData()` 起的 `proc_loop` 线程不做 join（release() 的 notify_all + join 有丢唤醒窗口，
//    见 bugs/p10_storage_writer.md BUG-P10-005），改用「等 marker.ini 出现」作为队列排空的信号：
//    transHisData 先 push 全部合约、**最后** push "MARK.<sid>"，proc_loop 按序处理并在 MARK 分支
//    写 marker.ini（:2395-2402），所以 marker 落地即代表前面所有合约的收盘作业已经完成。
#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
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
#include "Includes/IDataWriter.h"
#include "Includes/FasterDefs.h"
#include "WTSUtils/WTSCfgLoader.h"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "Share/fmtlib.h"
#include "WtDataStorage/DataDefine.h"
#include "WtDataStorage/WtDataWriter.h"

namespace r23 {

using std::map;
using std::set;
using std::string;
using std::vector;

// ---------------------------------------------------------------- 通用小工具
inline string read_file_bytes(const string& path)
{
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return string();
	std::stringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

inline bool file_exists(const string& path)
{
	std::error_code ec;
	return std::filesystem::exists(path, ec);
}

inline uintmax_t file_size(const string& path)
{
	std::error_code ec;
	const auto sz = std::filesystem::file_size(path, ec);
	return ec ? 0 : sz;
}

// 把 mmap 块文件按 header 字节读回来（复制一份再强转，避免对齐/越界读）
template<typename H>
inline H read_block_header(const string& path)
{
	H head;
	std::memset(&head, 0, sizeof(head));
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (ifs)
		ifs.read((char*)&head, (std::streamsize)sizeof(head));
	return head;
}

// 读块文件里第 idx 个 item（T 为载荷结构体，头类型为 H）
template<typename H, typename T>
inline bool read_block_item(const string& path, uint32_t idx, T& out)
{
	const H head = read_block_header<H>(path);
	const uint64_t off = (uint64_t)sizeof(H) + (uint64_t)idx * sizeof(T);
	if ((uint64_t)head._size <= (uint64_t)idx)
		return false;
	std::ifstream ifs(path.c_str(), std::ios::binary);
	if (!ifs)
		return false;
	ifs.seekg((std::streamoff)off);
	return ifs.read((char*)&out, (std::streamsize)sizeof(T)).good();
}

// ---------------------------------------------------------------- 宿主替身
class WriterBDMgr : public IBaseDataMgr
{
public:
	WriterBDMgr()
	{
		_day = WTSSessionInfo::create("R23_DAY", "r23 日盘", 0);
		_day->addTradingSection(900, 1130);
		_day->addTradingSection(1300, 1500);

		_night = WTSSessionInfo::create("R23_NIGHT", "r23 夜盘", 180);
		_night->addTradingSection(2100, 2300);

		_comm = WTSCommodityInfo::create("rb", "r23 螺纹钢", "SHFE", "R23_DAY", "CHN");
		_comm->setSessionInfo(_day);
		_comm->setCategory(ContractCategory::CC_Future);

		_ncomm = WTSCommodityInfo::create("fn", "r23 夜盘品种", "SHFE", "R23_NIGHT", "CHN");
		_ncomm->setSessionInfo(_night);
		_ncomm->setCategory(ContractCategory::CC_Future);

		add_contract("rb2210", _comm);
		add_contract("rb2211", _comm);
		add_contract("rb2401", _comm);
		add_contract("fn2201", _ncomm);
	}

	virtual ~WriterBDMgr()
	{
		for (auto& v : _cts)
			v.second->release();
		_comm->release();
		_ncomm->release();
		_day->release();
		_night->release();
	}

	void add_contract(const char* code, WTSCommodityInfo* comm)
	{
		WTSContractInfo* ct = WTSContractInfo::create(code, code, "SHFE", comm->getProduct());
		ct->setCommInfo(comm);
		_cts[string("SHFE.") + code] = ct;
		comm->addCode(code);
	}

	void forget_contract(const char* code)
	{
		const string key = string("SHFE.") + code;
		auto it = _cts.find(key);
		if (it == _cts.end())
			return;
		it->second->release();
		_cts.erase(it);
	}

	WTSCommodityInfo* getCommodity(const char*) override { return _comm; }

	WTSCommodityInfo* getCommodity(const char*, const char* pid) override
	{
		if (pid != NULL && strcmp(pid, "fn") == 0)
			return _ncomm;
		return _comm;
	}

	WTSContractInfo* getContract(const char* code, const char* exchg = "", uint32_t = 0) override
	{
		const string key = string(exchg == NULL ? "" : exchg) + "." + (code == NULL ? "" : code);
		auto it = _cts.find(key);
		return it == _cts.end() ? NULL : it->second;
	}

	WTSArray* getContracts(const char* = "", uint32_t = 0) override { return NULL; }

	WTSSessionInfo* getSession(const char* sid) override
	{
		if (sid != NULL && strcmp(sid, "R23_NIGHT") == 0)
			return _night;
		return _day;
	}

	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _day; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	uint32_t calcTradingDate(const char*, uint32_t uDate, uint32_t, bool) override
	{
		if (uDate == 0)
			return _cur_tdate;
		auto it = _override.find(uDate);
		return it == _override.end() ? uDate : it->second;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	// loadCache 会调它决定 cache.dmb 的容量（WtDataWriter.cpp:272）
	uint32_t getContractSize(const char* = "", uint32_t = 0) override
	{
		_size_calls++;
		return _contract_size;
	}

	void set_contract_size(uint32_t n) { _contract_size = n; }
	uint32_t size_calls() const { return _size_calls; }
	void set_cur_tdate(uint32_t d) { _cur_tdate = d; }
	void set_override(uint32_t from, uint32_t to) { _override[from] = to; }

private:
	WTSSessionInfo*	_day = NULL;
	WTSSessionInfo*	_night = NULL;
	WTSCommodityInfo*	_comm = NULL;
	WTSCommodityInfo*	_ncomm = NULL;
	map<string, WTSContractInfo*>	_cts;
	map<uint32_t, uint32_t>			_override;
	uint32_t	_contract_size = 8;
	uint32_t	_size_calls = 0;
	uint32_t	_cur_tdate = 20240108;
};

struct LogLine
{
	WTSLogLevel	_ll;
	string		_text;
};

class WriterSink : public IDataWriterSink
{
public:
	explicit WriterSink(IBaseDataMgr* bd) : _bd(bd) {}
	virtual ~WriterSink() {}

	IBaseDataMgr* getBDMgr() override { return _bd; }

	bool canSessionReceive(const char*) override { return _can_recv; }
	void set_can_receive(bool v) { _can_recv = v; }

	void broadcastTick(WTSTickData*) override { _ticks++; }
	void broadcastOrdQue(WTSOrdQueData*) override { _queues++; }
	void broadcastOrdDtl(WTSOrdDtlData*) override { _dtls++; }
	void broadcastTrans(WTSTransData*) override { _trans++; }

	uint32_t ticks() const { return _ticks; }
	uint32_t queues() const { return _queues; }
	uint32_t dtls() const { return _dtls; }
	uint32_t trans() const { return _trans; }

	CodeSet* getSessionComms(const char* sid) override
	{
		_last_sid = sid == NULL ? "" : sid;
		return _comms_null ? NULL : &_comms;
	}
	void add_comm(const char* exchgpid) { _comms.insert(exchgpid); }
	void set_comms_null(bool v) { _comms_null = v; }
	const string& last_sid() const { return _last_sid; }

	uint32_t getTradingDate(const char*) override { return _tdate; }
	void set_tdate(uint32_t d) { _tdate = d; }

	void outputLog(WTSLogLevel ll, const char* message) override
	{
		LogLine line;
		line._ll = ll;
		line._text = message == NULL ? "" : message;
		_logs.push_back(line);
		_counts[ll]++;
		_total++;
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _counts.find(ll);
		return it == _counts.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	string last_log() const { return _logs.empty() ? string() : _logs.back()._text; }

	// 返回最后一条包含 needle 的日志级别，没有则返回 LL_MAX 之外的哨兵 -1
	int find_log(const char* needle, WTSLogLevel ll) const
	{
		int idx = -1;
		for (size_t i = 0; i < _logs.size(); i++)
		{
			if (_logs[i]._ll == ll && _logs[i]._text.find(needle) != string::npos)
				idx = (int)i;
		}
		return idx;
	}

	bool has_log(const char* needle, WTSLogLevel ll) const { return find_log(needle, ll) >= 0; }

private:
	IBaseDataMgr*	_bd;
	CodeSet			_comms;
	string			_last_sid;
	bool			_can_recv = true;
	bool			_comms_null = false;
	uint32_t		_ticks = 0;
	uint32_t		_queues = 0;
	uint32_t		_dtls = 0;
	uint32_t		_trans = 0;
	uint32_t		_tdate = 20240108;
	uint32_t		_total = 0;
	vector<LogLine>					_logs;
	map<WTSLogLevel, uint32_t>		_counts;
};

// 只为把 protected 的 _dumpers 开放给用例；不改变任何行为
class R23Writer : public WtDataWriter
{
public:
	void add_dumper(const char* id, IHisDataDumper* dumper) { _dumpers[id] = dumper; }
};

class FakeDumper : public IHisDataDumper
{
public:
	virtual ~FakeDumper() {}

	bool dumpHisBars(const char* stdCode, const char* period, WTSBarStruct* bars, uint32_t count) override
	{
		_bar_calls++;
		_bar_keys.insert(stdCode);
		_bar_periods[period] = count;
		if (bars != NULL && count > 0)
			_last_bars.push_back(*bars);
		return _ret;
	}

	bool dumpHisTicks(const char* stdCode, uint32_t uDate, WTSTickStruct* ticks, uint32_t count) override
	{
		_tick_calls++;
		_tick_key = stdCode;
		_tick_date = uDate;
		_tick_count = count;
		if (ticks != NULL && count > 0)
			_first_tick = *ticks;
		return _ret;
	}

	bool dumpHisOrdQue(const char*, uint32_t uDate, WTSOrdQueStruct* items, uint32_t count) override
	{
		_que_calls++;
		_que_date = uDate;
		_que_count = count;
		if (items != NULL && count > 0)
			_first_que = *items;
		return _ret;
	}

	bool dumpHisOrdDtl(const char*, uint32_t uDate, WTSOrdDtlStruct* items, uint32_t count) override
	{
		_dtl_calls++;
		_dtl_date = uDate;
		_dtl_count = count;
		if (items != NULL && count > 0)
			_first_dtl = *items;
		return _ret;
	}

	bool dumpHisTrans(const char*, uint32_t uDate, WTSTransStruct* items, uint32_t count) override
	{
		_trns_calls++;
		_trns_date = uDate;
		_trns_count = count;
		if (items != NULL && count > 0)
			_first_trns = *items;
		return _ret;
	}

	void set_ret(bool v) { _ret = v; }
	uint32_t bar_calls() const { return _bar_calls; }
	uint32_t tick_calls() const { return _tick_calls; }
	uint32_t que_calls() const { return _que_calls; }
	uint32_t dtl_calls() const { return _dtl_calls; }
	uint32_t trns_calls() const { return _trns_calls; }
	uint32_t bar_count_of(const char* period) const
	{
		auto it = _bar_periods.find(period);
		return it == _bar_periods.end() ? 0u : it->second;
	}
	const set<string>& bar_keys() const { return _bar_keys; }
	const vector<WTSBarStruct>& last_bars() const { return _last_bars; }
	uint32_t tick_count() const { return _tick_count; }
	uint32_t tick_date() const { return _tick_date; }
	const WTSTickStruct& first_tick() const { return _first_tick; }
	const string& tick_key() const { return _tick_key; }
	uint32_t trns_count() const { return _trns_count; }
	const WTSTransStruct& first_trns() const { return _first_trns; }
	uint32_t que_count() const { return _que_count; }
	const WTSOrdQueStruct& first_que() const { return _first_que; }
	uint32_t dtl_count() const { return _dtl_count; }
	const WTSOrdDtlStruct& first_dtl() const { return _first_dtl; }

private:
	bool	_ret = true;
	uint32_t	_bar_calls = 0;
	uint32_t	_tick_calls = 0;
	uint32_t	_que_calls = 0;
	uint32_t	_dtl_calls = 0;
	uint32_t	_trns_calls = 0;
	set<string>	_bar_keys;
	map<string, uint32_t>	_bar_periods;
	vector<WTSBarStruct>	_last_bars;
	string		_tick_key;
	uint32_t	_tick_date = 0;
	uint32_t	_tick_count = 0;
	WTSTickStruct	_first_tick;
	uint32_t	_trns_date = 0;
	uint32_t	_trns_count = 0;
	WTSTransStruct	_first_trns;
	uint32_t	_que_date = 0;
	uint32_t	_que_count = 0;
	WTSOrdQueStruct	_first_que;
	uint32_t	_dtl_date = 0;
	uint32_t	_dtl_count = 0;
	WTSOrdDtlStruct	_first_dtl;
};

// ---------------------------------------------------------------- tick 规格
struct TickSpec
{
	string		code = "rb2210";
	string		exchg = "SHFE";
	uint32_t	trading_date = 20240108;
	uint32_t	action_date = 20240108;
	uint32_t	action_time = 93000000;
	double		price = 3600.0;
	double		open = 3590.0;
	double		high = 3610.0;
	double		low = 3580.0;
	double		settle = 3595.0;
	double		upper_limit = 3800.0;
	double		lower_limit = 3400.0;
	double		total_volume = 1000.0;
	double		volume = 10.0;
	double		total_turnover = 3600000.0;
	double		turn_over = 36000.0;
	double		open_interest = 90000.0;
	double		diff_interest = 10.0;
	double		pre_interest = 89990.0;
	double		pre_close = 3585.0;
	double		pre_settle = 3590.0;
};

struct OrdQueSpec
{
	uint32_t	trading_date = 20240108;
	uint32_t	action_date = 20240108;
	uint32_t	action_time = 93000000;
	double		price = 3600.0;
	uint32_t	order_items = 3;
};

// ---------------------------------------------------------------- 用例基类
class R23WriterTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		seq++;
		_dir = std::filesystem::temp_directory_path()
			/ fmtutil::format("r23_w_{}_{}", (uint32_t)::getpid(), seq);
		std::error_code ec;
		std::filesystem::create_directories(_dir, ec);
		ASSERT_FALSE(ec) << ec.message();
		_base = _dir.string() + "/";

		_bd.reset(new WriterBDMgr());
		_sink.reset(new WriterSink(_bd.get()));
		_sink->add_comm("SHFE.rb");
	}

	void TearDown() override
	{
		for (auto& d : _datas)
			d->release();
		_datas.clear();

		// 见文件头：init() 起的 _proc_chk 不可 join，delete 会 std::terminate，故只 release 不 delete
		for (auto& h : _holders)
		{
			if (!h._closed)
				h._w->release();
		}
		_holders.clear();

		_bd.reset();
		_sink.reset();

		std::error_code ec;
		std::filesystem::remove_all(_dir, ec);
	}

	string path_of(const char* name) const { return _base + name; }

	string write_params(const char* extra = "")
	{
		// 默认参数表。extra 里出现的同名键必须**替换**默认项而不是再追加一遍：
		// rapidjson 的 GetMember 只取第一个匹配，重复键会让覆盖静默失效（本车道自查修掉的坑）。
		vector<string> kv;
		kv.push_back("\"path\":\"" + _base + "\"");
		kv.push_back("\"cache\":\"cache.dmb\"");
		kv.push_back("\"async\":false");
		kv.push_back("\"savelog\":false");
		kv.push_back("\"groupsize\":1");
		kv.push_back("\"skip_notrade_tick\":false");
		kv.push_back("\"skip_notrade_bar\":false");
		kv.push_back("\"disablehis\":false");
		kv.push_back("\"disabletick\":false");
		kv.push_back("\"disablemin1\":false");
		kv.push_back("\"disablemin5\":false");
		kv.push_back("\"disableday\":false");
		kv.push_back("\"disabletrans\":false");
		kv.push_back("\"disableordque\":false");
		kv.push_back("\"disableorddtl\":false");
		kv.push_back("\"minbar_price_mode\":0");

		if (extra != NULL && extra[0] != '\0')
		{
			const string s(extra);
			size_t pos = 0;
			while (pos < s.size())
			{
				int depth = 0;
				const size_t start = pos;
				size_t i = pos;
				for (; i < s.size(); i++)
				{
					if (s[i] == '{' || s[i] == '[')
						depth++;
					else if (s[i] == '}' || s[i] == ']')
						depth--;
					else if (s[i] == ',' && depth == 0)
						break;
				}
				const string piece = s.substr(start, i - start);
				const size_t q1 = piece.find('"');
				const size_t q2 = q1 == string::npos ? string::npos : piece.find('"', q1 + 1);
				if (q1 != string::npos && q2 != string::npos)
				{
					const string key = piece.substr(q1 + 1, q2 - q1 - 1);
					const string probe = "\"" + key + "\"";
					bool replaced = false;
					for (size_t k = 0; k < kv.size(); k++)
					{
						const string& item = kv[k];
						if (item.size() > probe.size() + 1
							&& item[0] == '"'
							&& item.compare(0, probe.size(), probe) == 0
							&& item[probe.size()] == ':')
						{
							kv[k] = piece;
							replaced = true;
							break;
						}
					}
					if (!replaced)
						kv.push_back(piece);
				}
				if (i < s.size())
					pos = i + 1;
				else
					break;
			}
		}

		string body = "{";
		for (size_t k = 0; k < kv.size(); k++)
		{
			if (k > 0)
				body += ",";
			body += kv[k];
		}
		body += "}";

		const string file = path_of("writer.json");
		std::ofstream ofs(file);
		ofs << body;
		ofs.close();
		return file;
	}

	// 真正 init 一个 writer（会起 check_loop 线程，见文件头纪律）
	R23Writer* make_writer(const char* extra = "")
	{
		const string file = write_params(extra);
		WTSVariant* cfg = WTSCfgLoader::load_from_file(file.c_str());
		EXPECT_TRUE(cfg != NULL) << "解析 " << file << " 失败";
		R23Writer* w = new R23Writer();
		if (cfg != NULL)
		{
			EXPECT_TRUE(w->init(cfg, _sink.get()));
			cfg->release();
		}
		Holder h;
		h._w = w;
		h._closed = false;
		_holders.push_back(h);
		return w;
	}

	void close_writer(R23Writer* w)
	{
		for (auto& h : _holders)
		{
			if (h._w == w && !h._closed)
			{
				h._closed = true;
				w->release();
				return;
			}
		}
	}

	// 走过 transHisData() 的 writer 只能这么处理：
	// release() 里对 _proc_thrd 的 notify_all + join 有丢唤醒窗口（proc_loop 的
	// `_proc_que.empty()` 判在加锁之外，:2220-2224，无谓词 wait），
	// 撞上就是永久阻塞 → 会被 runner 记成 HANG。这里只标记「TearDown 不要再动它」，
	// proc_loop 线程留在 wait 里，由进程退出回收（一用例一进程）。
	void abandon_writer(R23Writer* w)
	{
		for (auto& h : _holders)
		{
			if (h._w == w)
				h._closed = true;
		}
	}

	R23Writer* unclosed_writer()
	{
		for (auto& h : _holders)
			if (!h._closed)
				return h._w;
		return NULL;
	}

	WTSTickData* make_tick_data(const TickSpec& spec)
	{
		WTSTickStruct ts;
		std::memset(&ts, 0, sizeof(ts));
		snprintf(ts.exchg, sizeof(ts.exchg), "%s", spec.exchg.c_str());
		snprintf(ts.code, sizeof(ts.code), "%s", spec.code.c_str());
		ts.price = spec.price;
		ts.open = spec.open;
		ts.high = spec.high;
		ts.low = spec.low;
		ts.settle_price = spec.settle;
		ts.upper_limit = spec.upper_limit;
		ts.lower_limit = spec.lower_limit;
		ts.total_volume = spec.total_volume;
		ts.volume = spec.volume;
		ts.total_turnover = spec.total_turnover;
		ts.turn_over = spec.turn_over;
		ts.open_interest = spec.open_interest;
		ts.diff_interest = spec.diff_interest;
		ts.trading_date = spec.trading_date;
		ts.action_date = spec.action_date;
		ts.action_time = spec.action_time;
		ts.pre_close = spec.pre_close;
		ts.pre_settle = spec.pre_settle;
		ts.pre_interest = spec.pre_interest;
		for (uint32_t i = 0; i < 10; i++)
		{
			ts.bid_prices[i] = spec.price - 0.5 - i * 0.1;
			ts.ask_prices[i] = spec.price + 0.5 + i * 0.1;
			ts.bid_qty[i] = 100.0 + i;
			ts.ask_qty[i] = 200.0 + i;
		}

		WTSTickData* ret = WTSTickData::create(ts);
		ret->setContractInfo(_bd->getContract(spec.code.c_str(), spec.exchg.c_str()));
		_datas.push_back(ret);
		return ret;
	}

	WTSOrdQueData* make_ordque_data(const OrdQueSpec& spec)
	{
		WTSOrdQueStruct oq;
		std::memset(&oq, 0, sizeof(oq));
		snprintf(oq.exchg, sizeof(oq.exchg), "%s", "SHFE");
		snprintf(oq.code, sizeof(oq.code), "%s", "rb2210");
		oq.trading_date = spec.trading_date;
		oq.action_date = spec.action_date;
		oq.action_time = spec.action_time;
		oq.price = spec.price;
		oq.order_items = spec.order_items;
		oq.qsize = 5;
		for (uint32_t i = 0; i < 50; i++)
			oq.volumes[i] = 10 + i;
		oq.side = (spec.order_items % 2) == 0 ? BDT_Buy : BDT_Sell;
		WTSOrdQueData* ret = WTSOrdQueData::create(oq);
		ret->setContractInfo(_bd->getContract("rb2210", "SHFE"));
		return ret;
	}

	WTSOrdDtlData* make_orddtl_data(uint32_t atime, double price, uint32_t index)
	{
		WTSOrdDtlStruct od;
		std::memset(&od, 0, sizeof(od));
		snprintf(od.exchg, sizeof(od.exchg), "%s", "SHFE");
		snprintf(od.code, sizeof(od.code), "%s", "rb2210");
		od.trading_date = 20240108;
		od.action_date = 20240108;
		od.action_time = atime;
		od.price = price;
		od.volume = 100.0;
		od.index = index;
		od.side = BDT_Buy;
		od.otype = ODT_LimitPrice;
		od.orderid = 700000 + index;
		WTSOrdDtlData* ret = WTSOrdDtlData::create(od);
		ret->setContractInfo(_bd->getContract("rb2210", "SHFE"));
		return ret;
	}

	WTSTransData* make_trans_data(uint32_t atime, double price, uint32_t index)
	{
		WTSTransStruct tr;
		std::memset(&tr, 0, sizeof(tr));
		snprintf(tr.exchg, sizeof(tr.exchg), "%s", "SHFE");
		snprintf(tr.code, sizeof(tr.code), "%s", "rb2210");
		tr.trading_date = 20240108;
		tr.action_date = 20240108;
		tr.action_time = atime;
		tr.price = price;
		tr.volume = 200.0;
		tr.index = index;
		tr.ttype = TT_Match;
		tr.side = BDT_Sell;
		tr.askorder = 300000 + index;
		tr.bidorder = 400000 + index;
		WTSTransData* ret = WTSTransData::create(tr);
		ret->setContractInfo(_bd->getContract("rb2210", "SHFE"));
		return ret;
	}

	// 便捷写入：返回被写入的 tick 对象（生产代码会原地改写它的 volume/turn_over/action_time 等）
	WTSTickData* write_tick(R23Writer* w, const TickSpec& spec, uint32_t proc_flag = 0)
	{
		WTSTickData* d = make_tick_data(spec);
		EXPECT_TRUE(w->writeTick(d, proc_flag));
		return d;
	}

	string cache_file() const { return _base + "cache.dmb"; }
	string rt_tick_file(const char* code) const { return _base + "rt/ticks/SHFE/" + code + ".dmb"; }
	string rt_min1_file(const char* code) const { return _base + "rt/min1/SHFE/" + code + ".dmb"; }
	string rt_min5_file(const char* code) const { return _base + "rt/min5/SHFE/" + code + ".dmb"; }
	string his_tick_file(const char* code, uint32_t date) const
	{
		return fmtutil::format("{}his/ticks/SHFE/{}/{}.dsb", _base, date, code);
	}
	string his_min1_file(const char* code) const { return _base + "his/min1/SHFE/" + code + ".dsb"; }
	string his_min5_file(const char* code) const { return _base + "his/min5/SHFE/" + code + ".dsb"; }
	string his_day_file(const char* code) const { return _base + "his/day/SHFE/" + code + ".dsb"; }

	// 等 proc_loop 把队列排空（marker.ini 是最后一个任务，见文件头）
	// 返回 true 表示等到了；上限 10s，超时就失败，绝不无界等待
	bool wait_marker(const char* sid, uint32_t max_ms = 10000)
	{
		const string file = _base + "marker.ini";
		uint32_t waited = 0;
		while (waited < max_ms)
		{
			if (file_exists(file))
			{
				const string content = read_file_bytes(file);
				if (content.find(sid) != string::npos)
					return true;
			}
			usleep(5000);
			waited += 5;
		}
		return false;
	}

	std::filesystem::path	_dir;
	string	_base;
	std::unique_ptr<WriterBDMgr>	_bd;
	std::unique_ptr<WriterSink>		_sink;

private:
	struct Holder
	{
		R23Writer*	_w;
		bool		_closed;
	};

	vector<WTSTickData*>	_datas;
	vector<Holder>			_holders;
};

} // namespace r23
