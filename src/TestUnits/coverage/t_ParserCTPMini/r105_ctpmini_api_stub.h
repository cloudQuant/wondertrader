/*
 * r105_ctpmini_api_stub.h — CThostFtdcMdApi（CTPMini1.5.8）的可控空实现
 *
 * ParserCTPMini::init() 在非 Windows 平台通过 DLLHelper 加载厂商库并 dlsym
 * （ParserCTPMini.cpp:109-132）；本机没有可 dlopen 的 mini 行情库，init 的
 * 后半段会空函数指针调用（BUG-T4-009），因此用例不调 init，而是通过测试侧
 * 白盒把 m_pUserAPI 指向本 stub，再走公开的 connect/回调/subscribe。
 *
 * stub 只记录调用并返回可控值，不打开 socket、不加载厂商库、不起线程。
 */
#pragma once

#include <API/CTPMini1.5.8/ThostFtdcMdApi.h>

#include <set>
#include <string>

class R105CtpMiniMdApiStub : public CThostFtdcMdApi
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
	CThostFtdcMdSpi* last_spi = NULL;

	const char* trading_day = "20260301";

	void Release() override { ++release_calls; }
	void Init(bool) override { ++init_calls; }
	int Join() override { ++join_calls; return 0; }
	const char* GetTradingDay() override { return trading_day; }

	void RegisterFront(char* p) override
	{
		++front_regs;
		front = (p == NULL) ? "" : p;
	}
	void RegisterSpi(CThostFtdcMdSpi* p) override
	{
		++spi_regs;
		last_spi = p;
	}

	int SubscribeMarketData(char* pp[], int n) override
	{
		++sub_calls;
		sub_count = n;
		for (int i = 0; i < n; ++i)
			sub_codes.insert((pp[i] == NULL) ? "" : pp[i]);
		return sub_ret;
	}
	int UnSubscribeMarketData(char* pp[], int n) override
	{
		++unsub_calls;
		(void)pp;
		(void)n;
		return sub_ret;
	}
	int SubscribeForQuoteRsp(char**, int) override { return 0; }
	int UnSubscribeForQuoteRsp(char**, int) override { return 0; }

	int ReqUserLogin(CThostFtdcReqUserLoginField* p, int id) override
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
	int ReqUserLogout(CThostFtdcUserLogoutField*, int) override
	{
		++logout_calls;
		return 0;
	}
};
