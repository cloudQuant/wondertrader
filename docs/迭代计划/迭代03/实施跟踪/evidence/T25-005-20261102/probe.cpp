// Appended only to the isolated snapshot test TU, not the product test suite.
TEST_F(T25TraderCTPTest, i03_no_final_query_response_disconnect_bounded)
{
    g_api_for_creator = &api_;
    trader_.setCreator(&fakeCreator);
    trader_.connect();
    trader_.setState(TraderCTP::WS_ALLREADY);
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitUntil([this]() { return api_.n_qry_acct_.load() == 1 && trader_.inQuery(); }, 2000));
    std::fprintf(stderr, "QUERY_IN_FLIGHT\n");
    std::fflush(stderr);
    trader_.disconnect();
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.n_release_.load());
}

TEST_F(T25TraderCTPTest, i03_error_final_disconnect_then_late_error_callback)
{
    g_api_for_creator = &api_;
    trader_.setCreator(&fakeCreator);
    trader_.connect();
    trader_.setState(TraderCTP::WS_ALLREADY);
    ASSERT_EQ(0, trader_.queryAccount());
    ASSERT_TRUE(waitUntil([this]() { return api_.n_qry_acct_.load() == 1 && trader_.inQuery(); }, 2000));
    CThostFtdcRspInfoField error;
    fillErr(error, 42, "fake query error");
    trader_.OnRspQryTradingAccount(nullptr, &error, 1, true);
    ASSERT_FALSE(trader_.inQuery());
    const auto start = std::chrono::steady_clock::now();
    trader_.disconnect();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(), 2000);
    EXPECT_FALSE(trader_.hasWorker());
    EXPECT_EQ(1, api_.n_release_.load());
    // Fake deliberately retains the adapter; this does not assert an SDK guarantee.
    trader_.OnRspQryTradingAccount(nullptr, &error, 1, true);
    EXPECT_EQ(1, api_.n_release_.load());
}
