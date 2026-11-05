// I03-NEW-038（迭代03 最终缺口，slice2）：WtDataWriter 的 pipe_writer_log / defer_writer_log
// 原以 fmtutil::format 把消息无界写入 512 字节 thread_local 缓冲；消息带配置根目录与文件路径，
// 长根目录会写穿缓冲。改后与 WtDataWriterAD、各读端相同：fmt::format_to_n 有界写入并加截断标记。
// ASan 不插桩 thread_local 缓冲，因此以 sink 收到的消息长度作为可观察 oracle（与
// S5ReaderADTest.LongConfiguredPathLogIsBoundedToTheMessageBuffer 相同手法）。
// 同时以长根目录写入一笔 tick，驱动 RT tick/min1/min5 块的创建：K 线块路径原写入 256 字节
// thread_local 缓冲，改后按字符串拼接，块文件必须落在完整的长路径下。
#include "r23_writer_fixture.h"

namespace i03new038w {

using namespace r23;
using std::string;

class CapturingWriterSink : public WriterSink
{
public:
	using WriterSink::WriterSink;
	void outputLog(WTSLogLevel ll, const char* message) override
	{
		WriterSink::outputLog(ll, message);
		_messages.emplace_back(message == NULL ? "" : message);
	}
	std::vector<string> _messages;
};

class I03New038WriterLogTest : public R23WriterTest
{
protected:
	// 约 620 字节的根目录（每段 200 字节，未超过单段 255 的文件系统上限）
	void use_long_root()
	{
		std::filesystem::path longDir = _dir / string(200, 'a') / string(200, 'b') / string(200, 'c');
		std::error_code ec;
		std::filesystem::create_directories(longDir, ec);
		ASSERT_FALSE(ec) << ec.message();
		_base = longDir.string() + "/";
		ASSERT_GT(_base.size(), 600u);

		_capture = new CapturingWriterSink(_bd.get());
		_sink.reset(_capture);
		_sink->add_comm("SHFE.rb");
	}

	CapturingWriterSink* _capture = NULL;
};

TEST_F(I03New038WriterLogTest, LongRootInitAndBlockLogsStayInsideTheMessageBuffer)
{
	use_long_root();
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);

	TickSpec spec;
	write_tick(w, spec, 0);

	ASSERT_FALSE(_capture->_messages.empty());
	bool sawTruncatedInit = false;
	bool sawTruncatedBlockLog = false;
	for (const string& message : _capture->_messages)
	{
		EXPECT_LT(message.size(), 512u) << "log message escaped the 512-byte buffer: " << message.size();
		const bool truncated = message.find("...[truncated]") != string::npos;
		if (truncated && message.rfind("WtDataWriter initialized, root dir: ", 0) == 0)
			sawTruncatedInit = true;
		if (truncated && message.find(" not exists, initializing...") == string::npos
			&& message.rfind("Data file ", 0) == 0)
			sawTruncatedBlockLog = true;
	}
	EXPECT_TRUE(sawTruncatedInit) << "init diagnostic with the long root must be cut and marked";
	EXPECT_TRUE(sawTruncatedBlockLog) << "deferred block-creation diagnostic must be cut and marked";

	// 块文件按完整长路径创建（K 线块路径不再经 256 字节缓冲）
	EXPECT_TRUE(file_exists(rt_tick_file("rb2210")));
	EXPECT_TRUE(file_exists(rt_min1_file("rb2210")));
	EXPECT_TRUE(file_exists(rt_min5_file("rb2210")));
}

TEST_F(I03New038WriterLogTest, ShortMessagesAreUnchanged)
{
	R23Writer* w = make_writer();
	ASSERT_TRUE(w != NULL);
	ASSERT_TRUE(_sink->has_log("WtDataWriter initialized, root dir: ", LL_INFO));
	// 正控：短根目录的完整诊断不被截断
	const int idx = _sink->find_log("WtDataWriter initialized, root dir: ", LL_INFO);
	ASSERT_GE(idx, 0);
	EXPECT_EQ(string::npos, _sink->last_log().find("...[truncated]"));
	EXPECT_TRUE(_sink->has_log(_base.c_str(), LL_INFO)) << "short root must appear in full";
}

} // namespace i03new038w
