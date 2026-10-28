/**
 * r103_replayer_cfg_store_test.cpp  (lane R103 · wave18)
 *
 * 靶子：HisDataReplayer::init 的两个公开配置分支
 *   - mode=bin/storage/wtp 且配置了 store 时，HisDataMgr 走 store 子对象  HisDataReplayer.cpp:182-196
 *   - 复权因子文件不存在时的告警与跳过                              :335-341
 *
 * 不装载任何行情；只验证配置面不崩、prepare 可进入。
 */
#include "r103_common.h"

using R103Fixture = r91::Fixture;

/*
 * 生产: HisDataReplayer.cpp:182-196（store 路径 + HisDtMgr::init(store)）
 *       294-299（loader 无因子后回落到 adjfactor 文件）
 *       335-341（复权因子文件不存在）
 */
TEST_F(R103Fixture, R103Replayer_StoreConfigAndMissingAdjFile)
{
	r91::CfgOpts opts;
	opts.mode = "bin";
	opts.tick = true;
	opts.adjfactor = (_base / "missing_adjfactors.json").string();

	WTSVariant* cfg = r103::make_cfg(_root, _base, 202001020930ull, 202001021500ull, opts,
		std::filesystem::path(), _base / "store");
	HisDataReplayer replayer;
	ASSERT_TRUE(replayer.init(cfg, NULL, NULL));
	cfg->release();

	r103::Sink sink;
	replayer.register_sink(&sink, "r103_store");
	ASSERT_TRUE(replayer.prepare());
	EXPECT_EQ(1u, sink.initCalls);

	// store 目录下的数据为空：公开查询返回 NULL，不崩
	EXPECT_EQ(nullptr, replayer.get_kline_slice("SSE.STK.600000", "d", 3, 1, false));
}
