#include <gtest/gtest.h>

#include <stdexcept>

#include "WtCore/ParserAdapter.h"
#include "Includes/WTSVariant.hpp"

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
			if (_shouldThrow)
				throw std::runtime_error("controlled parser init failure");
			return false;
		}

		bool connect() override
		{
			++connect_calls;
			return true;
		}

		void registerSpi(wtp::IParserSpi*) override {}
		void release() override
		{
			++release_calls;
			if (_releaseCount != nullptr)
				++(*_releaseCount);
		}
		void proveQuiescent() { _quiesced = true; }

		int connect_calls = 0;
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

	EXPECT_FALSE(adapter.initExt("false_init", api, nullptr, nullptr, nullptr));
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	EXPECT_EQ(0, api->connect_calls);
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

	EXPECT_FALSE(adapter.initExt("throwing_init", api, nullptr, nullptr, nullptr));
	EXPECT_FALSE(adapter.isInitialized());
	EXPECT_TRUE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	EXPECT_EQ(0, api->connect_calls);
	EXPECT_EQ(0, released);
	EXPECT_EQ(0, destroyed);

	api->proveQuiescent();
	adapter.release();
	EXPECT_EQ(1, released);
	EXPECT_EQ(1, destroyed);
	EXPECT_EQ(0, unsafe_destroy);
	EXPECT_FALSE(adapter.hasApi());
}

TEST(WtCoreParserAdapterInitFailure, missingModuleIsSafePreApiFailure)
{
	wtp::WTSVariant* cfg = wtp::WTSVariant::createObject();
	ASSERT_NE(nullptr, cfg);
	wtp::ParserAdapter adapter;

	EXPECT_FALSE(adapter.init("missing_module", cfg, nullptr, nullptr, nullptr));
	EXPECT_FALSE(adapter.hasApi());
	EXPECT_FALSE(adapter.run());
	// init() rolled back its retained config because no plugin entry point ran.
	EXPECT_EQ(1u, cfg->retainCount());
	cfg->release();
}
