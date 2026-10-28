// r70 / WtDataWriter：收盘作业里逐笔成交/委托明细/委托队列的历史落地、tick csv 流收尾、
// 缺省缓存名与异步任务管线。
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - proc_loop 的逐笔历史分支 :2492-2555（trans dumper 失败 + his/trans 落盘）
//    :2557-2621（orddtl dumper/文件）:2623-2687（ordque dumper/文件）；:2424-2425（_fstream 收尾）
//  - init 的缺省缓存名 :127-129 与建目录 :125-126；loadCache 新建分支 :274-300
//  - 异步管线 pushTask :578-622 与四个 write* 的 async 分派 :371-372 / :421-422 / :475-476 / :529-530
//
// 线程纪律沿用 r23：调过 init 的 writer 只 release 不 delete；走 transHisData 的只等 marker.ini
// 并 abandon（理由见 r23_writer_fixture.h 头注释）。
#include "r23_writer_fixture.h"

#include "WTSUtils/WTSCmpHelper.hpp"

namespace r70 {

using namespace r23;
using std::string;
using std::vector;

namespace {

// 读 writer 侧 CMP_V2 历史文件（BlockHeaderV2 + zstd 载荷），按 T 展开
template<typename T>
vector<T> load_cmp_items(const string& file, uint16_t expect_type, bool& ok)
{
	ok = false;
	const string raw = read_file_bytes(file);
	if (raw.size() < sizeof(BlockHeaderV2))
		return vector<T>();

	BlockHeaderV2 head;
	std::memset(&head, 0, sizeof(head));
	std::memcpy(&head, raw.data(), sizeof(head));
	if (head._type != expect_type || head._version != BLOCK_VERSION_CMP_V2)
		return vector<T>();
	if (raw.size() != sizeof(BlockHeaderV2) + head._size)
		return vector<T>();

	const string payload = WTSCmpHelper::uncompress_data(raw.data() + sizeof(BlockHeaderV2),
		(std::size_t)head._size);
	if (payload.size() % sizeof(T) != 0)
		return vector<T>();

	vector<T> items(payload.size() / sizeof(T));
	if (!items.empty())
		std::memcpy(items.data(), payload.data(), payload.size());
	ok = true;
	return items;
}

} // namespace

class R70WriterResidualTest : public R23WriterTest
{
};

// ------------------------------------------------------------------ 收盘作业：逐笔三类落地
// WtDataWriter.cpp: proc_loop L2492-2687（trans/orddtl/ordque 的 dumper 失败日志与 his/*.dsb）、
//   L2424-2425（tick 的 _fstream 收尾在 savelog=true 时才有对象）
TEST_F(R70WriterResidualTest, R70WriterClosingDumpsTransOrderAndQueueHis)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	FakeDumper good;
	FakeDumper bad;
	bad.set_ret(false);
	w->add_dumper("r70good", &good);
	w->add_dumper("r70bad", &bad);

	TickSpec t;
	t.total_volume = 1000.0;
	t.total_turnover = 3600000.0;
	write_tick(w, t, 0);

	OrdQueSpec q;
	q.price = 3600.0;
	q.order_items = 3;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	ASSERT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000, 3600.0, 7)));
	ASSERT_TRUE(w->writeTransaction(make_trans_data(93000000, 3601.0, 8)));

	abandon_writer(w);
	w->transHisData("R70_HFT");
	ASSERT_TRUE(wait_marker("R70_HFT")) << "收盘作业没在 10s 内排空队列";

	// 三类逐笔 + tick 都必须真正落地
	const string tick_dsb = his_tick_file("rb2210", 20240108);
	const string trans_dsb = _base + "his/trans/SHFE/20240108/rb2210.dsb";
	const string dtl_dsb = _base + "his/orders/SHFE/20240108/rb2210.dsb";
	const string que_dsb = _base + "his/queue/SHFE/20240108/rb2210.dsb";
	ASSERT_TRUE(file_exists(tick_dsb));
	ASSERT_TRUE(file_exists(trans_dsb));
	ASSERT_TRUE(file_exists(dtl_dsb));
	ASSERT_TRUE(file_exists(que_dsb));

	bool ok = false;
	const vector<WTSTransStruct> trs = load_cmp_items<WTSTransStruct>(trans_dsb,
		(uint16_t)BT_HIS_Trnsctn, ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(trs.size(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(trs[0].price, 3601.0);
	EXPECT_EQ(trs[0].index, 8u);

	const vector<WTSOrdDtlStruct> dtls = load_cmp_items<WTSOrdDtlStruct>(dtl_dsb,
		(uint16_t)BT_HIS_OrdDetail, ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(dtls.size(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(dtls[0].price, 3600.0);
	EXPECT_EQ(dtls[0].index, 7u);

	const vector<WTSOrdQueStruct> ques = load_cmp_items<WTSOrdQueStruct>(que_dsb,
		(uint16_t)BT_HIS_OrdQueue, ok);
	ASSERT_TRUE(ok);
	ASSERT_EQ(ques.size(), (std::size_t)1);
	EXPECT_DOUBLE_EQ(ques[0].price, 3600.0);
	EXPECT_EQ(ques[0].order_items, 3u);

	// 两个 dumper（一真一假）都收到四类数据；假的那份按级别打错误日志
	EXPECT_EQ(good.tick_calls(), 1u);
	EXPECT_EQ(good.trns_calls(), 1u);
	EXPECT_EQ(good.dtl_calls(), 1u);
	EXPECT_EQ(good.que_calls(), 1u);
	EXPECT_EQ(bad.tick_calls(), 1u);
	EXPECT_EQ(bad.trns_calls(), 1u);
	EXPECT_EQ(bad.dtl_calls(), 1u);
	EXPECT_EQ(bad.que_calls(), 1u);
	EXPECT_TRUE(_sink->has_log("via extended dumper r70bad failed", LL_ERROR));

	// d1/m1/m5 三段也都喂给了扩展 dumper
	EXPECT_EQ(good.bar_count_of("d1"), 1u);
	EXPECT_EQ(good.bar_count_of("m1"), 1u);
	EXPECT_EQ(good.bar_count_of("m5"), 1u);

	// 迁移完四类 RT 块条数归零
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 0u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/trans/SHFE/rb2210.dmb")._size, 0u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/orders/SHFE/rb2210.dmb")._size, 0u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/queue/SHFE/rb2210.dmb")._size, 0u);
}

// ------------------------------------------------------------------ savelog 的 fstream 收尾
// WtDataWriter.cpp: proc_loop L2424-2425（savelog=true 时 tick 块的 ofstream 在收盘时 reset/关闭）
TEST_F(R70WriterResidualTest, R70WriterClosingResetsTickCsvStream)
{
	R23Writer* w = make_writer("\"savelog\":true");
	ASSERT_TRUE(w != NULL);

	TickSpec t;
	t.price = 3611.5;
	t.total_volume = 1000.0;
	write_tick(w, t, 0);

	const string csv = _base + "rt/ticks/SHFE/rb2210.20240108.csv";
	ASSERT_TRUE(file_exists(csv));

	abandon_writer(w);
	w->transHisData("R70_CSV");
	ASSERT_TRUE(wait_marker("R70_CSV"));

	// reset 关闭流后 csv 仍保留，且内容已 flush
	const string content = read_file_bytes(csv);
	EXPECT_NE(content.find("rb2210"), string::npos);
	EXPECT_FALSE(content.empty());
	EXPECT_TRUE(file_exists(his_tick_file("rb2210", 20240108)));
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 0u);
}

// ------------------------------------------------------------------ 缺省缓存名 + 自动建目录
// WtDataWriter.cpp: init L124-129（path 不存在时 create_directories；cache 为空 → cache.dmb）
//   + loadCache L274-300（新建 RTTickCache 的容量/类型/标志）
TEST_F(R70WriterResidualTest, R70WriterInitCreatesDirAndDefaultCacheName)
{
	const string extra = fmtutil::format("\"path\":\"{}deep/newroot/\",\"cache\":\"\"", _base);
	R23Writer* w = make_writer(extra.c_str());
	ASSERT_TRUE(w != NULL);

	const string cache = _base + "deep/newroot/cache.dmb";
	ASSERT_TRUE(file_exists(cache));
	EXPECT_EQ(file_size(cache),
		(uintmax_t)(sizeof(RTTickCache) + 8 * sizeof(TickCacheItem)));

	const RTBlockHeader head = read_block_header<RTBlockHeader>(cache);
	EXPECT_EQ(head._type, (uint32_t)BT_RT_Cache);
	EXPECT_EQ(head._version, 1u);
	EXPECT_EQ(head._size, 0u);
	EXPECT_EQ(head._capacity, 8u);
}

// ------------------------------------------------------------------ 异步任务管线
// WtDataWriter.cpp: writeTick/Queue/Detail/Trans 的 async 分派 :371-372 / :421-422 / :475-476 /
//   :529-530；pushTask 的建线程与四类 switch :578-622
TEST_F(R70WriterResidualTest, R70WriterAsyncPipelineDrainsAllTypes)
{
	R23Writer* w = make_writer("\"async\":true");
	ASSERT_TRUE(w != NULL);

	TickSpec t;
	t.total_volume = 1000.0;
	write_tick(w, t, 0);

	OrdQueSpec q;
	q.price = 3600.0;
	ASSERT_TRUE(w->writeOrderQueue(make_ordque_data(q)));
	ASSERT_TRUE(w->writeOrderDetail(make_orddtl_data(93000000, 3600.0, 1)));
	ASSERT_TRUE(w->writeTransaction(make_trans_data(93000000, 3601.0, 2)));

	// 任务线程不可 join，先放弃 TearDown 的 release，避免失败路径上清块与线程竞争
	abandon_writer(w);

	bool ready = false;
	for (uint32_t i = 0; i < 400 && !ready; i++)
	{
		usleep(5000);
		ready = _sink->ticks() >= 1u && _sink->queues() >= 1u
			&& _sink->dtls() >= 1u && _sink->trans() >= 1u
			&& file_exists(rt_tick_file("rb2210"))
			&& file_exists(_base + "rt/queue/SHFE/rb2210.dmb")
			&& file_exists(_base + "rt/orders/SHFE/rb2210.dmb")
			&& file_exists(_base + "rt/trans/SHFE/rb2210.dmb");
	}
	ASSERT_TRUE(ready) << "async 管线 2s 内没有处理完四类数据";

	EXPECT_EQ(read_block_header<RTDayBlockHeader>(rt_tick_file("rb2210"))._size, 1u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/queue/SHFE/rb2210.dmb")._size, 1u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/orders/SHFE/rb2210.dmb")._size, 1u);
	EXPECT_EQ(read_block_header<RTDayBlockHeader>(_base + "rt/trans/SHFE/rb2210.dmb")._size, 1u);

	WTSTickData* cached = w->getCurTick("rb2210", "SHFE");
	ASSERT_TRUE(cached != NULL);
	EXPECT_DOUBLE_EQ(cached->totalvolume(), 1000.0);
	cached->release();
}

} // namespace r70
