#include "t6_ctploader_yaml_factory_stub.hpp"

namespace
{
	T6YamlCtpApiStub g_api;
}

T6YamlCtpApiStub& t6_yaml_ctp_api()
{
	return g_api;
}

void T6YamlCtpApiStub::reset()
{
	releaseRegisteredSpi();
	flow_path.clear();
	front.clear();
	create_calls = 0;
	register_spi_calls = 0;
	subscribe_public_calls = 0;
	subscribe_private_calls = 0;
	init_calls = 0;
	join_calls = 0;
	call_order.clear();
	n_calls_ = 0;
	stub_ret_ = 0;
}

void T6YamlCtpApiStub::releaseRegisteredSpi()
{
	delete spi;
	spi = nullptr;
}

void T6YamlCtpApiStub::RegisterSpi(CThostFtdcTraderSpi* value)
{
	spi = static_cast<CTraderSpi*>(value);
	++register_spi_calls;
	call_order.emplace_back("RegisterSpi");
}

void T6YamlCtpApiStub::SubscribePublicTopic(THOST_TE_RESUME_TYPE)
{
	++subscribe_public_calls;
	call_order.emplace_back("SubscribePublicTopic");
}

void T6YamlCtpApiStub::SubscribePrivateTopic(THOST_TE_RESUME_TYPE)
{
	++subscribe_private_calls;
	call_order.emplace_back("SubscribePrivateTopic");
}

void T6YamlCtpApiStub::RegisterFront(char* value)
{
	front = value == nullptr ? "" : value;
	call_order.emplace_back("RegisterFront");
}

void T6YamlCtpApiStub::Init()
{
	++init_calls;
	call_order.emplace_back("Init");
}

int T6YamlCtpApiStub::Join()
{
	++join_calls;
	call_order.emplace_back("Join");
	return 0;
}

// The dedicated test executable does not link the vendor CTP archive. This
// test-only definition satisfies CTPLoader.cpp's static factory reference.
CThostFtdcTraderApi* CThostFtdcTraderApi::CreateFtdcTraderApi(const char* flowPath)
{
	T6YamlCtpApiStub& api = t6_yaml_ctp_api();
	api.flow_path = flowPath == nullptr ? "" : flowPath;
	++api.create_calls;
	api.call_order.emplace_back("CreateFtdcTraderApi");
	return &api;
}
