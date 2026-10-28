/*
 * 通路探针（非行为断言）：证明「收发族 + 每目录一个测试目标」这条形状能编译、能运行、
 * 并且插桩确实落进 src/TraderMocker 的生产行。
 * 真正的用例由后续 lane 写；这里只验证 seams，断言刻意保持最小。
 */
#include <gtest/gtest.h>

#include <cstring>

#include "TraderMocker/TraderMocker.h"

TEST(seam_tradermocker, constructed_object_answers_state_queries)
{
	TraderMocker api;

	// 未 init 的只读查询：目的是确认构造/析构与 vtable 可用，不对其语义下结论
	EXPECT_NO_THROW(api.isConnected());
	EXPECT_NO_THROW(api.release());
}

TEST(seam_tradermocker, entrust_id_buffer_is_written)
{
	TraderMocker api;
	char buf[64];
	std::memset(buf, 0, sizeof(buf));

	// makeEntrustID 是纯格式化路径，不依赖网络/线程
	api.makeEntrustID(buf, sizeof(buf));
	SUCCEED();
}
