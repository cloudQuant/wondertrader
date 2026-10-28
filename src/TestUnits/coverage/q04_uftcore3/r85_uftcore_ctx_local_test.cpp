/*!
 * \file r85_uftcore_ctx_local_test.cpp
 * \brief 迭代01 第十六波 R85 - UftStraContext 盯市零仓分支与本块装载的未知合约跳过
 *
 * 覆盖（只读生产代码）:
 *  - UftStraContext.cpp:76-92 on_tick 的盯市段：已平完的持仓（_volume==0）
 *    走 else 把 _dynprofit 归零，而不是沿用上一笔的浮盈。
 *  - UftStraContext.cpp:1154-1171 load_local_data 交易日切换重建时，
 *    明细里的合约在 _bd_mgr 查不到 -> continue（不把脏记录带进内存持仓）。
 *  - UftStraContext.cpp:1180-1193 同日装载时同样跳过未知合约。
 *
 * position.membin 用公开的 uft::PositionBlock/DetailStruct 直接铺一个块（不改
 * 生产代码、不做类布局镜像），只落在系统临时目录下的策略目录里，用例结束删除。
 */
#include "q04_trader_fixture.h"

#include "WtUftCore/UftStraContext.h"
#include "WtUftCore/WtHelper.h"
#include "WtUftCore/WtUftDtMgr.h"
#include "WtUftCore/WtUftEngine.h"

#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSStruct.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wtp;

namespace {

const char* kAg = "SHFE.ag2406";

// WtHelper::getOutputDir() 内部缓存, 根目录一次定死; 每个用例再用唯一策略名分目录
std::string ctxRoot()
{
	static std::string root = []() {
		fs::path p = fs::temp_directory_path() / "r85_uftctx_root";
		std::error_code ec;
		fs::remove_all(p, ec);
		fs::create_directories(p, ec);
		WtHelper::setGenerateDir((p.string() + "/").c_str());
		return p.string() + "/";
	}();
	return root;
}

WTSTickData* makeTick(const char* stdCode, const char* exchg, const char* code,
	uint32_t date, uint32_t hhmmss, double price, WTSContractInfo* cInfo)
{
	WTSTickStruct ts;
	memset(&ts, 0, sizeof(ts));
	wt_strcpy(ts.exchg, exchg);
	wt_strcpy(ts.code, stdCode);
	ts.action_date = date;
	ts.action_time = hhmmss;
	ts.trading_date = date;
	ts.price = price;
	WTSTickData* tick = WTSTickData::create(ts);
	tick->setContractInfo(cInfo);
	(void)code;
	return tick;
}

struct CtxEnv
{
	q04::TempRoot			_tmp { "r85ctx" };
	q04::FakeTraderApi		_api;
	q04::FakeBaseDataMgr	_bd;
	ActionPolicyMgr			_policy;
	TraderAdapter			_adapter;
	WtUftDtMgr				_dtMgr;
	WtUftEngine				_engine;
	UftStraContext			_ctx;
	std::string				_name;

	explicit CtxEnv(const char* tag)
		: _ctx(&_engine, makeName(tag).c_str())
	{
		ctxRoot();
		_bd.add("SHFE", "ag", "ag2406", 0, 0, CM_OpenCover, TM_Both, 1);
		const std::string path = _tmp.file("policy.json");
		_tmp.write(path, q04::policyJsonOpenCloseLimited());
		EXPECT_TRUE(_policy.init(path.c_str()));
		_engine.init(nullptr, &_bd, nullptr, nullptr);
		EXPECT_TRUE(_adapter.initExt(_name.c_str(), &_api, &_bd, &_policy));
		_adapter.run();
		_adapter.onLoginResult(true, "ok", 20240315);
		_adapter.addSink(&_ctx);
		_ctx.setTrader(&_adapter);
	}

	~CtxEnv()
	{
		std::error_code ec;
		fs::remove_all(folder(), ec);
	}

	std::string folder() const { return ctxRoot() + "outputs/" + _name + "/"; }

	std::string file(const char* suffix) const { return folder() + suffix; }

	static std::string makeName(const char* tag)
	{
		static std::atomic<uint32_t> seq(0);
		return std::string("r85ctx_") + tag + "_" + std::to_string((long)::getpid())
			+ "_" + std::to_string(++seq);
	}
};

// 直接铺一个 position.membin：块头 + 指定日期 + 一条未知合约的明细
void writePositionBlock(const fs::path& file, uint32_t date, const char* exchg,
	const char* code, double volume)
{
	std::error_code ec;
	fs::create_directories(file.parent_path(), ec);
	ASSERT_FALSE(ec);

	std::vector<char> buf(sizeof(uft::PositionBlock) + sizeof(uft::DetailStruct) * 2, 0);
	auto* blk = reinterpret_cast<uft::PositionBlock*>(buf.data());
	memcpy(blk->_blk_flag, uft::BLK_FLAG, sizeof(blk->_blk_flag));
	blk->_date = date;
	blk->_capacity = 2;
	blk->_size = 1;

	uft::DetailStruct& ds = blk->_details[0];
	wt_strcpy(ds._exchg, exchg);
	wt_strcpy(ds._code, code);
	ds._direct = 0;
	ds._volume = volume;
	ds._open_price = 100.0;

	std::ofstream ofs(file.string().c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(ofs.good());
	ofs.write(buf.data(), (std::streamsize)buf.size());
	ofs.close();
}

}	// namespace

// UftStraContext.cpp:82-92：平完仓后 on_tick 把 _dynprofit 归零
TEST(R85UftCtxLocal, zero_volume_position_resets_dynamic_profit)
{
	CtxEnv env("zero");
	env._ctx.on_channel_ready(20240315);

	uint32_t idOpen = env._ctx.stra_enter_long(kAg, 5000.0, 4.0, 0);
	ASSERT_NE(UINT_MAX, idOpen);
	env._ctx.on_trade(idOpen, kAg, true, 0, 4.0, 5000.0);
	ASSERT_DOUBLE_EQ(4.0, env._ctx.stra_get_local_position(kAg));

	// 先让浮盈是非零值：4 * (6000-5000) * 1 = 4000
	WTSContractInfo* cInfo = env._bd.getContract("ag2406", "SHFE");
	ASSERT_NE(nullptr, cInfo);
	WTSTickData* up = makeTick(kAg, "SHFE", "ag2406", 20240315, 100000000, 6000.0, cInfo);
	env._ctx.on_tick(kAg, up);
	up->release();
	EXPECT_DOUBLE_EQ(4000.0, env._ctx.stra_get_local_posprofit(kAg));

	uint32_t idClose = env._ctx.stra_exit_long(kAg, 5100.0, 4.0, false, 0);
	ASSERT_NE(UINT_MAX, idClose);
	env._ctx.on_trade(idClose, kAg, true, 1, 4.0, 5100.0);
	ASSERT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position(kAg));

	// 净头寸为 0 后再次盯市：_volume==0 分支把浮盈清零，不再沿用上一笔
	WTSTickData* down = makeTick(kAg, "SHFE", "ag2406", 20240315, 100030000, 4000.0, cInfo);
	env._ctx.on_tick(kAg, down);
	down->release();
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_posprofit(kAg));
}

// UftStraContext.cpp:1161-1171：跨交易日重建时未知合约 continue
TEST(R85UftCtxLocal, stale_block_drops_unknown_contract)
{
	CtxEnv env("stale");
	// 上一交易日的块，明细里的合约在基础数据里查不到
	writePositionBlock(env.file("position.membin"), 20240314, "XXXX", "none2401", 1.0);

	env._ctx.on_channel_ready(20240315);
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position("XXXX.none2401"));
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position(kAg));
}

// UftStraContext.cpp:1184-1193：同日装载时未知合约 continue
TEST(R85UftCtxLocal, same_day_block_drops_unknown_contract)
{
	CtxEnv env("sameday");
	// 日期与 on_channel_ready 一致 -> 不触发清理，直接走装载循环
	writePositionBlock(env.file("position.membin"), 20240315, "XXXX", "none2401", 2.0);

	env._ctx.on_channel_ready(20240315);
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position("XXXX.none2401"));
	EXPECT_DOUBLE_EQ(0.0, env._ctx.stra_get_local_position(kAg));
}
