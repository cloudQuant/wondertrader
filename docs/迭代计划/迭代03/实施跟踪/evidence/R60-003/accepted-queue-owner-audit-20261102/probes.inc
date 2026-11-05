	class R60QueueBlockSink : public R60MockerSink
	{
	public:
		std::atomic<bool> entered{false};
		std::atomic<bool> resume{false};
		void handleEvent(WTSTraderEvent e, int32_t rc) override
		{
			entered.store(true);
			while (!resume.load()) std::this_thread::yield();
			R60MockerSink::handleEvent(e, rc);
		}
	};

	void probeAcceptedQueue(bool stopBeforeUnblock)
	{
		R60QueueBlockSink sink;
		TraderMocker api;
		api.registerSpi(&sink);
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("span", 5); cfg->append("mockerid", 9698);
		cfg->append("maxqty", 10); cfg->append("minqty", 1); cfg->append("udp_port", pick_free_udp_port());
		const bool ok = api.init(cfg);
		cfg->release();
		ASSERT_TRUE(ok);
		api.connect();
		const bool entered = wait_until([&] { return sink.entered.load(); });
		if (!entered) { sink.resume.store(true); api.release(); FAIL() << "connect callback not entered"; }
		WTSEntrust* order = WTSEntrust::create("no_contract", 1, 100, "SHFE");
		EXPECT_EQ(0, api.orderInsert(order));
		EXPECT_EQ(0, api.queryAccount());
		EXPECT_EQ(2u, order->retainCount());
		if (stopBeforeUnblock)
		{
			std::thread stopper([&] { api.release(); });
			const bool closed = wait_until([&] { return api.queryAccount() < 0; });
			EXPECT_TRUE(closed);
			sink.resume.store(true);
			stopper.join();
		}
		else
		{
			sink.resume.store(true);
			EXPECT_TRUE(wait_until([&] { return sink.n_entrust() == 1 && sink.n_account() == 1; }));
			api.release();
		}
		EXPECT_EQ(1u, sink.n_entrust()) << "accepted order received no result";
		EXPECT_GE(sink.n_account(), 1u) << "accepted query received no result";
		EXPECT_EQ(1u, order->retainCount()) << "queued order remains retained after release";
		order->release();
	}

	TEST_F(R60MockerTest, audit_accepted_queue_without_stop_control) { probeAcceptedQueue(false); }
	TEST_F(R60MockerTest, audit_accepted_queue_stop_does_not_lose_results) { probeAcceptedQueue(true); }
	TEST_F(R60MockerTest, audit_spi_unregister_does_not_drain_captured_listener)
	{
		class BlockingEntrustSink : public R60MockerSink
		{
		public:
			std::atomic<bool> entered{false};
			std::atomic<bool> resume{false};
			void onRspEntrust(WTSEntrust* entrust, WTSError* err) override
			{
				R60MockerSink::onRspEntrust(entrust, err);
				entered.store(true);
				while (!resume.load()) std::this_thread::yield();
			}
		};
		BlockingEntrustSink sink;
		TraderMocker api;
		api.registerSpi(&sink);
		WTSVariant* cfg = WTSVariant::createObject();
		cfg->append("span", 5); cfg->append("mockerid", 9699);
		cfg->append("maxqty", 10); cfg->append("minqty", 1); cfg->append("udp_port", pick_free_udp_port());
		const bool initialized = api.init(cfg);
		cfg->release();
		ASSERT_TRUE(initialized);
		api.connect();
		ASSERT_TRUE(wait_until([&] { return sink.n_event() == 1; }));
		WTSEntrust* order = make_entrust("ag2512", "SHFE", ag_ct_, 7000.0, 2, WDT_LONG, WOT_OPEN);
		EXPECT_EQ(0, api.orderInsert(order));
		order->release();
		const bool entered = wait_until([&] { return sink.entered.load(); });
		EXPECT_TRUE(entered);
		api.registerSpi(nullptr);
		sink.resume.store(true);
		const bool pushed = wait_until([&] { return sink.n_push_order() == 1; });
		api.release();
		EXPECT_TRUE(pushed) << "expected the captured listener to receive onPushOrder after unregister";
	}
