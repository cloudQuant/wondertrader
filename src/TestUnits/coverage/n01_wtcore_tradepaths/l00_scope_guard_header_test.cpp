#include <Share/threadpool/detail/scope_guard.hpp>

#include <gtest/gtest.h>
#include <type_traits>

static_assert(!std::is_copy_constructible<boost::threadpool::detail::scope_guard>::value,
	"scope_guard must remain non-copyable");

TEST(L00PublicHeaderTest, ScopeGuardHeaderIsSelfContained)
{
	SUCCEED();
}
