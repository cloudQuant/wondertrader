// r21 / WtDataStorageAD 共用替身（不是用例本体，仅供 r21_*.cpp 引用）
//
// 约束（本轮硬规则）：
//  - src/** 只读，不改产品代码、不放宽断言；
//  - 禁止 #define private public：只走 WtRdmDtReaderAD / WtBtDtReaderAD 的公开接口
//    （init / readTickSliceByRange / readKlineSliceByRange / read_raw_bars / read_raw_ticks），
//    缓存内容一律通过「返回值 + sink 日志」间接观测；
//  - 禁止起线程 / sleep / 真实 socket / 真实 shm / 读挂钟；
//  - LMDB 一律开在自己 mkdtemp 出来的临时目录（本夹具的 _root），仓库零污染：
//    WtRdmDtReaderAD::get_k_db/get_t_db 会对 `{base}{subdir}/{exchg}/` 调 create_directories，
//    所以 _base_dir 必须是绝对临时目录，绝不能用 cfg==NULL 的那条路径（会退化成 cwd 相对路径）；
//  - 键的写入形态复刻生产写入器 WtDataWriterAD::pipeToTicks:425-427
//    （offTime = offsetTime(HHMM,true)*100000 + SSsss，date 用交易日）与
//    pipeToM1Bars/pipeToDayBars 的 LMDBBarKey(exchg, code, bartime)。
//    本夹具的交易时段偏移固定为 0，故 offsetTime(HHMM) == HHMM，键时间 == action_time 本身。
#pragma once

#include <gtest/gtest.h>

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
#include "Includes/IHotMgr.h"
#include "Includes/IRdmDtReader.h"
#include "Includes/IBtDtReader.h"
#include "Share/StrUtil.hpp"
#include "Share/TimeUtils.hpp"
#include "WtDataStorageAD/LMDBKeys.h"
#include "WtDataStorageAD/WtRdmDtReaderAD.h"
#include "WtDataStorageAD/WtBtDtReaderAD.h"
#include "WTSUtils/WtLMDB.hpp"

using namespace wtp;

// 不能写成匿名命名空间：本头被 r21_lmdbkeys_btreader / r21_rdmbars_kline / r21_rdmtick_slice
// 三个 TU 包含，匿名命名空间会让每个 TU 得到「同名但不同种」的 R21ReaderADTest，
// gtest 在运行期就拒绝整族用例（"All tests in the same test suite must use the same
// test fixture class"），实测一次连带 17 条 FAIL。具名 + using 指令保持非限定查找可用。
namespace r21fx {

[[maybe_unused]] static void r21_set_str(char* dst, size_t cap, const char* src)
{
	snprintf(dst, cap, "%s", src);
}

//////////////////////////////////////////////////////////////////////////
// IBaseDataMgr 替身：交易时段 0000-2359、偏移 0 分钟
// —— offsetTime(t,left/right) 在偏移为 0 时是恒等映射（WTSSessionInfo.hpp:507-527），
//    这样测试里的期望值可以直接用字面量写死。
//////////////////////////////////////////////////////////////////////////
class R21BaseDataMgr : public IBaseDataMgr
{
public:
	R21BaseDataMgr()
	{
		_session = WTSSessionInfo::create("R21_DAY", "r21 全天候时段", 0);
		_session->addTradingSection(0, 2359);
		_comm = WTSCommodityInfo::create("rb", "螺纹钢", "XEX", "R21_DAY", "CHN");
		_comm->setSessionInfo(_session);
		_comm->setCategory(ContractCategory::CC_Future);
	}

	virtual ~R21BaseDataMgr()
	{
		// WTSCommodityInfo / WTSSessionInfo 是 WTSObject（析构 protected），只能 release
		_comm->release();
		_session->release();
	}

	WTSCommodityInfo* getCommodity(const char* exchgpid) override
	{
		_last_pid = exchgpid == NULL ? "" : exchgpid;
		return _comm;
	}

	WTSCommodityInfo* getCommodity(const char* exchg, const char* pid) override
	{
		_last_pid = std::string(exchg == NULL ? "" : exchg) + "." + (pid == NULL ? "" : pid);
		return _comm;
	}

	WTSContractInfo* getContract(const char*, const char*, uint32_t) override { return NULL; }
	WTSArray* getContracts(const char*, uint32_t) override { return NULL; }
	WTSSessionInfo* getSession(const char* sid) override { _last_sid = sid; return _session; }
	WTSSessionInfo* getSessionByCode(const char*, const char*) override { return _session; }
	WTSArray* getAllSessions() override { return NULL; }
	bool isHoliday(const char*, uint32_t, bool) override { return false; }

	// 不做日历推导：交易日 == 自然日，便于用固定键值断言
	uint32_t calcTradingDate(const char* stdPID, uint32_t uDate, uint32_t, bool) override
	{
		_pid_seen = stdPID == NULL ? "" : stdPID;
		_tdate_calls++;
		return uDate;
	}

	uint64_t getBoundaryTime(const char*, uint32_t tDate, bool, bool isStart) override
	{
		return isStart ? ((uint64_t)tDate) * 1000000000ull + 90000000ull
			: ((uint64_t)tDate) * 1000000000ull + 150000000ull;
	}

	const std::string& last_pid() const { return _last_pid; }
	const std::string& pid_seen() const { return _pid_seen; }
	uint32_t tdate_calls() const { return _tdate_calls; }

private:
	WTSSessionInfo*	_session = NULL;
	WTSCommodityInfo*	_comm = NULL;
	std::string		_last_pid;
	std::string		_pid_seen;
	std::string		_last_sid;
	uint32_t		_tdate_calls = 0;
};

class R21HotMgr : public IHotMgr
{
public:
	virtual ~R21HotMgr() {}

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
	const char* getRuleTag(const char*) override { return _tag.empty() ? "" : _tag.c_str(); }
	double getRuleFactor(const char*, const char*, uint32_t) override { return 1.0; }

	void set_rule_tag(const char* tag) { _tag = tag; }

private:
	std::string	_tag;
};

//////////////////////////////////////////////////////////////////////////
// IRdmDtReaderSink 替身：按级别计数 + 保留每条原文（用于「重复开环」这类计数断言）
//////////////////////////////////////////////////////////////////////////
class R21RdmSink : public IRdmDtReaderSink
{
public:
	R21RdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	virtual ~R21RdmSink() {}

	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_cnt[ll]++;
		_total++;
		if (message != NULL)
		{
			_last[ll] = message;
			_all.push_back(std::string(ll == LL_DEBUG ? "D|" : ll == LL_INFO ? "I|" :
				ll == LL_WARN ? "W|" : ll == LL_ERROR ? "E|" : "T|") + message);
		}
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _cnt.find(ll);
		return it == _cnt.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}
	// 包含子串的记录条数（例如 "opened" = 开环次数）
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
	IHotMgr*		_hot;
	wt_hashmap<WTSLogLevel, uint32_t> _cnt;
	wt_hashmap<WTSLogLevel, std::string> _last;
	std::vector<std::string> _all;
	uint32_t		_total = 0;
};

class R21BtSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_cnt[ll]++;
		_total++;
		if (message != NULL) _last[ll] = message;
	}

	uint32_t count_of(WTSLogLevel ll) const
	{
		auto it = _cnt.find(ll);
		return it == _cnt.end() ? 0u : it->second;
	}
	uint32_t total() const { return _total; }
	std::string last_log(WTSLogLevel ll) const
	{
		auto it = _last.find(ll);
		return it == _last.end() ? std::string() : it->second;
	}

private:
	wt_hashmap<WTSLogLevel, uint32_t> _cnt;
	wt_hashmap<WTSLogLevel, std::string> _last;
	uint32_t	_total = 0;
};

//////////////////////////////////////////////////////////////////////////
// 键/值构造：与生产写入器保持一致的落库形态
//////////////////////////////////////////////////////////////////////////
[[maybe_unused]] static std::string r21_key_str(const LMDBBarKey& key)
{
	return std::string((const char*)&key, sizeof(key));
}

[[maybe_unused]] static std::string r21_key_str(const LMDBHftKey& key)
{
	return std::string((const char*)&key, sizeof(key));
}

[[maybe_unused]] static std::string r21_raw_of(const WTSBarStruct& bar)
{
	return std::string((const char*)&bar, sizeof(bar));
}

[[maybe_unused]] static std::string r21_raw_of(const WTSTickStruct& tick)
{
	return std::string((const char*)&tick, sizeof(tick));
}

// WtDataWriterAD::pipeToTicks:425-426 的换算（本夹具偏移=0 → offsetTime 恒等）
[[maybe_unused]] static uint32_t r21_tick_key_time(uint32_t action_time)
{
	return (action_time / 100000) * 100000 + (action_time % 100000);
}

[[maybe_unused]] static WTSTickStruct r21_make_tick(const char* exchg, const char* code,
	uint32_t tdate, uint32_t adate, uint32_t atime, double price)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	r21_set_str(tick.exchg, sizeof(tick.exchg), exchg);
	r21_set_str(tick.code, sizeof(tick.code), code);
	tick.price = price;
	tick.open = price - 1.0;
	tick.high = price + 1.0;
	tick.low = price - 2.0;
	tick.volume = price * 2.0;
	tick.turn_over = price * 200.0;
	tick.total_volume = price * 10.0;
	tick.total_turnover = price * 1000.0;
	tick.open_interest = price * 5.0;
	tick.diff_interest = 1.0;
	tick.trading_date = tdate;
	tick.action_date = adate;
	tick.action_time = atime;
	return tick;
}

// 分钟 bar 的 time 语义：TimeUtils::timeToMinBar = (自然日-19900000)*10000 + HHMM
[[maybe_unused]] static uint64_t r21_min_bar_time(uint32_t uDate, uint32_t uHHMM)
{
	return (uint64_t)(uDate - 19900000) * 10000 + uHHMM;
}

[[maybe_unused]] static WTSBarStruct r21_make_bar(uint32_t date, uint64_t time, double base)
{
	WTSBarStruct bar;
	memset(&bar, 0, sizeof(bar));
	bar.date = date;
	bar.time = time;
	bar.open = base;
	bar.high = base + 2.0;
	bar.low = base - 2.0;
	bar.close = base + 1.0;
	bar.settle = base + 1.0;
	bar.money = base * 300.0;
	bar.vol = base * 3.0;
	bar.hold = base;
	bar.add = 1.0;
	return bar;
}

//////////////////////////////////////////////////////////////////////////
// 夹具：唯一临时目录 + 替身 + 独立作用域写库
//////////////////////////////////////////////////////////////////////////
class R21ReaderADTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("r21_rdmad_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root);
		_bd = new R21BaseDataMgr();
		_hot = new R21HotMgr();
		_sink = new R21RdmSink(_bd, _hot);
	}

	void TearDown() override
	{
		// 只读环境持有 mmap，reader 必须先于 remove_all 析构
		_reader.reset();
		delete _sink;
		delete _hot;
		delete _bd;
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::string path_of(const std::string& name) const { return (_root / name).string(); }

	// ---- 写库（独立读写环境，出作用域即关闭）----
	void put_ticks(const char* subdir_prefix, const char* exchg, const char* code,
		const std::vector<WTSTickStruct>& ticks)
	{
		const std::filesystem::path dir = _root / subdir_prefix / exchg / code;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSTickStruct& tick : ticks)
		{
			ASSERT_TRUE(query.put(r21_key_str(
				LMDBHftKey(exchg, code, tick.trading_date, r21_tick_key_time(tick.action_time))),
				r21_raw_of(tick)));
		}
		query.commit();
	}

	// reader 的 get_t_db 路径是 {base}ticks/{exchg}/{code}
	void put_tick_db(const char* exchg, const char* code, const std::vector<WTSTickStruct>& ticks)
	{
		put_ticks("ticks", exchg, code, ticks);
	}

	void put_bar_db(const char* subdir, const char* exchg, const char* code,
		const std::vector<WTSBarStruct>& bars, bool key_by_date)
	{
		const std::filesystem::path dir = _root / subdir / exchg;
		std::filesystem::create_directories(dir);
		WtLMDB db(false);
		ASSERT_TRUE(db.open(dir.string().c_str(), 8 * 1024 * 1024));
		WtLMDBQuery query(db);
		for (const WTSBarStruct& bar : bars)
		{
			const uint32_t tail = key_by_date ? (uint32_t)bar.date : (uint32_t)bar.time;
			ASSERT_TRUE(query.put(r21_key_str(LMDBBarKey(exchg, code, tail)), r21_raw_of(bar)));
		}
		query.commit();
	}

	// ---- 读取器 ----
	void init_reader(void)
	{
		_reader.reset(new WtRdmDtReaderAD());
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		_reader->init(cfg, _sink);
		cfg->release();
	}

	R21RdmSink* sink(void) { return _sink; }
	R21BaseDataMgr* bd(void) { return _bd; }
	WtRdmDtReaderAD* reader(void) { return _reader.get(); }

	std::filesystem::path _root;
	R21BaseDataMgr*		_bd = NULL;
	R21HotMgr*			_hot = NULL;
	R21RdmSink*			_sink = NULL;
	std::unique_ptr<WtRdmDtReaderAD> _reader;
};

}  // namespace r21fx

using namespace r21fx;
