/*
 * 迭代03 K55（BUG-R60-006，用户裁定 2026-11-06，替代 K04=C2）：ITraderApi::isConnected() 统一为“完全就绪”。
 *
 * 合同：只有登录成功且交易通道可用时为 true；初始、仅连上通道未登录、logout、disconnect、release 之后
 * 都为 false。TraderMocker 的登录结果由 io 线程异步投递，因此“就绪”从 onLoginResult(true) 送达开始，
 * 而不是 login() 返回时。TraderMocker 没有登录失败路径（真实插件的登录失败见各自用例，如
 * I03S5TraderCTPMini.InvalidTradingDayFailsClosed 断言 isConnected 为 false）。
 */
#include "r103_mocker_common.h"

#include <atomic>

extern "C" {
	ITraderApi* createTrader();
	void deleteTrader(ITraderApi*& trader);
}

namespace i03k55 {

class LoginSink : public r103m::Sink
{
public:
	void onLoginResult(bool succ, const char* msg, uint32_t date) override
	{
		// 登录结果送达时插件必须已经就绪（宿主在此回调里就会下单）
		if (api != NULL)
			ready_at_login_result.store(api->isConnected());
		r103m::Sink::onLoginResult(succ, msg, date);
		logins.fetch_add(1);
	}
	ITraderApi* api { NULL };
	std::atomic<uint32_t> logins { 0 };
	std::atomic<bool> ready_at_login_result { false };
};

class I03K55MockerIsConnected : public ::testing::Test
{
protected:
	r103m::TempCwd cwd_;

	ITraderApi* make(LoginSink& sink, uint32_t id)
	{
		ITraderApi* api = createTrader();
		sink.api = api;
		api->registerSpi(&sink);
		WTSVariant* cfg = r103m::make_cfg(id);
		EXPECT_TRUE(api->init(cfg));
		cfg->release();
		return api;
	}
};

} // namespace i03k55

using namespace i03k55;

TEST_F(I03K55MockerIsConnected, ReadyOnlyBetweenLoginResultAndLogout)
{
	LoginSink sink;
	ITraderApi* api = make(sink, 97551);
	EXPECT_FALSE(api->isConnected());				// 初始

	api->connect();
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_event() >= 1u; }));
	EXPECT_FALSE(api->isConnected());				// 通道已连上，尚未登录

	ASSERT_EQ(0, api->login("u", "p", ""));
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.logins.load() >= 1u; }));
	EXPECT_TRUE(sink.ready_at_login_result.load());	// 登录结果送达时已就绪
	EXPECT_TRUE(api->isConnected());

	EXPECT_EQ(0, api->logout());
	EXPECT_FALSE(api->isConnected());				// logout 后

	api->release();
	EXPECT_FALSE(api->isConnected());				// release 后
	deleteTrader(api);
}

TEST_F(I03K55MockerIsConnected, DisconnectAndReleaseClearReadiness)
{
	LoginSink sink;
	ITraderApi* api = make(sink, 97552);
	api->connect();
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.n_event() >= 1u; }));
	ASSERT_EQ(0, api->login("u", "p", ""));
	ASSERT_TRUE(r103m::wait_until([&sink]() { return sink.logins.load() >= 1u; }));
	ASSERT_TRUE(api->isConnected());

	api->disconnect();
	EXPECT_FALSE(api->isConnected());				// disconnect 后
	api->release();
	EXPECT_FALSE(api->isConnected());
	deleteTrader(api);
}

TEST_F(I03K55MockerIsConnected, LoginWithoutChannelIsNotReady)
{
	// 没有 connect()：没有 io 线程，登录结果无法送达，交易通道不可用
	LoginSink sink;
	ITraderApi* api = make(sink, 97553);
	ASSERT_EQ(0, api->login("u", "p", ""));
	EXPECT_FALSE(api->isConnected());
	api->release();
	EXPECT_FALSE(api->isConnected());
	deleteTrader(api);
}
