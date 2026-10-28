/*!
 * \file r173_uftcore_misc_test.cpp
 * \brief R173 · WtUftCore 小面积公开面（时钟、类型默认值、解析器/回报接口默认实现）
 *
 * 覆盖的生产行段（llvm-cov 基线）：
 *  - WtUftCore/WtHelper.h:87/99/105/111/123   getDate/getSecs/getTradingDate/getInstDir/setGenerateDir
 *  - WtUftCore/WtUftEngine.h:71-73           get_raw_time/get_secs/get_trading_date
 *  - WtUftCore/TraderAdapter.h:66-71/110/137 PosItem::total_pos、state、getUndoneQty 未命中分支
 *  - WtUftCore/UftDataDefs.h:65-67           uft::DetailStruct 零初始化
 *  - WtUftCore/ITrdNotifySink.h:71/99        on_position/on_entrust 默认空实现
 *  - WtUftCore/ParserAdapter.h:24-28/46/49   IParserStub 默认空实现、id/handleSymbolList
 *
 * 边界：只构造纯内存对象；不初始化 ShareManager、不建共享内存、不起线程、
 *      不装载策略/交易插件。临时目录不落盘（仅设置静态目录字符串）。
 */
#include <gtest/gtest.h>

// TraderAdapter.h 未自行 include boost/noncopyable，先显式给出（见 q04 夹具说明）
#include <boost/noncopyable.hpp>

#include "WtUftCore/ITrdNotifySink.h"
#include "WtUftCore/ParserAdapter.h"
#include "WtUftCore/TraderAdapter.h"
#include "WtUftCore/UftDataDefs.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftEngine.h"

#include <string>

using namespace wtp;

namespace {

// 只实现 ITrdNotifySink 的纯虚函数，故意不覆盖 on_position/on_entrust，
// 使基类默认实现被执行。
class R173BareSink : public ITrdNotifySink
{
public:
	void on_trade(uint32_t /*localid*/, const char* /*stdCode*/, bool /*isLong*/,
		uint32_t /*offset*/, double /*vol*/, double /*price*/) override
	{
	}

	void on_order(uint32_t /*localid*/, const char* /*stdCode*/, bool /*isLong*/,
		uint32_t /*offset*/, double /*totalQty*/, double /*leftQty*/,
		double /*price*/, bool /*isCanceled*/) override
	{
	}

	void on_channel_ready(uint32_t /*tradingday*/) override {}
	void on_channel_lost() override {}
};

// IParserStub 的四个 push 回调都是空实现，这里不覆盖任何一个。
struct R173BareStub : public IParserStub
{
};

}	// namespace

/*
 * 生产: WtHelper.h:87/99/105/111/123；WtUftEngine.h:71-73；
 *       WtUftEngine.cpp:81-110 的赋值链
 * 先验证 WtHelper 的静态时钟/目录，再构造引擎（构造会重置时钟），最后验证
 * rawTime 非 0 时 _cur_raw_time 独立于 _cur_time、set_trading_date 同步全局交易日。
 */
TEST(R173UftEngine, HelperClockAndEngineAccessorsRoundTrip)
{
	const std::string oldInst = WtHelper::getInstDir();

	WtHelper::setTime(20240315, 93000, 123456);
	WtHelper::setTDate(20240316);
	EXPECT_EQ(20240315U, WtHelper::getDate());
	EXPECT_EQ(93000U, WtHelper::getTime());
	EXPECT_EQ(123456U, WtHelper::getSecs());
	EXPECT_EQ(20240316U, WtHelper::getTradingDate());

	WtHelper::setInstDir("/tmp/r173_uft_inst_dir");
	EXPECT_EQ(std::string("/tmp/r173_uft_inst_dir"), WtHelper::getInstDir());
	WtHelper::setGenerateDir("/tmp/r173_uft_gen_dir");

	WtUftEngine engine;

	engine.set_date_time(20240315, 931, 654321, 932);
	EXPECT_EQ(20240315U, engine.get_date());
	EXPECT_EQ(931U, engine.get_min_time());
	EXPECT_EQ(932U, engine.get_raw_time());
	EXPECT_EQ(654321U, engine.get_secs());

	// set_date_time 通过 WtHelper::setTime 更新全局时钟（时间用 rawTime）
	EXPECT_EQ(20240315U, WtHelper::getDate());
	EXPECT_EQ(932U, WtHelper::getTime());
	EXPECT_EQ(654321U, WtHelper::getSecs());

	engine.set_trading_date(20240314);
	EXPECT_EQ(20240314U, engine.get_trading_date());
	EXPECT_EQ(20240314U, WtHelper::getTradingDate());

	// 静态全局状态恢复，避免同进程内后续用例读到本用例的目录
	WtHelper::setInstDir(oldInst.c_str());
}

/*
 * 生产: UftDataDefs.h:65-67；TraderAdapter.h:66-71/110/137
 * DetailStruct 构造清零；PosItem::total_pos 多空两条分支；适配器初始状态与空未成交流水。
 */
TEST(R173UftTradeTypes, PosItemTotalsAndAdapterDefaultState)
{
	uft::DetailStruct detail;
	EXPECT_EQ(0U, detail._direct);
	EXPECT_DOUBLE_EQ(0.0, detail._volume);
	EXPECT_DOUBLE_EQ(0.0, detail._open_price);
	EXPECT_DOUBLE_EQ(0.0, detail._closed_profit);

	TraderAdapter::PosItem item;
	item.l_newvol = 1.0;
	item.l_prevol = 2.0;
	item.s_newvol = 3.0;
	item.s_prevol = 4.0;
	EXPECT_DOUBLE_EQ(3.0, item.total_pos(true));
	EXPECT_DOUBLE_EQ(7.0, item.total_pos(false));

	TraderAdapter adapter;
	EXPECT_EQ(TraderAdapter::AS_NOTLOGIN, adapter.state());
	EXPECT_DOUBLE_EQ(0.0, adapter.getUndoneQty("SHFE.rb2510"));
}

/*
 * 生产: ITrdNotifySink.h:71/99
 * 基类的 on_position/on_entrust 默认实现是空操作，派生类不覆盖时调用不产生副作用。
 */
TEST(R173UftSink, DefaultPositionAndEntrustHooksAreNoops)
{
	R173BareSink sink;
	ITrdNotifySink* base = &sink;

	EXPECT_NO_THROW(base->on_position("SHFE.rb2510", true, 2.0, 2.0, 3.0, 1.0, 20240315));
	EXPECT_NO_THROW(base->on_entrust(7, "SHFE.rb2510", true, "accepted"));
}

/*
 * 生产: ParserAdapter.h:24-28/46/49
 * IParserStub 的四个默认空实现 + ParserAdapter 的空 id/handleSymbolList/getBaseDataMgr。
 */
TEST(R173UftParser, DefaultStubAndAdapterMetadata)
{
	R173BareStub stub;
	IParserStub* base = &stub;
	base->handle_push_quote(nullptr);
	base->handle_push_order_detail(nullptr);
	base->handle_push_order_queue(nullptr);
	base->handle_push_transaction(nullptr);

	ParserAdapter adapter;
	EXPECT_STREQ("", adapter.id());
	adapter.handleSymbolList(nullptr);
	EXPECT_EQ(nullptr, adapter.getBaseDataMgr());
}
