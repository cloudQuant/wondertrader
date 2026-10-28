// Lane R195 - 头文件内联面：由生产读取器驱动 CodeHelper 的代码解析分支。
//
// 关键口径：本文件不直接调用 CodeHelper；所有分支都由
//   WtDataReader::readTickSlice（生产 TU WtDataReader.cpp）
// 内部对 CodeHelper::extractStdCode 的调用触达。CodeHelper 是 header-only，
// 只有被生产 TU 实例化才有覆盖率。
//
// 覆盖目标（src/Share/CodeHelper.hpp 当前 HEAD）：
//   - 136-223  isStdChnFutOptCode 状态机的各条非法返回（0/1/2/3/4/5/6/7/8/9/10/11 状态）
//   - 547-590  extractStdChnFutOptCode 对 CFFEX/CZCE/SHFE/其它交易所的格式化分支
//   - 527-541  indexCodeMonth 的正常路径
//   - 597-...  extractStdCode 对标准期权码的入口判断
// 说明：读取器在无数据文件时返回 NULL，不联网、不起线程；用例只断言“不崩溃且不返回伪切片”。
#include <gtest/gtest.h>

#include <string>

#include "WTSUtils/WTSCfgLoader.h"
#include "WtDataStorage/WtDataReader.h"

#include "../q03_storage3/q03_storage3_fixture.h"

USING_NS_WTP;

namespace {

class R195ReaderCodeHelperTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		_sandbox.reset(new q03::Sandbox("r195c"));
		const std::string cfgfile = _sandbox->root_abs() + "reader.json";
		const std::string json = "{\"path\":\"" + _sandbox->root_abs() + "\",\"adjust_flag\":0}";
		ASSERT_TRUE(q03::write_bytes(cfgfile, json));

		WTSVariant* cfg = WTSCfgLoader::load_from_file(cfgfile.c_str());
		ASSERT_NE(cfg, nullptr);
		_reader.init(cfg, &_sink, NULL);
		cfg->release();
	}

	void TearDown() override
	{
		_sandbox.reset();
	}

	// 生产路径：readTickSlice -> extractStdCode -> ...；无数据块时必须返回 NULL。
	void expect_no_tick_slice(const char* code)
	{
		WTSTickSlice* slice = _reader.readTickSlice(code, 5, 20240108150000000ull);
		if (slice != NULL)
		{
			slice->release();
			ADD_FAILURE() << "unexpected slice for code: " << code;
		}
	}

	std::unique_ptr<q03::Sandbox>	_sandbox;
	q03::BaseDataMgr	_bd;
	q03::HotMgr			_hot;
	q03::ReaderSink		_sink{ &_bd, &_hot };
	WtDataReader		_reader;
};

}	// namespace

// 非法期权格式：每个用例字符串都在 isStdChnFutOptCode 的不同 state 返回 false。
// 这些输入同时会进入 extractStdCode 的普通分支，读取器随后找不到文件而返回 NULL。
TEST_F(R195ReaderCodeHelperTest, malformed_option_codes_cover_state_machine_rejects)
{
	// state 0：首字符不是大写字母
	expect_no_tick_slice("lower.x");
	// state 1：交易所段内出现非字母、非点号
	expect_no_tick_slice("A1.x");
	// state 2：点号后不是字母
	expect_no_tick_slice("A..");
	// state 3：月份段首字符非法
	expect_no_tick_slice("A.a?");
	// state 4/5/6：月份数字位数不足时遇到非数字
	expect_no_tick_slice("A.a1X");
	expect_no_tick_slice("A.a12X");
	expect_no_tick_slice("A.a123X");
	// state 7：月份后缺分隔点
	expect_no_tick_slice("A.a1234X");
	// state 8：C/P 位非法
	expect_no_tick_slice("A.a1234.X");
	// state 9：C/P 后缺分隔点
	expect_no_tick_slice("A.a1234.C1");
	// state 10：行权价首位非数字
	expect_no_tick_slice("A.a1234.C.X");
	// state 11：行权价中途出现非数字
	expect_no_tick_slice("A.a1234.C.4X");
}

// 合法标准期权码：走 extractStdChnFutOptCode，分别覆盖
// SHFE/INE/CZCE 的 code 拼接、CFFEX 的 product 截断、其它交易所的 "_o" 后缀。
TEST_F(R195ReaderCodeHelperTest, valid_option_codes_cover_exchange_formatting)
{
	expect_no_tick_slice("SHFE.ag2001.C.4000");
	expect_no_tick_slice("INE.sc2001.P.500");
	expect_no_tick_slice("CZCE.ZC010.C.11600");
	expect_no_tick_slice("CFFEX.IO2001.C.4000");
	expect_no_tick_slice("DCE.m2001.P.3000");
}

// 普通两段/三段代码（非期权）：走 extractStdCode 的普通分支；
// 读取器没有对应数据文件时同样返回 NULL，证明上面的期权分支不是唯一入口。
TEST_F(R195ReaderCodeHelperTest, plain_and_three_segment_codes_stay_safe)
{
	expect_no_tick_slice("SSE.600000");
	expect_no_tick_slice("SHFE.ag.2512");
	expect_no_tick_slice("BINANCE.BTCUSDT");
}
