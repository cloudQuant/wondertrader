#include "t6_ctploader_yaml_factory_stub.hpp"

#include <cstring>

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
	// K02=B（2026-11-04）：run() 在所有模式下等待加载器终态再返回。假 API 不联网，这里经真实 SPI
	// 链路（前置连接→认证→登录→合约查询入列，费率批次进入进行中）后报告前置断开，SPI 发布失败终态，
	// run() 随即返回而不是无限等待。
	if (spi != nullptr)
	{
		CThostFtdcRspInfoField ok;
		std::memset(&ok, 0, sizeof(ok));
		CThostFtdcRspUserLoginField login;
		std::memset(&login, 0, sizeof(login));
		login.FrontID = 1;
		login.SessionID = 2;
		std::strncpy(login.MaxOrderRef, "1", sizeof(login.MaxOrderRef) - 1);
		spi->OnFrontConnected();
		spi->OnRspAuthenticate(nullptr, &ok, 1, true);
		spi->OnRspUserLogin(&login, &ok, 2, true);
		spi->OnFrontDisconnected(0x1001);
	}
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
