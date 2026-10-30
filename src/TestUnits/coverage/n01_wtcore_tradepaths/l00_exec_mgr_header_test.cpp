#include <WtCore/WtExecMgr.h>

#include <gtest/gtest.h>
#include <type_traits>

static_assert(!std::is_copy_constructible<wtp::WtExecuterMgr>::value,
	"WtExecuterMgr must remain non-copyable");
static_assert(!std::is_copy_constructible<wtp::WtExecQuiesceCoordinator>::value,
	"WtExecQuiesceCoordinator must remain non-copyable");

TEST(L00PublicHeaderTest, ExecMgrHeaderIsSelfContained)
{
	SUCCEED();
}
