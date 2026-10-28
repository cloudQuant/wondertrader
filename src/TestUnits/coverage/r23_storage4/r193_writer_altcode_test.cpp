// r193 / WtDataWriter：收盘转储的 altCode 文件改名与非压缩块直通
//
// 覆盖（src/WtDataStorage/WtDataWriter.cpp）：
//  - dump_bars_to_file 分钟线 altCode 历史文件改名 :2079-2083
//  - dump_bars_to_file 5分钟线 altCode 历史文件改名 :2158-2163
//  - proc_block_data 的「非压缩、非老版本」bKeepHead=false 直通 :1849-1854
//
// 手法：沿用 r23 夹具；先把 rb2210 的 altCode 改成 rb9999，并预置
// his/min{1,5}/SHFE/rb9999.dsb 两个合法 RAW_V2 块头。写一笔 tick 后经公开的
// transHisData 收盘队列驱动（dump_bars_to_file 是私有实现），生产代码应把
// rb9999.dsb 改名为 rb2210.dsb，读取旧块走 proc_block_data 直通分支，再追加当日 bar。
#include "r23_writer_fixture.h"

namespace r193w {

using namespace r23;
using std::string;

class R193WriterAltCodeTest : public R23WriterTest
{
protected:
	static bool write_all(const string& path, const string& bytes)
	{
		std::error_code ec;
		std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
		std::ofstream ofs(path.c_str(), std::ios::binary | std::ios::trunc);
		if (!ofs.is_open())
			return false;
		ofs.write(bytes.data(), (std::streamsize)bytes.size());
		ofs.flush();
		return ofs.good();
	}

	static string raw_header(uint16_t type)
	{
		BlockHeader head;
		std::memset(&head, 0, sizeof(head));
		std::memcpy(head._blk_flag, BLK_FLAG, FLAG_SIZE);
		head._type = type;
		head._version = BLOCK_VERSION_RAW_V2;
		return string((const char*)&head, sizeof(head));
	}
};

// WtDataWriter.cpp:2079-2083 / 2158-2163 / 1849-1854
TEST_F(R193WriterAltCodeTest, DumpBarsRenamesAltCodeFileAndAppends)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	WTSContractInfo* ct = _bd->getContract("rb2210", "SHFE");
	ASSERT_TRUE(ct != NULL);
	ct->setAltCode("rb9999");

	const string alt1 = _base + "his/min1/SHFE/rb9999.dsb";
	const string alt5 = _base + "his/min5/SHFE/rb9999.dsb";
	ASSERT_TRUE(write_all(alt1, raw_header(BT_HIS_Minute1)));
	ASSERT_TRUE(write_all(alt5, raw_header(BT_HIS_Minute5)));

	TickSpec spec;
	spec.code = "rb2210";
	spec.action_time = 93000000u;
	write_tick(w, spec, 0);

	ASSERT_TRUE(file_exists(rt_min1_file("rb2210")));
	ASSERT_TRUE(file_exists(rt_min5_file("rb2210")));

	// dump_bars_to_file 是私有实现，只能经公开的 transHisData 收盘队列驱动；
	// proc_loop 的 wait 无谓词，按 r23 夹具纪律只等 marker、不 release。
	abandon_writer(w);
	w->transHisData("R193");
	ASSERT_TRUE(wait_marker("R193", 10000));

	const string target1 = _base + "his/min1/SHFE/rb2210.dsb";
	const string target5 = _base + "his/min5/SHFE/rb2210.dsb";
	EXPECT_TRUE(file_exists(target1));
	EXPECT_TRUE(file_exists(target5));
	EXPECT_FALSE(file_exists(alt1));
	EXPECT_FALSE(file_exists(alt5));
	EXPECT_GT(file_size(target1), (uintmax_t)sizeof(BlockHeaderV2));
	EXPECT_GT(file_size(target5), (uintmax_t)sizeof(BlockHeaderV2));
}

} // namespace r193w
