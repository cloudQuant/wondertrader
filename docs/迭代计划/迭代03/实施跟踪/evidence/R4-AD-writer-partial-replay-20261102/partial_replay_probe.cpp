// Manual expected-red probes: all storage and dumpers are isolated fakes.
#include "r4_ad_fixture.h"
#include <atomic>
#include <fcntl.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct Counters { std::atomic<int> calls{0}; std::atomic<int> accepted{0}; };
class ThrowSecond : public IHisDataDumper {
public:
	explicit ThrowSecond(Counters& c) : c_(c) {}
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override {
		int n = ++c_.calls;
		if (n == 2) throw std::runtime_error("after first destination committed");
		++c_.accepted;
		return true;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
private: Counters& c_;
};
class AppendAndExit : public IHisDataDumper {
public:
	explicit AppendAndExit(std::string path, bool die) : path_(std::move(path)), die_(die) {}
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override {
		int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0 || ::write(fd, "x", 1) != 1 || ::fsync(fd) != 0) _exit(90);
		::close(fd);
		if (die_) _exit(86);
		return true;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
private: std::string path_; bool die_;
};
class Reenter : public IHisDataDumper {
public:
	Reenter(WtDataWriterAD* writer, WTSContractInfo* ct) : writer_(writer), ct_(ct) {}
	bool dumpHisBars(const char*, const char*, WTSBarStruct*, uint32_t) override {
		auto value = make_tick("REENTER", "rb01", 20240108, 20240108, 93200000, 102, 1, 10, 2, 1);
		auto* tick = WTSTickData::create(value);
		tick->setContractInfo(ct_);
		bool result = writer_->writeTick(tick, 0);
		tick->release();
		return result;
	}
	bool dumpHisTicks(const char*, uint32_t, WTSTickStruct*, uint32_t) override { return true; }
private: WtDataWriterAD* writer_; WTSContractInfo* ct_;
};

TEST_F(R4WriterADTest, ManualSyncPartialSuccessRetryDuplicatesCommittedDestination)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* comm = _bd->add_commodity("MULTI", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("MULTI", "rb01", comm);
	Counters counters; ThrowSecond one(counters), two(counters);
	_writer->add_ext_dumper("one", &one); _writer->add_ext_dumper("two", &two);
	auto make = [&](unsigned minute) { return tick_of(ct, make_tick("MULTI", "rb01", 20240108, 20240108, minute * 100000, 100 + minute - 930, 1, 10, 2, 1)); };
	write_or_fail(make(930), 0);
	WTSTickData* tick = make(931);
	EXPECT_THROW(_writer->writeTick(tick, 0), std::runtime_error);
	tick->release();
	ASSERT_EQ(counters.accepted.load(), 1);
	write_or_fail(make(931), 0);
	EXPECT_EQ(counters.accepted.load(), 2) << "one destination accepted before the exception; retry must only deliver to the other";
}

TEST_F(R4WriterADTest, ManualAsyncAcceptedEnqueueStillDuplicatesOnRetry)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true, true));
	auto* comm = _bd->add_commodity("ASYNC", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("ASYNC", "rb01", comm);
	Counters counters; ThrowSecond one(counters), two(counters);
	_writer->add_ext_dumper("one", &one); _writer->add_ext_dumper("two", &two);
	auto make = [&](unsigned minute) { return tick_of(ct, make_tick("ASYNC", "rb01", 20240108, 20240108, minute * 100000, 100 + minute - 930, 1, 10, 2, 1)); };
	write_or_fail(make(930), 0); write_or_fail(make(931), 0); write_or_fail(make(931), 0);
	_writer->release(); // joins worker before observing fake state or destroying dumpers
	ASSERT_EQ(counters.calls.load(), 4);
	EXPECT_EQ(counters.accepted.load(), 2) << "async enqueue succeeds although the first attempt partially committed";
}

TEST_F(R4WriterADTest, ManualCrashAfterExternalEffectCanDuplicateOnRestart)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* comm = _bd->add_commodity("CRASH", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("CRASH", "rb01", comm);
	auto make = [&](unsigned minute) { return tick_of(ct, make_tick("CRASH", "rb01", 20240108, 20240108, minute * 100000, 100 + minute - 930, 1, 10, 2, 1)); };
	write_or_fail(make(930), 0);
	const auto output = (_root / "external-destination").string();
	AppendAndExit crash(output, true);
	_writer->add_ext_dumper("out", &crash);
	pid_t child = ::fork(); ASSERT_GE(child, 0);
	if (child == 0) { auto* tick = make(931); _writer->writeTick(tick, 0); _exit(91); }
	int status = 0; ASSERT_EQ(::waitpid(child, &status, 0), child);
	ASSERT_TRUE(WIFEXITED(status)); ASSERT_EQ(WEXITSTATUS(status), 86);
	_writer->release(); _writer.reset();
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	AppendAndExit recovered(output, false); _writer->add_ext_dumper("out", &recovered);
	write_or_fail(make(931), 0);
	EXPECT_EQ(std::filesystem::file_size(output), 1u) << "the external effect committed before the process died";
}

TEST_F(R4WriterADTest, ManualSyncDumperReentryCanDeadlock)
{
	init_writer(make_writer_cfg(_root.string(), 1000, true, false, true, true));
	auto* comm = _bd->add_commodity("REENTER", "rb", ContractCategory::CC_Future);
	auto* ct = _bd->add_contract("REENTER", "rb01", comm);
	auto make = [&](unsigned minute) { return tick_of(ct, make_tick("REENTER", "rb01", 20240108, 20240108, minute * 100000, 100 + minute - 930, 1, 10, 2, 1)); };
	write_or_fail(make(930), 0);
	Reenter dumper(_writer.get(), ct); _writer->add_ext_dumper("reentrant", &dumper);
	pid_t child = ::fork(); ASSERT_GE(child, 0);
	if (child == 0) { alarm(2); auto* tick = make(931); _writer->writeTick(tick, 0); _exit(0); }
	int status = 0; ASSERT_EQ(::waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "child timed out while the callback reentered the writer; raw wait status " << status;
}
} // namespace
