/*
 * r105_ctpopt_api_stub.h — ctp_sopt::CThostFtdcMdApi（CTPOpt3.5.8）的可控空实现
 *
 * ParserCTPOpt::init() 非 Windows 分支通过 DLLHelper 加载 soptthostmduserapi_se
 * 并 dlsym 带命名空间的符号（ParserCTPOpt.cpp:110-132）；本机没有可 dlopen 的
 * 期权行情库，init 末尾会空函数指针调用（T2/T4 均有记录）。用例因此不调 init
 * 的后半段，而是用测试侧白盒把 m_pUserAPI 指向本 stub，再走公开的
 * connect/回调/subscribe。
 *
 * stub 只记录调用并返回可控值，不打开 socket、不加载厂商库、不起线程。
 */
#pragma once

#include <API/CTPOpt3.5.8/ThostFtdcMdApi.h>

#include <set>
#include <string>

class R105CtpOptMdApiStub : public ctp_sopt::CThostFtdcMdApi
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
	ctp_sopt::CThostFtdcMdSpi* last_spi = NULL;

	const char* trading_day = "20260301";

	void Release() override { ++release_calls; }
	void Init() override { ++init_calls; }
	int Join() override { ++join_calls; return 0; }
	const char* GetTradingDay() override { return trading_day; }

	void RegisterFront(char* p) override
	{
		++front_regs;
		front = (p == NULL) ? "" : p;
	}
	void RegisterNameServer(char*) override {}
	void RegisterFensUserInfo(ctp_sopt::CThostFtdcFensUserInfoField*) override {}
	void RegisterSpi(ctp_sopt::CThostFtdcMdSpi* p) override
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

	int ReqUserLogin(ctp_sopt::CThostFtdcReqUserLoginField* p, int id) override
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
	int ReqUserLogout(ctp_sopt::CThostFtdcUserLogoutField*, int) override
	{
		++logout_calls;
		return 0;
	}
};
