/**
 * r121_exec_mocker_factory_test.cpp  (lane R121)
 *
 * 靶子：src/WtBtCore/ExecMocker.cpp
 *   缺少 executer 配置时 init fail-closed，并返回明确诊断；空订单查询仍返回
 *   调用方拥有的非空快照对象。
 *
 * 不联网、不起线程；缺少 executer 时应在加载插件前失败。
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

// Missing executer configuration must fail closed instead of registering a
// factory-backed mocker that cannot safely dispatch replay callbacks.
TEST_F(R121ExecMocker, MissingExecuterFailsClosed)
{
	ExecMocker m(&_rp);
	WTSVariant* cfg = r121::make_module_cfg(module());
	cfg->append("code", kRb);
	cfg->append("period", "m1");
	cfg->append("volunit", 1.0);
	cfg->append("volmode", static_cast<int32_t>(0));

	std::string error;
	EXPECT_FALSE(m.init(cfg, &error));
	cfg->release();
	EXPECT_NE(std::string::npos, error.find("execution unit configuration is missing"));
	EXPECT_DOUBLE_EQ(0.0, m.getPosition(kRb));
	EXPECT_DOUBLE_EQ(0.0, m.getUndoneQty(kRb));
	OrderMap* orders = m.getOrders(kRb);
	ASSERT_NE(nullptr, orders);
	EXPECT_EQ(0u, orders->size());
	orders->release();
}
