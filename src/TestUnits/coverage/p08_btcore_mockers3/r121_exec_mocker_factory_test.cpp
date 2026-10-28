/**
 * r121_exec_mocker_factory_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/ExecMocker.cpp
 *   init 的插件装载成功段（L71-75）、无 executer 配置的 false 臂（L77-78）
 *   与成功返回（L85）；handle_schedule 的 15:00 早退（L163-164）；
 *   handle_session_end 的撮合器清理（L113-120）。
 *
 * R111 的 ExecMocker 用例在 _exec_unit 非空路径崩溃（见 manifests/wave19/R111.md
 * 与 bugs/R111.md 的登记），本用例只走到「工厂已装载、执行单元未创建」为止，
 * 不调用 handle_init/handle_schedule(非 15:00)，避开 R111 崩溃点。
 * 不联网、不起线程；插件只 dlopen 本机构建产物。
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <WtBtCore/ExecMocker.h>

#include "r121_common.h"

namespace
{
	const char* const kRb = "SHFE.rb.2405";
}

class R121ExecMocker : public r62::ReplayFixture
{
protected:
	std::string module() const
	{
		return r111::plugin_path("WtExeFact");
	}
};

// 工厂装载成功但 cfg 无 executer：init 只填 _factory 后返回 true
TEST_F(R121ExecMocker, FactoryWithoutExecuter)
{
	ASSERT_TRUE(std::filesystem::exists(module())) << module();

	fprintf(stderr, "E1 exec ctor\n");
	ExecMocker m(&_rp);
	WTSVariant* cfg = r121::make_module_cfg(module());
	cfg->append("code", kRb);
	cfg->append("period", "m1");
	cfg->append("volunit", 1.0);
	cfg->append("volmode", static_cast<int32_t>(0));

	fprintf(stderr, "E2 init\n");
	ASSERT_TRUE(m.init(cfg));
	cfg->release();
	fprintf(stderr, "E3 init ok\n");

	// 插件已装载，查询仍透传回放器；没有执行单元，账户保持零值
	fprintf(stderr, "E4 getcomm\n");
	ASSERT_NE(nullptr, m.getCommodityInfo(kRb));
	fprintf(stderr, "E5 getters\n");
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kRb));
	EXPECT_TRUE(m.getOrders(kRb) == nullptr);

	// 15:00 早退不触碰 _last_tick；会话收尾只清撮合器/统计
	fprintf(stderr, "E6 sched\n");
	m.handle_schedule(20240103u, 1500u);
	fprintf(stderr, "E7 sessend\n");
	m.handle_session_end(20240103u);
	fprintf(stderr, "E8 end\n");
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
}
