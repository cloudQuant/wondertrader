/*
 * Q06 - WtPorter 包装层：ExpParser / ExpExecuter 的转发契约
 *
 * 覆盖 src/WtPorter/ExpParser.cpp（init / release / connect / disconnect /
 * subscribe / unsubscribe / registerSpi 与 ExpParser.h 的内联 isConnected）和
 * src/WtPorter/ExpExecuter.cpp（init / set_position / on_position_changed），
 * 以及 src/WtPorter/WtRtRunner.cpp 的 parser_* 与 executer_* 六个薄转发函数。
 *
 * 这两个包装类不依赖柜台与共享内存：它们只做「把事件转给 getRunner()」，
 * 因此可以在未初始化 runner 的情况下用回调计数验证透传。
 * 只读生产代码；缺陷记录见 docs/迭代计划/迭代01/bugs/q06_porters.md。
 */
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "WtPorter/PorterDefs.h"
#include "WtPorter/WtRtRunner.h"
#include "WtPorter/ExpParser.h"
#include "WtPorter/ExpExecuter.h"

#include "Includes/IParserApi.h"

USING_NS_WTP;

extern WtRtRunner& getRunner();

namespace {

	struct WrapperProbe
	{
		uint32_t	evt_calls = 0;
		uint32_t	sub_calls = 0;
		uint32_t	exec_init = 0;
		uint32_t	exec_cmd = 0;

		std::string			last_parser_id;
		std::string			last_code;
		std::vector<std::string>	subbed;
		std::vector<std::string>	unsubbed;
		std::vector<double>	targets;
		WtUInt32			last_evt = 0;
		bool				last_is_sub = false;

		void clear()
		{
			*this = WrapperProbe();
		}
	};

	WrapperProbe g_probe;

	void wrapper_parser_evt(WtUInt32 evtId, const char* id)
	{
		g_probe.evt_calls++;
		g_probe.last_evt = evtId;
		g_probe.last_parser_id = id;
	}

	void wrapper_parser_sub(const char* id, const char* fullCode, bool isForSub)
	{
		g_probe.sub_calls++;
		g_probe.last_parser_id = id;
		g_probe.last_code = fullCode;
		g_probe.last_is_sub = isForSub;
		if (isForSub)
			g_probe.subbed.push_back(fullCode);
		else
			g_probe.unsubbed.push_back(fullCode);
	}

	void wrapper_exec_init(const char* id)
	{
		g_probe.exec_init++;
		g_probe.last_parser_id = id;
	}

	void wrapper_exec_cmd(const char* id, const char* code, double target)
	{
		g_probe.exec_cmd++;
		g_probe.last_code = code;
		g_probe.targets.push_back(target);
		(void)id;
	}

	class Q06PorterWrapper : public ::testing::Test
	{
	protected:
		void SetUp() override
		{
			g_probe.clear();
			WtRtRunner& r = getRunner();
			r.registerParserPorter(wrapper_parser_evt, wrapper_parser_sub);
			r.registerExecuterPorter(wrapper_exec_init, wrapper_exec_cmd);
		}

		void TearDown() override
		{
			WtRtRunner& r = getRunner();
			r.registerParserPorter(NULL, NULL);
			r.registerExecuterPorter(NULL, NULL);
			g_probe.clear();
		}
	};
}

/* ExpParser 的四个生命周期方法各自转成一次 parser 事件回调 */
TEST_F(Q06PorterWrapper, ParserLifecycleForwardsFourEvents)
{
	ExpParser parser("q06_exp_parser");

	EXPECT_TRUE(parser.init(NULL));
	EXPECT_EQ(g_probe.evt_calls, 1U);
	EXPECT_EQ(g_probe.last_evt, EVENT_PARSER_INIT);
	EXPECT_EQ(g_probe.last_parser_id, "q06_exp_parser");

	EXPECT_TRUE(parser.connect());
	EXPECT_EQ(g_probe.last_evt, EVENT_PARSER_CONNECT);

	EXPECT_TRUE(parser.disconnect());
	EXPECT_EQ(g_probe.last_evt, EVENT_PARSER_DISCONNECT);

	parser.release();
	EXPECT_EQ(g_probe.last_evt, EVENT_PARSER_RELEASE);
	EXPECT_EQ(g_probe.evt_calls, 4U);

	// isConnected() 是 ExpParser.h 的内联实现，恒为已连接
	EXPECT_TRUE(parser.isConnected());
}

/* 订阅集合逐项展开：N 个代码要产生 N 次回调，且顺序按集合遍历 */
TEST_F(Q06PorterWrapper, ParserSubscribeExpandsCodeSet)
{
	ExpParser parser("q06p");

	CodeSet codes;
	codes.insert("SHFE.au2406");
	codes.insert("SHFE.cu2406");
	codes.insert("DCE.i2409");

	parser.subscribe(codes);
	EXPECT_EQ(g_probe.sub_calls, 3U);
	EXPECT_EQ(g_probe.subbed.size(), 3U);
	EXPECT_TRUE(g_probe.last_is_sub);
	for (const auto& c : g_probe.subbed)
		EXPECT_NE(codes.find(c), codes.end()) << c;

	g_probe.sub_calls = 0;
	g_probe.unsubbed.clear();
	parser.unsubscribe(codes);
	EXPECT_EQ(g_probe.sub_calls, 3U);
	EXPECT_EQ(g_probe.unsubbed.size(), 3U);
	EXPECT_FALSE(g_probe.last_is_sub);

	// 空集合不产生任何回调
	CodeSet empty;
	parser.subscribe(empty);
	parser.unsubscribe(empty);
	EXPECT_EQ(g_probe.sub_calls, 3U);
}

/* registerSpi 为空时不得去取基础数据管理器 */
TEST_F(Q06PorterWrapper, ParserRegisterSpiStoresSink)
{
	ExpParser parser("q06p");
	parser.registerSpi(NULL);
	SUCCEED();
}

/* ExpExecuter：init 一次，仓位目标逐条透传 */
TEST_F(Q06PorterWrapper, ExecuterForwardsPositionTargets)
{
	ExpExecuter executer("q06_exec");

	executer.init();
	EXPECT_EQ(g_probe.exec_init, 1U);

	executer.on_position_changed("SHFE.au2406", 3.0);
	EXPECT_EQ(g_probe.exec_cmd, 1U);
	EXPECT_EQ(g_probe.last_code, "SHFE.au2406");
	ASSERT_EQ(g_probe.targets.size(), 1U);
	EXPECT_DOUBLE_EQ(g_probe.targets[0], 3.0);

	wt_hashmap<std::string, double> batch;
	batch["DCE.i2409"] = 5.0;
	batch["CZCE.TA409"] = -2.0;
	executer.set_position(batch);
	EXPECT_EQ(g_probe.exec_cmd, 3U);
	ASSERT_EQ(g_probe.targets.size(), 3U);
	EXPECT_DOUBLE_EQ(g_probe.targets[1] + g_probe.targets[2], 3.0);

	// 空批次不产生回调
	wt_hashmap<std::string, double> none;
	executer.set_position(none);
	EXPECT_EQ(g_probe.exec_cmd, 3U);
}

/*
 * 未注册回调时，包装层不得解引用空函数指针。
 * ExpParser/ExpExecuter 的每一次调用都只走 WtRtRunner 的 if(_cb_xxx) 保护分支。
 */
TEST_F(Q06PorterWrapper, WrapperCallsAreSafeWithoutCallbacks)
{
	WtRtRunner& r = getRunner();
	r.registerParserPorter(NULL, NULL);
	r.registerExecuterPorter(NULL, NULL);

	ExpParser parser("q06p_nocb");
	EXPECT_TRUE(parser.init(NULL));
	EXPECT_TRUE(parser.connect());
	EXPECT_TRUE(parser.disconnect());
	parser.release();

	CodeSet codes;
	codes.insert("SHFE.au2406");
	parser.subscribe(codes);
	parser.unsubscribe(codes);

	ExpExecuter executer("q06_exec_nocb");
	executer.init();
	executer.on_position_changed("SHFE.au2406", 1.0);

	EXPECT_EQ(g_probe.evt_calls, 0U);
	EXPECT_EQ(g_probe.exec_cmd, 0U);
}

/*
 * 跳过目标（详见文件头与 manifest）：
 * - WtRtRunner::createExtParser()/createExtExecuter()：_engine 为空即崩溃，
 *   需要完整 config()+initEngine()，属起进程路径 → DISABLED。
 */
TEST_F(Q06PorterWrapper, DISABLED_ExtParserCreationNeedsEngine)
{
	WtRtRunner& r = getRunner();
	(void)r.createExtParser("q06_never");
	(void)r.createExtExecuter("q06_never");
}
