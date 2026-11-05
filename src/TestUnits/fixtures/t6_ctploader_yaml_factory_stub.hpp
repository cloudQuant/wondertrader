#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
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
	int ReqQryInstrument(CThostFtdcQryInstrumentField* pField, int nRequestID) override;

private:
	// I03-NEW-115 (test-side TSan): Init() waits until the SPI query worker
	// has dispatched the queued instrument query before reporting the front
	// disconnect, so the worker's stub call happens-before run()'s teardown.
	std::mutex _qry_mtx;
	std::condition_variable _qry_cv;
	bool _qry_dispatched = false;
};

T6YamlCtpApiStub& t6_yaml_ctp_api();
