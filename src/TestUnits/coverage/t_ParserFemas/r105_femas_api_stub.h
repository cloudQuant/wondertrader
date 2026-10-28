/*
 * r105_femas_api_stub.h — CUstpFtdcMduserApi（Femas3.02）的可控空实现
 *
 * ParserFemas::init() 通过 DLLHelper 加载 USTPmduserapiAF 并 dlsym
 * （ParserFemas.cpp:109-131）；本机没有可 dlopen 的 Femas 行情库，init 末尾
 * 会空函数指针调用（BUG-T3-001）。用例因此不调 init 的后半段，而是用测试侧
 * 白盒把 m_pUserAPI 指向本 stub，再走公开的 connect/回调/subscribe。
 *
 * stub 只记录调用并返回可控值，不打开 socket、不加载厂商库、不起线程。
 */
#pragma once

#include <API/Femas3.02/USTPFtdcMduserApi.h>

#include <set>
#include <string>

class R105FemasMdApiStub : public CUstpFtdcMduserApi
{
public:
	int release_calls = 0;
	int init_calls = 0;
	int join_calls = 0;
	int spi_regs = 0;
	int front_regs = 0;

	int login_calls = 0;
	int login_ret = 0;
	int logout_calls = 0;

	int sub_calls = 0;
	int sub_ret = 0;
	int sub_count = 0;
	int unsub_calls = 0;

	std::string front;
	std::string broker;
	std::string user;
	std::string pass;
	std::string product;
	int last_reqid = -1;

	std::set<std::string> sub_codes;
	CUstpFtdcMduserSpi* last_spi = NULL;

	const char* trading_day = "20260301";

	void Release() override { ++release_calls; }
	void Init() override { ++init_calls; }
	int Join() override { ++join_calls; return 0; }
	void SetUseMultiChannel(bool) override {}
	void RegisterMultiChannel(const char*) override {}
	const char* GetTradingDay() override { return trading_day; }

	void RegisterFront(char* p) override
	{
		++front_regs;
		front = (p == NULL) ? "" : p;
	}
	void RegisterNameServer(char*) override {}
	void RegisterSpi(CUstpFtdcMduserSpi* p) override
	{
		++spi_regs;
		last_spi = p;
	}
	int RegisterCertificateFile(const char*, const char*, const char*, const char*) override { return 0; }
	void SubscribeMarketDataTopic(int, USTP_TE_RESUME_TYPE) override {}

	int SubMarketData(char* pp[], int n) override
	{
		++sub_calls;
		sub_count = n;
		for (int i = 0; i < n; ++i)
			sub_codes.insert((pp[i] == NULL) ? "" : pp[i]);
		return sub_ret;
	}
	int UnSubMarketData(char* pp[], int n) override
	{
		++unsub_calls;
		(void)pp;
		(void)n;
		return sub_ret;
	}
	int GetMarketTopic(char*) override { return 0; }
	int GetMarketData(char*, char*) override { return 0; }
	void SetHeartbeatTimeout(unsigned int) override {}

	int ReqUserLogin(CUstpFtdcReqUserLoginField* p, int id) override
	{
		++login_calls;
		last_reqid = id;
		if (p != NULL)
		{
			broker = p->BrokerID;
			user = p->UserID;
			pass = p->Password;
			product = p->UserProductInfo;
		}
		return login_ret;
	}
	int ReqUserLogout(CUstpFtdcReqUserLogoutField*, int) override
	{
		++logout_calls;
		return 0;
	}
};
