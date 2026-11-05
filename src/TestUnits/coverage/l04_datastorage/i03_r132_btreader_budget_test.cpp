// BUG-R132-001（迭代03 最终缺口，slice2）：WtBtDtReader 从自身 store 配置读取 K37 解压预算
// （uncompress_max_bytes / uncompress_max_ratio，与 path 同级）。解压在 WtDataStorage 模块内
// 进行，只有读端在自身 init 中应用才对该模块生效。
//  - 配置的压缩比上限低于数据实际压缩比：CMP_V2 历史块读取失败并记 ERROR；
//  - 非法值：记 ERROR，本模块 fail-closed（压缩块全部拒绝），未压缩块仍可读；
//  - 缺省：默认 1 GiB / 1000:1，同一文件正常读出（正控）。
// 红控：以改前 WtBtDtReader.cpp 对象重链（配置被忽略，压缩块照常读出）。
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#include "Includes/WTSVariant.hpp"
#include "Includes/WTSStruct.h"
#include "Includes/IBtDtReader.h"
#include "WtDataStorage/DataDefine.h"
#include "WtDataStorage/WtBtDtReader.h"
#include "WTSUtils/WTSCmpHelper.hpp"

using namespace wtp;

namespace i03r132bt {

class LogSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		if (ll == LL_ERROR)
			_errors.emplace_back(message == NULL ? "" : message);
	}
	bool has_error(const char* sub) const
	{
		for (const std::string& e : _errors)
			if (e.find(sub) != std::string::npos)
				return true;
		return false;
	}
	std::vector<std::string> _errors;
};

class I03R132BtReaderBudget : public ::testing::Test
{
protected:
	void SetUp() override
	{
		static uint32_t seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("i03_r132_bt_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
		std::filesystem::remove_all(_root);
		std::filesystem::create_directories(_root);
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
		write_bars("rb2401", true);
		write_bars("rb2402", false);
	}

	void TearDown() override
	{
		WTSCmpHelper::set_uncompress_budget(1ull << 30, 1000);
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	// 2000 根相同日期的 m1 K 线：CMP_V2 压缩比约几十倍（低于默认 1000、高于配置的 5）
	void write_bars(const char* code, bool compressed)
	{
		std::vector<WTSBarStruct> bars(2000);
		for (size_t i = 0; i < bars.size(); i++)
		{
			bars[i] = WTSBarStruct();
			bars[i].date = 20240102;
			bars[i].time = 2401020931ull + i;
			bars[i].close = 3500.0 + (double)(i % 7);
			bars[i].vol = (double)(i % 13);
		}
		const std::string raw((const char*)bars.data(), bars.size() * sizeof(WTSBarStruct));
		std::string content;
		if (compressed)
		{
			const std::string cmped = WTSCmpHelper::compress_data(raw.data(), raw.size());
			content.assign(BLOCK_HEADERV2_SIZE, '\0');
			BlockHeaderV2* header = (BlockHeaderV2*)&content[0];
			memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
			header->_type = BT_HIS_Minute1;
			header->_version = BLOCK_VERSION_CMP_V2;
			header->_size = cmped.size();
			content += cmped;
			_ratio = raw.size() / cmped.size();
		}
		else
		{
			// 未压缩 RAW_V2 块使用不带 _size 的基础头
			content.assign(BLOCK_HEADER_SIZE, '\0');
			BlockHeader* header = (BlockHeader*)&content[0];
			memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
			header->_type = BT_HIS_Minute1;
			header->_version = BLOCK_VERSION_RAW_V2;
			content += raw;
		}
		const std::filesystem::path file = _root / "his" / "min1" / "SHFE" / (std::string(code) + ".dsb");
		std::filesystem::create_directories(file.parent_path());
		std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
		ofs.write(content.data(), (std::streamsize)content.size());
	}

	WTSVariant* base_cfg()
	{
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("path", _root.string().c_str());
		return cfg;
	}

	std::filesystem::path _root;
	size_t _ratio = 0;
};

TEST_F(I03R132BtReaderBudget, DefaultBudgetReadsCompressedHistory)
{
	ASSERT_GT(_ratio, 5u);
	ASSERT_LT(_ratio, 1000u);
	LogSink sink;
	WTSVariant* cfg = base_cfg();
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	std::string buffer;
	ASSERT_TRUE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer));
	EXPECT_EQ(2000u * sizeof(WTSBarStruct), buffer.size());
}

TEST_F(I03R132BtReaderBudget, ConfiguredRatioCeilingRejectsTheCompressedBlock)
{
	LogSink sink;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", (uint32_t)5);
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	EXPECT_EQ(5u, WTSCmpHelper::uncompress_max_ratio()) << "store config must reach this module's budget";

	std::string buffer;
	EXPECT_FALSE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer));
	EXPECT_TRUE(sink.has_error("Processing back")) << "rejected block must be reported";
	// 未压缩块不受压缩比约束
	buffer.clear();
	EXPECT_TRUE(reader.read_raw_bars("SHFE", "rb2402", KP_Minute1, buffer));
}

TEST_F(I03R132BtReaderBudget, YamlStyleDigitStringIsAccepted)
{
	LogSink sink;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_bytes", "4096");
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	EXPECT_EQ(4096u, WTSCmpHelper::uncompress_max_bytes());
	std::string buffer;
	EXPECT_FALSE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer))
		<< "declared size above the configured 4 KiB ceiling";
}

TEST_F(I03R132BtReaderBudget, InvalidBudgetFailsClosedForCompressedBlocks)
{
	LogSink sink;
	WTSVariant* cfg = base_cfg();
	cfg->append("uncompress_max_ratio", (int32_t)-5);
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	EXPECT_TRUE(sink.has_error("uncompress_max_ratio")) << "invalid budget must be diagnosed";

	std::string buffer;
	EXPECT_FALSE(reader.read_raw_bars("SHFE", "rb2401", KP_Minute1, buffer))
		<< "an invalid budget must not fall back to an unbounded or default budget";
	buffer.clear();
	EXPECT_TRUE(reader.read_raw_bars("SHFE", "rb2402", KP_Minute1, buffer)) << "raw blocks need no decompression";
}

} // namespace i03r132bt
