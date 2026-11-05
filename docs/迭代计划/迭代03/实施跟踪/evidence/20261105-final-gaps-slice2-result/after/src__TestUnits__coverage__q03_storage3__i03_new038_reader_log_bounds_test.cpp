// I03-NEW-038（迭代03 最终缺口，slice2）：WtRdmDtReader / WtBtDtReader 的 pipe_*_log 已改为
// fmt::format_to_n 有界写入 512 字节 thread_local 缓冲并加截断标记，但此前只有静态检查。
// 本文件补运行用例：以超过 512 字节的配置路径驱动带路径的诊断，sink 收到的每条消息
// 必须短于缓冲且被标记截断（thread_local 缓冲不受 ASan 插桩，故以长度作 oracle，
// 与 S5ReaderADTest.LongConfiguredPathLogIsBoundedToTheMessageBuffer 相同手法）。
#include "q03_storage3_fixture.h"

#include "Includes/IBtDtReader.h"
#include "WtDataStorage/WtBtDtReader.h"
#include "WtDataStorage/WtRdmDtReader.h"

namespace i03new038r {

using namespace q03;
using std::string;

class AllLogsRdmSink : public IRdmDtReaderSink
{
public:
	AllLogsRdmSink(IBaseDataMgr* bd, IHotMgr* hot) : _bd(bd), _hot(hot) {}
	IBaseDataMgr* get_basedata_mgr() override { return _bd; }
	IHotMgr* get_hot_mgr() override { return _hot; }
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.emplace_back(ll, message == NULL ? string() : string(message));
	}
	std::vector<std::pair<WTSLogLevel, string>> _logs;

private:
	IBaseDataMgr* _bd;
	IHotMgr* _hot;
};

class AllLogsBtSink : public IBtDtReaderSink
{
public:
	void reader_log(WTSLogLevel ll, const char* message) override
	{
		_logs.emplace_back(ll, message == NULL ? string() : string(message));
	}
	std::vector<std::pair<WTSLogLevel, string>> _logs;
};

// 约 620 字节的路径（每段 200 字节）
static string long_path_under(const std::filesystem::path& root, const char* leaf)
{
	std::filesystem::path p = root / string(200, 'x') / string(200, 'y') / string(200, 'z');
	return (p / leaf).string();
}

class I03New038ReaderLogTest : public ::testing::Test
{
protected:
	void SetUp() override { _sandbox.reset(new Sandbox("n038r")); }
	void TearDown() override { _sandbox.reset(); }

	template<typename Logs>
	static void expect_bounded_and_marked(const Logs& logs, const char* prefix)
	{
		ASSERT_FALSE(logs.empty());
		bool sawMarked = false;
		for (const auto& item : logs)
		{
			EXPECT_LT(item.second.size(), 512u) << "log message escaped the 512-byte buffer: " << item.second.size();
			if (item.second.rfind(prefix, 0) == 0 && item.second.find("...[truncated]") != string::npos)
				sawMarked = true;
		}
		EXPECT_TRUE(sawMarked) << "long diagnostic starting with '" << prefix << "' must be cut and marked";
	}

	std::unique_ptr<Sandbox> _sandbox;
};

TEST_F(I03New038ReaderLogTest, RdmReaderLongAdjFactorPathDiagnosticIsBounded)
{
	BaseDataMgr bd;
	HotMgr hot;
	AllLogsRdmSink sink(&bd, &hot);
	const string adjfile = long_path_under(_sandbox->root_abs(), "adjfactors.json");
	ASSERT_GT(adjfile.size(), 600u);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _sandbox->root_abs().c_str());
	cfg->append("adjfactor", adjfile.c_str());	// 文件不存在：记 "Adjusting factors file {} not exists"
	{
		WtRdmDtReader reader;
		reader.init(cfg, &sink);
		cfg->release();
	}
	expect_bounded_and_marked(sink._logs, "Adjusting factors file ");
}

TEST_F(I03New038ReaderLogTest, BtReaderLongRootDiagnosticIsBounded)
{
	AllLogsBtSink sink;
	const string root = long_path_under(_sandbox->root_abs(), "store");
	ASSERT_GT(root.size(), 600u);

	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", root.c_str());	// init 记 "WtBtDtReader initialized, root data dir is {}"
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	expect_bounded_and_marked(sink._logs, "WtBtDtReader initialized, root data dir is ");
}

TEST_F(I03New038ReaderLogTest, BtReaderShortRootDiagnosticIsComplete)
{
	AllLogsBtSink sink;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("path", _sandbox->root_abs().c_str());
	WtBtDtReader reader;
	reader.init(cfg, &sink);
	cfg->release();
	ASSERT_FALSE(sink._logs.empty());
	const string& message = sink._logs.back().second;
	EXPECT_EQ(string::npos, message.find("...[truncated]"));
	EXPECT_NE(string::npos, message.find(StrUtil::standardisePath(_sandbox->root_abs())));
}

} // namespace i03new038r
