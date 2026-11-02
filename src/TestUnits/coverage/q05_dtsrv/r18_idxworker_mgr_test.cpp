/*!
 * \file r18_idxworker_mgr_test.cpp
 * \brief WtDtCore/IndexWorkerMgr.cpp 补充覆盖用例（迭代01 R18 lane）
 *
 * 与既有 q05_idxworker_mgr_test.cpp 不重叠：那份覆盖了
 * loadFactories 的「目录缺失/空目录/子目录/非 .dylib」、init 的「无 indice /
 * active=false / 名称无点号 / 工厂缺失 / poolsize=0」、handle_quote 的
 * 「NULL / 未订阅 / 已订阅同步」，本文件补的是：
 *  - loadFactories 里「扩展名是 .dylib 但 dlopen 失败」（L62-64）与
 *    「dlopen 成功但找不到 create_index_factory」（L66-71）两条真实扫描分支
 *  - init 的「indice 存在但不是数组」（L142 第二个子条件）与
 *    「indice 是空数组」（L147-148 循环 0 次 -> L167 return true）
 *  - handle_quote 同步分支的 retain/release 配对及 worker 抛异常时的清理
 *  - 用单 worker gate 确认异步任务排队/执行期间持有 tick，完成后回到调用者引用基线
 *  - output_log（L225）与 get_tick/push_tick/sub_ticks 的组合委托
 * dlopen 只用本机已存在的第三方 .dylib 做「找不到工厂符号」的负样本，不加载任何指标插件；
 * 唯一异步用例使用可超时的本地 gate，其他用例 poolsize=0；不 socket、不 shm。
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <filesystem>

#include <WtDtCore/DataManager.h>
#include <WtDtCore/IndexWorkerMgr.h>
#include <Includes/WTSContractInfo.hpp>
#include <Includes/WTSDataDef.hpp>
#include <Includes/WTSVariant.hpp>
#include <WTSUtils/WTSCfgLoader.h>
#include <WTSTools/WTSBaseDataMgr.h>

namespace fs = std::filesystem;

struct R18IndexWorkerMgrTestAccess
{
	static boost::threadpool::pool* pool(IndexWorkerMgr& mgr)
	{
		return mgr._pool.get();
	}

	static void addWorker(IndexWorkerMgr& mgr, const char* id,
		wtp::IIndexWorker* worker, wtp::IIndexWorkerFact* fact)
	{
		mgr._workers[id] = IndexWorkerPtr(new IdxWorkerWrapper(worker, fact));
	}
};

namespace {

struct R18AsyncControl
{
	std::atomic<bool> blockerEntered { false };
	std::atomic<bool> releaseBlocker { false };
	std::atomic<bool> blockerTimedOut { false };
	std::atomic<bool> workerEntered { false };
	std::atomic<bool> releaseWorker { false };
	std::atomic<bool> workerTimedOut { false };
	std::atomic<bool> dispatchComplete { false };
	std::atomic<uint32_t> refCountOnWorkerEntry { 0 };
	std::atomic<uint32_t> actionDateOnWorkerEntry { 0 };
	std::atomic<uint32_t> refCountAfterDispatch { 0 };
};

bool waitForFlag(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!flag.load(std::memory_order_acquire)
		&& std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	return flag.load(std::memory_order_acquire);
}

class R18AsyncGateCleanup
{
public:
	explicit R18AsyncGateCleanup(R18AsyncControl& control) : _control(control) {}
	~R18AsyncGateCleanup()
	{
		_control.releaseWorker.store(true, std::memory_order_release);
		_control.releaseBlocker.store(true, std::memory_order_release);
	}

private:
	R18AsyncControl& _control;
};

class R18TickRef
{
public:
	explicit R18TickRef(WTSTickData* tick = nullptr) : _tick(tick) {}
	~R18TickRef() { release(); }
	R18TickRef(const R18TickRef&) = delete;
	R18TickRef& operator=(const R18TickRef&) = delete;

	WTSTickData* get() const { return _tick; }
	void release()
	{
		if (_tick != nullptr)
		{
			_tick->release();
			_tick = nullptr;
		}
	}

private:
	WTSTickData* _tick;
};

class R18ProbeIndexWorker final : public wtp::IIndexWorker
{
public:
	R18ProbeIndexWorker(R18AsyncControl& control, bool throwOnQuote, bool waitForRelease)
		: IIndexWorker("r18_probe"), _control(control), _throwOnQuote(throwOnQuote), _waitForRelease(waitForRelease) {}

	const char* get_fact_name() override { return "r18_probe_fact"; }
	bool init(WTSVariant*) override { return true; }

	void handle_quote(WTSTickData* tick) override
	{
		if (_throwOnQuote)
			throw std::runtime_error("controlled index-worker callback failure");

		_control.refCountOnWorkerEntry.store(tick->retainCount(), std::memory_order_relaxed);
		_control.actionDateOnWorkerEntry.store(tick->actiondate(), std::memory_order_relaxed);
		_control.workerEntered.store(true, std::memory_order_release);
		if (_waitForRelease && !waitForFlag(_control.releaseWorker, std::chrono::seconds(5)))
			_control.workerTimedOut.store(true, std::memory_order_release);
	}

private:
	R18AsyncControl& _control;
	bool _throwOnQuote;
	bool _waitForRelease;
};

class R18ProbeIndexWorkerFact final : public wtp::IIndexWorkerFact
{
public:
	R18ProbeIndexWorkerFact(R18AsyncControl& control, bool throwOnQuote = false, bool waitForRelease = false)
		: _worker(control, throwOnQuote, waitForRelease) {}

	const char* get_name() override { return "r18_probe_fact"; }
	wtp::IIndexWorker* create_worker(const char*, const char*) override { return &_worker; }
	bool delete_worker(wtp::IIndexWorker* worker) override
	{
		++_deleteCount;
		return worker == &_worker;
	}
	wtp::IIndexWorker* worker() { return &_worker; }
	uint32_t deleteCount() const { return _deleteCount; }

private:
	R18ProbeIndexWorker _worker;
	uint32_t _deleteCount { 0 };
};

fs::path make_tmp_dir(const char* tag)
{
	static std::atomic<int> seq { 0 };
	fs::path p = fs::temp_directory_path()
		/ (std::string(tag) + "_" + std::to_string(::getpid()) + "_" + std::to_string(seq++));
	fs::create_directories(p);
	return p;
}

void write_file(const fs::path& p, const std::string& content)
{
	std::ofstream ofs(p.string(), std::ios::out | std::ios::trunc);
	ofs << content;
	ofs.close();
}

// 一个「本机一定存在、且肯定没有 create_index_factory 符号」的第三方动态库
// （只是作为 dlopen 成功的负样本，不调用它任何东西）
fs::path find_foreign_dylib()
{
	static const char* candidates[] = {
		"/opt/homebrew/lib/libFLAC++.dylib",
		"/opt/homebrew/lib/libboost_regex-mt.dylib",
		"/usr/local/lib/libboost_regex-mt.dylib",
		"/opt/homebrew/lib/liblz4.1.dylib",
		"/opt/homebrew/lib/libzstd.1.dylib",
	};
	for (const char* c : candidates)
	{
		std::error_code ec;
		if (fs::is_regular_file(fs::path(c), ec))
			return fs::path(c);
	}
	return fs::path();
}

class R18IdxWorkerMgrTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_dir = make_tmp_dir("r18_idx");
		write_file(_dir / "sessions.json",
			R"JSON({
				"DAY0": {"name": "日盘", "offset": 0,
					"sections": [{"from": 930, "to": 1500}]}
			})JSON");
		write_file(_dir / "commodities.json",
			R"JSON({
				"SSE": {"STK": {"name": "股票", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 0.01, "volscale": 1, "category": 0, "covermode": 0,
					"pricemode": 0, "trademode": 1, "lotstick": 1, "minlots": 1}},
				"SHFE": {"rb": {"name": "螺纹钢", "session": "DAY0", "holiday": "CHINA",
					"pricetick": 1, "volscale": 10, "covermode": 0, "pricemode": 1}}
			})JSON");
		write_file(_dir / "contracts.json",
			R"JSON({
				"SSE": {"600000": {"name": "浦发银行", "exchg": "SSE", "product": "STK",
					"altcode": "SH600000", "opendate": 20200101, "expiredate": 20500101}},
				"SHFE": {"rb2410": {"name": "螺纹2410", "exchg": "SHFE", "product": "rb",
					"opendate": 20230110, "expiredate": 20241115}}
			})JSON");
		ASSERT_TRUE(_bdMgr.loadSessions((_dir / "sessions.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadCommodities((_dir / "commodities.json").string().c_str()));
		ASSERT_TRUE(_bdMgr.loadContracts((_dir / "contracts.json").string().c_str()));
		_ctSse = _bdMgr.getContract("600000", "SSE");
		_ctShfe = _bdMgr.getContract("rb2410", "SHFE");
		ASSERT_NE(_ctSse, nullptr);
		ASSERT_NE(_ctShfe, nullptr);
	}

	void TearDown() override
	{
		std::error_code ec;
		fs::remove_all(_dir, ec);
	}

	static WTSTickData* make_tick(WTSContractInfo* cInfo, uint32_t adate, uint32_t tdate)
	{
		WTSTickStruct s;
		memset(&s, 0, sizeof(s));
		strcpy(s.code, "600000");
		strcpy(s.exchg, "SSE");
		s.action_date = adate;
		s.trading_date = tdate;
		s.price = 10.0;
		WTSTickData* tick = WTSTickData::create(s);
		if (cInfo != nullptr)
			tick->setContractInfo(cInfo);
		return tick;
	}

	fs::path _dir;
	WTSBaseDataMgr _bdMgr;
	WTSContractInfo* _ctSse { nullptr };
	WTSContractInfo* _ctShfe { nullptr };
};

TEST_F(R18IdxWorkerMgrTest, LoadFactoriesSkipsDirectoryAndNonLibraryAndUnloadableFiles)
{
	const fs::path libDir = _dir / "idxfact";
	fs::create_directories(libDir);
	fs::create_directories(libDir / "nested.dylib");		// L48-49 is_directory -> continue
	write_file(libDir / "readme.txt", "not a library");	// L55 扩展名不是 .dylib -> continue
	write_file(libDir / "bogus.dylib", "this is not a Mach-O file");	// L62-64 dlopen 失败 -> continue
	write_file(libDir / "bogus2.dylib", "\x00\x01\x02\x03 garbage");

	IndexWorkerMgr mgr;
	// 扫描完整跑完仍然是 true（L95-97），只是 0 个工厂
	EXPECT_TRUE(mgr.loadFactories(libDir.string().c_str()));
	// 不存在的目录才是 false（L37-41）
	EXPECT_FALSE(mgr.loadFactories((_dir / "nope_dir").string().c_str()));
}

TEST_F(R18IdxWorkerMgrTest, LoadFactoriesSurvivesLibraryWithoutFactorySymbol)
{
	const fs::path foreign = find_foreign_dylib();
	if (foreign.empty())
	{
		GTEST_SKIP() << "本机没有可用于负样本的第三方 .dylib";
	}

	const fs::path libDir = _dir / "idxfact2";
	fs::create_directories(libDir);
	std::error_code ec;
	fs::copy_file(foreign, libDir / "libidx_any.dylib",
		fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();
	// 两个同内容样本，覆盖 creator==NULL -> free_library -> continue（L66-71）两次
	fs::copy_file(foreign, libDir / "libidx_two.dylib",
		fs::copy_options::overwrite_existing, ec);
	ASSERT_FALSE(ec) << ec.message();

	IndexWorkerMgr mgr;
	// 无论 dlopen 成功与否，这条扫描路径都不得抛出、不得崩，且返回 true
	EXPECT_TRUE(mgr.loadFactories(libDir.string().c_str()));
}

TEST_F(R18IdxWorkerMgrTest, InitAcceptsEmptyIndiceArrayButRejectsNonArrayIndice)
{
	DataManager dm;	// 未 init，_writer==NULL

	write_file(_dir / "idx_object.json", R"JSON({"poolsize": 0, "indice": {"a": 1}})JSON");
	WTSVariant* cfgObj = WTSCfgLoader::load_from_file((_dir / "idx_object.json").string().c_str());
	ASSERT_NE(cfgObj, nullptr);
	IndexWorkerMgr mgrA;
	// L141-145：cfgIdx 非空但 !isArray() -> false
	EXPECT_FALSE(mgrA.init(cfgObj, nullptr, nullptr, &dm));
	// 上下文指针在函数头部（L131-133）就已写入，早于任何校验
	EXPECT_EQ(mgrA.get_hot_mgr(), nullptr);
	EXPECT_EQ(mgrA.get_bd_mgr(), nullptr);
	cfgObj->release();

	write_file(_dir / "idx_empty.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfgArr = WTSCfgLoader::load_from_file((_dir / "idx_empty.json").string().c_str());
	ASSERT_NE(cfgArr, nullptr);
	IndexWorkerMgr mgrB;
	// L136 poolsize==0 -> 不建线程池；L147-148 循环 0 次；L167 return true
	EXPECT_TRUE(mgrB.init(cfgArr, nullptr, nullptr, &dm));
	cfgArr->release();
}

TEST_F(R18IdxWorkerMgrTest, SyncHandleQuoteReleasesTemporaryRetainForEachQuote)
{
	DataManager dm;
	IndexWorkerMgr mgr;
	write_file(_dir / "idx_pool0.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_pool0.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	// 订阅 SSE.600000（L212 入 _subbed；L215 走 _data_mgr->getCurTick，writer 为空返回 NULL）
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);

	// 未订阅的代码：L177-178 在 retain 之前就返回，引用计数不动
	WTSTickData* other = make_tick(_ctShfe, 20240102, 20240102);
	ASSERT_EQ(other->retainCount(), 1u);
	mgr.handle_quote(other);	// fullCode = SHFE.rb2410，不在 _subbed 里
	EXPECT_EQ(other->retainCount(), 1u);
	other->release();

	WTSTickData* tick = make_tick(_ctSse, 20240102, 20240102);
	// 新建对象引用计数为 1（本用例自己 create，不依赖任何墙钟）
	ASSERT_EQ(tick->retainCount(), 1u);

	mgr.handle_quote(nullptr);		// L172-173 守卫
	EXPECT_EQ(tick->retainCount(), 1u);

	// 已订阅 + _pool 为空 -> 同步处理结束后应归还本次临时引用。
	mgr.handle_quote(tick);
	EXPECT_EQ(tick->retainCount(), 1u);
	mgr.handle_quote(tick);
	EXPECT_EQ(tick->retainCount(), 1u);

	tick->release();
}

TEST_F(R18IdxWorkerMgrTest, SyncHandleQuoteReleasesTemporaryRetainWhenWorkerThrows)
{
	R18AsyncControl control;
	R18ProbeIndexWorkerFact fact(control, true);
	DataManager dm;
	IndexWorkerMgr mgr;
	write_file(_dir / "idx_pool0_throw.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_pool0_throw.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();
	ASSERT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);

	R18IndexWorkerMgrTestAccess::addWorker(mgr, "r18_throw", fact.worker(), &fact);
	WTSTickData* tick = make_tick(_ctSse, 20240102, 20240102);
	ASSERT_NE(tick, nullptr);
	ASSERT_EQ(tick->retainCount(), 1u);

	EXPECT_THROW(mgr.handle_quote(tick), std::runtime_error);
	EXPECT_EQ(tick->retainCount(), 1u);
	tick->release();
}

TEST_F(R18IdxWorkerMgrTest, AsyncQueuedDispatchKeepsTickAliveUntilWorkerCompletes)
{
	R18AsyncControl control;
	R18ProbeIndexWorkerFact fact(control, false, true);
	IndexWorkerMgr mgr;
	R18AsyncGateCleanup gateCleanup(control);
	DataManager dm;
	write_file(_dir / "idx_pool1_gate.json", R"JSON({"poolsize": 1, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_pool1_gate.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();
	ASSERT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);
	R18IndexWorkerMgrTestAccess::addWorker(mgr, "r18_gate", fact.worker(), &fact);

	boost::threadpool::pool* pool = R18IndexWorkerMgrTestAccess::pool(mgr);
	ASSERT_NE(pool, nullptr);
	ASSERT_TRUE(pool->schedule([&control]() {
		control.blockerEntered.store(true, std::memory_order_release);
		if (!waitForFlag(control.releaseBlocker, std::chrono::seconds(5)))
			control.blockerTimedOut.store(true, std::memory_order_release);
	})) << "pool rejected the gate task";
	ASSERT_TRUE(waitForFlag(control.blockerEntered, std::chrono::seconds(2)))
		<< "pool worker did not enter the blocking gate";

	WTSTickData* tick = make_tick(_ctSse, 20240102, 20240102);
	ASSERT_NE(tick, nullptr);
	R18TickRef callerRef(tick);
	ASSERT_EQ(tick->retainCount(), 1u);

	// The worker is blocked, so this frame is genuinely queued rather than racing
	// a fast empty-worker lambda to completion.
	mgr.handle_quote(tick);
	EXPECT_EQ(pool->pending(), 1u);
	EXPECT_EQ(tick->retainCount(), 2u);

	// Drop the caller's ownership while the task is queued. The test observer and
	// queued dispatch reference keep the tick alive for the subsequent callback.
	tick->retain();
	R18TickRef observerRef(tick);
	callerRef.release();
	EXPECT_EQ(observerRef.get()->retainCount(), 2u);

	control.releaseBlocker.store(true, std::memory_order_release);
	ASSERT_TRUE(waitForFlag(control.workerEntered, std::chrono::seconds(2)))
		<< "queued index-worker callback did not start";
	EXPECT_EQ(control.refCountOnWorkerEntry.load(std::memory_order_relaxed), 2u);
	EXPECT_EQ(control.actionDateOnWorkerEntry.load(std::memory_order_relaxed), 20240102u);
	EXPECT_EQ(observerRef.get()->retainCount(), 2u);

	// FIFO ordering places this sentinel after the manager's callback task. Its
	// observation proves the temporary dispatch reference was released only
	// after the worker returned.
	observerRef.get()->retain();
	std::shared_ptr<WTSTickData> sentinelTick(observerRef.get(), [](WTSTickData* retained) {
		retained->release();
	});
	ASSERT_TRUE(pool->schedule([&control, sentinelTick]() {
		control.refCountAfterDispatch.store(sentinelTick->retainCount(), std::memory_order_relaxed);
		control.dispatchComplete.store(true, std::memory_order_release);
	})) << "pool rejected the completion sentinel";
	control.releaseWorker.store(true, std::memory_order_release);
	ASSERT_TRUE(waitForFlag(control.dispatchComplete, std::chrono::seconds(2)))
		<< "completion sentinel did not run";
	sentinelTick.reset();
	EXPECT_FALSE(control.blockerTimedOut.load(std::memory_order_acquire));
	EXPECT_FALSE(control.workerTimedOut.load(std::memory_order_acquire));
	// At the sentinel observation, only its capture and the test observer remain;
	// the manager's dispatch reference has already been released.
	EXPECT_EQ(control.refCountAfterDispatch.load(std::memory_order_relaxed), 2u);
	const auto refReleaseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (observerRef.get()->retainCount() != 1u
		&& std::chrono::steady_clock::now() < refReleaseDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	EXPECT_EQ(observerRef.get()->retainCount(), 1u);
	observerRef.release();
}

TEST_F(R18IdxWorkerMgrTest, ContextDelegatesToDataManagerAndLogger)
{
	DataManager dm;
	IndexWorkerMgr mgr;
	write_file(_dir / "idx_ctx.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_ctx.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	// L218-221：get_tick 直接转发 _data_mgr->getCurTick，writer 为空 -> NULL
	EXPECT_EQ(mgr.get_tick("600000", "SSE"), nullptr);
	// 空 exchg 也照样原样转发给 _data_mgr->getCurTick（:220 无过滤）
	EXPECT_EQ(mgr.get_tick("600000", ""), nullptr);

	// L210-216：sub_ticks 会 split fullCode 成 [exchg, code] 后转 getCurTick
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);
	// 订阅过一次之后，同代码的行情会走同步分支（见上一条用例），这里只验证委托不崩
	EXPECT_EQ(mgr.sub_ticks("SSE.600000"), nullptr);

	// L205-208：push_tick -> _data_mgr->writeTick(tick, 1)，writer 为空返回 false
	WTSTickData* tick = make_tick(_ctSse, 20240102, 20240102);
	mgr.push_tick(tick);
	EXPECT_EQ(tick->retainCount(), 1u);	// push_tick 不接管引用
	tick->release();

	// L223-226：output_log -> WTSLogger::log_raw
	mgr.output_log(LL_INFO, "r18 idx worker mgr output_log");
}

/*
 * 触发条件（禁用原因）：IndexWorkerMgr::handle_quote（L175）直接
 * `newTick->getContractInfo()->getFullCode()`，没有判空。
 * 喂一个合约信息为 NULL 的 tick（例如解析模块尚未补齐合约表时的裸行情）
 * 就是空指针解引用 -> SIGSEGV。缺陷登记：bugs/R18_wtdtcore.md BUG-R18-005。
 */
TEST_F(R18IdxWorkerMgrTest, DISABLED_HandleQuoteCrashesWhenContractInfoMissing)
{
	DataManager dm;
	IndexWorkerMgr mgr;
	write_file(_dir / "idx_null.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_null.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	WTSTickData* tick = make_tick(nullptr, 20240102, 20240102);
	mgr.handle_quote(tick);		// L175 解引用 NULL
	tick->release();
}

/*
 * 触发条件（禁用原因）：IndexWorkerMgr::sub_ticks（L214-215）把 fullCode 按 "." 切开
 * 后直接用 ay[1]，既不校验 split 结果长度也不校验空串。
 * 传 "600000"（无点号）时 ay.size()==1，ay[1] 越界访问 std::vector（operator[] 无检查）
 * ——UB/崩溃。缺陷登记：bugs/R18_wtdtcore.md BUG-R18-006。
 */
TEST_F(R18IdxWorkerMgrTest, DISABLED_SubTicksWithoutDotCrashesOnIndex)
{
	DataManager dm;
	IndexWorkerMgr mgr;
	write_file(_dir / "idx_dot.json", R"JSON({"poolsize": 0, "indice": []})JSON");
	WTSVariant* cfg = WTSCfgLoader::load_from_file((_dir / "idx_dot.json").string().c_str());
	ASSERT_NE(cfg, nullptr);
	ASSERT_TRUE(mgr.init(cfg, nullptr, nullptr, &dm));
	cfg->release();

	EXPECT_EQ(mgr.sub_ticks("600000"), nullptr);	// L215 ay[1] 越界
}

}  // namespace
