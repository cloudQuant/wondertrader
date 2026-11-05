#include <array>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

#include "Includes/FasterDefs.h"
#include "Includes/ITraderApi.h"
#include "Share/StdUtils.hpp"
#include "Includes/WTSCollection.hpp"
#include "Includes/IBaseDataMgr.h"
#include "Includes/WTSContractInfo.hpp"
#include "Includes/WTSDataDef.hpp"
#include "Includes/WTSTradeDef.hpp"
#include "Includes/WTSVariant.hpp"

#include <boost/asio.hpp>
#include <boost/array.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/executor_work_guard.hpp>

#define private public
#include "TraderMocker/TraderMocker.h"
#undef private

TEST(NEW020UdpAlignment, CompleteTickAcceptedAndTruncatedOrWrongTypeIgnored)
{
    TraderMocker mocker;
    const std::string fullCode = "SHFE.ag2512";
    mocker._codes.insert(fullCode);
    WTSTickStruct tick{};
    std::strcpy(tick.exchg, "SHFE");
    std::strcpy(tick.code, "ag2512");
    tick.price = 7000.0;
    tick.action_date = tick.trading_date = 20200102u;
    tick.action_time = 100000000u;
    uint32_t type = 0x200u;
    std::array<unsigned char, sizeof(type) + sizeof(tick)> bytes{};
    std::memcpy(bytes.data(), &type, sizeof(type));
    std::memcpy(bytes.data() + sizeof(type), &tick, sizeof(tick));
    std::memcpy(mocker._b_buffer.data(), bytes.data(), bytes.size());
    mocker.extract_buffer(static_cast<uint32_t>(bytes.size()), true);
    ASSERT_NE(nullptr, mocker._ticks);
    WTSObject* original = mocker._ticks->get(fullCode);
    ASSERT_NE(nullptr, original);
    EXPECT_DOUBLE_EQ(7000.0, static_cast<WTSTickData*>(original)->price());

    // Simulate the receive buffer retaining bytes from the preceding datagram.
    tick.price = 6999.0;
    std::memcpy(bytes.data() + sizeof(type), &tick, sizeof(tick));
    for (std::size_t n : {std::size_t{0}, std::size_t{3}, sizeof(type), bytes.size() - 1})
    {
        std::memcpy(mocker._b_buffer.data(), bytes.data(), n);
        mocker.extract_buffer(static_cast<uint32_t>(n), true);
        EXPECT_EQ(original, mocker._ticks->get(fullCode)) << n;
    }
    type = 0x201u;
    std::memcpy(bytes.data(), &type, sizeof(type));
    std::memcpy(mocker._b_buffer.data(), bytes.data(), bytes.size());
    mocker.extract_buffer(static_cast<uint32_t>(bytes.size()), true);
    EXPECT_EQ(original, mocker._ticks->get(fullCode));

    type = 0x200u;
    std::memcpy(bytes.data(), &type, sizeof(type));
    std::memcpy(mocker._b_buffer.data(), bytes.data(), bytes.size());
    mocker.extract_buffer(static_cast<uint32_t>(bytes.size()), true);
    auto* updated = static_cast<WTSTickData*>(mocker._ticks->get(fullCode));
    ASSERT_NE(nullptr, updated);
    EXPECT_DOUBLE_EQ(6999.0, updated->price());
}
