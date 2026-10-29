// Lane R56 - src/WTSTools/CsvHelper.cpp 剩余未覆盖面
// 覆盖：UTF-8 BOM 头剥离（CsvHelper.cpp:23-29）、表头字段名拼装 fields()、
//       只有表头没有数据行时 next_row 的 eof 早退（CsvHelper.cpp:56-59）。
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>

#include "WTSTools/CsvHelper.h"
#include "r56_tmp.h"

class R56Csv : public ::testing::Test
{
protected:
	void SetUp() override { m_dir = r56tmp::unique_dir("r56_csv"); }
	void TearDown() override { r56tmp::clean_dir(m_dir); }

	std::filesystem::path	m_dir;
};

// CsvHelper.cpp:15-54；CsvHelper.h:146-160
TEST_F(R56Csv, BomHeaderParsedAndFieldsExposed)
{
	const std::string path = r56tmp::write_file(m_dir, "bom.csv",
		std::string("\xEF\xBB\xBF") + "Code,<Vol>,'Name'\n" "rb2405,12,螺纹\n");

	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(3u, reader.col_count());
	// 字段名已转小写且剔除了 < > ' 等符号
	const std::string fields = reader.fields();
	EXPECT_NE(std::string::npos, fields.find("code"));
	EXPECT_NE(std::string::npos, fields.find("vol"));
	EXPECT_NE(std::string::npos, fields.find("name"));

	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("rb2405", reader.get_string("code"));
	EXPECT_EQ(12u, reader.get_uint32("vol"));
	EXPECT_STREQ("螺纹", reader.get_string("name"));
	EXPECT_FALSE(reader.next_row());
}

// CsvHelper.cpp:56-59；CsvHelper.h:46
TEST_F(R56Csv, HeaderOnlyFileStopsAtEof)
{
	// 只有表头且末尾不带换行：getline 读到文件尾即置 eof，next_row 首行判断直接返回
	const std::string path = r56tmp::write_file(m_dir, "header_only.csv", "a,b,c");
	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_EQ(3u, reader.col_count());
	EXPECT_FALSE(reader.next_row());
	// 再次调用仍为 false，不能读到脏数据
	EXPECT_FALSE(reader.next_row());
}

// 短行边界探针：先成功读取一行，再访问表头合法但本行缺失的 b/c；期望返回空串/默认值。
// 当前 check_cell 只按表头列数判界，未检查当前行长度，可能越过 _current_cells 边界，故保持禁用。
// CsvHelper.cpp:56-75,117-123,161-170；见 BUG-R56-003。
TEST_F(R56Csv, DISABLED_ShortRowMissingCellsReturnDefaults)
{
	const std::string path = r56tmp::write_file(m_dir, "short_row.csv", "a,b,c\n1\n");
	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	ASSERT_TRUE(reader.next_row());
	EXPECT_STREQ("", reader.get_string("b"));
	EXPECT_EQ(0u, reader.get_uint32("c"));
}
