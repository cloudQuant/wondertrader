// Iteration 03 / decision card 13 (13a=A, 13b=B): WtDataStorage RT multi-writer publish protocol.
//
// Every writer runs in its own exec'd process (fork + execv of this binary in
// --mw-helper mode), so each writer has its own mappings, threads and address
// space. Crash injection uses the writer test hooks to stop a helper at a fixed
// point with the cross-process publish lock held; the parent then sends SIGKILL.
// All files live in a fresh per-test temporary directory; no market or broker access.
#include "r23_writer_fixture.h"
#include "p10_writer_test_hooks.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <set>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <chrono>

namespace {

std::string g_self;

// Local oracle of the sidecar journal layout (kept independent from the product header so
// the same test source also builds against the pre-change baseline).
#pragma pack(push, 1)
struct JournalView
{
	char		magic[8];
	uint32_t	version;
	uint32_t	state;
	uint64_t	generation;
	int64_t		owner_pid;
	uint64_t	target;
};
#pragma pack(pop)

bool read_journal(const std::string& dataPath, JournalView& j)
{
	std::memset(&j, 0, sizeof(j));
	std::ifstream ifs(dataPath + ".wlk", std::ios::binary);
	if (!ifs)
		return false;
	ifs.read((char*)&j, sizeof(j));
	return ifs.gcount() == (std::streamsize)sizeof(j) && std::memcmp(j.magic, "WTRTPUB", 8) == 0;
}

uint64_t file_len(const std::string& p)
{
	struct stat st{};
	return ::stat(p.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

// ------------------------------------------------------------------ helper process
struct CrashState
{
	std::string	kind;		// none | publish | extend | cachepub
	uint32_t	slot = 0;
	int			fd = -1;
};

void stop_here(CrashState* st)
{
	char token = 'P';
	ssize_t n;
	do { n = ::write(st->fd, &token, 1); } while (n < 0 && errno == EINTR);
	for (;;)
		::pause();	// parent sends SIGKILL while the publish lock is held
}

void hook_before_publish(void* ctx, const char* path, uint32_t slot) noexcept
{
	auto* st = static_cast<CrashState*>(ctx);
	if (st == nullptr || path == nullptr)
		return;
	if (st->kind == "publish" && std::strstr(path, "/rt/trans/") != nullptr && slot == st->slot)
		stop_here(st);
	if (st->kind == "cachepub" && std::strstr(path, "cache.dmb") != nullptr && slot == st->slot)
		stop_here(st);
}

void hook_after_extend(void* ctx, const char* path) noexcept
{
	auto* st = static_cast<CrashState*>(ctx);
	if (st != nullptr && path != nullptr && st->kind == "extend" && std::strstr(path, "/rt/trans/") != nullptr)
		stop_here(st);
}

WTSVariant* writer_cfg(const std::string& base)
{
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", base.c_str());
	cfg->append("cache", "cache.dmb");
	cfg->append("async", false);
	cfg->append("savelog", false);
	cfg->append("groupsize", (uint32_t)100000000);
	cfg->append("skip_notrade_tick", false);
	cfg->append("skip_notrade_bar", false);
	cfg->append("disablehis", false);
	cfg->append("disabletick", false);
	cfg->append("disablemin1", false);
	cfg->append("disablemin5", false);
	cfg->append("disableday", false);
	cfg->append("disabletrans", false);
	cfg->append("disableordque", false);
	cfg->append("disableorddtl", false);
	cfg->append("minbar_price_mode", (uint32_t)0);
	return cfg;
}

WTSTickData* make_tick(r23::WriterBDMgr& bd, const std::string& code, uint32_t round)
{
	WTSTickStruct ts;
	std::memset(&ts, 0, sizeof(ts));
	snprintf(ts.exchg, sizeof(ts.exchg), "SHFE");
	snprintf(ts.code, sizeof(ts.code), "%s", code.c_str());
	ts.price = 3600.0 + round;
	ts.open = ts.high = ts.low = ts.price;
	ts.total_volume = 1000.0 + round;
	ts.volume = 1.0;
	ts.total_turnover = 3600000.0 + round;
	ts.turn_over = 3600.0;
	ts.open_interest = 90000.0;
	ts.trading_date = 20240108;
	ts.action_date = 20240108;
	ts.action_time = 93000000 + round * 1000;
	WTSTickData* t = WTSTickData::create(ts);
	t->setContractInfo(bd.getContract(code.c_str(), "SHFE"));
	return t;
}

std::string cache_code(uint32_t id, uint32_t k)
{
	return "m" + std::to_string(id) + "c" + std::to_string(k);
}

// argv: self --mw-helper kind base id count crash_kind crash_slot notify_fd codes
int run_helper(int argc, char** argv)
{
	if (argc < 11)
		return 90;
	const std::string kind = argv[2];
	const std::string base = argv[3];
	const uint32_t id = (uint32_t)std::strtoul(argv[4], nullptr, 10);
	const uint32_t count = (uint32_t)std::strtoul(argv[5], nullptr, 10);
	CrashState crash;
	crash.kind = argv[6];
	crash.slot = (uint32_t)std::strtoul(argv[7], nullptr, 10);
	crash.fd = std::atoi(argv[8]);
	const uint32_t codes = (uint32_t)std::strtoul(argv[9], nullptr, 10);
	const std::string goFile = argv[10];

	r23::WriterBDMgr bd;
	r23::WriterSink sink(&bd);
	sink.add_comm("SHFE.rb");
	WTSCommodityInfo* comm = bd.getCommodity("");
	for (uint32_t i = 1; i <= 2; ++i)
		for (uint32_t k = 0; k < codes; ++k)
			bd.add_contract(cache_code(i, k).c_str(), comm);
	bd.add_contract("rbshared", comm);

	WtDataWriter* writer = new WtDataWriter();
	WTSVariant* cfg = writer_cfg(base);
	if (!writer->init(cfg, &sink))
		return 91;
	cfg->release();

#if defined(WT_MW_GREEN)
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &crash;
	hooks.skip_initial_check_wait = true;
	hooks.before_rt_publish = &hook_before_publish;
	hooks.after_rt_extend_before_capacity = &hook_after_extend;
	if (crash.kind != "none" && !p10_writer_test_hooks::install(writer, hooks))
		return 92;
#endif

	// readiness + barrier so both writers start appending together
	{
		std::ofstream(base + "ready." + std::to_string(id)) << "1";
		for (int i = 0; i < 20000 && !r23::file_exists(goFile); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	uint32_t rejected = 0;
	if (kind == "trans")
	{
		WTSContractInfo* ct = bd.getContract("rb2210", "SHFE");
		for (uint32_t seq = 0; seq < count; ++seq)
		{
			WTSTransStruct ts;
			snprintf(ts.exchg, sizeof(ts.exchg), "SHFE");
			snprintf(ts.code, sizeof(ts.code), "rb2210");
			ts.trading_date = ts.action_date = 20240108;
			ts.action_time = 93000000 + seq;
			ts.index = (uint64_t)id * 1000000ull + seq;
			ts.price = (double)id;
			ts.volume = seq + 1;
			WTSTransData* t = WTSTransData::create(ts);
			t->setContractInfo(ct);
			if (!writer->writeTransaction(t))
				++rejected;
			t->release();
		}
	}
	else if (kind == "ticks")
	{
		for (uint32_t round = 0; round < count; ++round)
		{
			for (uint32_t k = 0; k < codes; ++k)
			{
				WTSTickData* t = make_tick(bd, cache_code(id, k), round);
				if (!writer->writeTick(t, 0))
					++rejected;
				t->release();
			}
			WTSTickData* s = make_tick(bd, "rbshared", round);
			if (!writer->writeTick(s, 0))
				++rejected;
			s->release();
		}
	}
	else
		return 93;

	std::printf("REJECTED=%u\nRECOVERED=%d\nERRORS=%u\nLAST=%s\n", rejected,
		sink.has_log("recovered", LL_WARN) || sink.has_log("revalidated", LL_WARN) ? 1 : 0,
		sink.count_of(LL_ERROR), sink.last_log().c_str());
	std::fflush(stdout);
	// Writer threads are not joinable by contract (see r23 fixture); leave via _exit.
	::_exit(0);
}

// ------------------------------------------------------------------ parent side
struct Helper
{
	pid_t pid = -1;
	int notify = -1;	// read end of the crash pipe
	std::string out;
};

Helper spawn(const std::string& kind, const std::string& base, uint32_t id, uint32_t count,
	const std::string& crashKind = "none", uint32_t crashSlot = 0, uint32_t codes = 0)
{
	Helper h;
	int fds[2];
	if (::pipe(fds) != 0)
		return h;
	h.notify = fds[0];
	h.out = base + "helper." + std::to_string(id) + "." + crashKind + ".out";
	const std::string goFile = base + "go";
	const pid_t pid = ::fork();
	if (pid == 0)
	{
		::close(fds[0]);
		const int out = ::open(h.out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out >= 0) { ::dup2(out, 1); ::dup2(out, 2); ::close(out); }
		const std::string sid = std::to_string(id), scount = std::to_string(count), sslot = std::to_string(crashSlot),
			sfd = std::to_string(fds[1]), scodes = std::to_string(codes);
		std::vector<char*> args = { (char*)g_self.c_str(), (char*)"--mw-helper", (char*)kind.c_str(), (char*)base.c_str(),
			(char*)sid.c_str(), (char*)scount.c_str(), (char*)crashKind.c_str(), (char*)sslot.c_str(),
			(char*)sfd.c_str(), (char*)scodes.c_str(), (char*)goFile.c_str(), nullptr };
		::execv(g_self.c_str(), args.data());
		::_exit(127);
	}
	::close(fds[1]);
	h.pid = pid;
	return h;
}

int wait_exit(pid_t pid, int timeoutSec, bool& signaled, int& sig)
{
	signaled = false;
	sig = 0;
	for (int i = 0; i < timeoutSec * 100; ++i)
	{
		int status = 0;
		const pid_t r = ::waitpid(pid, &status, WNOHANG);
		if (r == pid)
		{
			if (WIFSIGNALED(status)) { signaled = true; sig = WTERMSIG(status); return -1; }
			return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	::kill(pid, SIGKILL);
	int status = 0;
	::waitpid(pid, &status, 0);
	return -3;	// timeout
}

bool wait_ready(const std::string& base, std::initializer_list<uint32_t> ids, int timeoutMs = 20000)
{
	for (int i = 0; i < timeoutMs; ++i)
	{
		bool all = true;
		for (uint32_t id : ids)
			all = all && r23::file_exists(base + "ready." + std::to_string(id));
		if (all)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

void go(const std::string& base) { std::ofstream(base + "go") << "1"; }

bool wait_token(int fd, int timeoutMs)
{
	pollfd p{ fd, POLLIN, 0 };
	if (::poll(&p, 1, timeoutMs) != 1)
		return false;
	char c = 0;
	return ::read(fd, &c, 1) == 1 && c == 'P';
}

class MwStorage : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_dir = std::filesystem::temp_directory_path() / ("i03_mw_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_dir);
		std::filesystem::create_directories(_dir);
		_base = _dir.string() + "/";
	}
	void TearDown() override
	{
		if (std::getenv("MW_KEEP") == nullptr)
		{
			std::error_code ec;
			std::filesystem::remove_all(_dir, ec);
		}
	}
	std::string trans_file() const { return _base + "rt/trans/SHFE/rb2210.dmb"; }

	std::vector<WTSTransStruct> read_trans(RTDayBlockHeader& head) const
	{
		head = r23::read_block_header<RTDayBlockHeader>(trans_file());
		std::vector<WTSTransStruct> out(head._size);
		std::ifstream ifs(trans_file(), std::ios::binary);
		ifs.seekg(sizeof(RTDayBlockHeader));
		if (head._size > 0)
			ifs.read((char*)out.data(), (std::streamsize)(sizeof(WTSTransStruct) * head._size));
		if (!ifs)
			out.clear();
		return out;
	}

	std::filesystem::path _dir;
	std::string _base;
};

// ------------------------------------------------------------------ tests
TEST_F(MwStorage, TwoProcessTransAppendNoLossNoTear)
{
	const uint32_t N = 6000;	// forces 2500 -> 5000 -> 10000 -> 20000 expansions
	Helper a = spawn("trans", _base, 1, N);
	Helper b = spawn("trans", _base, 2, N);
	ASSERT_TRUE(wait_ready(_base, { 1, 2 }));
	go(_base);
	bool sa, sb; int siga, sigb;
	const int ra = wait_exit(a.pid, 120, sa, siga);
	const int rb = wait_exit(b.pid, 120, sb, sigb);
	EXPECT_EQ(ra, 0) << "writer 1 signaled=" << sa << " sig=" << siga << " " << r23::read_file_bytes(a.out);
	EXPECT_EQ(rb, 0) << "writer 2 signaled=" << sb << " sig=" << sigb << " " << r23::read_file_bytes(b.out);

	RTDayBlockHeader head{};
	std::vector<WTSTransStruct> recs = read_trans(head);
	EXPECT_EQ(head._size, 2 * N) << "published record count";
	EXPECT_LE(head._size, head._capacity);
	EXPECT_GE(file_len(trans_file()), sizeof(RTDayBlockHeader) + (uint64_t)head._capacity * sizeof(WTSTransStruct))
		<< "capacity published beyond the physical file";
	std::set<uint64_t> seen;
	uint32_t torn = 0, dup = 0;
	std::map<uint32_t, uint32_t> lastSeqOf;
	uint32_t reordered = 0;
	for (const auto& r : recs)
	{
		const uint32_t id = (uint32_t)(r.index / 1000000ull), seq = (uint32_t)(r.index % 1000000ull);
		if ((id != 1 && id != 2) || r.price != (double)id || r.volume != seq + 1 || std::strcmp(r.code, "rb2210") != 0)
		{
			++torn;
			continue;
		}
		if (!seen.insert(r.index).second)
			++dup;
		auto it = lastSeqOf.find(id);
		if (it != lastSeqOf.end() && seq != it->second + 1)
			++reordered;
		lastSeqOf[id] = seq;
	}
	EXPECT_EQ(torn, 0u) << "records with mixed or empty payload";
	EXPECT_EQ(dup, 0u);
	EXPECT_EQ(reordered, 0u) << "each writer's own records must keep their order";
	EXPECT_EQ(seen.size(), (size_t)(2 * N)) << "lost records";
#if defined(WT_MW_GREEN)
	JournalView j{};
	ASSERT_TRUE(read_journal(trans_file(), j));
	EXPECT_EQ(j.state, 0u) << "journal must be IDLE after both writers finished";
	EXPECT_GE(j.generation, 4u) << "create + three expansions are structural changes";
#endif
}

TEST_F(MwStorage, TwoProcessTickCacheIndexAndSharedStream)
{
	const uint32_t rounds = 60, codes = 24;	// 49 cache entries > initial capacity 8 -> cache resize
	Helper a = spawn("ticks", _base, 1, rounds, "none", 0, codes);
	Helper b = spawn("ticks", _base, 2, rounds, "none", 0, codes);
	ASSERT_TRUE(wait_ready(_base, { 1, 2 }));
	go(_base);
	bool s; int sig;
	const int ra = wait_exit(a.pid, 120, s, sig);
	EXPECT_EQ(ra, 0) << r23::read_file_bytes(a.out);
	const int rb = wait_exit(b.pid, 120, s, sig);
	EXPECT_EQ(rb, 0) << r23::read_file_bytes(b.out);

	const std::string cache = _base + "cache.dmb";
	RTBlockHeader ch = r23::read_block_header<RTBlockHeader>(cache);
	EXPECT_EQ(ch._size, 2 * codes + 1) << "every code exactly one cache entry";
	std::map<std::string, uint32_t> entries;
	std::ifstream ifs(cache, std::ios::binary);
	ifs.seekg(sizeof(RTTickCache));
	for (uint32_t i = 0; i < ch._size && ifs; ++i)
	{
		TickCacheItem item;
		ifs.read((char*)&item, sizeof(item));
		entries[item._tick.code]++;
		if (std::string(item._tick.code).rfind("m", 0) == 0)
			EXPECT_DOUBLE_EQ(item._tick.total_volume, 1000.0 + rounds - 1) << item._tick.code;
	}
	for (uint32_t id = 1; id <= 2; ++id)
	{
		for (uint32_t k = 0; k < codes; ++k)
		{
			const std::string code = cache_code(id, k);
			EXPECT_EQ(entries[code], 1u) << code;
			const auto th = r23::read_block_header<RTDayBlockHeader>(_base + "rt/ticks/SHFE/" + code + ".dmb");
			EXPECT_EQ(th._size, rounds) << code;
		}
	}
	EXPECT_EQ(entries["rbshared"], 1u);
	const auto sh = r23::read_block_header<RTDayBlockHeader>(_base + "rt/ticks/SHFE/rbshared.dmb");
	// Both writers feed the same cumulative volumes. The shared cache filter (now updated
	// under the publish lock) rejects a tick whose volume is below the other writer's last
	// one, so the stream holds between `rounds` and `2*rounds` complete records.
	EXPECT_GE(sh._size, rounds);
	EXPECT_LE(sh._size, 2 * rounds);
	std::ifstream sf(_base + "rt/ticks/SHFE/rbshared.dmb", std::ios::binary);
	sf.seekg(sizeof(RTDayBlockHeader));
	for (uint32_t i = 0; i < sh._size && sf; ++i)
	{
		WTSTickStruct t;
		sf.read((char*)&t, sizeof(t));
		EXPECT_STREQ(t.code, "rbshared") << i;
		EXPECT_GE(t.total_volume, 1000.0) << i;
		EXPECT_LE(t.total_volume, 1000.0 + rounds - 1) << i;
	}
}

#if defined(WT_MW_GREEN)
TEST_F(MwStorage, KillBetweenPrepareAndPublishLeavesSlotInvisible)
{
	Helper a = spawn("trans", _base, 1, 100, "publish", 50);
	ASSERT_TRUE(wait_ready(_base, { 1 }));
	go(_base);
	ASSERT_TRUE(wait_token(a.notify, 30000)) << r23::read_file_bytes(a.out);

	// The helper is parked inside the publish step: payload written, size not yet published.
	RTDayBlockHeader head{};
	read_trans(head);
	EXPECT_EQ(head._size, 50u);
	WTSTransStruct prepared;
	{
		std::ifstream ifs(trans_file(), std::ios::binary);
		ifs.seekg(sizeof(RTDayBlockHeader) + 50 * sizeof(WTSTransStruct));
		ifs.read((char*)&prepared, sizeof(prepared));
	}
	EXPECT_EQ(prepared.index, 1000050u) << "prepare must have written the unpublished slot";

	ASSERT_EQ(::kill(a.pid, SIGKILL), 0);
	bool s; int sig;
	EXPECT_EQ(wait_exit(a.pid, 10, s, sig), -1);
	EXPECT_TRUE(s && sig == SIGKILL);

	std::vector<WTSTransStruct> recs = read_trans(head);
	EXPECT_EQ(head._size, 50u) << "killed prepare must stay invisible";
	JournalView j{};
	ASSERT_TRUE(read_journal(trans_file(), j));
	EXPECT_EQ(j.state, 0u) << "a record append is not a structural change";

	Helper b = spawn("trans", _base, 2, 10);
	ASSERT_TRUE(wait_ready(_base, { 2 }));
	EXPECT_EQ(wait_exit(b.pid, 60, s, sig), 0) << r23::read_file_bytes(b.out);
	recs = read_trans(head);
	ASSERT_EQ(head._size, 60u);
	ASSERT_EQ(recs.size(), 60u);
	for (uint32_t i = 0; i < 50; ++i)
		EXPECT_EQ(recs[i].index, 1000000u + i);
	EXPECT_EQ(recs[50].index, 2000000u) << "next writer reuses the unpublished slot";
	for (uint32_t i = 50; i < 60; ++i)
		EXPECT_DOUBLE_EQ(recs[i].price, 2.0);
}

TEST_F(MwStorage, KillInsideExpansionIsRecoveredByNextWriter)
{
	Helper a = spawn("trans", _base, 1, 3000, "extend", 0);
	ASSERT_TRUE(wait_ready(_base, { 1 }));
	go(_base);
	ASSERT_TRUE(wait_token(a.notify, 60000)) << r23::read_file_bytes(a.out);

	RTDayBlockHeader head{};
	read_trans(head);
	EXPECT_EQ(head._capacity, 2500u) << "capacity must not be published before the extension completes";
	EXPECT_EQ(head._size, 2500u);
	EXPECT_EQ(file_len(trans_file()), sizeof(RTDayBlockHeader) + 5000ull * sizeof(WTSTransStruct))
		<< "file already extended (uncommitted tail)";
	JournalView j{};
	ASSERT_TRUE(read_journal(trans_file(), j));
	EXPECT_EQ(j.state, 1u) << "journal shows the structural change in progress";
	EXPECT_EQ(j.target, 5000u);
	EXPECT_EQ(j.owner_pid, (int64_t)a.pid);
	const uint64_t genBefore = j.generation;

	ASSERT_EQ(::kill(a.pid, SIGKILL), 0);
	bool s; int sig;
	EXPECT_EQ(wait_exit(a.pid, 10, s, sig), -1);

	Helper b = spawn("trans", _base, 2, 10);
	ASSERT_TRUE(wait_ready(_base, { 2 }));
	go(_base);
	EXPECT_EQ(wait_exit(b.pid, 60, s, sig), 0);
	const std::string out = r23::read_file_bytes(b.out);
	EXPECT_NE(out.find("RECOVERED=1"), std::string::npos) << out;
	EXPECT_NE(out.find("REJECTED=0"), std::string::npos) << out;

	std::vector<WTSTransStruct> recs = read_trans(head);
	ASSERT_EQ(head._size, 2510u);
	EXPECT_EQ(head._capacity, 5000u) << "the uncommitted tail is reused by the next expansion";
	ASSERT_EQ(recs.size(), 2510u);
	for (uint32_t i = 0; i < 2500; ++i)
		ASSERT_EQ(recs[i].index, 1000000u + i) << i;
	for (uint32_t i = 2500; i < 2510; ++i)
		EXPECT_EQ(recs[i].index, 2000000u + (i - 2500));
	ASSERT_TRUE(read_journal(trans_file(), j));
	EXPECT_EQ(j.state, 0u);
	EXPECT_GT(j.generation, genBefore);
}

TEST_F(MwStorage, KillBeforeCacheEntryPublishDoesNotLeakIndex)
{
	// Writer 1 is stopped while publishing its second new cache entry (slot 1).
	Helper a = spawn("ticks", _base, 1, 1, "cachepub", 1, 3);
	ASSERT_TRUE(wait_ready(_base, { 1 }));
	go(_base);
	ASSERT_TRUE(wait_token(a.notify, 30000)) << r23::read_file_bytes(a.out);
	ASSERT_EQ(::kill(a.pid, SIGKILL), 0);
	bool s; int sig;
	EXPECT_EQ(wait_exit(a.pid, 10, s, sig), -1);
	RTBlockHeader ch = r23::read_block_header<RTBlockHeader>(_base + "cache.dmb");
	EXPECT_EQ(ch._size, 1u) << "the staged second entry must not be published";

	Helper b = spawn("ticks", _base, 2, 2, "none", 0, 3);
	ASSERT_TRUE(wait_ready(_base, { 2 }));
	EXPECT_EQ(wait_exit(b.pid, 60, s, sig), 0);
	ch = r23::read_block_header<RTBlockHeader>(_base + "cache.dmb");
	EXPECT_EQ(ch._size, 1u + 3u + 1u) << "m1c0 + writer 2 codes + rbshared";
	std::set<std::string> codes;
	std::ifstream ifs(_base + "cache.dmb", std::ios::binary);
	ifs.seekg(sizeof(RTTickCache));
	for (uint32_t i = 0; i < ch._size; ++i)
	{
		TickCacheItem item;
		ifs.read((char*)&item, sizeof(item));
		EXPECT_TRUE(codes.insert(item._tick.code).second) << "duplicate cache entry " << item._tick.code;
	}
	EXPECT_EQ(codes.count("m1c0"), 1u);
	EXPECT_EQ(codes.count("m1c1"), 0u);
}
#endif

// A separate reader process (no writer code, own read-only MAP_SHARED view) polls the
// trans block while two writer processes append. It only uses the reader-side rule:
// visible = min(acquire(size), acquire(capacity), records covered by its own mapping),
// remapping when the published capacity exceeds its mapping. Every visible record must be
// a complete record from one writer.
TEST_F(MwStorage, ConcurrentReaderSeesOnlyCompletePublishedRecords)
{
	const uint32_t N = 6000;
	const pid_t reader = ::fork();
	ASSERT_GE(reader, 0);
	if (reader == 0)
	{
		::alarm(150);
		const std::string path = trans_file();
		int fd = -1;
		for (int i = 0; i < 60000 && fd < 0; ++i)
		{
			fd = ::open(path.c_str(), O_RDONLY);
			if (fd < 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (fd < 0) ::_exit(10);
		size_t len = 0;
		const char* map = nullptr;
		auto remap = [&]() -> bool {
			if (map) ::munmap((void*)map, len);
			struct stat st{};
			if (::fstat(fd, &st) != 0 || st.st_size <= 0) return false;
			len = (size_t)st.st_size;
			void* m = ::mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
			if (m == MAP_FAILED) { map = nullptr; return false; }
			map = (const char*)m;
			return true;
		};
		if (!remap()) ::_exit(11);
		uint32_t checked = 0, bad = 0, remaps = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(140);
		while (checked < 2 * N && std::chrono::steady_clock::now() < deadline)
		{
			const uint32_t size = __atomic_load_n((const uint32_t*)(map + 12), __ATOMIC_ACQUIRE);
			const uint32_t cap = __atomic_load_n((const uint32_t*)(map + 16), __ATOMIC_ACQUIRE);
			const uint64_t mapped = len < sizeof(RTDayBlockHeader) ? 0 : (len - sizeof(RTDayBlockHeader)) / sizeof(WTSTransStruct);
			if (cap > mapped) { if (!remap()) ::_exit(12); ++remaps; continue; }
			uint32_t visible = size < cap ? size : cap;
			if (visible > mapped) visible = (uint32_t)mapped;
			for (; checked < visible; ++checked)
			{
				WTSTransStruct r;
				std::memcpy(&r, map + sizeof(RTDayBlockHeader) + (size_t)checked * sizeof(r), sizeof(r));
				const uint32_t id = (uint32_t)(r.index / 1000000ull), seq = (uint32_t)(r.index % 1000000ull);
				if ((id != 1 && id != 2) || r.price != (double)id || r.volume != seq + 1)
					++bad;
			}
		}
		std::printf("checked=%u bad=%u remaps=%u\n", checked, bad, remaps);
		std::fflush(stdout);
		::_exit(bad != 0 ? 2 : (checked == 2 * N ? 0 : 3));
	}
	Helper a = spawn("trans", _base, 1, N);
	Helper b = spawn("trans", _base, 2, N);
	ASSERT_TRUE(wait_ready(_base, { 1, 2 }));
	go(_base);
	bool s; int sig;
	EXPECT_EQ(wait_exit(a.pid, 120, s, sig), 0);
	EXPECT_EQ(wait_exit(b.pid, 120, s, sig), 0);
	const int rr = wait_exit(reader, 150, s, sig);
	EXPECT_EQ(rr, 0) << "reader exit (2=torn/foreign record seen, 3=published count never reached, -1=signal " << sig << ")";
}

}	// namespace

int main(int argc, char** argv)
{
	char buf[4096];
	if (::realpath(argv[0], buf) != nullptr)
		g_self = buf;
	if (argc > 1 && std::strcmp(argv[1], "--mw-helper") == 0)
		return run_helper(argc, argv);
	::testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
