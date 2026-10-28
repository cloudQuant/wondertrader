// Lane L02 - CsvReader 与 ShareBlocks 内存内可测部分单元测试
// 覆盖：CsvReader（load_from_file/next_row/按列与按字段取值/特殊字符清洗/自定义分隔符）、
//       shareblock 结构体布局（KeyInfo/SecInfo/ShmBlock/CmdInfo/CmdBlock/SMVT_SIZES）
// 跳过：ShareBlocks 的 init_master/init_slave/allocate_*/set_*/get_* 等接口
//       依赖 BoostMappingFile 映射文件与跨进程共享内存段，不在单进程单测范围内。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "WTSTools/CsvHelper.h"
#include "WtShareHelper/ShareBlocks.h"

namespace {

std::filesystem::path g_unique_dir()
{
	static uint32_t s_seq = 0;
	std::string name = "l02_csv_" + std::to_string((long)::getpid()) + "_" + std::to_string(s_seq++);
	std::filesystem::path dir = std::filesystem::temp_directory_path() / name;
	std::error_code ec;
	std::filesystem::remove_all(dir, ec);
	std::filesystem::create_directories(dir, ec);
	return dir;
}

class L02CsvFixture : public ::testing::Test
{
protected:
	void SetUp() override { m_dir = g_unique_dir(); }

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(m_dir, ec);
	}

	// 写文件并返回完整路径
	std::string write_file(const char* filename, const std::string& content)
	{
		std::filesystem::path p = m_dir / filename;
		std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
		ofs << content;
		ofs.close();
		return p.string();
	}

	std::filesystem::path m_dir;
};

}  // namespace

///////////////////////////////////////////////////////// CsvReader
TEST_F(L02CsvFixture, LoadMissingFileReturnsFalse)
{
	CsvReader reader;
	const std::string path = (m_dir / "not_exists.csv").string();
	EXPECT_FALSE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(0u, reader.col_count());
}

TEST_F(L02CsvFixture, HeaderAndRowAccess)
{
	const std::string path = write_file("basic.csv",
		"code,name,price,vol,flag\n"
		"IF2107,沪深2107,4500.6,10,true\n"
		"CU2108,沪铜2108,52000,5,false\n");

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	// 字段名统一转小写
	EXPECT_EQ(5u, reader.col_count());

	// 首行是表头，next_row 从第二行开始
	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("IF2107", reader.get_string(0));
	EXPECT_STREQ("沪深2107", reader.get_string(1));
	EXPECT_DOUBLE_EQ(4500.6, reader.get_double(2));
	EXPECT_EQ(10, reader.get_int32(3));
	EXPECT_STREQ("true", reader.get_string(4));

	// 按字段名读取
	EXPECT_STREQ("IF2107", reader.get_string("code"));
	EXPECT_DOUBLE_EQ(4500.6, reader.get_double("price"));
	EXPECT_EQ(10u, reader.get_uint32("vol"));
	EXPECT_EQ((int64_t)10, reader.get_int64("vol"));
	EXPECT_EQ(10u, reader.get_uint64("vol"));
	EXPECT_EQ(10, reader.get_int32("vol"));

	// 字段名大小写不敏感（表头已转小写，查询原样匹配）
	EXPECT_STREQ("IF2107", reader.get_string("code"));

	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("CU2108", reader.get_string("code"));
	EXPECT_DOUBLE_EQ(52000.0, reader.get_double("price"));
	EXPECT_FALSE(reader.get_int32("flag"));

	// 数据读完
	EXPECT_FALSE(reader.next_row());
}

TEST_F(L02CsvFixture, MissingFieldAndColumnDefaults)
{
	const std::string path = write_file("sparse.csv",
		"a,b\n"
		"1,2\n");

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(2u, reader.col_count());
	ASSERT_TRUE(reader.next_row());

	// 未知字段 -> get_col_by_filed 返回 INT_MAX -> check_cell 拦截
	EXPECT_EQ(0, reader.get_int32("zzz"));
	EXPECT_EQ(0u, reader.get_uint32("zzz"));
	EXPECT_EQ((int64_t)0, reader.get_int64("zzz"));
	EXPECT_EQ(0ull, reader.get_uint64("zzz"));
	EXPECT_DOUBLE_EQ(0.0, reader.get_double("zzz"));
	EXPECT_STREQ("", reader.get_string("zzz"));

	// 越界列号同样返回默认值
	EXPECT_EQ(0, reader.get_int32(9));
	EXPECT_STREQ("", reader.get_string(9));
	EXPECT_DOUBLE_EQ(0.0, reader.get_double(-1));

	// 非数字文本按 0 处理
	const std::string path2 = write_file("text.csv", "v\nabc\n");
	CsvReader r2;
	ASSERT_TRUE(r2.load_from_file(path2.c_str()));
	ASSERT_TRUE(r2.next_row());
	EXPECT_EQ(0, r2.get_int32("v"));
	EXPECT_DOUBLE_EQ(0.0, r2.get_double("v"));
	EXPECT_STREQ("abc", r2.get_string("v"));
}

TEST_F(L02CsvFixture, HeaderSpecialCharsStrippedAndLowerCased)
{
	// 表头中的 < > " ' 会被剔除，并整体转小写
	const std::string path = write_file("special.csv",
		"C<Ode,Na\"me,W'eird,Va<lue\n"
		"10,20,30,40\n");

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(4u, reader.col_count());
	ASSERT_TRUE(reader.next_row());
	EXPECT_EQ(10, reader.get_int32("code"));
	EXPECT_EQ(20, reader.get_int32("name"));
	EXPECT_EQ(30, reader.get_int32("weird"));
	EXPECT_EQ(40, reader.get_int32("value"));
}

TEST_F(L02CsvFixture, TrimAndCustomSplitter)
{
	// 表头两侧空格会被 trim，分隔符可自定义
	const std::string path = write_file("semi.csv",
		" code ; name ;vol\n"
		"x1;na1;3\n"
		"\n"                 // 空行会被跳过
		"x2;na2;4\n");

	CsvReader reader(";");
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(3u, reader.col_count());

	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("x1", reader.get_string("code"));
	EXPECT_STREQ("na1", reader.get_string("name"));
	EXPECT_EQ(3u, reader.get_uint32("vol"));

	// 中间的空行不会返回给调用方
	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("x2", reader.get_string("code"));
	EXPECT_EQ(4u, reader.get_uint32("vol"));
	EXPECT_FALSE(reader.next_row());
}

TEST_F(L02CsvFixture, TrailingEmptyFieldStopsMapping)
{
	// 表头末尾多一个分隔符时，空字段会终止映射
	const std::string path = write_file("trail.csv",
		"a,b,,d\n"
		"1,2,3,4\n");

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(2u, reader.col_count());
	ASSERT_TRUE(reader.next_row());
	EXPECT_EQ(1, reader.get_int32("a"));
	EXPECT_EQ(2, reader.get_int32("b"));
	// 第 3、4 列没有建立字段映射
	EXPECT_STREQ("", reader.get_string("d"));
}

TEST_F(L02CsvFixture, TabSplitterAndLongIntegers)
{
	const std::string path = write_file("tab.tsv",
		"tradeid\tamount\tts\n"
		"9007199254740993\t-12345\t1700000000\n");

	CsvReader reader("\t");
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(3u, reader.col_count());
	ASSERT_TRUE(reader.next_row());
	EXPECT_EQ((int64_t)9007199254740993LL, reader.get_int64("tradeid"));
	EXPECT_EQ(9007199254740993ULL, reader.get_uint64("tradeid"));
	EXPECT_EQ((int32_t)-12345, reader.get_int32("amount"));
	EXPECT_DOUBLE_EQ(1700000000.0, reader.get_double("ts"));
}

///////////////////////////////////////////////////////// ShareBlocks 结构布局
TEST(L02ShareBlock, KeyInfoPackedLayout)
{
	using namespace shareblock;
	// #pragma pack(1) 下无填充
	EXPECT_EQ(sizeof(KeyInfo), sizeof(ValueType) + 32 + sizeof(uint32_t) + sizeof(uint64_t));
	EXPECT_EQ(sizeof(ValueType), sizeof(uint64_t));

	KeyInfo key;
	memset(&key, 0, sizeof(key));
	snprintf(key._key, sizeof(key._key), "%s", "k1");
	key._type = SMVT_DOUBLE;
	key._offset = 8;
	key._updatetime = 1234567890ULL;
	EXPECT_STREQ("k1", key._key);
	EXPECT_EQ(SMVT_DOUBLE, key._type);
	EXPECT_EQ(8u, key._offset);
	EXPECT_EQ(1234567890ULL, key._updatetime);
}

TEST(L02ShareBlock, ValueTypeSizeTable)
{
	using namespace shareblock;
	EXPECT_EQ(sizeof(SMVT_SIZES) / sizeof(SMVT_SIZES[0]), (size_t)SMVT_STRING + 1);
	EXPECT_EQ(SMVT_SIZES[SMVT_INT32], (size_t)4);
	EXPECT_EQ(SMVT_SIZES[SMVT_UINT32], (size_t)4);
	EXPECT_EQ(SMVT_SIZES[SMVT_INT64], (size_t)8);
	EXPECT_EQ(SMVT_SIZES[SMVT_UINT64], (size_t)8);
	EXPECT_EQ(SMVT_SIZES[SMVT_DOUBLE], (size_t)8);
	// 字符串槽位固定 64 字节
	EXPECT_EQ(SMVT_SIZES[SMVT_STRING], (size_t)64);
	EXPECT_EQ(SMVT_SIZES[0], (size_t)0);

	EXPECT_EQ(FLAG_SIZE, 8);
	EXPECT_EQ(MAX_SEC_CNT, 64);
	EXPECT_EQ(MAX_KEY_CNT, 128);
	EXPECT_EQ(MAX_CMD_SIZE, 64);
	// BLK_FLAG 字面量含显式 \0，因此 sizeof 比 FLAG_SIZE 多 1
	EXPECT_EQ(sizeof(BLK_FLAG), (size_t)FLAG_SIZE + 1);
	EXPECT_STREQ("&^%$#@!", BLK_FLAG);
}

TEST(L02ShareBlock, SectionInfoZeroInitializedAndTypedAccess)
{
	using namespace shareblock;
	SecInfo sec;
	// 构造函数整体 memset
	EXPECT_EQ(0, sec._count);
	EXPECT_EQ(0, sec._state);
	EXPECT_EQ(0u, sec._offset);
	EXPECT_EQ(0ULL, sec._updatetime);
	EXPECT_EQ('\0', sec._name[0]);

	// get<T> 直接指向 _data 内的偏移
	int32_t* i32 = sec.get<int32_t>(0);
	ASSERT_TRUE(i32 != NULL);
	*i32 = -2026;
	EXPECT_EQ(-2026, *sec.get<int32_t>(0));
	EXPECT_TRUE((char*)i32 == sec._data);

	uint64_t* u64 = sec.get<uint64_t>(4);
	*u64 = 18446744073709551615ULL;
	EXPECT_EQ(18446744073709551615ULL, *sec.get<uint64_t>(4));

	double* dbl = sec.get<double>(1024 - 8);
	*dbl = 3.5;
	EXPECT_DOUBLE_EQ(3.5, *sec.get<double>(1024 - 8));

	// _data 尾部恰好可容纳一个 double
	EXPECT_EQ(sizeof(SecInfo), (size_t)(32 + MAX_KEY_CNT * sizeof(KeyInfo)
		+ sizeof(uint16_t) * 2 + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(sec._data)));

	snprintf(sec._name, sizeof(sec._name), "%s", "section1");
	sec._count = 3;
	sec._state = 1;
	sec._offset = 1024;
	sec._updatetime = 42;
	EXPECT_STREQ("section1", sec._name);
	EXPECT_EQ(3, sec._count);
	EXPECT_EQ(1, sec._state);
	EXPECT_EQ(1024u, sec._offset);
	EXPECT_EQ(42ULL, sec._updatetime);
}

TEST(L02ShareBlock, ShmBlockLayout)
{
	using namespace shareblock;
	ShmBlock blk;
	EXPECT_EQ(0u, blk._count);
	EXPECT_EQ(0ULL, blk._updatetime);
	EXPECT_EQ('\0', blk._flag[0]);
	EXPECT_EQ('\0', blk._name[0]);
	EXPECT_EQ(sizeof(ShmBlock), (size_t)(8 + 32 + MAX_SEC_CNT * sizeof(SecInfo)
		+ sizeof(uint64_t) + sizeof(uint32_t)));

	snprintf(blk._name, sizeof(blk._name), "%s", "domain");
	blk._count = 2;
	EXPECT_STREQ("domain", blk._name);
	EXPECT_EQ(2u, blk._count);
	// 第一个 section 也已被 memset 清零
	EXPECT_EQ(0, blk._sections[0]._count);
	EXPECT_EQ(0, blk._sections[MAX_SEC_CNT - 1]._state);
}

TEST(L02ShareBlock, CmdBlockRingIndices)
{
	using namespace shareblock;
	CmdInfo cmd;
	EXPECT_EQ(0u, cmd._state);
	EXPECT_EQ('\0', cmd._command[0]);
	snprintf(cmd._command, sizeof(cmd._command), "%s", "reload");
	cmd._state = 1;
	EXPECT_STREQ("reload", cmd._command);
	EXPECT_EQ(1u, cmd._state);

	CmdBlock blk;
	EXPECT_EQ(128u, blk._capacity);
	// 初始可读位置为 UINT32_MAX，表示尚无命令
	EXPECT_EQ(UINT32_MAX, blk._readable);
	EXPECT_EQ(0u, blk._writable);
	EXPECT_EQ(0u, blk._cmdpid);
	EXPECT_EQ(sizeof(CmdBlock), sizeof(uint32_t) + sizeof(uint32_t) * 3
		+ sizeof(CmdInfo) * 128);

	blk._readable = 5;
	blk._writable = 6;
	EXPECT_EQ(5u, blk._readable);
	EXPECT_EQ(6u, blk._writable);
}

TEST(L02ShareBlock, SmallCmdBlockTemplate)
{
	using namespace shareblock;
	_CmdBlock<4> small;
	EXPECT_EQ(4u, small._capacity);
	EXPECT_EQ(UINT32_MAX, small._readable);
	small._commands[0]._state = 1;
	snprintf(small._commands[0]._command, sizeof(small._commands[0]._command), "%s", "sync");
	EXPECT_STREQ("sync", small._commands[0]._command);
	EXPECT_EQ(0u, small._commands[3]._state);
}
