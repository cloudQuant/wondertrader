/*
 * r105_ctp_api_stub.h — CThostFtdcMdApi（CTP6.6.9）的可控空实现
 *
 * 只实现厂商头里 13 个纯虚函数，记录请求参数并返回 stub_ret_，
 * 不打开 socket、不启动线程、不加载厂商动态库。
 *
 * 注入方式：ParserCTP::init() 在 __APPLE__ 分支直接取
 *   CThostFtdcMdApi::CreateFtdcMdApi
 * 的函数地址（ParserCTP.cpp:119-120），本文件在测试 TU 里给出同名强定义，
 * 让 init 拿到这里的 stub。厂商静态归档里的同名成员不会被抽取：
 * ParserCTP.o 对厂商库只有这一个未定义引用（nm 实测位于
 * ThostFtdcMdApiImpl.o），因此链接期不会重复定义。
 */
#pragma once

#include "ParserCTP/ParserCTP.h"

#include <set>
#include <string>

class R105CtpMdApiStub : public CThostFtdcMdApi
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

	std::string flow_path;
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
	void Init() override { ++init_calls; }
	int Join() override { ++join_calls; return 0; }
	const char* GetTradingDay() override { return trading_day; }

	void RegisterFront(char* p) override
	{
		++front_regs;
		front = (p == NULL) ? "" : p;
	}
	void RegisterNameServer(char*) override {}
	void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
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
	int ReqQryMulticastInstrument(CThostFtdcQryMulticastInstrumentField*, int) override { return 0; }
};

namespace r105_ctp {

inline R105CtpMdApiStub*& factory_target()
{
	static R105CtpMdApiStub* target = NULL;
	return target;
}

}	// namespace r105_ctp

CThostFtdcMdApi* CThostFtdcMdApi::CreateFtdcMdApi(const char* pszFlowPath, const bool, const bool)
{
	R105CtpMdApiStub* stub = r105_ctp::factory_target();
	if (stub != NULL)
		stub->flow_path = (pszFlowPath == NULL) ? "" : pszFlowPath;
	return stub;
}
