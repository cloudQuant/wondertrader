#include <WtCore/TraderAdapter.h>

#include <gtest/gtest.h>
#include <type_traits>

static_assert(!std::is_copy_constructible<wtp::TraderAdapterMgr>::value,
	"TraderAdapterMgr must remain non-copyable");

TEST(L00PublicHeaderTest, TraderAdapterHeaderIsSelfContained)
{
	SUCCEED();
}
