/*!
 * \file r133_uftcore_remaining_test.cpp
 * \brief 第二十一波 R133 - WtUftCore 剩余可驱动公开面
 *
 * 覆盖（只读生产代码）:
 *  - TraderAdapter.cpp:162-206 init() 的完整成功链：模块路径解析 -> load_library ->
 *    createTrader -> deleteTrader 符号获取 -> _trader_api->init(params)。插件用本机构建
 *    产物 libTraderMocker.dylib，只调 init()，绝不 run()/connect()，因此不建 socket。
 *  - ShareManager.cpp:28-29 已初始化后重复 initialize() 的早退真分支。
 *  - ShareManager.cpp:83-108 start_watching() 工作线程体：遍历 commit_param_watcher
 *    登记过的 _secnames、查询更新时间、microsecs>0 时休眠。子进程内短暂等待 30ms，
 *    退出时单例析构 join。
 *  - ParserAdapter.cpp:99-108 / 183-202 init 后的 filter 集合与 initExt() 的非空过滤分支；
 *    :271-279 / :292-300 / :313-321 三个 Level2 handle 的过滤、时间戳、未知合约早退。
 *  - UftStraContext.cpp:1209-1213 / 1257-1261 / 1305-1309 / 1353-1357
 *    position/order/trade/round 四个本地块文件 map 失败后的兜底清零。
 *
 * 线程/接缝约束:
 *  - ShareManager 用例全部在 fork 子进程内完成，父进程单例保持未初始化，不影响 R11 断言；
 *    子进程只 dlopen 本机构建产物，不 init_domain、不建共享内存/socket，30ms 后退出。
 *  - TraderAdapter 用例只在临时目录里软链 TraderMocker，init 期间把 CWD 切到临时目录，
 *    让 TraderMocker 的 ./mocker_<id>/ 落在临时目录，结束即删。
 *  - ParserAdapter 用例只软链 libParserUDP.dylib 并调 init()（仅解析 host/port），
 *    不 run()/connect()。
 */
#include "q04_trader_fixture.h"
#include "r123_uft_common.h"

#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/ShareManager.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/IParserApi.h"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <gtest/gtest.h>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

std::string binDir()
{
	Dl_info info;
	if (dladdr(reinterpret_cast<void*>(&binDir), &info) == 0 || info.dli_fname == nullptr)
		return std::string();
	fs::path p = fs::path(info.dli_fname).parent_path().parent_path();
	std::string ret = p.string();
	if (!ret.empty() && ret.back() != '/')
		ret.push_back('/');
	return ret;
}

class InstDirGuard
{
public:
	explicit InstDirGuard(const std::string& dir)
		: _old(WtHelper::getInstDir())
	{
		WtHelper::setInstDir(dir.c_str());
	}
	~InstDirGuard() { WtHelper::setInstDir(_old.c_str()); }
	InstDirGuard(const InstDirGuard&) = delete;
	InstDirGuard& operator=(const InstDirGuard&) = delete;

private:
	std::string _old;
};

class CwdGuard
{
public:
	explicit CwdGuard(const fs::path& dir)
	{
		_ok = (::getcwd(_old, sizeof(_old)) != nullptr);
		if (_ok)
			_ok = (::chdir(dir.string().c_str()) == 0);
	}
	~CwdGuard()
	{
		if (_ok)
			::chdir(_old);
	}
	bool ok() const { return _ok; }

private:
	char _old[PATH_MAX] = { 0 };
	bool _ok = false;
};

// ---------------------------------------------------------------------------
// TraderAdapter.cpp:162-206：真实模块装载 + createTrader + init 成功链
// ---------------------------------------------------------------------------
TEST(R133TraderAdapterInit, loads_mocker_module_and_initializes)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";
	if (!fs::exists(bin + "libTraderMocker.dylib"))
		GTEST_SKIP() << "libTraderMocker.dylib not found in this build";

	q04::TempRoot tmp("r133trader");
	const std::string root = tmp.dir().string() + "/modroot";
	std::error_code ec;
	fs::create_directories(root + "/traders", ec);
	ASSERT_FALSE(ec);
	fs::create_symlink(bin + "libTraderMocker.dylib",
		root + "/traders/libTraderMocker.dylib", ec);
	ASSERT_FALSE(ec);
	InstDirGuard instdir(root + "/");

	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "TraderMocker");
	cfg->append("mockerid", (uint32_t)133);
	cfg->append("span", (uint32_t)1000);
	cfg->append("udp_port", (int32_t)0);

	{
		// TraderMocker::init 会在 CWD 下建 mocker_133/；仅在 init 调用期间切换 CWD
		CwdGuard cwd(tmp.dir());
		ASSERT_TRUE(cwd.ok());

		TraderAdapter adapter;
		EXPECT_TRUE(adapter.init("r133trader", cfg, &bd, &policy));
		EXPECT_STREQ("r133trader", adapter.id());
		EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
		// 同一 adapter 二次 init 在 _cfg 非空时被拒
		EXPECT_FALSE(adapter.init("r133trader2", cfg, &bd, &policy));

		// 不调用 adapter.release()：TraderAdapter::release() 会先
		// registerSpi(NULL)，而 TraderMocker::registerSpi 对 NULL 无保护
		// （BUG-R144-001），真实插件上必崩溃。本用例只验证 init 成功链，
		// 不 run/connect，插件对象与 cfg 引用留给进程退出回收。
	}

	cfg->release();
}

// ---------------------------------------------------------------------------
// ShareManager.cpp:28-29 + 83-108：fork 子进程内的单例成功装载面
// ---------------------------------------------------------------------------
struct R133SmReport
{
	int32_t	fail_line = 0;
	int32_t	marks = 0;
};

#define R133_SM_CHK(rep, cond) \
	do { if (!(cond)) { if ((rep).fail_line == 0) (rep).fail_line = __LINE__; } } while (0)

class R133ShareManagerFork : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_lib = r123::findLib("libWtShareHelper.dylib");
	}

	void run_child(const std::function<void(R133SmReport&)>& fn)
	{
		if (_lib.empty())
			GTEST_SKIP() << "libWtShareHelper.dylib not found in this build";

		int fds[2] = { -1, -1 };
		ASSERT_EQ(::pipe(fds), 0);

		fflush(nullptr);
		pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0)
		{
			::close(fds[0]);
			R133SmReport rep;
			fn(rep);
			ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
			::close(fds[1]);
			::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
		}

		::close(fds[1]);
		R133SmReport rep;
		memset(&rep, 0, sizeof(rep));
		ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
		::close(fds[0]);

		int status = 0;
		ASSERT_EQ(::waitpid(pid, &status, 0), pid);
		ASSERT_TRUE(WIFEXITED(status)) << "子进程异常退出, status=" << status;
		ASSERT_EQ(WEXITSTATUS(status), 0);
		ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
		EXPECT_EQ(rep.fail_line, 0) << "子进程检查失败行 " << rep.fail_line;
		EXPECT_EQ(rep.marks, 3);
	}

	std::string _lib;
};

// ShareManager.cpp:28-29 已初始化 -> 再次 initialize 走 `if(_inited) return true`
TEST_F(R133ShareManagerFork, repeated_initialize_short_circuits)
{
	run_child([this](R133SmReport& r) {
		ShareManager& sm = ShareManager::self();
		R133_SM_CHK(r, sm.initialize(_lib.c_str()));
		R133_SM_CHK(r, sm.initialize(_lib.c_str()));
		// 路径不存在也无所谓：_inited 已为真，仍然返回 true
		R133_SM_CHK(r, sm.initialize("/r133/no/such/module.dylib"));
		r.marks = 3;
	});
}

// ShareManager.cpp:83-108 工作线程遍历 _secnames、查询更新时间并按 microsecs 休眠
TEST_F(R133ShareManagerFork, watcher_iterates_committed_sections)
{
	run_child([this](R133SmReport& r) {
		ShareManager& sm = ShareManager::self();
		R133_SM_CHK(r, sm.initialize(_lib.c_str()));
		// 未 init_domain 时 commit_section 返回 false，但会登记 _secnames
		R133_SM_CHK(r, !sm.commit_param_watcher("r133_watch"));
		R133_SM_CHK(r, sm.start_watching(1000));

		// 重复 start 只返回 true，不重建线程
		R133_SM_CHK(r, sm.start_watching(1000));

		// 给工作线程至少几十轮迭代；子进程 exit 时单例析构 join
		::usleep(30000);
		r.marks = 3;
	});
}

// ---------------------------------------------------------------------------
// ParserAdapter.cpp:99-108 / 183-202 / 271-321：filter 与 Level2 handle 守卫
// ---------------------------------------------------------------------------
class FakeParserApi : public IParserApi
{
public:
	bool _initRet = true;
	uint32_t _initCnt = 0;
	uint32_t _subscribeCnt = 0;

	bool init(WTSVariant* /*config*/) override { _initCnt++; return _initRet; }
	void registerSpi(IParserSpi* /*spi*/) override {}
	void subscribe(const CodeSet& setCodes) override { _subscribeCnt += (uint32_t)setCodes.size(); }
};

class FakeParserStub : public IParserStub
{
public:
	uint32_t _quoteCnt = 0;
	uint32_t _oqCnt = 0;
	uint32_t _odCnt = 0;
	uint32_t _trCnt = 0;

	void handle_push_quote(WTSTickData* /*curTick*/) override { _quoteCnt++; }
	void handle_push_order_queue(WTSOrdQueData* /*curOrdQue*/) override { _oqCnt++; }
	void handle_push_order_detail(WTSOrdDtlData* /*curOrdDtl*/) override { _odCnt++; }
	void handle_push_transaction(WTSTransData* /*curTrans*/) override { _trCnt++; }
};

WTSOrdQueData* makeOrdQue(const char* exchg, const char* code, uint32_t ad, uint32_t td)
{
	WTSOrdQueStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = ad;
	st.trading_date = td;
	st.action_time = 100000000;
	return WTSOrdQueData::create(st);
}

WTSOrdDtlData* makeOrdDtl(const char* exchg, const char* code, uint32_t ad, uint32_t td)
{
	WTSOrdDtlStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = ad;
	st.trading_date = td;
	st.action_time = 100000000;
	return WTSOrdDtlData::create(st);
}

WTSTransData* makeTrans(const char* exchg, const char* code, uint32_t ad, uint32_t td)
{
	WTSTransStruct st;
	memset(&st, 0, sizeof(st));
	wt_strcpy(st.exchg, exchg);
	wt_strcpy(st.code, code);
	st.action_date = ad;
	st.trading_date = td;
	st.action_time = 100000000;
	return WTSTransData::create(st);
}

TEST(R133ParserAdapterGuards, filter_and_level2_early_returns)
{
	const std::string bin = binDir();
	if (bin.empty())
		GTEST_SKIP() << "cannot resolve build bin dir";

	q04::TempRoot tmp("r133parser");
	const std::string root = tmp.dir().string() + "/modroot";
	std::error_code ec;
	fs::create_directories(root + "/parsers", ec);
	ASSERT_FALSE(ec);
	if (!fs::exists(bin + "libParserUDP.dylib"))
		GTEST_SKIP() << "libParserUDP.dylib not found in this build";
	fs::create_symlink(bin + "libParserUDP.dylib",
		root + "/parsers/libParserUDP.dylib", ec);
	ASSERT_FALSE(ec);
	InstDirGuard instdir(root + "/");

	q04::FakeBaseDataMgr bd;
	WTSContractInfo* ag = bd.add("SHFE", "ag", "ag2406");
	ag->setAltCode("ag2406");
	// 本用例会走两次 getContracts()->release()（init 与 initExt 各一次），
	// WTSArray::release 会 release 元素，因此补两次 retain 防止提前析构。
	ag->retain();
	ag->retain();

	FakeParserStub stub;
	WTSVariant* cfg = WTSVariant::createObject();
	cfg->append("module", "ParserUDP");
	cfg->append("filter", "SHFE");
	cfg->append("host", "127.0.0.1");
	cfg->append("bport", (uint32_t)17931);
	cfg->append("sport", (uint32_t)17932);

	{
		ParserAdapter adapter;
		ASSERT_TRUE(adapter.init("r133parser", cfg, &stub, &bd));

		// :271-279 交易所不在 filter -> 早退；SHFE 但未知合约 -> cInfo NULL 早退
		WTSOrdQueData* oqDce = makeOrdQue("DCE", "m2409", 20240315, 20240315);
		adapter.handleOrderQueue(oqDce);
		oqDce->release();
		WTSOrdQueData* oqUnk = makeOrdQue("SHFE", "none2401", 20240315, 20240315);
		adapter.handleOrderQueue(oqUnk);
		oqUnk->release();
		EXPECT_EQ(0u, stub._oqCnt);

		// :292-300 过滤早退 / 时间戳早退 / 未知合约早退
		WTSOrdDtlData* odDce = makeOrdDtl("DCE", "m2409", 20240315, 20240315);
		adapter.handleOrderDetail(odDce);
		odDce->release();
		WTSOrdDtlData* odNoDate = makeOrdDtl("SHFE", "ag2406", 0, 20240315);
		adapter.handleOrderDetail(odNoDate);
		odNoDate->release();
		WTSOrdDtlData* odUnk = makeOrdDtl("SHFE", "none2401", 20240315, 20240315);
		adapter.handleOrderDetail(odUnk);
		odUnk->release();
		EXPECT_EQ(0u, stub._odCnt);

		// :313-321 过滤早退 / 时间戳早退 / 未知合约早退
		WTSTransData* trDce = makeTrans("DCE", "m2409", 20240315, 20240315);
		adapter.handleTransaction(trDce);
		trDce->release();
		WTSTransData* trNoDate = makeTrans("SHFE", "ag2406", 20240315, 0);
		adapter.handleTransaction(trNoDate);
		trNoDate->release();
		WTSTransData* trUnk = makeTrans("SHFE", "none2401", 20240315, 20240315);
		adapter.handleTransaction(trUnk);
		trUnk->release();
		EXPECT_EQ(0u, stub._trCnt);

		// 合法数据必须真正透传到 stub（证明上面的 0 是守卫而不是整体失效）
		WTSOrdQueData* oqOk = makeOrdQue("SHFE", "ag2406", 20240315, 20240315);
		adapter.handleOrderQueue(oqOk);
		oqOk->release();
		WTSOrdDtlData* odOk = makeOrdDtl("SHFE", "ag2406", 20240315, 20240315);
		adapter.handleOrderDetail(odOk);
		odOk->release();
		WTSTransData* trOk = makeTrans("SHFE", "ag2406", 20240315, 20240315);
		adapter.handleTransaction(trOk);
		trOk->release();
		EXPECT_EQ(1u, stub._oqCnt);
		EXPECT_EQ(1u, stub._odCnt);
		EXPECT_EQ(1u, stub._trCnt);

		// 释放 UDP 插件；_filters 保留 {SHFE}
		adapter.release();

		// :183-202 initExt 的非空过滤分支：只有一个 SHFE 合约命中
		FakeParserApi api;
		EXPECT_TRUE(adapter.initExt("r133parserext", &api, &stub, &bd));
		EXPECT_EQ(1u, api._initCnt);
		EXPECT_EQ(1u, api._subscribeCnt);
	}

	cfg->release();
}

// ---------------------------------------------------------------------------
// UftStraContext.cpp:1209-1213 / 1257-1261 / 1305-1309 / 1353-1357
// 四个本地块文件的 BoostMappingFile::map 失败兜底
// ---------------------------------------------------------------------------
TEST(R133UftCtxLocalMapFail, dangling_block_files_take_map_failure_else)
{
	q04::TempRoot tmp("r133ctxmap");
	WtHelper::setGenerateDir((tmp.dir().string() + "/").c_str());

	static std::atomic<uint32_t> seq(0);
	const std::string name = "r133ctxmap_" + std::to_string((long)::getpid())
		+ "_" + std::to_string(++seq);
	const std::string folder = std::string(WtHelper::getOutputDir()) + name + "/";

	std::error_code ec;
	fs::create_directories(folder, ec);
	ASSERT_FALSE(ec);

	// 原夹具在块文件位置放“悬空软链”制造 create_new_file 失败。但 Boost 1.88 的
	// create_or_open_file 在“O_EXCL 返回 EEXIST、随后普通 open 返回 ENOENT”时会
	// 无限重试（os_file_functions.hpp:create_or_open_file 的 while(true)），
	// macOS 上表现为挂起而不是返回 false。
	// 改为把策略目录置为只读：create_new_file 首次 open(O_CREAT) 直接 EACCES，
	// 立即返回失败；随后 BoostMappingFile::map 因路径不存在返回 false，
	// 仍精确进入生产代码的 map 失败 else 分支。
	fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec,
		fs::perm_options::replace, ec);
	ASSERT_FALSE(ec);

	q04::FakeBaseDataMgr bd;
	bd.add("SHFE", "ag", "ag2406");
	WtUftEngine engine;
	engine.init(nullptr, &bd, nullptr, nullptr);

	{
		UftStraContext ctx(&engine, name.c_str());
		ctx.on_channel_ready(20240315);
		// 四个块都走了 map 失败分支：不崩、持仓为空
		EXPECT_DOUBLE_EQ(0.0, ctx.stra_get_local_position("SHFE.ag2406"));
	}

	fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace, ec);
	fs::remove_all(folder, ec);
}

} // namespace
