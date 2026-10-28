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

// 产品缺陷占位：next_row 尚未成功时 _current_cells 为空，但 check_cell 只按
// _fields_map.size() 判界，get_string(0) 会对空 vector 取下标。
// 见 docs/迭代计划/迭代01/bugs/R56.md BUG-R56-003；默认不执行。
// CsvHelper.cpp:117-123,161-170
TEST_F(R56Csv, DISABLED_GetValueBeforeFirstRow)
{
	const std::string path = r56tmp::write_file(m_dir, "norow.csv", "a,b\n1,2\n");
	CsvReader reader;
	ASSERT_TRUE(reader.load_from_file(path.c_str()));
	EXPECT_STREQ("", reader.get_string("a"));
}
