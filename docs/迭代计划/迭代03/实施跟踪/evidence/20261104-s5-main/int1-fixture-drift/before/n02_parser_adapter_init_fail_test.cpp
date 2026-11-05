#include <gtest/gtest.h>

#include <stdexcept>

#include "WtCore/ParserAdapter.h"
#include "Includes/WTSVariant.hpp"
#include "../p02_wtcore_engines2/r42_core_fakes.h"

namespace
{
	class InitFailureParserApi final : public wtp::IParserApi
	{
	public:
		InitFailureParserApi(bool shouldThrow, int* destroyed, int* releaseCount, int* unsafeDestroy)
			: _shouldThrow(shouldThrow), _destroyed(destroyed), _releaseCount(releaseCount), _unsafeDestroy(unsafeDestroy) {}

		~InitFailureParserApi() override
		{
			if (_destroyed != nullptr)
				++(*_destroyed);
			if (!_quiesced && _unsafeDestroy != nullptr)
				++(*_unsafeDestroy);
		}

		bool init(wtp::WTSVariant*) override
		{
			++init_calls;
			if (_shouldThrow)
				throw std::runtime_error("controlled parser init failure");
			return false;
		}

		bool connect() override
		{
			++connect_calls;
			return true;
		}

		void registerSpi(wtp::IParserSpi*) override { ++register_calls; }
		void subscribe(const wtp::CodeSet&) override { ++subscribe_calls; }
		void release() override
		{
			++release_calls;
			if (_releaseCount != nullptr)
				++(*_releaseCount);
		}
		void proveQuiescent() { _quiesced = true; }

		int connect_calls = 0;
		int init_calls = 0;
		int register_calls = 0;
		int subscribe_calls = 0;
		int release_calls = 0;

	private:
		bool _shouldThrow;
		bool _quiesced = false;
		int* _destroyed;
		int* _releaseCount;
		int* _unsafeDestroy;
	};
}

TEST(WtCoreParserAdapterInitFailure, falseResultCannotRunApi)
{
	int destroyed = 0;
	int released = 0;
	int unsafe_destroy = 0;
	InitFailureParserApi* api = new InitFailureParserApi(false, &destroyed, &released, &unsafe_destroy);
	wtp::ParserAdapter adapter;
	r42fakes::R42BaseDataMgr bd;

	EXPECT_FALSE(adapter.initExt("false_init", api, nullptr, &bd, nullptr));
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	EXPECT_EQ(0, api->connect_calls);
	EXPECT_EQ(1, api->init_calls);
	EXPECT_EQ(1, api->register_calls);
	EXPECT_EQ(0, api->subscribe_calls);
	EXPECT_EQ(0, released);
	EXPECT_EQ(0, destroyed);

	// The fake's explicit drain proof authorizes teardown; release() itself is
	// deliberately not treated as that proof.
	api->proveQuiescent();
	adapter.release();
	EXPECT_EQ(1, released);
	EXPECT_EQ(1, destroyed);
	EXPECT_EQ(0, unsafe_destroy);
	EXPECT_FALSE(adapter.hasApi());
}

TEST(WtCoreParserAdapterInitFailure, exceptionCannotRunApi)
{
	int destroyed = 0;
	int released = 0;
	int unsafe_destroy = 0;
	InitFailureParserApi* api = new InitFailureParserApi(true, &destroyed, &released, &unsafe_destroy);
	wtp::ParserAdapter adapter;
	r42fakes::R42BaseDataMgr bd;

	EXPECT_FALSE(adapter.initExt("throwing_init", api, nullptr, &bd, nullptr));
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	EXPECT_EQ(0, api->connect_calls);
	EXPECT_EQ(1, api->init_calls);
	EXPECT_EQ(1, api->register_calls);
	EXPECT_EQ(0, api->subscribe_calls);
	EXPECT_EQ(0, released);
	EXPECT_EQ(0, destroyed);

	api->proveQuiescent();
	adapter.release();
	EXPECT_EQ(1, released);
	EXPECT_EQ(1, destroyed);
	EXPECT_EQ(0, unsafe_destroy);
	EXPECT_FALSE(adapter.hasApi());
}

TEST(WtCoreParserAdapterInitFailure, nullManagerRetainsTransferredApiWithoutCallingProvider)
{
	int destroyed = 0;
	int released = 0;
	int unsafe_destroy = 0;
	InitFailureParserApi* api = new InitFailureParserApi(false, &destroyed, &released, &unsafe_destroy);
	wtp::ParserAdapter adapter;

	EXPECT_FALSE(adapter.initExt("null_manager", api, nullptr, nullptr, nullptr));
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	EXPECT_EQ(0, api->connect_calls);
	EXPECT_EQ(0, api->init_calls);
	EXPECT_EQ(0, api->register_calls);
	EXPECT_EQ(0, api->subscribe_calls);
	EXPECT_EQ(0, released);
	EXPECT_EQ(0, destroyed);
	EXPECT_EQ(0, unsafe_destroy);

	// initExt has transferred ownership. Only an explicit provider quiescence
	// proof makes it safe for the adapter to release and destroy this candidate.
	api->proveQuiescent();
	adapter.release();
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_EQ(1, released);
	EXPECT_EQ(1, destroyed);
	EXPECT_EQ(0, unsafe_destroy);
}

TEST(WtCoreParserAdapterInitFailure, missingModuleIsSafePreApiFailure)
{
	wtp::WTSVariant* cfg = wtp::WTSVariant::createObject();
	ASSERT_NE(nullptr, cfg);
	wtp::ParserAdapter adapter;
	r42fakes::R42BaseDataMgr bd;

	EXPECT_FALSE(adapter.init("missing_module", cfg, nullptr, &bd, nullptr));
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	// init() rolled back its retained config because no plugin entry point ran.
	EXPECT_EQ(1u, cfg->retainCount());
	cfg->release();
}
