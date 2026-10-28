/**
 * r113_btporter_wired_orders_test.cpp  (lane R113 · wave19)
 *
 * 靶子：WtBtPorter 在「底座数据已装载 + mocker 非空」时的剩余公开面
 *   - cta_get_all_position 的逐项回调（信号表非空）      WtBtPorter.cpp:589-603
 *   - hft_buy / hft_sell 的 OrderIDs 非空拼串            :1900-1917 / 1931-1948
 *   - hft_cancel_all 的非空返回值                        :1868-1887
 *
 * 关键前置：WtBtRunner::config 的 replayer.basefiles 里装载 sessions/commodities/
 * contracts，使 HisDataReplayer 能按 "SHFE.rb2510" 查到 rb 品种；此后
 * init_cta_mocker 的 stra_set_position 会写入信号表，init_hft_mocker 的
 * stra_buy/stra_sell 会返回非空 OrderIDs。
 *
 * 这些 mocker 槽位在进程单例上，因此每个用例整体放在 fork 子进程；子进程
 * exit() 前通过管道回传断言结果，父进程只校验报告，不继承任何 mocker 状态。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

#include "WtBtPorter/PorterDefs.h"
#include "WtBtPorter/WtBtPorter.h"
#include "WtBtPorter/WtBtRunner.h"

USING_NS_WTP;

extern WtBtRunner& getRunner();

namespace
{
	namespace stdfs = std::filesystem;

	struct ChildReport
	{
		int		fails;
		int		cta_enum_calls;
		int		cta_enum_last;
		double	cta_enum_qty;
		double	cta_pos;
		int		buy_len;
		int		sell_len;
		int		cancel_all_len;
		int		cancel_ok;
		unsigned long	buy_id;
		unsigned long	sell_id;
	};

	inline void child_expect(ChildReport& r, bool cond)
	{
		if (!cond)
			r.fails++;
	}

	struct EnumRec
	{
		int		calls { 0 };
		bool	last { false };
		double	qty { 0.0 };
		std::string code;
	};

	EnumRec g_enum;

	void enum_cb(CtxHandler, const char* code, double qty, bool isLast)
	{
		g_enum.calls++;
		g_enum.last = isLast;
		g_enum.qty = qty;
		g_enum.code = code ? code : "";
	}

	void write_text(const std::string& path, const std::string& body)
	{
		std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
		ofs << body;
	}

	stdfs::path prepare_dir(const char* tag)
	{
		stdfs::path dir = stdfs::temp_directory_path()
			/ (std::string("r113_btporter_") + tag + "_" + std::to_string((long)::getpid()));
		std::error_code ec;
		stdfs::remove_all(dir, ec);
		stdfs::create_directories(dir, ec);
		return dir;
	}

	const char* kSessions =
		"{\"TRADING\":{\"name\":\"Trading\",\"offset\":0,"
		"\"sections\":[{\"from\":900,\"to\":1500}]}}";

	const char* kCommodities =
		"{\"SHFE\":{\"rb\":{\"name\":\"Rebar\",\"session\":\"TRADING\",\"pricetick\":1,"
		"\"volscale\":10,\"covermode\":0,\"pricemode\":0,\"trademode\":0,\"category\":1}}}";

	const char* kContracts =
		"{\"SHFE\":{\"rb2510\":{\"name\":\"rb2510\",\"product\":\"rb\",\"exchg\":\"SHFE\"}}}";

	void write_fixtures(const stdfs::path& dir)
	{
		write_text((dir / "sessions.json").string(), kSessions);
		write_text((dir / "commodities.json").string(), kCommodities);
		write_text((dir / "contracts.json").string(), kContracts);
	}

	std::string replayer_cfg(const stdfs::path& dir)
	{
		return "{\"replayer\":{\"mode\":\"none\",\"path\":\"" + dir.string() + "\","
			"\"basefiles\":{"
			"\"session\":\"" + (dir / "sessions.json").string() + "\","
			"\"commodity\":\"" + (dir / "commodities.json").string() + "\","
			"\"contract\":\"" + (dir / "contracts.json").string() + "\"}},"
			"\"env\":{\"mocker\":\"none\"}}";
	}
}

/*
 * 生产: WtBtPorter.cpp:589-603（enum_position 逐项回调 + 终止符）
 */
TEST(R113BtPorterCtaSignal, CtaSetPositionEnumeratesOneSignal)
{
	const stdfs::path dir = prepare_dir("cta");
	write_fixtures(dir);
	const std::string cfg = replayer_cfg(dir);

	int fds[2] = { -1, -1 };
	ASSERT_EQ(0, ::pipe(fds));

	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		::close(fds[0]);
		ChildReport rep;
		memset(&rep, 0, sizeof(rep));

		getRunner().config(cfg.c_str(), false);

		const CtxHandler cta = init_cta_mocker("r113_cta_wired", 0, false, false, false, false);
		child_expect(rep, cta > 0U);
		child_expect(rep, getRunner().cta_mocker() != nullptr);

		cta_set_position(cta, "SHFE.rb2510", 2.0, "r113", 0.0, 0.0);
		rep.cta_pos = cta_get_position(cta, "SHFE.rb2510", false, "");
		child_expect(rep, rep.cta_pos == 2.0);

		g_enum = EnumRec();
		cta_get_all_position(cta, enum_cb);
		rep.cta_enum_calls = g_enum.calls;
		rep.cta_enum_last = g_enum.last ? 1 : 0;
		rep.cta_enum_qty = g_enum.qty;
		child_expect(rep, rep.cta_enum_calls == 2);
		child_expect(rep, rep.cta_enum_last == 1);
		child_expect(rep, rep.cta_enum_qty == 2.0);

		const ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
		::close(fds[1]);
		::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
	}

	::close(fds[1]);
	ChildReport rep;
	memset(&rep, 0, sizeof(rep));
	const ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
	::close(fds[0]);

	int status = 0;
	ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
	ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(0, rep.fails);
	EXPECT_DOUBLE_EQ(2.0, rep.cta_pos);
	EXPECT_EQ(2, rep.cta_enum_calls);
	EXPECT_EQ(1, rep.cta_enum_last);
	EXPECT_DOUBLE_EQ(2.0, rep.cta_enum_qty);

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}

/*
 * 生产: WtBtPorter.cpp:1868-1887（cancel_all 非空拼串与尾逗号裁剪）
 *       1900-1917 / 1931-1948（buy/sell 非空 OrderIDs）
 */
TEST(R113BtPorterWiredOrders, HftOrderIdsWithBaseData)
{
	const stdfs::path dir = prepare_dir("hft");
	write_fixtures(dir);
	const std::string cfg = replayer_cfg(dir);

	int fds[2] = { -1, -1 };
	ASSERT_EQ(0, ::pipe(fds));

	const pid_t pid = ::fork();
	ASSERT_GE(pid, 0);
	if (pid == 0)
	{
		::close(fds[0]);
		ChildReport rep;
		memset(&rep, 0, sizeof(rep));

		getRunner().config(cfg.c_str(), false);

		const CtxHandler hft = init_hft_mocker("r113_hft_wired", false);
		child_expect(rep, hft > 0U);
		child_expect(rep, getRunner().hft_mocker() != nullptr);

		const char* buy = hft_buy(hft, "SHFE.rb2510", 4500.0, 2.0, "r113", 0);
		rep.buy_len = (int)strlen(buy);
		rep.buy_id = strtoul(buy, NULL, 10);
		child_expect(rep, rep.buy_len > 0);
		child_expect(rep, rep.buy_id > 0UL);

		const char* sell = hft_sell(hft, "SHFE.rb2510", 4500.0, 1.0, "r113", 0);
		rep.sell_len = (int)strlen(sell);
		rep.sell_id = strtoul(sell, NULL, 10);
		child_expect(rep, rep.sell_len > 0);
		child_expect(rep, rep.sell_id > 0UL);
		child_expect(rep, rep.sell_id != rep.buy_id);

		const char* canceled = hft_cancel_all(hft, "SHFE.rb2510", true);
		rep.cancel_all_len = (int)strlen(canceled);
		child_expect(rep, rep.cancel_all_len > 0);
		child_expect(rep, strtoul(canceled, NULL, 10) == rep.buy_id);

		rep.cancel_ok = hft_cancel(hft, (WtUInt32)rep.sell_id) ? 1 : 0;
		child_expect(rep, rep.cancel_ok == 1);

		const ssize_t nw = ::write(fds[1], &rep, sizeof(rep));
		::close(fds[1]);
		::exit(nw == (ssize_t)sizeof(rep) ? 0 : 3);
	}

	::close(fds[1]);
	ChildReport rep;
	memset(&rep, 0, sizeof(rep));
	const ssize_t nr = ::read(fds[0], &rep, sizeof(rep));
	::close(fds[0]);

	int status = 0;
	ASSERT_EQ(pid, ::waitpid(pid, &status, 0));
	ASSERT_EQ(nr, (ssize_t)sizeof(rep)) << "子进程未回传报告";
	ASSERT_TRUE(WIFEXITED(status));
	EXPECT_EQ(0, WEXITSTATUS(status));
	EXPECT_EQ(0, rep.fails);
	EXPECT_GT(rep.buy_len, 0);
	EXPECT_GT(rep.sell_len, 0);
	EXPECT_GT(rep.cancel_all_len, 0);
	EXPECT_EQ(1, rep.cancel_ok);

	std::error_code ec;
	stdfs::remove_all(dir, ec);
}
