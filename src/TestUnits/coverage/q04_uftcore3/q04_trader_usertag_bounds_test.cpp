#include "q04_trader_fixture.h"

#include <array>
#include <cstring>
#include <string>

using namespace wtp;

namespace {

constexpr std::size_t kUserTagBufferBytes = sizeof(((WTSEntrustStruct*)nullptr)->m_strUserTag);

class CapturingUserTagApi : public q04::FakeTraderApi
{
public:
	int orderInsert(WTSEntrust* entrust) override
	{
		std::memcpy(_rawUserTag.data(), entrust->getUserTag(), _rawUserTag.size());
		return q04::FakeTraderApi::orderInsert(entrust);
	}

	std::array<char, kUserTagBufferBytes> _rawUserTag{};
};

} // namespace

TEST(q04_trader_usertag, maximum_adapter_id_fits_full_uint32_local_id_tag)
{
	const std::string adapterId(48, 'x');
	q04::TempRoot temp("usertag_bounds");
	CapturingUserTagApi api;
	q04::FakeBaseDataMgr bd;
	ActionPolicyMgr policy;
	TraderAdapter adapter;

	bd.add("SHFE", "ag", "ag2406");
	const std::string policyPath = temp.file("policy.json");
	temp.write(policyPath, q04::policyJsonOpenCloseLimited());
	ASSERT_TRUE(policy.init(policyPath.c_str()));
	ASSERT_TRUE(adapter.initExt(adapterId.c_str(), &api, &bd, &policy));

	const uint32_t localId = adapter.openLong("SHFE.ag2406", 5000.0, 1.0, 0);
	ASSERT_NE(UINT_MAX, localId);
	ASSERT_EQ(1u, api._entrusts.size());
	const std::string expected = "wtp." + adapterId + "." + std::to_string(localId);
	EXPECT_EQ(expected, api._entrusts.front()._usertag);
	RecordProperty("generated_local_id_digits", std::to_string(localId).size());
	EXPECT_LT(expected.size(), kUserTagBufferBytes);
	EXPECT_EQ('\0', api._rawUserTag[expected.size()]);
	EXPECT_EQ('\0', api._rawUserTag[kUserTagBufferBytes - 1]);

	// The production order id is a uint32_t. Check the maximum decimal value
	// directly so this regression does not depend on the runtime seed's width.
	const std::string maximumTag = "wtp." + adapterId + "." + std::to_string(UINT32_MAX);
	EXPECT_EQ(kUserTagBufferBytes - 1, maximumTag.size());
	EXPECT_EQ(kUserTagBufferBytes, maximumTag.size() + 1u);
}
