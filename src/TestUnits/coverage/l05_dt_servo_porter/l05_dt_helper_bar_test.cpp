/*
 * L05 - WtDtHelper：K线（bar）相关的二进制/CSV 转换入口
 *
 * 覆盖 src/WtDtHelper/WtDtHelper.cpp 中 store_bars / read_dsb_bars / read_dmb_bars /
 * dump_bars / trans_csv_bars，以及内部 proc_block_data、strToDate、strToTime（经由导出
 * 函数间接覆盖，避免依赖非导出符号的链接）。
 *
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/l05_dt_servo_porter.md。
 * 全部落盘在 std::filesystem::temp_directory_path() 下的 l05_ 前缀唯一目录，TearDown 清理。
 */
#include <gtest/gtest.h>

#include "WtDtHelper/WtDtHelper.h"
#include "Includes/WTSDataDef.hpp"
#include "WtDataStorage/DataDefine.h"
#include "WTSUtils/WTSCmpHelper.hpp"
#include "Share/BoostFile.hpp"

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

USING_NS_WTP;

#ifdef WT_DT_HELPER_CLOSE_TEST_HOOKS
extern "C" void wt_dt_helper_fail_next_close_for_test();
#endif

namespace {

	std::vector<std::string>& log_sink()
	{
		static std::vector<std::string> s_logs;
		return s_logs;
	}

	std::vector<WTSBarStruct>& bar_sink()
	{
		static std::vector<WTSBarStruct> s_bars;
		return s_bars;
	}

	std::vector<WTSTickStruct>& tick_sink()
	{
		static std::vector<WTSTickStruct> s_ticks;
		return s_ticks;
	}

	uint32_t& count_sink()
	{
		static uint32_t s_cnt = 0;
		return s_cnt;
	}

	uint32_t& data_callback_calls()
	{
		static uint32_t s_calls = 0;
		return s_calls;
	}

	void log_cb(WtString message)
	{
		log_sink().emplace_back(message == NULL ? std::string("<null>") : std::string(message));
	}

	void bars_cb(WTSBarStruct* bar, WtUInt32 count, bool isLast)
	{
		if (bar != NULL && count > 0)
			bar_sink().assign(bar, bar + count);
		(void)isLast;
	}

	void ticks_cb(WTSTickStruct* tick, WtUInt32 count, bool isLast)
	{
		if (tick != NULL && count > 0)
			tick_sink().assign(tick, tick + count);
		(void)isLast;
	}

	void order_details_cb(WTSOrdDtlStruct*, WtUInt32, bool)
	{
		++data_callback_calls();
	}

	void order_queues_cb(WTSOrdQueStruct*, WtUInt32, bool)
	{
		++data_callback_calls();
	}

	void transactions_cb(WTSTransStruct*, WtUInt32, bool)
	{
		++data_callback_calls();
	}

	void count_cb(WtUInt32 dataCnt)
	{
		count_sink() = dataCnt;
	}

	bool any_log_has(const char* key)
	{
		for (const auto& s : log_sink())
		{
			if (s.find(key) != std::string::npos)
				return true;
		}
		return false;
	}

	bool any_log_has_file_read_error()
	{
		for (const auto& message : log_sink())
		{
			if (message.find("读取数据文件") != std::string::npos && message.find("失败:") != std::string::npos)
				return true;
		}
		return false;
	}

	WTSBarStruct make_day_bar(uint32_t date, double price)
	{
		WTSBarStruct bar;
		memset(&bar, 0, sizeof(WTSBarStruct));
		bar.date = date;
		bar.time = 0;
		bar.open = price;
		bar.high = price + 1;
		bar.low = price - 1;
		bar.close = price + 0.5;
		bar.settle = price;
		bar.vol = price * 10;
		bar.money = price * 100;
		bar.hold = price * 5;
		bar.add = 1;
		return bar;
	}

	class ScopedFileSizeLimit
	{
	public:
		ScopedFileSizeLimit()
		{
			if (::getrlimit(RLIMIT_FSIZE, &_oldLimit) != 0 ||
				(_oldLimit.rlim_max != RLIM_INFINITY && _oldLimit.rlim_max < 1))
				return;

			struct sigaction ignoreAction = {};
			ignoreAction.sa_handler = SIG_IGN;
			sigemptyset(&ignoreAction.sa_mask);
			if (::sigaction(SIGXFSZ, &ignoreAction, &_oldAction) != 0)
				return;
			_signalChanged = true;

			struct rlimit limited = _oldLimit;
			limited.rlim_cur = 1;
			if (::setrlimit(RLIMIT_FSIZE, &limited) != 0)
			{
				::sigaction(SIGXFSZ, &_oldAction, NULL);
				_signalChanged = false;
				return;
			}
			_active = true;
		}

		~ScopedFileSizeLimit()
		{
			if (_active)
				::setrlimit(RLIMIT_FSIZE, &_oldLimit);
			if (_signalChanged)
				::sigaction(SIGXFSZ, &_oldAction, NULL);
		}

		bool active() const { return _active; }

	private:
		struct rlimit _oldLimit = {};
		struct sigaction _oldAction = {};
		bool _active = false;
		bool _signalChanged = false;
	};

} // namespace

class L05HelperBarTest : public ::testing::Test
{
protected:
	std::filesystem::path _root;

	void SetUp() override
	{
		static int s_seq = 0;
		_root = std::filesystem::temp_directory_path()
			/ ("l05_helper_bar_" + std::to_string(::getpid()) + "_" + std::to_string(++s_seq));
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
		std::filesystem::create_directories(_root, ec);
		log_sink().clear();
		bar_sink().clear();
		tick_sink().clear();
		count_sink() = 0;
		data_callback_calls() = 0;
		ASSERT_FALSE(ec);
	}

	void TearDown() override
	{
		std::error_code ec;
		std::filesystem::remove_all(_root, ec);
	}

	std::string file(const std::string& name) const
	{
		return (_root / name).string();
	}
};

TEST_F(L05HelperBarTest, StoreBarsRejectsZeroCount)
{
	EXPECT_FALSE(store_bars(file("none.dsb").c_str(), NULL, 0, "d", log_cb));
	EXPECT_TRUE(any_log_has("0"));
	// 未注册日志回调时同样只返回 false，不崩溃
	EXPECT_FALSE(store_bars(file("none.dsb").c_str(), NULL, 0, "d", NULL));
	EXPECT_FALSE(std::filesystem::exists(_root / "none.dsb"));
}

TEST_F(L05HelperBarTest, StoreBarsRejectsUnknownPeriod)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	EXPECT_FALSE(store_bars(file("bad.dsb").c_str(), bars.data(), (int)bars.size(), "w1", log_cb));
	EXPECT_TRUE(any_log_has("m1"));
	EXPECT_FALSE(std::filesystem::exists(_root / "bad.dsb"));
}

TEST_F(L05HelperBarTest, StoreAndReadBarsRoundtrip)
{
	const char* periods[] = { "d", "m1", "m5" };
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 5; i++)
		bars.push_back(make_day_bar(20200101 + i, 100 + i));

	for (const char* period : periods)
	{
		const std::string name = std::string("rb2001_") + period + ".dsb";
		ASSERT_TRUE(store_bars(file(name).c_str(), bars.data(), (int)bars.size(), period, NULL));
		ASSERT_TRUE(std::filesystem::exists(_root / name));

		bar_sink().clear();
		count_sink() = 0;
		WtUInt32 reaCnt = read_dsb_bars(file(name).c_str(), bars_cb, count_cb, log_cb);
		EXPECT_EQ(reaCnt, (WtUInt32)bars.size());
		EXPECT_EQ(count_sink(), (uint32_t)bars.size());
		ASSERT_EQ(bar_sink().size(), bars.size());
		for (size_t i = 0; i < bars.size(); i++)
		{
			EXPECT_DOUBLE_EQ(bar_sink()[i].open, bars[i].open);
			EXPECT_DOUBLE_EQ(bar_sink()[i].close, bars[i].close);
			EXPECT_DOUBLE_EQ(bar_sink()[i].vol, bars[i].vol);
			EXPECT_EQ(bar_sink()[i].date, bars[i].date);
		}
		EXPECT_TRUE(any_log_has("read") || !log_sink().empty());
	}
}

TEST_F(L05HelperBarTest, StoreBarsReportsMissingParentDirectoryFailure)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	const std::filesystem::path missing_dir = _root / "no_such_dir";
	ASSERT_FALSE(std::filesystem::exists(missing_dir));
	const std::filesystem::path output = missing_dir / "x.dsb";
	log_sink().clear();
	EXPECT_FALSE(store_bars(output.string().c_str(), bars.data(), (int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("创建失败"));
	EXPECT_TRUE(any_log_has(output.string().c_str()));
	EXPECT_FALSE(any_log_has("K线数据写入文件成功"));
	EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(L05HelperBarTest, StoreBarsLongPathCreateFailureKeepsDiagnosticAndReturnsFalse)
{
	std::filesystem::path longParent = _root;
	for (int i = 0; i < 4; ++i)
	{
		longParent /= std::string(100, static_cast<char>('a' + i));
		std::filesystem::create_directories(longParent);
	}
	const std::filesystem::path blockerPath = longParent / "not_a_directory";
	{
		std::ofstream blocker(blockerPath);
		ASSERT_TRUE(blocker.is_open());
		blocker << "blocked";
	}

	const std::string output = (blockerPath / "output.dsb").string();
	ASSERT_GT(output.size(), 400U);
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	log_sink().clear();
	EXPECT_FALSE(store_bars(output.c_str(), bars.data(), (int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("创建失败"));
	EXPECT_TRUE(any_log_has(output.c_str()));
	EXPECT_FALSE(any_log_has("K线数据写入文件成功"));
	EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(L05HelperBarTest, StoreBarsReportsReadOnlyFileFailure)
{
	if (::geteuid() == 0)
		GTEST_SKIP() << "root can bypass file mode write permissions";

	const std::string output = file("read_only.dsb");
	{
		std::ofstream seed(output, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(seed.is_open());
		seed << "preserve";
	}
	std::error_code ec;
	std::filesystem::permissions(output, std::filesystem::perms::owner_read,
		std::filesystem::perm_options::replace, ec);
	ASSERT_FALSE(ec);

	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	log_sink().clear();
	EXPECT_FALSE(store_bars(output.c_str(), bars.data(), (int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("创建失败"));
	EXPECT_FALSE(any_log_has("K线数据写入文件成功"));
	std::ifstream input(output, std::ios::binary);
	std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
	EXPECT_EQ(saved, "preserve");
}

TEST_F(L05HelperBarTest, StoreBarsReportsWriteFailure)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	ScopedFileSizeLimit fileLimit;
	ASSERT_TRUE(fileLimit.active());
	log_sink().clear();

	EXPECT_FALSE(store_bars(file("write_failure.dsb").c_str(), bars.data(),
		(int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("写入失败"));
	EXPECT_FALSE(any_log_has("K线数据写入文件成功"));
}

TEST_F(L05HelperBarTest, CheckedCloseIsIdempotentAfterClosingRealFile)
{
	BoostFile fileHandle;
	EXPECT_TRUE(fileHandle.close_file_checked());
	ASSERT_TRUE(fileHandle.create_new_file(file("checked_close.bin").c_str()));
	ASSERT_TRUE(fileHandle.write_file(std::string("abc")));
	EXPECT_TRUE(fileHandle.close_file_checked());
	EXPECT_FALSE(fileHandle.valid());
	EXPECT_TRUE(fileHandle.close_file_checked());
	std::ifstream input(file("checked_close.bin"), std::ios::binary);
	EXPECT_EQ(std::string((std::istreambuf_iterator<char>(input)),
		std::istreambuf_iterator<char>()), "abc");
}

TEST_F(L05HelperBarTest, CheckedCloseReportsNativeFailureWithoutSecondClose)
{
	const std::string path = file("closed_externally.bin");
	BoostFile fileHandle;
	ASSERT_TRUE(fileHandle.create_new_file(path.c_str()));
	struct stat expected = {};
	ASSERT_EQ(0, ::stat(path.c_str(), &expected));
	int ownedFd = -1;
	const int maxFd = ::getdtablesize();
	for (int fd = 0; fd < maxFd; ++fd)
	{
		struct stat actual = {};
		if (::fstat(fd, &actual) == 0 && actual.st_dev == expected.st_dev &&
			actual.st_ino == expected.st_ino)
		{
			ownedFd = fd;
			break;
		}
	}
	ASSERT_GE(ownedFd, 0);
	ASSERT_EQ(0, ::close(ownedFd));
	// Native close now returns EBADF. The wrapper must report failure and
	// relinquish ownership so destruction cannot close a reused descriptor.
	EXPECT_FALSE(fileHandle.close_file_checked());
	EXPECT_FALSE(fileHandle.valid());
	const int newFd = ::open(path.c_str(), O_RDONLY);
	ASSERT_GE(newFd, 0);
	EXPECT_TRUE(fileHandle.close_file_checked());
	EXPECT_NE(-1, ::fcntl(newFd, F_GETFD));
	EXPECT_EQ(0, ::close(newFd));
}

#ifdef WT_DT_HELPER_CLOSE_TEST_HOOKS
TEST_F(L05HelperBarTest, StoreBarsReportsInjectedCloseFailureWithoutSuccess)
{
	std::vector<WTSBarStruct> bars{ make_day_bar(20200101, 100) };
	const std::string failed = file("close_failure.dsb");
	wt_dt_helper_fail_next_close_for_test();
	EXPECT_FALSE(store_bars(failed.c_str(), bars.data(), (int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("关闭失败"));
	EXPECT_FALSE(any_log_has("K线数据写入文件成功"));
	EXPECT_TRUE(std::filesystem::exists(failed));

	// The injected result is one-shot and did not prevent the native close.
	log_sink().clear();
	const std::string success = file("close_success.dsb");
	ASSERT_TRUE(store_bars(success.c_str(), bars.data(), (int)bars.size(), "d", log_cb));
	EXPECT_TRUE(any_log_has("K线数据写入文件成功"));
	EXPECT_EQ(read_dsb_bars(success.c_str(), bars_cb, count_cb, log_cb), 1u);
	ASSERT_EQ(bar_sink().size(), 1u);
	EXPECT_DOUBLE_EQ(bar_sink().front().close, bars.front().close);
}
#endif

TEST_F(L05HelperBarTest, ReadBarsHeaderValidation)
{
	// 文件过小：直接返回 0，不调用计数回调
	{
		std::ofstream ofs(file("tiny.dsb"), std::ios::binary | std::ios::trunc);
		ofs << "abc";
	}
	count_sink() = 12345;
	bar_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("tiny.dsb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("头部校验失败"));

	// 文件不存在是读取错误，和可读但过短的文件头校验错误分开记录。
	log_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("not_exists.dsb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_TRUE(any_log_has_file_read_error());
	EXPECT_TRUE(any_log_has("not_exists.dsb"));
	EXPECT_FALSE(any_log_has("头部校验失败"));

	// 不传日志回调也不能崩
	log_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("not_exists.dsb").c_str(), bars_cb, count_cb, NULL), (WtUInt32)0);
}

TEST_F(L05HelperBarTest, ReadDsbRejectsMalformedCompressedBlocks)
{
	auto write_file = [](const std::string& path, const std::string& content) -> bool
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		if (!ofs.is_open())
			return false;
		ofs.write(content.data(), (std::streamsize)content.size());
		return ofs.good();
	};
	auto make_compressed_header = [](std::uint64_t compressedSize)
	{
		BlockHeaderV2 header;
		memset(&header, 0, sizeof(header));
		memcpy(header._blk_flag, BLK_FLAG, FLAG_SIZE);
		header._type = BT_HIS_Day;
		header._version = BLOCK_VERSION_CMP_V2;
		header._size = compressedSize;
		return header;
	};

	count_sink() = 12345;
	bar_sink().clear();

	// Legacy base header with a compressed version is truncated before the
	// extended header's _size field. The reader must reject it before decoding.
	BlockHeader shortHeader;
	memset(&shortHeader, 0, sizeof(shortHeader));
	memcpy(shortHeader._blk_flag, BLK_FLAG, FLAG_SIZE);
	shortHeader._type = BT_HIS_Day;
	shortHeader._version = BLOCK_VERSION_CMP_V2;
	ASSERT_TRUE(write_file(file("compressed_short_header.dsb"),
		std::string((const char*)&shortHeader, sizeof(shortHeader))));

	EXPECT_EQ(read_dsb_bars(file("compressed_short_header.dsb").c_str(), bars_cb, count_cb, log_cb), 0u);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("数据块校验失败"));

	// The compressed byte count must exactly match the bytes following V2 header.
	log_sink().clear();
	BlockHeaderV2 mismatchedHeader = make_compressed_header(2);
	std::string mismatched((const char*)&mismatchedHeader, sizeof(mismatchedHeader));
	mismatched.push_back('x');
	ASSERT_TRUE(write_file(file("compressed_size_mismatch.dsb"), mismatched));
	EXPECT_EQ(read_dsb_bars(file("compressed_size_mismatch.dsb").c_str(), bars_cb, count_cb, log_cb), 0u);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("数据块校验失败"));

	// Correct framing with a non-Zstandard payload must fail without leaking
	// the decompressor exception through the public reader.
	log_sink().clear();
	BlockHeaderV2 invalidFrameHeader = make_compressed_header(4);
	std::string invalidFrame((const char*)&invalidFrameHeader, sizeof(invalidFrameHeader));
	invalidFrame.append("bad!", 4);
	ASSERT_TRUE(write_file(file("compressed_invalid_frame.dsb"), invalidFrame));
	EXPECT_EQ(read_dsb_bars(file("compressed_invalid_frame.dsb").c_str(), bars_cb, count_cb, log_cb), 0u);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("数据块校验失败"));

	// Keep a valid frame header with its declared content size, but truncate the
	// compressed body. The frame-size precheck passes and ZSTD_decompress fails.
	log_sink().clear();
	WTSBarStruct bar = make_day_bar(20240105, 105.0);
	const std::string rawBar((const char*)&bar, sizeof(bar));
	std::string truncatedBody = WTSCmpHelper::compress_data(rawBar.data(), rawBar.size());
	ASSERT_GT(truncatedBody.size(), 1u);
	truncatedBody.pop_back();
	ASSERT_EQ(ZSTD_getFrameContentSize(truncatedBody.data(), truncatedBody.size()), sizeof(bar));
	std::string decompressed(sizeof(bar), '\0');
	EXPECT_TRUE(ZSTD_isError(ZSTD_decompress(decompressed.data(), decompressed.size(),
		truncatedBody.data(), truncatedBody.size())));
	BlockHeaderV2 truncatedHeader = make_compressed_header(truncatedBody.size());
	std::string truncatedFrame((const char*)&truncatedHeader, sizeof(truncatedHeader));
	truncatedFrame.append(truncatedBody);
	ASSERT_TRUE(write_file(file("compressed_truncated_body.dsb"), truncatedFrame));
	WtUInt32 readCount = 12345;
	EXPECT_NO_THROW(readCount = read_dsb_bars(file("compressed_truncated_body.dsb").c_str(),
		bars_cb, count_cb, log_cb));
	EXPECT_EQ(readCount, 0u);
	EXPECT_EQ(count_sink(), 12345u);
	EXPECT_TRUE(bar_sink().empty());
	EXPECT_TRUE(any_log_has("数据块校验失败"));
}

TEST_F(L05HelperBarTest, ReadDsbPreservesRawV2AndLegacyRawBars)
{
	BlockHeader rawV2Header;
	memset(&rawV2Header, 0, sizeof(rawV2Header));
	memcpy(rawV2Header._blk_flag, BLK_FLAG, FLAG_SIZE);
	rawV2Header._type = BT_HIS_Day;
	rawV2Header._version = BLOCK_VERSION_RAW_V2;
	WTSBarStruct rawBar = make_day_bar(20240102, 100.0);
	std::string rawV2((const char*)&rawV2Header, sizeof(rawV2Header));
	rawV2.append((const char*)&rawBar, sizeof(rawBar));
	{
		std::ofstream ofs(file("raw_v2.dsb"), std::ios::binary | std::ios::trunc);
		ofs.write(rawV2.data(), (std::streamsize)rawV2.size());
	}

	bar_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("raw_v2.dsb").c_str(), bars_cb, count_cb, log_cb), 1u);
	ASSERT_EQ(bar_sink().size(), 1u);
	EXPECT_EQ(bar_sink()[0].date, 20240102u);
	EXPECT_DOUBLE_EQ(bar_sink()[0].close, rawBar.close);

	BlockHeader legacyHeader;
	memset(&legacyHeader, 0, sizeof(legacyHeader));
	memcpy(legacyHeader._blk_flag, BLK_FLAG, FLAG_SIZE);
	legacyHeader._type = BT_HIS_Day;
	legacyHeader._version = BLOCK_VERSION_RAW;
	WTSBarStructOld legacyBar;
	legacyBar.date = 20240103;
	legacyBar.time = 930;
	legacyBar.open = 101;
	legacyBar.high = 103;
	legacyBar.low = 100;
	legacyBar.close = 102;
	legacyBar.settle = 102;
	legacyBar.money = 1020;
	legacyBar.vol = 10;
	legacyBar.hold = 5;
	legacyBar.add = 1;
	std::string legacyRaw((const char*)&legacyHeader, sizeof(legacyHeader));
	legacyRaw.append((const char*)&legacyBar, sizeof(legacyBar));
	{
		std::ofstream ofs(file("legacy_raw.dsb"), std::ios::binary | std::ios::trunc);
		ofs.write(legacyRaw.data(), (std::streamsize)legacyRaw.size());
	}

	bar_sink().clear();
	EXPECT_EQ(read_dsb_bars(file("legacy_raw.dsb").c_str(), bars_cb, count_cb, log_cb), 1u);
	ASSERT_EQ(bar_sink().size(), 1u);
	EXPECT_EQ(bar_sink()[0].date, 20240103u);
	EXPECT_DOUBLE_EQ(bar_sink()[0].close, legacyBar.close);
	EXPECT_DOUBLE_EQ(bar_sink()[0].vol, legacyBar.vol);
}

TEST_F(L05HelperBarTest, ReadFileErrorsAreCaughtAndShortHeadersRemainDistinct)
{
	count_sink() = 12345;
	log_sink().clear();
	EXPECT_EQ(read_dsb_ticks(file("missing_ticks.dsb").c_str(), ticks_cb, count_cb, log_cb), 0u);
	EXPECT_TRUE(any_log_has("missing_ticks.dsb"));
	EXPECT_TRUE(any_log_has_file_read_error());
	EXPECT_FALSE(any_log_has("头部校验失败"));
	EXPECT_EQ(count_sink(), 12345u);

	log_sink().clear();
	EXPECT_EQ(read_dmb_bars(file("missing_bars.dmb").c_str(), bars_cb, count_cb, log_cb), 0u);
	EXPECT_TRUE(any_log_has("missing_bars.dmb"));
	EXPECT_TRUE(any_log_has_file_read_error());
	EXPECT_FALSE(any_log_has("头部校验失败"));
	EXPECT_EQ(count_sink(), 12345u);

	std::ofstream(file("short.dsb"), std::ios::binary | std::ios::trunc) << "x";
	log_sink().clear();
	EXPECT_EQ(read_dsb_ticks(file("short.dsb").c_str(), ticks_cb, count_cb, log_cb), 0u);
	EXPECT_TRUE(any_log_has("头部校验失败"));
	EXPECT_FALSE(any_log_has_file_read_error());
	EXPECT_EQ(count_sink(), 12345u);

	std::ofstream(file("short.dmb"), std::ios::binary | std::ios::trunc) << "x";
	log_sink().clear();
	EXPECT_EQ(read_dmb_bars(file("short.dmb").c_str(), bars_cb, count_cb, log_cb), 0u);
	EXPECT_TRUE(any_log_has("头部校验失败"));
	EXPECT_FALSE(any_log_has_file_read_error());
	EXPECT_EQ(count_sink(), 12345u);

	// 错误日志不是必需依赖；无 logger 时，异常仍在导出边界内转换成 0。
	EXPECT_EQ(read_dsb_bars(file("missing_no_logger.dsb").c_str(), bars_cb, count_cb, NULL), 0u);
}

TEST_F(L05HelperBarTest, OrderDataReadersCatchFileErrorsAndKeepShortHeaderDiagnostics)
{
	const uint32_t sentinelCount = 12345;
	auto verify_read_error = [&](const std::string& path, auto reader, auto callback)
	{
		count_sink() = sentinelCount;
		data_callback_calls() = 0;
		log_sink().clear();
		WtUInt32 result = 99;
		EXPECT_NO_THROW(result = reader(path.c_str(), callback, count_cb, log_cb));
		EXPECT_EQ(result, 0u);
		EXPECT_EQ(count_sink(), sentinelCount);
		EXPECT_EQ(data_callback_calls(), 0u);
		EXPECT_TRUE(any_log_has(path.c_str()));
		EXPECT_TRUE(any_log_has_file_read_error());
		EXPECT_FALSE(any_log_has("头部校验失败"));
	};
	auto verify_short_header = [&](const std::string& path, auto reader, auto callback)
	{
		{
			std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
			ASSERT_TRUE(ofs.is_open());
			ofs << "x";
		}
		count_sink() = sentinelCount;
		data_callback_calls() = 0;
		log_sink().clear();
		WtUInt32 result = 99;
		EXPECT_NO_THROW(result = reader(path.c_str(), callback, count_cb, log_cb));
		EXPECT_EQ(result, 0u);
		EXPECT_EQ(count_sink(), sentinelCount);
		EXPECT_EQ(data_callback_calls(), 0u);
		EXPECT_TRUE(any_log_has("头部校验失败"));
		EXPECT_FALSE(any_log_has_file_read_error());
	};
	auto verify_unreadable_path = [&](const std::string& path, auto reader, auto callback)
	{
		std::error_code ec;
		ASSERT_TRUE(std::filesystem::create_directory(path, ec));
		ASSERT_FALSE(ec);
		count_sink() = sentinelCount;
		data_callback_calls() = 0;
		log_sink().clear();
		WtUInt32 result = 99;
		EXPECT_NO_THROW(result = reader(path.c_str(), callback, count_cb, log_cb));
		EXPECT_EQ(result, 0u);
		EXPECT_EQ(count_sink(), sentinelCount);
		EXPECT_EQ(data_callback_calls(), 0u);
		EXPECT_TRUE(any_log_has(path.c_str()));
		// Some platforms report directory reads as an I/O error; others expose
		// them as an empty read, which follows the distinct short-header path.
		EXPECT_TRUE(any_log_has_file_read_error() || any_log_has("头部校验失败"));
	};

	verify_read_error(file("missing_details.dsb"), read_dsb_order_details, order_details_cb);
	verify_read_error(file("missing_queues.dsb"), read_dsb_order_queues, order_queues_cb);
	verify_read_error(file("missing_transactions.dsb"), read_dsb_transactions, transactions_cb);

	verify_unreadable_path(file("unreadable_details.dsb"), read_dsb_order_details, order_details_cb);
	verify_unreadable_path(file("unreadable_queues.dsb"), read_dsb_order_queues, order_queues_cb);
	verify_unreadable_path(file("unreadable_transactions.dsb"), read_dsb_transactions, transactions_cb);

	verify_short_header(file("short_details.dsb"), read_dsb_order_details, order_details_cb);
	verify_short_header(file("short_queues.dsb"), read_dsb_order_queues, order_queues_cb);
	verify_short_header(file("short_transactions.dsb"), read_dsb_transactions, transactions_cb);
}

TEST_F(L05HelperBarTest, ResampleReadFailureAndShortHeaderHaveSeparateDiagnostics)
{
	const char* daySession = R"JSON({"offset": 0, "auction": {"from": 0, "to": 0},
		"sections": [{"from": 0, "to": 2359}]})JSON";
	EXPECT_EQ(resample_bars(file("missing_resample.dsb").c_str(), bars_cb, count_cb,
		20200101, 20200131, "d", 1, daySession, log_cb), 0u);
	EXPECT_TRUE(any_log_has("missing_resample.dsb"));
	EXPECT_TRUE(any_log_has_file_read_error());
	EXPECT_FALSE(any_log_has("头部校验失败"));

	std::ofstream(file("short_resample.dsb"), std::ios::binary | std::ios::trunc) << "x";
	log_sink().clear();
	EXPECT_EQ(resample_bars(file("short_resample.dsb").c_str(), bars_cb, count_cb,
		20200101, 20200131, "d", 1, daySession, log_cb), 0u);
	EXPECT_TRUE(any_log_has("头部校验失败"));
	EXPECT_FALSE(any_log_has_file_read_error());

	EXPECT_EQ(resample_bars(file("missing_resample_no_logger.dsb").c_str(), bars_cb, count_cb,
		20200101, 20200131, "d", 1, daySession, NULL), 0u);
}

TEST_F(L05HelperBarTest, DumpBarsMissingSourceFolder)
{
	dump_bars((_root / "nothing").string().c_str(), file("csv").c_str(), "", log_cb);
	EXPECT_TRUE(any_log_has("不存在"));
}

TEST_F(L05HelperBarTest, DumpBarsWritesCsvAndSkipsOthers)
{
	std::vector<WTSBarStruct> bars;
	for (uint32_t i = 0; i < 3; i++)
		bars.push_back(make_day_bar(20200101 + i, 3000 + i));
	ASSERT_TRUE(store_bars(file("SHFE.rb2010.dsb").c_str(), bars.data(), (int)bars.size(), "d", NULL));

	// tick 文件（块类型不属于 K 线）应被跳过
	std::string tick_blob(sizeof(HisTickBlock) + sizeof(WTSTickStruct), '\0');
	{
		HisTickBlock* header = (HisTickBlock*)&tick_blob[0];
		memcpy(header->_blk_flag, BLK_FLAG, FLAG_SIZE);
		header->_type = BT_HIS_Ticks;
		header->_version = BLOCK_VERSION_RAW_V2;
		std::ofstream ofs(file("tick_like.dsb"), std::ios::binary | std::ios::trunc);
		ofs.write(tick_blob.data(), (std::streamsize)tick_blob.size());
	}
	// 非法小文件 + 非 .dsb 文件 + 子目录
	{
		std::ofstream ofs(file("broken.dsb"), std::ios::binary | std::ios::trunc);
		ofs << "xy";
		ofs.close();
		std::ofstream secondBroken(file("second_broken.dsb"), std::ios::binary | std::ios::trunc);
		secondBroken << "short";
		std::ofstream ignore(file("ignore.txt"), std::ios::trunc);
		ignore << "whatever";
	}
	std::filesystem::create_directories(_root / "sub_dir");

	const std::string csvDir = file("csv_out");
	dump_bars(file("").c_str(), csvDir.c_str(), "", log_cb);
	// srcFolder 用 standardisePath 归一化，直接传根目录
	dump_bars(_root.string().c_str(), csvDir.c_str(), "", log_cb);
	const std::string brokenHeader = "文件" + file("broken.dsb") + "头部校验失败";
	const std::string secondBrokenHeader = "文件" + file("second_broken.dsb") + "头部校验失败";
	const std::string nonBarType = "文件" + file("tick_like.dsb") + "不是K线数据，跳过转换";
	EXPECT_TRUE(any_log_has(brokenHeader.c_str())) << brokenHeader;
	EXPECT_TRUE(any_log_has(secondBrokenHeader.c_str())) << secondBrokenHeader;
	EXPECT_TRUE(any_log_has(nonBarType.c_str())) << nonBarType;

	const std::string csv = (std::filesystem::path(csvDir) / "SHFE.rb2010.csv").string();
	ASSERT_TRUE(std::filesystem::exists(csv));
	std::ifstream ifs(csv);
	std::string line;
	ASSERT_TRUE(std::getline(ifs, line));
	EXPECT_EQ(line, "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest");
	int dataLines = 0;
	while (std::getline(ifs, line))
	{
		if (!line.empty())
			dataLines++;
	}
	EXPECT_EQ(dataLines, 3);
	EXPECT_EQ(line.find("3002"), std::string::npos);
	EXPECT_TRUE(any_log_has("导出完成"));
}

TEST_F(L05HelperBarTest, TransCsvBarsMissingSourceFolder)
{
	trans_csv_bars((_root / "no_csv").string().c_str(), file("bin").c_str(), "d", log_cb);
	EXPECT_FALSE(std::filesystem::exists(_root / "bin"));
	EXPECT_TRUE(log_sink().empty());
}

TEST_F(L05HelperBarTest, TransCsvBarsDailyRoundtrip)
{
	const std::string csvDir = file("csv_in");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "DCE.i2001.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ofs << "2020/01/16,0,1745,1752,1740,1748.5,1746,12000,21000000,35000,120" << std::endl;
		ofs << "20200117,0,1748,1760,1745,1755,1753,15000,26325000,36000,1000" << std::endl;
		std::ofstream ignore((std::filesystem::path(csvDir) / "notes.md").string());
		ignore << "ignored";
		std::filesystem::create_directories(std::filesystem::path(csvDir) / "nested");
	}

	const std::string binDir = file("bin_out");
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);

	const std::string dsb = (std::filesystem::path(binDir) / "DCE.i2001.dsb").string();
	ASSERT_TRUE(std::filesystem::exists(dsb));

	bar_sink().clear();
	count_sink() = 0;
	WtUInt32 reaCnt = read_dsb_bars(dsb.c_str(), bars_cb, count_cb, log_cb);
	EXPECT_EQ(reaCnt, (WtUInt32)2);
	ASSERT_EQ(bar_sink().size(), (size_t)2);
	EXPECT_EQ(bar_sink()[0].date, 20200116u);
	EXPECT_DOUBLE_EQ(bar_sink()[0].open, 1745);
	EXPECT_DOUBLE_EQ(bar_sink()[1].close, 1755);
	EXPECT_TRUE(any_log_has("转储至"));
}

TEST_F(L05HelperBarTest, TransCsvBarsRejectsMalformedDatesAndKeepsValidDates)
{
	const std::string csvDir = file("csv_dates");
	std::filesystem::create_directories(csvDir);
	const std::string csvPath = (std::filesystem::path(csvDir) / "dates.csv").string();
	{
		std::ofstream ofs(csvPath);
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest\n";
		ofs << "2020/01/16,0,100,101,99,100,100,10,1000,5,0\n";
		ofs << "2020-01-17 00:00:00,0,101,102,100,101,101,11,1111,6,1\n";
		ofs << "20200118,0,102,103,101,102,102,12,1224,7,1\n";
		ofs << "2020/01,0,103,104,102,103,103,13,1339,8,1\n";
		ofs << "2020//19,0,104,105,103,104,104,14,1456,9,1\n";
		ofs << "2020-02-30,0,105,106,104,105,105,15,1575,10,1\n";
		ofs << "2020/13/01,0,106,107,105,106,106,16,1696,11,1\n";
		ofs << "2000-02-29,0,107,108,106,107,107,17,1819,12,1\n";
		ofs << "1900-02-29,0,108,109,107,108,108,18,1944,13,1\n";
	}

	const std::string binDir = file("bin_dates");
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);
	EXPECT_TRUE(any_log_has("日期无效"));
	EXPECT_TRUE(any_log_has(csvPath.c_str()));
	EXPECT_TRUE(any_log_has("第5行"));

	bar_sink().clear();
	const std::string dsb = (std::filesystem::path(binDir) / "dates.dsb").string();
	ASSERT_TRUE(std::filesystem::exists(dsb));
	EXPECT_EQ(read_dsb_bars(dsb.c_str(), bars_cb, count_cb, log_cb), 4u);
	ASSERT_EQ(bar_sink().size(), 4u);
	EXPECT_EQ(bar_sink()[0].date, 20200116u);
	EXPECT_EQ(bar_sink()[1].date, 20200117u);
	EXPECT_EQ(bar_sink()[2].date, 20200118u);
	EXPECT_EQ(bar_sink()[3].date, 20000229u);
}

TEST_F(L05HelperBarTest, ReadDsbTicksRoundtrip)
{
	WTSTickStruct tick;
	memset(&tick, 0, sizeof(tick));
	tick.action_date = 20200116;
	tick.action_time = 93000000;
	ASSERT_TRUE(store_ticks(file("ticks.dsb").c_str(), &tick, 1, NULL));
	EXPECT_EQ(read_dsb_ticks(file("ticks.dsb").c_str(), ticks_cb, count_cb, log_cb), 1u);
	EXPECT_EQ(count_sink(), 1u);
	ASSERT_EQ(tick_sink().size(), 1u);
	EXPECT_EQ(tick_sink()[0].action_date, 20200116u);
}

TEST_F(L05HelperBarTest, TransCsvBarsMinuteAndUnknownPeriod)
{
	const std::string csvDir = file("csv_min");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "CFFEX.if2003.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest" << std::endl;
		ofs << "2020-03-02 00:00:00,09:30:00,3785,3786,3780,3782,0,120,453840,0,0" << std::endl;
		ofs << "2020-03-02,09:31:00,3782,3790,3782,3788,0,200,757800,0,0" << std::endl;
	}

	for (const char* period : { "m1", "m5", "week" })
	{
		const std::string binDir = file(std::string("bin_") + period);
		trans_csv_bars(csvDir.c_str(), binDir.c_str(), period, log_cb);
		const std::string dsb = (std::filesystem::path(binDir) / "CFFEX.if2003.dsb").string();
		ASSERT_TRUE(std::filesystem::exists(dsb)) << period;

		bar_sink().clear();
		count_sink() = 0;
		EXPECT_EQ(read_dsb_bars(dsb.c_str(), bars_cb, count_cb, NULL), (WtUInt32)2) << period;
		ASSERT_EQ(bar_sink().size(), (size_t)2) << period;
		EXPECT_EQ(bar_sink()[0].date, 20200302u) << period;
		if (strcmp(period, "week") != 0)
			EXPECT_NE(bar_sink()[0].time, 0u) << period;
		else
			EXPECT_EQ(bar_sink()[0].time, 0u) << period;
	}
}

TEST_F(L05HelperBarTest, TransCsvBarsUnreadableFileIsSkipped)
{
	const std::string csvDir = file("csv_bad");
	std::filesystem::create_directories(csvDir);
	std::filesystem::create_directories(std::filesystem::path(csvDir) / "empty.csv");

	const std::string binDir = file("bin_bad");
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);
	EXPECT_TRUE(log_sink().empty() || any_log_has("失败"));
	EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(binDir) / "empty.dsb"));
}

TEST_F(L05HelperBarTest, TransCsvBarsDoesNotReportWriteFailureAsSuccess)
{
	const std::string csvDir = file("csv_write_failure");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "one.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest\n";
		ofs << "20200116,0,100,101,99,100,100,10,1000,5,0\n";
	}

	const std::string binDir = file("bin_write_failure");
	std::filesystem::create_directories(binDir);

	ScopedFileSizeLimit fileLimit;
	ASSERT_TRUE(fileLimit.active());
	log_sink().clear();
	trans_csv_bars(csvDir.c_str(), binDir.c_str(), "d", log_cb);
	EXPECT_TRUE(any_log_has("写入失败"));
	EXPECT_TRUE(any_log_has("one.dsb"));
	EXPECT_FALSE(any_log_has("数据已转储至"));
}

TEST_F(L05HelperBarTest, TransCsvBarsDoesNotReportCreationFailureAsSuccess)
{
	const std::string csvDir = file("csv_create_failure");
	std::filesystem::create_directories(csvDir);
	{
		std::ofstream ofs((std::filesystem::path(csvDir) / "one.csv").string());
		ofs << "date,time,open,high,low,close,settle,volume,turnover,open_interest,diff_interest\n";
		ofs << "20200116,0,100,101,99,100,100,10,1000,5,0\n";
	}

	const std::string blockedPath = file("not_a_directory");
	{
		std::ofstream blocker(blockedPath);
		ASSERT_TRUE(blocker.is_open());
		blocker << "blocked";
	}

	log_sink().clear();
	trans_csv_bars(csvDir.c_str(), blockedPath.c_str(), "d", log_cb);
	EXPECT_TRUE(any_log_has("创建失败"));
	EXPECT_TRUE(any_log_has("one.dsb"));
	EXPECT_FALSE(any_log_has("数据已转储至"));

	std::filesystem::path longParent = _root;
	for (int i = 0; i < 4; ++i)
	{
		longParent /= std::string(100, static_cast<char>('a' + i));
		std::filesystem::create_directories(longParent);
	}
	const std::string longBlockedPath = (longParent / "not_a_directory").string();
	{
		std::ofstream blocker(longBlockedPath);
		ASSERT_TRUE(blocker.is_open());
		blocker << "blocked";
	}

	const std::string blockedChild = (std::filesystem::path(longBlockedPath) / "output").string();
	ASSERT_GT(blockedChild.size(), 400U);
	log_sink().clear();
	trans_csv_bars(csvDir.c_str(), blockedChild.c_str(), "d", log_cb);
	EXPECT_TRUE(any_log_has("创建输出目录"));
	EXPECT_TRUE(any_log_has(blockedChild.c_str()));
	EXPECT_FALSE(any_log_has("数据已转储至"));
}

TEST_F(L05HelperBarTest, ReadDmbBarsValidationPaths)
{
	{
		std::ofstream ofs(file("tiny.dmb"), std::ios::binary | std::ios::trunc);
		ofs << "123";
	}
	log_sink().clear();
	count_sink() = 7;
	EXPECT_EQ(read_dmb_bars(file("tiny.dmb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)0);
	EXPECT_TRUE(any_log_has("头部校验失败"));
	EXPECT_EQ(count_sink(), 7u);

	// 合法头部但 _size 为 0
	std::string blob(sizeof(RTKlineBlock), '\0');
	((RTKlineBlock*)&blob[0])->_size = 0;
	{
		std::ofstream ofs(file("zero.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	log_sink().clear();
	count_sink() = 0;
	EXPECT_EQ(read_dmb_bars(file("zero.dmb").c_str(), bars_cb, count_cb, NULL), (WtUInt32)0);
	EXPECT_EQ(count_sink(), 0u);

	// 头部 + 2 条 bar
	blob.resize(sizeof(RTKlineBlock) + 2 * sizeof(WTSBarStruct));
	RTKlineBlock* block = (RTKlineBlock*)&blob[0];
	block->_size = 2;
	WTSBarStruct* outBars = block->_bars;
	outBars[0] = make_day_bar(20200102, 100);
	outBars[1] = make_day_bar(20200103, 101);
	{
		std::ofstream ofs(file("two.dmb"), std::ios::binary | std::ios::trunc);
		ofs.write(blob.data(), (std::streamsize)blob.size());
	}
	bar_sink().clear();
	count_sink() = 0;
	EXPECT_EQ(read_dmb_bars(file("two.dmb").c_str(), bars_cb, count_cb, log_cb), (WtUInt32)2);
	EXPECT_EQ(count_sink(), 2u);
	ASSERT_EQ(bar_sink().size(), (size_t)2);
	EXPECT_EQ(bar_sink()[1].date, 20200103u);
	EXPECT_TRUE(any_log_has("读取完成"));
}
