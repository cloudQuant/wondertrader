/*
 * BUG-R100-004（迭代03 最终缺口，slice2；决策 K36=A + 卡13 发布协议）
 *
 * 跨进程发布证明：fork 出的子进程运行真实 WtDataWriter（WT_DATA_WRITER_TEST_HOOKS），在
 * “槽位载荷已写入、_size 尚未发布”的 before_rt_publish 钩子处以管道确定性停住；本进程用
 * 真实 WtRdmDtReader 读取同一 RT 映射，断言：
 *   - 停住时未发布的末槽不可见，已发布记录全部可见且逐条序号/校验和正确；
 *   - 与未发布槽同一时间戳的已发布记录仍可见（重复时间戳）；
 *   - writer 扩容（2500 → 5000）后，读端重新映射并看到超出旧映射的已发布记录；
 *   - 放行后同一 reader 读到完整的末槽；全部写完后 0..N-1 不丢不重。
 * 覆盖逐笔成交（range，含两次停点）、委托队列（range）与 tick（count 与 range）。
 * 只用本地临时目录，不接行情源。
 */
#include "q03_storage3_fixture.h"
#include "../r23_storage4/r23_writer_fixture.h"

#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "WtDataStorage/WtRdmDtReader.h"
#include "../p10_storage_writer/p10_writer_test_hooks.h"

namespace i03s2r100 {

using std::string;

constexpr uint32_t kTDate = 20240110;	// q03::BaseDataMgr 的当前交易日
constexpr uint32_t kTrans = 2600;		// > HFT_SIZE_STEP(2500)：触发一次扩容
constexpr uint32_t kQueues = 10;
constexpr uint32_t kTicks = 10;

struct Park
{
	const char* suffix;
	uint32_t slot;
	char id;
};

const Park kParks[] = {
	{ "rt/trans/SHFE/rb2210.dmb", 101, 'a' },	// 槽 100/101 同一时间戳
	{ "rt/trans/SHFE/rb2210.dmb", 2550, 'b' },	// 扩容后
	{ "rt/queue/SHFE/rb2210.dmb", 5, 'c' },
	{ "rt/ticks/SHFE/rb2210.dmb", 3, 'd' },
};

struct ParkCtx
{
	int notifyFd;
	int goFd;
};

void park_hook(void* context, const char* path, uint32_t slot) noexcept
{
	ParkCtx* ctx = static_cast<ParkCtx*>(context);
	if (path == nullptr)
		return;
	const string p(path);
	for (const Park& park : kParks)
	{
		const string suffix(park.suffix);
		if (slot == park.slot && p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0)
		{
			char id = park.id;
			(void)!::write(ctx->notifyFd, &id, 1);
			char go = 0;
			(void)!::read(ctx->goFd, &go, 1);
			return;
		}
	}
}

// 第 k 个 100ms 步长的 09:30:00.000 之后时间（HHMMSSmmm）
uint32_t hms_ms(uint32_t k)
{
	const uint64_t total = (9ull * 3600 + 30 * 60) * 1000 + (uint64_t)k * 100;
	const uint32_t hh = (uint32_t)(total / 3600000), mm = (uint32_t)(total / 60000 % 60);
	const uint32_t ss = (uint32_t)(total / 1000 % 60), ms = (uint32_t)(total % 1000);
	return hh * 10000000 + mm * 100000 + ss * 1000 + ms;
}

uint32_t fnv1a(const void* data, size_t n)
{
	const unsigned char* p = static_cast<const unsigned char*>(data);
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

uint32_t trans_checksum(const WTSTransStruct& t)
{
	WTSTransStruct c = t;
	c.askorder = 0;
	return fnv1a(&c, sizeof(c));
}

uint32_t queue_checksum(const WTSOrdQueStruct& q)
{
	WTSOrdQueStruct c = q;
	c.volumes[49] = 0;
	return fnv1a(&c, sizeof(c));
}

WTSTransStruct make_trans(uint32_t seq)
{
	WTSTransStruct t;
	strcpy(t.exchg, "SHFE");
	strcpy(t.code, "rb2210");
	t.trading_date = kTDate;
	t.action_date = kTDate;
	t.action_time = hms_ms(seq / 2);	// 两两同一时间戳
	t.index = seq;
	t.ttype = TT_Match;
	t.side = BDT_Buy;
	t.price = 3000.0 + seq;
	t.volume = seq % 50 + 1;
	t.bidorder = 500000 + seq;
	t.askorder = trans_checksum(t);
	return t;
}

WTSOrdQueStruct make_queue(uint32_t seq)
{
	WTSOrdQueStruct q;
	strcpy(q.exchg, "SHFE");
	strcpy(q.code, "rb2210");
	q.trading_date = kTDate;
	q.action_date = kTDate;
	q.action_time = hms_ms(seq);
	q.side = BDT_Sell;
	q.price = 3100.0 + seq;
	q.order_items = seq;
	q.qsize = 5;
	for (int i = 0; i < 49; i++)
		q.volumes[i] = seq * 100 + i;
	q.volumes[49] = queue_checksum(q);
	return q;
}

int writer_child(const string& base, int notifyFd, int goFd)
{
	r23::WriterBDMgr bd;
	r23::WriterSink sink(&bd);
	sink.add_comm("SHFE.rb");
	sink.set_tdate(kTDate);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", base.c_str());
	cfg->append("cache", "cache.dmb");
	cfg->append("async", false);
	cfg->append("savelog", false);
	cfg->append("groupsize", (uint32_t)100000);
	cfg->append("disablehis", true);
	WtDataWriter* writer = new WtDataWriter();	// init 后只 release 不 delete（同 r23 纪律）
	const bool ok = writer->init(cfg, &sink);
	cfg->release();
	if (!ok)
		return 50;
	ParkCtx ctx{ notifyFd, goFd };
	p10_writer_test_hooks::Hooks hooks;
	hooks.context = &ctx;
	hooks.before_rt_publish = &park_hook;
	if (!p10_writer_test_hooks::install(writer, hooks))
		return 51;

	WTSContractInfo* ct = bd.getContract("rb2210", "SHFE");
	for (uint32_t seq = 0; seq < kTrans; seq++)
	{
		WTSTransStruct ts = make_trans(seq);
		WTSTransData* d = WTSTransData::create(ts);
		d->setContractInfo(ct);
		const bool w = writer->writeTransaction(d);
		d->release();
		if (!w)
			return 52;
	}
	for (uint32_t seq = 0; seq < kQueues; seq++)
	{
		WTSOrdQueStruct qs = make_queue(seq);
		WTSOrdQueData* d = WTSOrdQueData::create(qs);
		d->setContractInfo(ct);
		const bool w = writer->writeOrderQueue(d);
		d->release();
		if (!w)
			return 53;
	}
	for (uint32_t seq = 0; seq < kTicks; seq++)
	{
		WTSTickStruct ts;
		memset(&ts, 0, sizeof(ts));
		strcpy(ts.exchg, "SHFE");
		strcpy(ts.code, "rb2210");
		ts.trading_date = kTDate;
		ts.action_date = kTDate;
		ts.action_time = hms_ms(seq * 10);
		ts.price = ts.open = ts.high = ts.low = 3600.0 + seq;
		ts.total_volume = 1000.0 + seq * 10;
		ts.volume = 10;
		ts.total_turnover = ts.total_volume * 3600.0;
		ts.open_interest = 90000;
		ts.upper_limit = 3800; ts.lower_limit = 3400;
		WTSTickData* d = WTSTickData::create(ts);
		d->setContractInfo(ct);
		const bool w = writer->writeTick(d, 0);
		d->release();
		if (!w)
			return 54;
	}
	p10_writer_test_hooks::uninstall(writer);
	writer->release();
	return 0;
}

bool wait_token(int fd, char& id, int timeoutMs = 60000)
{
	pollfd pfd{ fd, POLLIN, 0 };
	if (::poll(&pfd, 1, timeoutMs) <= 0)
		return false;
	return ::read(fd, &id, 1) == 1;
}

uint64_t at(uint32_t k) { return (uint64_t)kTDate * 1000000000ull + hms_ms(k); }

class I03S2R100RdmCrossProcess : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("s2r100"));
		_base = _sandbox->root_abs();
	}
	void TearDown() override { _sandbox.reset(); }

	// 逐条核对 [0, n) 的成交：序号连续、载荷完整
	static void expect_trans_prefix(WTSTransSlice* s, uint32_t n)
	{
		ASSERT_NE(nullptr, s);
		ASSERT_EQ((size_t)n, s->size());
		for (uint32_t i = 0; i < n; i++)
		{
			const WTSTransStruct* t = s->at(i);
			ASSERT_NE(nullptr, t);
			ASSERT_EQ((uint64_t)i, t->index) << "position " << i;
			ASSERT_EQ((uint32_t)t->askorder, trans_checksum(*t)) << "torn record at " << i;
		}
	}

	std::unique_ptr<q03::Sandbox> _sandbox;
	string _base;
};

} // namespace i03s2r100

using namespace i03s2r100;

TEST_F(I03S2R100RdmCrossProcess, ParkedWriterSlotStaysInvisibleUntilPublishedAcrossProcesses)
{
	int toParent[2], toChild[2];
	ASSERT_EQ(0, ::pipe(toParent));
	ASSERT_EQ(0, ::pipe(toChild));
	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		::close(toParent[0]);
		::close(toChild[1]);
		::_exit(writer_child(_base, toParent[1], toChild[0]));
	}
	::close(toParent[1]);
	::close(toChild[0]);
	auto release_writer = [&]() { char go = 'g'; (void)!::write(toChild[1], &go, 1); };

	q03::BaseDataMgr bd;
	bd.set_cur_tdate(kTDate);
	q03::HotMgr hot;
	q03::RdmSink sink(&bd, &hot);
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _base.c_str());
	WtRdmDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();

	const uint64_t from = at(0);
	char id = 0;

	// 停点 a：成交槽 101 已写未发布（与已发布的槽 100 同一时间戳）
	ASSERT_TRUE(wait_token(toParent[0], id)) << "writer never reached park a";
	ASSERT_EQ('a', id);
	{
		WTSTransSlice* s = reader.readTransSliceByRange("SHFE.rb2210", from, at(50));
		expect_trans_prefix(s, 101);
		if (s) s->release();
	}
	release_writer();

	// 停点 b：扩容到 5000 之后，槽 2550 已写未发布；读端须重映射看到 2500 以后的记录
	ASSERT_TRUE(wait_token(toParent[0], id)) << "writer never reached park b";
	ASSERT_EQ('b', id);
	{
		WTSTransSlice* s = reader.readTransSliceByRange("SHFE.rb2210", from, at(1275));
		expect_trans_prefix(s, 2550);
		if (s) s->release();
	}
	release_writer();

	// 停点 c：委托队列槽 5 已写未发布
	ASSERT_TRUE(wait_token(toParent[0], id)) << "writer never reached park c";
	ASSERT_EQ('c', id);
	{
		// 停点 a 放行后，同一 reader 读到完整末槽：全部 2600 条成交
		WTSTransSlice* s = reader.readTransSliceByRange("SHFE.rb2210", from, at(1300));
		expect_trans_prefix(s, kTrans);
		if (s) s->release();

		WTSOrdQueSlice* q = reader.readOrdQueSliceByRange("SHFE.rb2210", from, at(100));
		ASSERT_NE(nullptr, q);
		ASSERT_EQ(5u, q->size());
		for (uint32_t i = 0; i < 5; i++)
		{
			ASSERT_EQ(i, q->at(i)->order_items);
			ASSERT_EQ(q->at(i)->volumes[49], queue_checksum(*q->at(i)));
		}
		q->release();
	}
	release_writer();

	// 停点 d：tick 槽 3 已写未发布
	ASSERT_TRUE(wait_token(toParent[0], id)) << "writer never reached park d";
	ASSERT_EQ('d', id);
	{
		WTSOrdQueSlice* q = reader.readOrdQueSliceByRange("SHFE.rb2210", from, at(100));
		ASSERT_NE(nullptr, q);
		EXPECT_EQ((size_t)kQueues, q->size()) << "released queue tail must become visible";
		q->release();

		WTSTickSlice* byCount = reader.readTickSliceByCount("SHFE.rb2210", 100, at(1000));
		ASSERT_NE(nullptr, byCount);
		EXPECT_EQ(3u, byCount->size()) << "unpublished tick slot must not be counted";
		if (byCount->size() == 3u)
			EXPECT_EQ(hms_ms(20), byCount->at(2)->action_time);
		byCount->release();

		WTSTickSlice* byRange = reader.readTickSliceByRange("SHFE.rb2210", from, at(1000));
		ASSERT_NE(nullptr, byRange);
		EXPECT_EQ(3u, byRange->size());
		byRange->release();
	}
	release_writer();

	int status = 0;
	ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
	ASSERT_TRUE(WIFEXITED(status)) << "writer child was signalled";
	ASSERT_EQ(0, WEXITSTATUS(status)) << "writer child failed (50 init, 52 trans, 53 queue, 54 tick)";

	WTSTickSlice* ticks = reader.readTickSliceByCount("SHFE.rb2210", 100, at(1000));
	ASSERT_NE(nullptr, ticks);
	EXPECT_EQ((size_t)kTicks, ticks->size());
	ticks->release();
	reader.clearCache();
	::close(toParent[0]);
	::close(toChild[1]);
}
