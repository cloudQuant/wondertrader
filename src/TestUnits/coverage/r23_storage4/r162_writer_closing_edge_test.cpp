// r162 / WtDataWriter：收盘转储时历史文件打开失败的分支 + 旧块头直通分支
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - 六个 ClosingTask 的 create_*_file 失败日志 :2126-2129 / :2203-2206 / :2479-2482
//    / :2546-2549 / :2612-2615 / :2678-2681
//  - proc_block_data 的 bKeepHead=false 且「非压缩、非老版本」直通分支 :1849-1854
//
// 手法：把各类历史文件的**父目录**置为只读（目录本身存在，文件不存在）——
// 这样 StdFile::exists 为假、create_*_file 以 EACCES 失败，不会去读目录内容；
// 用例结束前恢复权限，TearDown 才能清理临时目录。
// 收盘线程纪律沿用 r23 夹具：transHisData 之后只等 marker，不 release（丢唤醒窗口）。
#include "r23_writer_fixture.h"

#include <sys/stat.h>

namespace r162w {

using namespace r23;
using std::string;

class R162WriterClosingEdgeTest : public R23WriterTest
{
protected:
	static void set_mode(const string& path, mode_t mode)
	{
		ASSERT_EQ(::chmod(path.c_str(), mode), 0) << path;
	}
};

// WtDataWriter.cpp:2126-2129, 2203-2206, 2479-2482, 2546-2549, 2612-2615, 2678-2681
TEST_F(R162WriterClosingEdgeTest, R162WriterClosingOpenFailuresAreLogged)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec tick;
	tick.code = "rb2210";
	write_tick(w, tick, 0);

	WTSOrdQueData* oq = make_ordque_data(OrdQueSpec());
	ASSERT_TRUE(w->writeOrderQueue(oq));
	oq->release();

	WTSOrdDtlData* od = make_orddtl_data(93000100u, 3601.0, 1);
	ASSERT_TRUE(w->writeOrderDetail(od));
	od->release();

	WTSTransData* tr = make_trans_data(93000200u, 3602.0, 1);
	ASSERT_TRUE(w->writeTransaction(tr));
	tr->release();

	// 只读父目录：目标文件不存在，打开必然失败
	const string dirs[] = {
		_base + "his/min1/SHFE",
		_base + "his/min5/SHFE",
		_base + "his/ticks/SHFE/20240108",
		_base + "his/trans/SHFE/20240108",
		_base + "his/orders/SHFE/20240108",
		_base + "his/queue/SHFE/20240108",
	};
	std::error_code ec;
	for (const string& d : dirs)
	{
		std::filesystem::create_directories(d, ec);
		ASSERT_FALSE(ec) << d << ": " << ec.message();
		set_mode(d, 0555);
	}

	abandon_writer(w);
	w->transHisData("R23_CACHE");
	ASSERT_TRUE(wait_marker("R23_CACHE"));

	EXPECT_TRUE(_sink->has_log("ClosingTask of min1 bar failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("ClosingTask of min5 bar failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("ClosingTask of tick failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("ClosingTask of transaction failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("ClosingTask of order detail failed", LL_ERROR));
	EXPECT_TRUE(_sink->has_log("ClosingTask of order queue failed", LL_ERROR));

	for (const string& d : dirs)
		set_mode(d, 0755);
}

// WtDataWriter.cpp:1849-1854（proc_block_data 的 bKeepHead=false 直通分支）
TEST_F(R162WriterClosingEdgeTest, R162WriterRawMin1BlockHeaderIsStrippedBeforeAppend)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec tick;
	tick.code = "rb2210";
	write_tick(w, tick, 0);

	// 预置一个「新结构体、未压缩」的 min1 历史文件：只有块头，没有 bar
	BlockHeader head;
	std::memset(&head, 0, sizeof(head));
	std::memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
	head._type = BT_HIS_Minute1;
	head._version = BLOCK_VERSION_RAW_V2;

	const string file = his_min1_file("rb2210");
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);
	ASSERT_FALSE(ec) << ec.message();
	{
		std::ofstream ofs(file.c_str(), std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(ofs.is_open());
		ofs.write((const char*)&head, (std::streamsize)sizeof(head));
		ASSERT_TRUE(ofs.good());
	}

	abandon_writer(w);
	w->transHisData("R23_CACHE");
	ASSERT_TRUE(wait_marker("R23_CACHE"));

	ASSERT_TRUE(file_exists(file));
	const BlockHeaderV2 out = read_block_header<BlockHeaderV2>(file);
	EXPECT_EQ(out._version, BLOCK_VERSION_CMP_V2);
	// 原来 0 条 + 本次 1 条 = 1 条（压缩后 payload 大小等于一条 bar 的压缩结果）
	EXPECT_GT(out._size, 0u);
	EXPECT_TRUE(_sink->has_log("Transfering min1 bars", LL_INFO));
}

} // namespace r162w
