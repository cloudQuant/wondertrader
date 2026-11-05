// Iteration 03 / decision card 13 on WtDataStorageAD:
//  - 13a=A prepare-then-publish: the tick cache is published only after the LMDB commit
//    (R4-BUG-05); a failed commit leaves cache/broadcast untouched and the same tick retries.
//  - 13b=B: two exec'd writer processes share one tick cache file (publish lock + index resync).
//  - 13c=A: unknown LMDB key layouts and legacy zero-date tick keys are quarantined (fail
//    closed, untouched); minute keys beyond uint32 are refused instead of wrapping
//    (R32-003/R100-003). The legacy keys below are SYNTHETIC layouts, not sourced samples.
#include "r4_ad_fixture.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <set>
#include <sys/wait.h>
#include <thread>
#include <chrono>

namespace {

std::string g_self;

std::string read_file_text(const std::string& p)
{
	std::ifstream ifs(p);
	std::stringstream ss;
	ss << ifs.rdbuf();
	return ss.str();
}

std::vector<std::pair<std::string, std::string>> read_db(const std::string& dir)
{
	std::vector<std::pair<std::string, std::string>> rows;
	if (!std::filesystem::exists(dir))
		return rows;
	WtLMDB db(false);
	if (!db.open(dir.c_str(), 8 * 1024 * 1024))
		return rows;
	MDB_txn* txn = nullptr;
	if (mdb_txn_begin(db.env(), nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS)
		return rows;
	MDB_dbi dbi = db.update_dbi(txn);
	MDB_cursor* cur = nullptr;
	if (mdb_cursor_open(txn, dbi, &cur) == MDB_SUCCESS)
	{
		MDB_val k, v;
		for (int rc = mdb_cursor_get(cur, &k, &v, MDB_FIRST); rc == MDB_SUCCESS; rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT))
			rows.emplace_back(std::string((const char*)k.mv_data, k.mv_size), std::string((const char*)v.mv_data, v.mv_size));
		mdb_cursor_close(cur);
	}
	mdb_txn_commit(txn);
	return rows;
}

bool put_raw(const std::string& dir, const std::string& key, const std::string& val)
{
	std::filesystem::create_directories(dir);
	WtLMDB db(false);
	if (!db.open(dir.c_str(), 8 * 1024 * 1024))
		return false;
	WtLMDBQuery q(db);
	return q.put_and_commit((void*)key.data(), key.size(), (void*)val.data(), val.size());
}

class MwAd : public R4WriterADTest
{
protected:
	void SetUp() override
	{
		R4WriterADTest::SetUp();
		_comm = _bd->add_commodity("SHFE", "rb", CC_Future);
		_ct = _bd->add_contract("SHFE", "rb2210", _comm);
	}
	std::string base() const { return _root.string() + "/"; }
	void init(bool disabletick, bool disablemin1, bool async = false)
	{
		init_writer(make_writer_cfg(_root.string(), 1000000, disabletick, disablemin1, true, true, async));
	}
	void stop_writer()
	{
		if (_writer)
			_writer->release();
		_writer.reset();
	}
	WTSTickStruct tick_at(uint32_t date, uint32_t atime, double px, double tv)
	{
		return make_tick("SHFE", "rb2210", date, date, atime, px, 1, px, 1000, 0, tv, px * tv);
	}

	WTSCommodityInfo* _comm = nullptr;
	WTSContractInfo* _ct = nullptr;
};

// R4-BUG-05: a failing LMDB path must not advance the cache, broadcast, or lose the retry.
TEST_F(MwAd, SyncDbFailureDoesNotPublishAndSameTickRetrySucceeds)
{
	init(false, true);
	const std::string dbDir = base() + "ticks/SHFE/rb2210";
	std::filesystem::create_directories(base() + "ticks/SHFE");
	{ std::ofstream(dbDir) << "not a directory"; }	// blocks directory + env creation

	const WTSTickStruct original = tick_at(20230103, 93000000, 4000.0, 10);
	WTSTickData* t1 = tick_of(_ct, original);
	bool ok = true;
	EXPECT_NO_THROW(ok = _writer->writeTick(t1, 0));
	t1->release();
	EXPECT_FALSE(ok) << "sync writeTick must report the persistence failure";
	WTSTickData* cur = _writer->getCurTick("rb2210", "SHFE");
	EXPECT_TRUE(cur == NULL) << "cache advanced although the tick was not persisted";
	if (cur) cur->release();
	EXPECT_EQ(_sink->bcast_tick(), 0u);
#if defined(WT_MW_GREEN)
	EXPECT_EQ(_writer->tickPersistFailures(), 1u);
#endif

	std::filesystem::remove(dbDir);
	WTSTickData* t2 = tick_of(_ct, original);
	EXPECT_TRUE(_writer->writeTick(t2, 0));
	t2->release();
	cur = _writer->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cur != NULL);
	EXPECT_EQ(cur->actiontime(), 93000000u) << "retry must not see a shifted duplicate";
	cur->release();
	EXPECT_EQ(_sink->bcast_tick(), 1u);
	stop_writer();

	auto rows = read_db(dbDir);
	ASSERT_EQ(rows.size(), 1u);
	ASSERT_EQ(rows[0].second.size(), sizeof(WTSTickStruct));
	WTSTickStruct stored;
	memcpy(&stored, rows[0].second.data(), sizeof(stored));
	EXPECT_EQ(stored.action_time, 93000000u);
	EXPECT_DOUBLE_EQ(stored.total_volume, 10.0);
}

#if defined(WT_MW_GREEN)
TEST_F(MwAd, AsyncDbFailureIsObservableAndNotPublished)
{
	init(false, true, true);
	const std::string dbDir = base() + "ticks/SHFE/rb2210";
	std::filesystem::create_directories(base() + "ticks/SHFE");
	{ std::ofstream(dbDir) << "x"; }
	WTSTickData* t = tick_of(_ct, tick_at(20230103, 93000000, 4000.0, 10));
	EXPECT_TRUE(_writer->writeTick(t, 0)) << "async accept only means queued";
	t->release();
	for (int i = 0; i < 5000 && _writer->tickPersistFailures() == 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	EXPECT_EQ(_writer->tickPersistFailures(), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2210", "SHFE");
	EXPECT_TRUE(cur == NULL);
	if (cur) cur->release();
	EXPECT_EQ(_sink->bcast_tick(), 0u);
}
#endif

// R32-003 / R100-003: v1 minute key is uint32; a 2034 bar must be refused, not wrapped.
TEST_F(MwAd, MinuteBarBeyondUint32KeyIsRefusedNotWrapped)
{
	init(true, false);
	WTSTickData* a = tick_of(_ct, tick_at(20340103, 93000000, 4000.0, 10));
	WTSTickData* b = tick_of(_ct, tick_at(20340103, 93100000, 4001.0, 20));
	EXPECT_TRUE(_writer->writeTick(a, 0));
	EXPECT_TRUE(_writer->writeTick(b, 0));	// closes the 09:31 bar -> pipeToM1Bars
	a->release();
	b->release();
	stop_writer();
	auto rows = read_db(base() + "min1/SHFE/");
	EXPECT_TRUE(rows.empty()) << "a wrapped uint32 key was written (" << rows.size() << " rows)";
	EXPECT_GE(_sink->count_containing("not representable"), 1u);
}

TEST_F(MwAd, MinuteBarWithinUint32KeyIsStillWritten)
{
	init(true, false);
	WTSTickData* a = tick_of(_ct, tick_at(20230103, 93000000, 4000.0, 10));
	WTSTickData* b = tick_of(_ct, tick_at(20230103, 93100000, 4001.0, 20));
	EXPECT_TRUE(_writer->writeTick(a, 0));
	EXPECT_TRUE(_writer->writeTick(b, 0));
	a->release();
	b->release();
	stop_writer();
	EXPECT_EQ(read_db(base() + "min1/SHFE/").size(), 1u);
}

// P05-010 / 13c=A: an existing DB whose key layout differs from the current one is not
// guessed or migrated; it is left untouched and the tick is not published.
TEST_F(MwAd, UnknownKeyLayoutIsQuarantinedUntouched)
{
	const std::string dbDir = base() + "ticks/SHFE/rb2210";
	std::string legacyKey(10 + 32 + 4, '\0');	// synthetic 10-byte-exchange layout
	memcpy(&legacyKey[0], "SHFE", 4);
	memcpy(&legacyKey[10], "rb2210", 6);
	WTSTickStruct v = tick_at(20230102, 93000000, 1.0, 1);
	ASSERT_TRUE(put_raw(dbDir, legacyKey, std::string((const char*)&v, sizeof(v))));

	init(false, true);
	WTSTickData* t = tick_of(_ct, tick_at(20230103, 93000000, 4000.0, 10));
	bool ok = true;
	EXPECT_NO_THROW(ok = _writer->writeTick(t, 0));
	t->release();
	EXPECT_FALSE(ok);
	EXPECT_GE(_sink->count_containing("quarantined"), 1u);
	WTSTickData* cur = _writer->getCurTick("rb2210", "SHFE");
	EXPECT_TRUE(cur == NULL);
	if (cur) cur->release();
	stop_writer();
	auto rows = read_db(dbDir);
	ASSERT_EQ(rows.size(), 1u) << "quarantined DB must not be written";
	EXPECT_EQ(rows[0].first, legacyKey);
}

TEST_F(MwAd, LegacyZeroDateTickKeyIsQuarantined)
{
	const std::string dbDir = base() + "ticks/SHFE/rb2210";
	LMDBHftKey zero("SHFE", "rb2210", 0, 0);
	WTSTickStruct v = tick_at(20230102, 93000000, 1.0, 1);
	ASSERT_TRUE(put_raw(dbDir, std::string((const char*)&zero, sizeof(zero)), std::string((const char*)&v, sizeof(v))));
	init(false, true);
	WTSTickData* t = tick_of(_ct, tick_at(20230103, 93000000, 4000.0, 10));
	EXPECT_FALSE(_writer->writeTick(t, 0));
	t->release();
	stop_writer();
	EXPECT_EQ(read_db(dbDir).size(), 1u);
}

TEST_F(MwAd, CurrentKeyLayoutDbKeepsWorking)
{
	const std::string dbDir = base() + "ticks/SHFE/rb2210";
	LMDBHftKey k("SHFE", "rb2210", 20230102, 0);
	WTSTickStruct v = tick_at(20230102, 93000000, 1.0, 1);
	ASSERT_TRUE(put_raw(dbDir, std::string((const char*)&k, sizeof(k)), std::string((const char*)&v, sizeof(v))));
	init(false, true);
	WTSTickData* t = tick_of(_ct, tick_at(20230103, 93000000, 4000.0, 10));
	EXPECT_TRUE(_writer->writeTick(t, 0));
	t->release();
	stop_writer();
	EXPECT_EQ(read_db(dbDir).size(), 2u);
}

// L04-004 control: an identity that does not fit the LMDB key is rejected before any side effect.
TEST_F(MwAd, OverlongExchangeRejectedBeforeSideEffects)
{
	WTSCommodityInfo* comm = _bd->add_commodity("ABCDEFGHIJKLMNOP", "rb", CC_Future);
	WTSContractInfo* ct = _bd->add_contract("ABCDEFGHIJKLMNOP", "rb2210", comm);
	init(false, true);
	WTSTickStruct s = make_tick("ABCDEFGHIJKLMNO", "rb2210", 20230103, 20230103, 93000000, 1, 1, 1, 1, 0, 1, 1);
	WTSTickData* t = WTSTickData::create(s);
	t->setContractInfo(ct);
	EXPECT_FALSE(_writer->writeTick(t, 0));
	t->release();
	EXPECT_FALSE(std::filesystem::exists(base() + "ticks"));
	EXPECT_EQ(_sink->bcast_tick(), 0u);
}

// ---------------------------------------------------------------- 13b=B: two processes
int run_ad_helper(int argc, char** argv)
{
	// argv: self --ad-helper base id codes rounds go
	if (argc < 7)
		return 90;
	const std::string base = argv[2];
	const uint32_t id = (uint32_t)std::strtoul(argv[3], nullptr, 10);
	const uint32_t codes = (uint32_t)std::strtoul(argv[4], nullptr, 10);
	const uint32_t rounds = (uint32_t)std::strtoul(argv[5], nullptr, 10);
	const std::string goFile = argv[6];
	R4BaseDataMgr bd;
	R4WriterSink sink(&bd);
	WTSCommodityInfo* comm = bd.add_commodity("SHFE", "rb", CC_Future);
	std::vector<WTSContractInfo*> cts;
	for (uint32_t k = 0; k < codes; ++k)
		cts.push_back(bd.add_contract("SHFE", ("a" + std::to_string(id) + "c" + std::to_string(k)).c_str(), comm));
	WtDataWriterAD* w = new WtDataWriterAD();
	// tick LMDB disabled (fd budget); m1/m5/day caches enabled so all four RT caches are shared
	WTSVariant* cfg = make_writer_cfg(base, 1000000, true, false, false, false);
	if (!w->init(cfg, &sink))
		return 91;
	cfg->release();
	std::ofstream(base + "ready." + std::to_string(id)) << "1";
	for (int i = 0; i < 20000 && !std::filesystem::exists(goFile); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	uint32_t rejected = 0;
	for (uint32_t r = 0; r < rounds; ++r)
	{
		for (uint32_t k = 0; k < codes; ++k)
		{
			WTSTickStruct s = make_tick("SHFE", cts[k]->getCode(), 20230103, 20230103, 93000000 + r * 1000,
				4000.0 + r, 1, 1, 1000, 0, 100.0 + r, 1000.0 + r);
			WTSTickData* t = WTSTickData::create(s);
			t->setContractInfo(cts[k]);
			if (!w->writeTick(t, 0))
				++rejected;
			t->release();
		}
	}
	std::printf("REJECTED=%u ERRORS=%u\n", rejected, sink.log_count(LL_ERROR));
	std::fflush(stdout);
	::_exit(rejected == 0 ? 0 : 1);
}

pid_t spawn_ad(const std::string& base, uint32_t id, uint32_t codes, uint32_t rounds)
{
	const pid_t pid = ::fork();
	if (pid == 0)
	{
		const std::string out = base + "adhelper." + std::to_string(id) + ".out";
		const int fd = ::open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) { ::dup2(fd, 1); ::dup2(fd, 2); ::close(fd); }
		const std::string sid = std::to_string(id), sc = std::to_string(codes), sr = std::to_string(rounds), go = base + "go";
		std::vector<char*> args = { (char*)g_self.c_str(), (char*)"--ad-helper", (char*)base.c_str(), (char*)sid.c_str(),
			(char*)sc.c_str(), (char*)sr.c_str(), (char*)go.c_str(), nullptr };
		::execv(g_self.c_str(), args.data());
		::_exit(127);
	}
	return pid;
}

int wait_pid(pid_t pid, int timeoutSec)
{
	for (int i = 0; i < timeoutSec * 100; ++i)
	{
		int status = 0;
		if (::waitpid(pid, &status, WNOHANG) == pid)
			return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	::kill(pid, SIGKILL);
	int status = 0;
	::waitpid(pid, &status, 0);
	return -3;
}

TEST_F(MwAd, TwoProcessesShareTickCacheWithoutIndexCollision)
{
	const uint32_t codes = 220, rounds = 3;	// 440 entries > 400 initial capacity -> one expansion
	const std::string b = base();
	const pid_t p1 = spawn_ad(b, 1, codes, rounds);
	const pid_t p2 = spawn_ad(b, 2, codes, rounds);
	for (int i = 0; i < 20000 && !(std::filesystem::exists(b + "ready.1") && std::filesystem::exists(b + "ready.2")); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	std::ofstream(b + "go") << "1";
	EXPECT_EQ(wait_pid(p1, 120), 0) << read_file_text(b + "adhelper.1.out");
	EXPECT_EQ(wait_pid(p2, 120), 0) << read_file_text(b + "adhelper.2.out");

	const std::string cache = b + "cache_tick.dmb";
	R4BlkHead head = read_head(cache);
	EXPECT_EQ(head._size, 2 * codes) << "each code exactly one cache entry";
	std::map<std::string, uint32_t> seen;
	for (uint32_t i = 0; i < head._size; ++i)
	{
		TickCacheItem item;
		ASSERT_TRUE(read_tick_item(cache, i, item));
		WTSTickStruct t;
		memcpy(&t, (const char*)&item + sizeof(uint32_t), sizeof(t));
		seen[t.code]++;
		EXPECT_DOUBLE_EQ(t.total_volume, 100.0 + rounds - 1) << t.code;
	}
	for (uint32_t id = 1; id <= 2; ++id)
		for (uint32_t k = 0; k < codes; ++k)
			EXPECT_EQ(seen["a" + std::to_string(id) + "c" + std::to_string(k)], 1u);

	for (const char* bar : { "cache_m1.dmb", "cache_m5.dmb", "cache_d1.dmb" })
	{
		const std::string path = b + bar;
		R4BlkHead bh = read_head(path);
		EXPECT_EQ(bh._size, 2 * codes) << bar;
		std::map<std::string, uint32_t> codesSeen;
		std::ifstream fin(path, std::ios::binary);
		fin.seekg((std::streamoff)sizeof(RTBarCache));
		for (uint32_t i = 0; i < bh._size && fin; ++i)
		{
			BarCacheItem item;
			fin.read((char*)&item, sizeof(item));
			codesSeen[item._code]++;
			EXPECT_DOUBLE_EQ(item._bar.close, 4000.0 + rounds - 1) << bar << " " << item._code;
		}
		uint32_t bad = 0;
		for (uint32_t id = 1; id <= 2; ++id)
			for (uint32_t k = 0; k < codes; ++k)
				bad += codesSeen["a" + std::to_string(id) + "c" + std::to_string(k)] != 1 ? 1 : 0;
		EXPECT_EQ(bad, 0u) << bar << " codes missing or duplicated";
	}
}

}	// namespace

int main(int argc, char** argv)
{
	char buf[4096];
	if (::realpath(argv[0], buf) != nullptr)
		g_self = buf;
	if (argc > 1 && std::strcmp(argv[1], "--ad-helper") == 0)
		return run_ad_helper(argc, argv);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
