#pragma once

#include <string>
#include <vector>

#include "../coverage/t_CTPLoader/t26_ctp_api_stub.hpp"

class T6YamlCtpApiStub : public T26CtpApiStub
{
public:
	std::string flow_path;
	std::string front;
	CTraderSpi* spi = nullptr;
	int create_calls = 0;
	int register_spi_calls = 0;
	int subscribe_public_calls = 0;
	int subscribe_private_calls = 0;
	int init_calls = 0;
	int join_calls = 0;
	std::vector<std::string> call_order;

	void reset();
	void releaseRegisteredSpi();

	void RegisterSpi(CThostFtdcTraderSpi* value) override;
	void SubscribePublicTopic(THOST_TE_RESUME_TYPE) override;
	void SubscribePrivateTopic(THOST_TE_RESUME_TYPE) override;
	void RegisterFront(char* value) override;
	void Init() override;
	int Join() override;
};

T6YamlCtpApiStub& t6_yaml_ctp_api();
