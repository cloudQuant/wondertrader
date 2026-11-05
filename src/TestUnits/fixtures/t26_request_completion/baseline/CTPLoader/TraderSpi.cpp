#include <iostream>
#include <set>
#include <stdint.h>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <fstream>
#include <exception>
#include <filesystem>
#include <string_view>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#ifdef WT_T26_LOADJSON_TEST_HOOKS
#include <algorithm>
#include <vector>
#endif

#include "../Share/StrUtil.hpp"
#include "../Share/fmtlib.h"
#include "../Share/charconv.hpp"
#include "../Share/Converter.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/decimal.h"

#include "../Includes/WTSVariant.hpp"

#include "../WTSUtils/WTSCfgLoader.h"

#include <boost/filesystem.hpp>

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
namespace rj = rapidjson;

#include "TraderSpi.h"

inline const char* encode_text(const char* s)
{
#ifdef _MSC_VER
	return s;
#else
	static std::string ret;
	ret = ChartoUTF8(s);
	return ret.c_str();
#endif
}

USING_NS_WTP;

#ifdef WT_T26_FEES_TEST_HOOKS
// Faults are compiled only into the isolated test target.
extern bool wtT26FeesFailIo(int stage);
#endif

namespace
{
bool isValidFeeValue(double value)
{
	// CTP uses these finite maxima as missing-value markers (see checkValid).
	return std::isfinite(value) && value != DBL_MAX && value != FLT_MAX;
}

bool publishFeeFile(const std::string& path, const char* data, size_t size)
{
	static std::atomic<unsigned long long> sequence{0};
#ifdef _WIN32
	const int processId = _getpid();
#else
	struct stat existing{};
	bool preserveMode = false;
	if (lstat(path.c_str(), &existing) == 0)
	{
		// Do not replace a link or change the owner of a file belonging to
		// another account when committing the new template.
		if (!S_ISREG(existing.st_mode) || existing.st_uid != geteuid()) return false;
		// Atomic rename must not silently bypass write permission on an
		// existing template that the previous in-place writer could not open.
		const int existingFd = open(path.c_str(), O_WRONLY | O_NOFOLLOW);
		if (existingFd < 0) return false;
		struct stat opened{};
		const bool sameFile = fstat(existingFd, &opened) == 0 && S_ISREG(opened.st_mode) &&
			opened.st_dev == existing.st_dev && opened.st_ino == existing.st_ino;
		const bool closed = close(existingFd) == 0;
		if (!sameFile || !closed) return false;
		preserveMode = true;
	}
	else if (errno != ENOENT) return false;
	const int processId = getpid();
#endif
	std::string temp;
	int fd = -1;
	for (unsigned attempt = 0; attempt < 128; ++attempt)
	{
		temp = path + ".wtfees." + std::to_string(processId) + "." +
			std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
#ifdef _WIN32
		fd = _open(temp.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
			_S_IREAD | _S_IWRITE);
#else
		fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
#endif
		if (fd >= 0 || errno != EEXIST) break;
	}
	if (fd < 0) return false;
#ifndef _WIN32
	struct stat staged{};
	if (preserveMode && (fstat(fd, &staged) != 0 ||
		(staged.st_gid != existing.st_gid && fchown(fd, static_cast<uid_t>(-1), existing.st_gid) != 0) ||
		fchmod(fd, existing.st_mode & 0777) != 0))
	{
		close(fd);
		std::remove(temp.c_str());
		return false;
	}
#endif
#ifdef _WIN32
	FILE* file = _fdopen(fd, "wb");
#else
	FILE* file = fdopen(fd, "wb");
#endif
	if (!file)
	{
#ifdef _WIN32
		_close(fd);
#else
		close(fd);
#endif
		std::remove(temp.c_str());
		return false;
	}
	bool ok = true;
#ifdef WT_T26_FEES_TEST_HOOKS
	if (wtT26FeesFailIo(1)) ok = false;
#endif
	if (ok && std::fwrite(data, 1, size, file) != size) ok = false;
#ifdef WT_T26_FEES_TEST_HOOKS
	if (wtT26FeesFailIo(2)) ok = false;
#endif
	if (std::fflush(file) != 0) ok = false;
	if (std::fclose(file) != 0) ok = false;
#ifdef WT_T26_FEES_TEST_HOOKS
	if (wtT26FeesFailIo(3)) ok = false;
	if (wtT26FeesFailIo(4)) ok = false;
#endif
	if (ok)
	{
#ifdef _WIN32
		// ReplaceFile preserves the destination's attributes and ACL. If no
		// destination exists, MoveFileEx without REPLACE refuses a racing create.
		ok = ReplaceFileA(path.c_str(), temp.c_str(), NULL, 0, NULL, NULL) != 0;
		if (!ok && GetLastError() == ERROR_FILE_NOT_FOUND)
			ok = MoveFileExA(temp.c_str(), path.c_str(), 0) != 0;
#else
		ok = std::rename(temp.c_str(), path.c_str()) == 0;
#endif
	}
	if (!ok) std::remove(temp.c_str());
	return ok;
}
}

#ifdef WT_T26_LOADJSON_TEST_HOOKS
namespace t26_test::load_json_test_hook
{
	static size_t load_count = 0;
	static size_t release_count = 0;
	static std::vector<WTSVariant*> loaded_variants;
	static std::vector<WTSVariant*> released_variants;

	void reset()
	{
		load_count = 0;
		release_count = 0;
		loaded_variants.clear();
		released_variants.clear();
	}

	void note_load() { ++load_count; }
	void note_loaded(WTSVariant* value)
	{
		if (value != nullptr)
			loaded_variants.push_back(value);
	}
	void note_release(WTSVariant* value)
	{
		++release_count;
		released_variants.push_back(value);
	}
	size_t loads() { return load_count; }
	size_t releases() { return release_count; }
	bool every_loaded_variant_released_once()
	{
		if (loaded_variants.size() != released_variants.size())
			return false;

		std::vector<WTSVariant*> unmatched = loaded_variants;
		for (WTSVariant* released : released_variants)
		{
			auto it = std::find(unmatched.begin(), unmatched.end(), released);
			if (it == unmatched.end())
				return false;
			unmatched.erase(it);
		}
		return unmatched.empty();
	}
}

namespace
{
	class T26ObservedCfgLoader
	{
	public:
		static WTSVariant* load_from_file(const char* filename)
		{
			t26_test::load_json_test_hook::note_load();
			WTSVariant* value = WTSCfgLoader::load_from_file(filename);
			t26_test::load_json_test_hook::note_loaded(value);
			return value;
		}

		static WTSVariant* load_from_file(const std::string& filename)
		{
			t26_test::load_json_test_hook::note_load();
			WTSVariant* value = WTSCfgLoader::load_from_file(filename);
			t26_test::load_json_test_hook::note_loaded(value);
			return value;
		}
	};
}

#define WTSCfgLoader T26ObservedCfgLoader
#endif

extern std::map<std::string, std::string>	MAP_NAME;
extern std::map<std::string, std::string>	MAP_SESSION;
extern std::set<std::string>				SET_FILTERS;

#ifdef _MSC_VER
#pragma warning(disable : 4996)
#endif

// USER_API参数
extern CThostFtdcTraderApi* pUserApi;

// 配置参数
extern std::string	FRONT_ADDR;	// 前置地址
extern std::string	BROKER_ID;	// 经纪公司代码
extern std::string	INVESTOR_ID;// 投资者代码
extern std::string	PASSWORD;	// 用户密码
extern std::string	SAVEPATH;	//保存位置
extern std::string	APPID;
extern std::string	AUTHCODE;
extern uint32_t		CLASSMASK;
extern bool			ONLYINCFG;
extern bool			INCREMENTAL;//是否增量拉取，默认false
extern bool			QRYFEES;	//是否查询费率，默认false

extern std::string COMM_FILE;		//输出的品种文件名
extern std::string CONT_FILE;		//输出的合约文件名
extern std::string FEES_FILE;		//输出的手续费文件名

// 请求编号
extern int iRequestID;

// 会话参数
TThostFtdcFrontIDType	FRONT_ID;	//前置编号
TThostFtdcSessionIDType	SESSION_ID;	//会话编号
TThostFtdcOrderRefType	ORDER_REF;	//报单引用

CommodityMap _commodities;
ContractMap _contracts;
FeeMap		_fees;
MarginMap	_margins;

std::string extractProductID(const char* instrument)
{
	std::string strRet;
	int nLen = 0;
	while ('A' <= instrument[nLen] && instrument[nLen] <= 'z')
	{
		strRet += instrument[nLen];
		nLen++;
	}

	return strRet;
}

std::string extractProductName(const char* cname)
{
	std::string strRet;
	if (cname == NULL)
		return strRet;

	const size_t length = strlen(cname);
	if (length == 0)
		return strRet;

	size_t prefixLength = length;
	while (prefixLength > 1 && std::isdigit(static_cast<unsigned char>(cname[prefixLength - 1])))
	{
		prefixLength--;
	}

	strRet.append(cname, prefixLength);
	return strRet;
}

template <size_t N>
static bool hasTerminator(const char (&value)[N])
{
	return std::memchr(value, '\0', N) != NULL;
}

static bool hasAsciiDigit(std::string_view value)
{
	const size_t terminator = value.find('\0');
	if (terminator == std::string_view::npos)
		return false;
	value = value.substr(0, terminator);
	for (unsigned char ch : value)
	{
		if (ch >= '0' && ch <= '9')
			return true;
	}
	return false;
}

std::set<std::string>	prod_set;

inline double checkValid(double val)
{
	if (val == DBL_MAX || val == FLT_MAX)
		return 0;

	return val;
}


void CTraderSpi::OnFrontConnected()
{
	std::cerr << "--->>> " << "OnFrontConnected" << std::endl;
	///用户登录请求
	ReqAuth();
}

void CTraderSpi::ReqAuth()
{
	CThostFtdcReqAuthenticateField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, BROKER_ID.c_str());
	strcpy(req.UserID, INVESTOR_ID.c_str());
	//strcpy(req.UserProductInfo, m_strProdInfo.c_str());
	strcpy(req.AuthCode, AUTHCODE.c_str());
	strcpy(req.AppID, APPID.c_str());
	int iResult = pUserApi->ReqAuthenticate(&req, ++iRequestID);
	std::cerr << "--->>> Requesting authentication: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
}

void CTraderSpi::OnRspAuthenticate(CThostFtdcRspAuthenticateField */*pRspAuthenticateField*/, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	std::cerr << "--->>> " << "OnRspAuthenticate" << std::endl;
	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		ReqUserLogin();
	}
}

void CTraderSpi::ReqUserLogin()
{
	CThostFtdcReqUserLoginField req;
	memset(&req, 0, sizeof(req));
	strcpy(req.BrokerID, BROKER_ID.c_str());
	strcpy(req.UserID, INVESTOR_ID.c_str());
	strcpy(req.Password, PASSWORD.c_str());
	int iResult = pUserApi->ReqUserLogin(&req, ++iRequestID);
	std::cerr << "--->>> Requesting user login: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
}

void CTraderSpi::OnRspUserLogin(CThostFtdcRspUserLoginField *pRspUserLogin,
	CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	std::cerr << "--->>> " << "OnRspUserLogin" << std::endl;
	if (bIsLast && !IsErrorRspInfo(pRspInfo))
	{
		// 保存会话参数
		FRONT_ID = pRspUserLogin->FrontID;
		SESSION_ID = pRspUserLogin->SessionID;
		int iNextOrderRef = atoi(pRspUserLogin->MaxOrderRef);
		iNextOrderRef++;
		fmtutil::format_to(ORDER_REF, "{}", iNextOrderRef);
		///获取当前交易日
		_trading_day = atoi(pUserApi->GetTradingDay());

		LoadFromJson();

		ReqQryInstrument();
	}
}

void CTraderSpi::ReqQryInstrument()
{
	AppendQuery([]() {
		CThostFtdcQryInstrumentField req;
		memset(&req, 0, sizeof(req));
		int iResult = pUserApi->ReqQryInstrument(&req, ++iRequestID);
		std::cerr << "--->>> Quering instruments: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
		return true;
	});
}

void CTraderSpi::ReqQryCommission(const Contract& cInfo)
{
	AppendQuery([&cInfo]() {
		//auto it = _fees.find(cInfo.m_strProduct);
		//if (it != _fees.end())
		//	return false;

		CThostFtdcQryInstrumentCommissionRateField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, BROKER_ID.c_str());
		strcpy(req.InvestorID, INVESTOR_ID.c_str());
		const std::string& instrumentId = cInfo.m_strAltCode.empty() ? cInfo.m_strCode : cInfo.m_strAltCode;
		std::snprintf(req.InstrumentID, sizeof(req.InstrumentID), "%s", instrumentId.c_str());
		strcpy(req.ExchangeID, cInfo.m_strExchg.c_str());
		int iResult = pUserApi->ReqQryInstrumentCommissionRate(&req, ++iRequestID);
		std::cerr << "--->>> Quering commission ratio: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
		return true;
	});
}

void CTraderSpi::ReqQryMargin(const Contract& cInfo)
{
	AppendQuery([&cInfo]() {
		//auto it = _margins.find(cInfo.m_strProduct);
		//if (it != _margins.end())
		//	return false;

		CThostFtdcQryInstrumentMarginRateField req;
		memset(&req, 0, sizeof(req));
		strcpy(req.BrokerID, BROKER_ID.c_str());
		strcpy(req.InvestorID, INVESTOR_ID.c_str());
		const std::string& instrumentId = cInfo.m_strAltCode.empty() ? cInfo.m_strCode : cInfo.m_strAltCode;
		std::snprintf(req.InstrumentID, sizeof(req.InstrumentID), "%s", instrumentId.c_str());
		strcpy(req.ExchangeID, cInfo.m_strExchg.c_str());
		req.HedgeFlag = THOST_FTDC_HF_Speculation;
		int iResult = pUserApi->ReqQryInstrumentMarginRate(&req, ++iRequestID);
		std::cerr << "--->>> Quering margin ratio: " << ((iResult == 0) ? "succeed" : "failed") << std::endl;
		return true;
	});
}

void CTraderSpi::AppendQuery(const QueryTask& task)
{
	bool rejected = false;
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (_stopped)
		{
			rejected = true;
		}
		else
		{
			_queries.emplace(task);
			if (!_worker)
				_worker.reset(new boost::thread([this]() { RunQueryWorker(); }));
		}
	}

	if (rejected)
	{
		std::cerr << "--->>> Query rejected because CTP loader is stopping" << std::endl;
		return;
	}

	_query_ready.notify_one();
}

bool CTraderSpi::IsQueryQueueEmpty()
{
	std::lock_guard<std::mutex> lock(_mtx);
	return _queries.empty();
}

void CTraderSpi::RunQueryWorker()
{
	uint64_t last_qrytime = 0;
	for (;;)
	{
		QueryTask task;
		{
			std::unique_lock<std::mutex> lock(_mtx);
			_query_ready.wait(lock, [this]() { return _stopped || !_queries.empty(); });
			if (_stopped)
				return;
			if (_queries.empty())
				continue;

			// Keep the active task in the queue until its API call returns. This
			// preserves the existing queue-empty callback check while avoiding an
			// unlocked reference into the queue during a reentrant CTP callback.
			task = _queries.front();
		}

		for (;;)
		{
			uint64_t now = TimeUtils::getLocalTimeNow();
			if (now - last_qrytime > 1010)
				break;

			std::unique_lock<std::mutex> lock(_mtx);
			if (_query_ready.wait_for(lock, std::chrono::milliseconds(10), [this]() { return _stopped; }))
				return;
		}

		// This lock-held check is the task's commit/active linearization point:
		// a stop already requested cancels this pending task, while a stop set
		// after this check may wait for this committed task to finish. Keep the
		// API call outside the queue lock so callbacks can reenter AppendQuery.
		{
			std::lock_guard<std::mutex> lock(_mtx);
			if (_stopped)
				return;
		}

		bool ret = task();
		size_t queueLeft = 0;
		{
			std::lock_guard<std::mutex> lock(_mtx);
			if (!_queries.empty())
				_queries.pop();
			queueLeft = _queries.size();
		}

		if (ret)
			last_qrytime = TimeUtils::getLocalTimeNow();
		std::cerr << "--->>> Quering queue left: " << queueLeft << std::endl;
	}
}

CTraderSpi::~CTraderSpi()
{
	std::shared_ptr<boost::thread> worker;
	{
		std::lock_guard<std::mutex> lock(_mtx);
		_stopped = true;
		worker = _worker;
	}
	_query_ready.notify_all();

	if (worker && worker->joinable())
		worker->join();
}

inline bool isOption(TThostFtdcProductClassType pClass)
{
	if (pClass == THOST_FTDC_PC_Options || pClass == THOST_FTDC_PC_SpotOption)
		return true;

	return false;
}

inline bool isFuture(TThostFtdcProductClassType pClass)
{
	return pClass == THOST_FTDC_PC_Futures;
}

inline ContractCategory wrapCategory(TThostFtdcProductClassType cType)
{
	switch (cType)
	{
	case THOST_FTDC_PC_Futures: return CC_Future;
	case THOST_FTDC_PC_Options: return CC_FutOption;
	case THOST_FTDC_PC_Combination: return CC_Combination;
	case THOST_FTDC_PC_Spot: return CC_Spot;
	case THOST_FTDC_PC_EFP: return CC_EFP;
	case THOST_FTDC_PC_SpotOption: return CC_SpotOption;
	default:
		throw std::runtime_error("non implemented category");
	}
}

bool fillCzceCode(std::string_view code, uint32_t tdate, std::string& result)
{
	result.clear();
	const size_t terminator = code.find('\0');
	if (terminator == std::string_view::npos)
		return false;
	code = code.substr(0, terminator);
	if (code.empty())
		return false;

	static uint32_t yearFlag = UINT_MAX;
	static uint32_t yearPref = UINT_MAX;
	static uint32_t nextYPref = UINT_MAX;

	uint32_t pid_len = 0;
	while (pid_len < code.size() && (code[pid_len] < '0' || code[pid_len] > '9'))
	{
		pid_len++;
	}
	if (pid_len == code.size())
		return false;
	if (yearFlag == UINT_MAX)
	{
		yearFlag = tdate / 10000 % 10;
		yearPref = tdate / 100000 % 10;
		nextYPref = (yearPref + 1) % 10;
	}

	uint32_t cYFlag = static_cast<uint32_t>(code[pid_len] - '0');
	std::string expanded;
	if (yearFlag == 0 || (yearPref != 0 && cYFlag >= yearFlag))
	{
		expanded.append(code.data(), pid_len);
		expanded.append(convert::to_str(yearPref));
		expanded.append(code.data() + pid_len, code.size() - pid_len);
	}
	else
	{
		expanded.append(code.data(), pid_len);
		expanded.append(convert::to_str(nextYPref));
		expanded.append(code.data() + pid_len, code.size() - pid_len);
	}

	result.swap(expanded);
	return true;
}


void CTraderSpi::OnRspQryInstrument(CThostFtdcInstrumentField *pInstrument, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool bIsLast)
{
	if (!IsErrorRspInfo(pRspInfo))
	{
		if (pInstrument)
		{
			if (!hasTerminator(pInstrument->InstrumentID) ||
				!hasTerminator(pInstrument->ExchangeID) ||
				!hasTerminator(pInstrument->ProductID) ||
				!hasTerminator(pInstrument->InstrumentName) ||
				!hasTerminator(pInstrument->UnderlyingInstrID) ||
				!hasTerminator(pInstrument->OpenDate) ||
				!hasTerminator(pInstrument->ExpireDate))
			{
				std::cerr << "--->>> OnRspQryInstrument: unterminated instrument field, skipped" << std::endl;
			}
			else
			{
			std::string fullInstId = fmtutil::format<64>("{}.{}", pInstrument->ExchangeID, pInstrument->InstrumentID);
			auto it = _contracts.find(pInstrument->InstrumentID);
			if (it != _contracts.end())
			{
				std::cerr << "--->>> " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << " already exists, skipped" << std::endl;
				return;
			}

			bool bOption = isOption(pInstrument->ProductClass);
			bool bFuture = isFuture(pInstrument->ProductClass);
			const std::string_view instrumentCode(pInstrument->InstrumentID, sizeof(pInstrument->InstrumentID));
			const std::string_view underlyingCode(pInstrument->UnderlyingInstrID, sizeof(pInstrument->UnderlyingInstrID));

			bool isGranted = false;
			if (bOption && (CLASSMASK & 2) != 0)
				isGranted = true;
			else if (bFuture && (CLASSMASK & 1) != 0)
				isGranted = true;

			do 
			{
				if (isGranted)
				{
					if (strcmp(pInstrument->ExchangeID, "CZCE") == 0 &&
						(!hasAsciiDigit(instrumentCode) ||
						(pInstrument->UnderlyingInstrID[0] != '\0' && !hasAsciiDigit(underlyingCode))))
					{
						std::cerr << "--->>> OnRspQryInstrument: invalid CZCE code, skipped" << std::endl;
						break;
					}

					std::string pname = MAP_NAME[pInstrument->ProductID];
					if (pname.empty())
					{
						std::stringstream ss;
						ss << pInstrument->ExchangeID << "." << pInstrument->ProductID;
						pname = MAP_NAME[ss.str()];
					}

					std::string cname = "";
					if (pname.empty())
					{
						if (ONLYINCFG)
						{
							std::cerr << "--->>> OnRspQryInstrument: " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << std::endl;
							break;
						}

						cname = pInstrument->InstrumentName;
						pname = bFuture ? extractProductName(pInstrument->InstrumentName) : pInstrument->InstrumentName;

						std::cerr << "--->>> Name confirmed automatically: " << pInstrument->ExchangeID << "." << pInstrument->InstrumentID << std::endl;
					}
					else
					{
						if (bFuture)
						{
							std::string month = pInstrument->InstrumentID;
							month = month.substr(strlen(pInstrument->ProductID));
							cname = pname + month;
						}
						else if (bOption)
						{
							std::string underlyPID = extractProductID(pInstrument->UnderlyingInstrID);
							std::string month = pInstrument->InstrumentID;
							month = month.substr(underlyPID.size());
							cname = pname + month;
						}
						else
						{
							cname = pInstrument->InstrumentName;
						}

					}

					//合约名称转成UTF8
					cname = StrUtil::trim(cname.c_str());

					if (!EncodingHelper::isUtf8((unsigned char*)cname.c_str(), cname.size()))
						cname = ChartoUTF8(cname);

					Contract contract;
					if(strcmp(pInstrument->ExchangeID, "CZCE") == 0)
					{
						if (!fillCzceCode(instrumentCode, _trading_day, contract.m_strCode))
						{
							std::cerr << "--->>> OnRspQryInstrument: invalid CZCE code, skipped" << std::endl;
							break;
						}
						contract.m_strAltCode = pInstrument->InstrumentID;
					}
					else
					{
						contract.m_strCode = pInstrument->InstrumentID;
					}

					contract.m_strExchg = pInstrument->ExchangeID;
					contract.m_strName = cname;
					contract.m_strProduct = pInstrument->ProductID;

					contract.m_maxMktQty = pInstrument->MaxMarketOrderVolume;
					contract.m_maxLmtQty = pInstrument->MaxLimitOrderVolume;
					contract.m_minMktQty = pInstrument->MinMarketOrderVolume;
					contract.m_minLmtQty = pInstrument->MinLimitOrderVolume;

					contract.m_optType = bOption ? (OptionType)pInstrument->OptionsType : OT_None;
					if (strcmp(pInstrument->ExchangeID, "CZCE") == 0)
					{
						if (pInstrument->UnderlyingInstrID[0] != '\0' &&
							!fillCzceCode(underlyingCode, _trading_day, contract.m_strUnderlying))
						{
							std::cerr << "--->>> OnRspQryInstrument: invalid CZCE underlying code, skipped" << std::endl;
							break;
						}
					}
					else
						contract.m_strUnderlying = pInstrument->UnderlyingInstrID;
					contract.m_strikePrice = pInstrument->StrikePrice;
					contract.m_dUnderlyingScale = pInstrument->UnderlyingMultiple;

					contract.m_uOpenDate = convert::to_uint32(pInstrument->OpenDate);
					contract.m_uExpireDate = convert::to_uint32(pInstrument->ExpireDate);

					contract.m_dLongMarginRatio = checkValid(pInstrument->LongMarginRatio);
					contract.m_dShortMarginRatio = checkValid(pInstrument->ShortMarginRatio);

					std::string fullPid = fmtutil::format<64>("{}.{}", pInstrument->ExchangeID, pInstrument->ProductID);
					auto it = _commodities.find(fullPid);
					if (it == _commodities.end())
					{
						//品种名称也转成UTF8
						pname = StrUtil::trim(pname.c_str());
						if (!EncodingHelper::isUtf8((unsigned char*)pname.c_str(), pname.size()))
							pname = ChartoUTF8(pname);

						Commodity commInfo;
						commInfo.m_strProduct = pInstrument->ProductID;
						commInfo.m_strName = pname;
						commInfo.m_strExchg = pInstrument->ExchangeID;
						commInfo.m_strCurrency = "CNY";

						commInfo.m_strSession = MAP_SESSION[fullPid];
						if (commInfo.m_strSession.empty())
							std::cerr << "--->>> warning: No session configured for " << fullPid << std::endl;
						commInfo.m_ccCategory = wrapCategory(pInstrument->ProductClass);

						commInfo.m_uVolScale = (pInstrument->VolumeMultiple == 0 ? 1 : pInstrument->VolumeMultiple);
						commInfo.m_fPriceTick = pInstrument->PriceTick;

						CoverMode cm = CM_OpenCover;
						/*
						 *	By Wesley @ 2023.05.04
						 *	有用户反馈上期所和上能所的期权合约也区分平昨平今
						 *	把这个bFuture的判断去掉
						 */
						//if (bFuture)
						{
							if (strcmp(pInstrument->ExchangeID, "SHFE") == 0 || strcmp(pInstrument->ExchangeID, "INE") == 0)
								cm = CM_CoverToday;
							//上期所的就是平今,非上期所的就是开平
						}

						commInfo.m_coverMode = cm;

						PriceMode pm = PM_Both;
						/*
						 *	By Wesley @ 2023.05.04
						 *	有用户反馈上期所和上能所的期权合约也区分平昨平今
						 *	把这个bFuture的判断去掉
						 */
						//if (bFuture)
						{
							if (strcmp(pInstrument->ExchangeID, "SHFE") == 0 || strcmp(pInstrument->ExchangeID, "INE") == 0)
								pm = PM_Limit;
						}
						commInfo.m_priceMode = pm;
						commInfo.m_tradeMode = TM_Both;

						if (pInstrument->PriceTick < 0.001)
							commInfo.m_uPrecision = 4;
						else if (pInstrument->PriceTick < 0.01)
							commInfo.m_uPrecision = 3;
						else if (pInstrument->PriceTick < 0.1)
							commInfo.m_uPrecision = 2;
						else if (pInstrument->PriceTick < 1)
							commInfo.m_uPrecision = 1;
						else
							commInfo.m_uPrecision = 0;

						_commodities[fullPid] = commInfo;
					}

					_contracts[pInstrument->InstrumentID] = contract;
				}
			} while (false);
			}
		}
	}

	if (bIsLast)
	{
		if(QRYFEES)
		{
			for (auto& v : _contracts)
			{
				const Contract& cInfo = v.second;
				if(!SET_FILTERS.empty() && SET_FILTERS.find(cInfo.m_strCode) == SET_FILTERS.end() 
					&& SET_FILTERS.find(cInfo.m_strProduct) == SET_FILTERS.end() && SET_FILTERS.find(cInfo.m_strAltCode) == SET_FILTERS.end())
					continue;

				ReqQryCommission(cInfo);
				ReqQryMargin(cInfo);
			}
		}
		else
		{
			DumpToJson();
			exit(0);
		}		
	}
}

void CTraderSpi::OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField *pInstrumentCommissionRate, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	const bool responseError = IsErrorRspInfo(pRspInfo);
	if (responseError) _fee_response_failed.store(true, std::memory_order_release);
	if(!responseError && pInstrumentCommissionRate != NULL)
	{
		const auto& rate = *pInstrumentCommissionRate;
		const bool finite = isValidFeeValue(rate.OpenRatioByMoney) && isValidFeeValue(rate.CloseRatioByMoney) &&
			isValidFeeValue(rate.CloseTodayRatioByMoney) && isValidFeeValue(rate.OpenRatioByVolume) &&
			isValidFeeValue(rate.CloseRatioByVolume) && isValidFeeValue(rate.CloseTodayRatioByVolume);
		const bool money = rate.OpenRatioByMoney != 0.0 || rate.CloseRatioByMoney != 0.0 ||
			rate.CloseTodayRatioByMoney != 0.0;
		const bool volume = rate.OpenRatioByVolume != 0.0 || rate.CloseRatioByVolume != 0.0 ||
			rate.CloseTodayRatioByVolume != 0.0;
		// The fee schema has one mode shared by open/close/close-today. Do not
		// silently discard a component merely because it uses the other mode.
		if (!finite || (money && volume))
		{
			_fee_response_failed.store(true, std::memory_order_release);
			std::cerr << "--->>> Cannot export fees: invalid or mixed commission units" << std::endl;
		}
		else
		{
			FeeInfo& fInfo = _fees[rate.InstrumentID];
			fInfo._byvol = !money; // Includes the all-zero, mode-independent case.
			fInfo._open = money ? rate.OpenRatioByMoney : rate.OpenRatioByVolume;
			fInfo._close = money ? rate.CloseRatioByMoney : rate.CloseRatioByVolume;
			fInfo._closet = money ? rate.CloseTodayRatioByMoney : rate.CloseTodayRatioByVolume;
		}
	}

	if (IsQueryQueueEmpty() && QRYFEES)
	{
		if (_fee_response_failed.load(std::memory_order_acquire)) exit(EXIT_FAILURE);
		DumpToJson();
		exit(DumpFees() ? EXIT_SUCCESS : EXIT_FAILURE);
	}
}

void CTraderSpi::OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField *pInstrumentMarginRate, CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	const bool responseError = IsErrorRspInfo(pRspInfo);
	if (responseError) _fee_response_failed.store(true, std::memory_order_release);
	if (!responseError && pInstrumentMarginRate != NULL)
	{
		const auto& rate = *pInstrumentMarginRate;
		// The exported margin field has no per-volume mode. Validate even the
		// fields not selected for output so unsupported data cannot become zero.
		if (!isValidFeeValue(rate.LongMarginRatioByMoney) || !isValidFeeValue(rate.ShortMarginRatioByMoney) ||
			!isValidFeeValue(rate.LongMarginRatioByVolume) || !isValidFeeValue(rate.ShortMarginRatioByVolume) ||
			rate.LongMarginRatioByVolume != 0.0 || rate.ShortMarginRatioByVolume != 0.0 ||
			rate.LongMarginRatioByMoney != rate.ShortMarginRatioByMoney)
		{
			_fee_response_failed.store(true, std::memory_order_release);
			std::cerr << "--->>> Cannot export fees: invalid or unrepresentable margin" << std::endl;
		}
		else
		{
			MarginInfo& mInfo = _margins[rate.InstrumentID];
			mInfo._long = rate.LongMarginRatioByMoney;
			mInfo._short = rate.ShortMarginRatioByMoney;
		}
	}

	if (IsQueryQueueEmpty() && QRYFEES)
	{
		if (_fee_response_failed.load(std::memory_order_acquire)) exit(EXIT_FAILURE);
		DumpToJson();
		exit(DumpFees() ? EXIT_SUCCESS : EXIT_FAILURE);
	}
}

void CTraderSpi::LoadFromJson()
{
	if (!INCREMENTAL)
		return;

	std::string path = SAVEPATH;
	path += COMM_FILE;
	WTSVariant* root = WTSCfgLoader::load_from_file(path);
	if(root)
	{
		for(const auto& exchg : root->memberNames())
		{
			WTSVariant* jExchg = root->get(exchg.c_str());
			for(const auto& pid : jExchg->memberNames())
			{
				WTSVariant* pComm = jExchg->get(pid.c_str());
				std::string key = fmt::format("{}.{}", exchg, pid);

				Commodity& commInfo = _commodities[key];
				commInfo.m_strProduct = pid;
				commInfo.m_strName = pComm->getCString("name");
				commInfo.m_strExchg = exchg;
				commInfo.m_strCurrency = "CNY";

				commInfo.m_strSession = MAP_SESSION[key];
				commInfo.m_ccCategory = (ContractCategory)pComm->getUInt32("category");

				commInfo.m_uVolScale = pComm->getUInt32("volscale");;
				commInfo.m_fPriceTick = pComm->getDouble("pricetick");

				commInfo.m_coverMode = (CoverMode)pComm->getUInt32("covermode");

				commInfo.m_priceMode = (PriceMode)pComm->getUInt32("pricemode");;
				commInfo.m_tradeMode = (TradingMode)pComm->getUInt32("trademode");

				commInfo.m_uPrecision = pComm->getUInt32("precision");
			}
		}

		root->release();
#ifdef WT_T26_LOADJSON_TEST_HOOKS
		t26_test::load_json_test_hook::note_release(root);
#endif
	}

	path = SAVEPATH;
	path += CONT_FILE;
	root = WTSCfgLoader::load_from_file(path);
	if (root)
	{
		for (const auto& exchg_id : root->memberNames())
		{
			WTSVariant* jExchg = root->get(exchg_id.c_str());
			for (const auto& inst_id : jExchg->memberNames())
			{
				WTSVariant* pCont = jExchg->get(inst_id.c_str());
				//std::string key = fmt::format("{}.{}", exchg_id, inst_id);

				Contract& contract = _contracts[inst_id];
				contract.m_strCode = inst_id;
				contract.m_strExchg = exchg_id;
				contract.m_strName = pCont->getCString("name");
				contract.m_strProduct = pCont->getCString("product");
				contract.m_strAltCode = pCont->getCString("altcode");

				contract.m_maxMktQty = pCont->getUInt32("maxmarketqty");
				contract.m_maxLmtQty = pCont->getUInt32("maxlimitqty");
				contract.m_minMktQty = pCont->getUInt32("minmarketqty");
				contract.m_minLmtQty = pCont->getUInt32("minlimitqty");

				// DumpToJson stores option-only fields in a nested object. Reset the
				// optional fields before loading so a non-option contract (or a
				// malformed option value) keeps the non-option defaults.
				contract.m_optType = OT_None;
				contract.m_strUnderlying.clear();
				contract.m_strikePrice = 0.0;
				contract.m_dUnderlyingScale = 0.0;
				WTSVariant* pOption = pCont->get("option");
				if (pOption != NULL && pOption->isObject())
				{
					contract.m_optType = (OptionType)pOption->getUInt32("optiontype");
					contract.m_strUnderlying = pOption->getCString("underlying");
					contract.m_strikePrice = pOption->getDouble("strikeprice");
					contract.m_dUnderlyingScale = pOption->getDouble("underlyingscale");
				}
				

				contract.m_uOpenDate = pCont->getUInt32("opendate");
				contract.m_uExpireDate = pCont->getUInt32("expiredate");

				contract.m_dLongMarginRatio = pCont->getDouble("longmarginratio");
				contract.m_dShortMarginRatio = pCont->getDouble("shortmarginratio");
			}
		}

		root->release();
#ifdef WT_T26_LOADJSON_TEST_HOOKS
		t26_test::load_json_test_hook::note_release(root);
#endif
	}
	std::cerr << "--->>> " << "LoadFromJson" << std::endl;
}

bool CTraderSpi::DumpFees()
{
	if (_margins.empty())
	{
		std::cerr << "--->>> Cannot export fees: no margin records were received" << std::endl;
		return false;
	}
	struct ContractGroup
	{
		const Contract* contract = nullptr;
		std::set<std::string> aliases;
		bool selected = false;
	};
	std::map<std::string, ContractGroup> groups;
	std::map<std::string, std::string> groupByAlias;
	std::set<std::string> requiredKeys;
	for (const auto& item : _contracts)
	{
		const Contract& contract = item.second;
		if (contract.m_strExchg.empty() || contract.m_strCode.empty() || contract.m_strProduct.empty() ||
			contract.m_strExchg.find('.') != std::string::npos || contract.m_strCode.find('.') != std::string::npos)
		{
			std::cerr << "--->>> Cannot export fees: invalid contract identity for " << item.first << std::endl;
			return false;
		}
		const std::string key = contract.m_strExchg + "." + contract.m_strCode;
		ContractGroup& group = groups[key];
		if (group.contract && (group.contract->m_strProduct != contract.m_strProduct ||
			(!group.contract->m_strAltCode.empty() && !contract.m_strAltCode.empty() &&
			group.contract->m_strAltCode != contract.m_strAltCode)))
		{
			std::cerr << "--->>> Cannot export fees: conflicting contract " << key << std::endl;
			return false;
		}
		if (!group.contract || (group.contract->m_strAltCode.empty() && !contract.m_strAltCode.empty()))
			group.contract = &contract;
		group.selected = group.selected || SET_FILTERS.empty() || SET_FILTERS.count(contract.m_strCode) ||
			SET_FILTERS.count(contract.m_strProduct) || SET_FILTERS.count(contract.m_strAltCode);
		for (const std::string* alias : {&item.first, &contract.m_strCode, &contract.m_strAltCode})
		{
			if (alias->empty()) continue;
			auto inserted = groupByAlias.emplace(*alias, key);
			if (!inserted.second && inserted.first->second != key)
			{
				std::cerr << "--->>> Cannot export fees: ambiguous contract alias " << *alias << std::endl;
				return false;
			}
			group.aliases.insert(*alias);
		}
	}
	for (const auto& entry : groups)
		if (entry.second.selected) requiredKeys.insert(entry.first);
	if (requiredKeys.empty())
	{
		std::cerr << "--->>> Cannot export fees: no selected contracts" << std::endl;
		return false;
	}
	rj::Document root(rj::kObjectType);
	rj::Document::AllocatorType &allocator = root.GetAllocator();
	std::set<std::string> outputKeys;

	for(auto& v : _margins)
	{
		const std::string& rawCode = v.first;
		const MarginInfo& mInfo = v.second;

		auto alias = groupByAlias.find(rawCode);
		if (alias == groupByAlias.end())
		{
			std::cerr << "--->>> Cannot export fees: missing contract for " << rawCode << std::endl;
			return false;
		}
		const ContractGroup& group = groups.find(alias->second)->second;
		if (!group.selected) continue;
		const Contract& cInfo = *group.contract;
		const std::string& outputKey = alias->second;
		if (!outputKeys.insert(outputKey).second)
		{
			std::cerr << "--->>> Cannot export fees: duplicate contract key " << outputKey << std::endl;
			return false;
		}

		const FeeInfo* foundFee = nullptr;
		for (const std::string& feeAlias : group.aliases)
		{
			auto fee = _fees.find(feeAlias);
			if (fee == _fees.end()) continue;
			const FeeInfo& candidate = fee->second;
			if (foundFee && (foundFee->_open != candidate._open || foundFee->_close != candidate._close ||
				foundFee->_closet != candidate._closet || foundFee->_byvol != candidate._byvol))
			{
				std::cerr << "--->>> Cannot export fees: conflicting commission aliases for " << outputKey << std::endl;
				return false;
			}
			foundFee = &candidate;
		}
		if (!foundFee)
		{
			auto fee = _fees.find(cInfo.m_strProduct);
			if (fee != _fees.end()) foundFee = &fee->second;
		}
		if (!foundFee)
		{
			std::cerr << "--->>> Cannot export fees: missing commission for " << rawCode
				<< " (exchange=" << cInfo.m_strExchg << ", product=" << cInfo.m_strProduct << ")" << std::endl;
			return false;
		}
		const FeeInfo& fInfo = *foundFee;
		if (!isValidFeeValue(fInfo._open) || !isValidFeeValue(fInfo._close) ||
			!isValidFeeValue(fInfo._closet) || !isValidFeeValue(mInfo._long) || !isValidFeeValue(mInfo._short) ||
			mInfo._long != mInfo._short)
		{
			std::cerr << "--->>> Cannot export fees: non-finite value for " << rawCode << std::endl;
			return false;
		}

		rj::Value jFees(rj::kObjectType);
		jFees.AddMember("open", fInfo._open, allocator);
		jFees.AddMember("close", fInfo._close, allocator);
		jFees.AddMember("closetoday", fInfo._closet, allocator);
		jFees.AddMember("byvolume", fInfo._byvol, allocator);
		jFees.AddMember("margin", mInfo._long, allocator);

		root.AddMember(rj::Value(outputKey.c_str(), static_cast<rj::SizeType>(outputKey.size()), allocator), jFees, allocator);
	}
	if (outputKeys != requiredKeys)
	{
		std::cerr << "--->>> Cannot export fees: at least one selected contract has no margin" << std::endl;
		return false;
	}

	std::string path;
	if (boost::filesystem::path(FEES_FILE).is_absolute())
	{
		path = FEES_FILE;
	}
	else
	{
		path = SAVEPATH;
		path += FEES_FILE;
	}

	rj::StringBuffer sb;
	rj::PrettyWriter<rj::StringBuffer> writer(sb);
	if (!root.Accept(writer) || !publishFeeFile(path, sb.GetString(), sb.GetSize()))
	{
		std::cerr << "--->>> Cannot export fees to " << path << std::endl;
		return false;
	}
	std::cerr << "--->>> " << outputKeys.size() << " fees dumped into : " << path << std::endl;
	return true;
}

void CTraderSpi::DumpToJson()
{
	//两个文件,一个contracts.json,一个commodities.json
	//Json::Value jComms(Json::objectValue);
	rj::Document jComms(rj::kObjectType);
	{
		rj::Document::AllocatorType &allocator = jComms.GetAllocator();
		for (auto it = _commodities.begin(); it != _commodities.end(); it++)
		{
			const Commodity& commInfo = it->second;
			if (!jComms.HasMember(commInfo.m_strExchg.c_str()))
			{
				//jComms[commInfo.m_strExchg] = Json::Value(Json::objectValue);
				jComms.AddMember(rj::Value(commInfo.m_strExchg.c_str(), allocator), rj::Value(rj::kObjectType), allocator);
			}

			rj::Value jComm(rj::kObjectType);
			jComm.AddMember("covermode", (uint32_t)commInfo.m_coverMode, allocator);
			jComm.AddMember("pricemode", (uint32_t)commInfo.m_priceMode, allocator);
			jComm.AddMember("category", (uint32_t)commInfo.m_ccCategory, allocator);
			jComm.AddMember("trademode", (uint32_t)commInfo.m_tradeMode, allocator);
			jComm.AddMember("precision", commInfo.m_uPrecision, allocator);
			jComm.AddMember("pricetick", commInfo.m_fPriceTick, allocator);
			jComm.AddMember("volscale", commInfo.m_uVolScale, allocator);

			jComm.AddMember("name", rj::Value(commInfo.m_strName.c_str(), allocator), allocator);
			jComm.AddMember("exchg", rj::Value(commInfo.m_strExchg.c_str(), allocator), allocator);
			jComm.AddMember("session", rj::Value(commInfo.m_strSession.c_str(), allocator), allocator);
			jComm.AddMember("holiday", rj::Value("CHINA", allocator), allocator);

			//jComms[commInfo.m_strExchg][commInfo.m_strProduct] = jComm;
			jComms[commInfo.m_strExchg.c_str()].AddMember(rj::Value(commInfo.m_strProduct.c_str(), allocator), jComm, allocator);
		}
	}

	//Json::Value jContracts(Json::objectValue);
	rj::Document jContracts(rj::kObjectType);
	{
		rj::Document::AllocatorType &allocator = jContracts.GetAllocator();
		for (auto it = _contracts.begin(); it != _contracts.end(); it++)
		{
			const Contract& cInfo = it->second;
			if (!jContracts.HasMember(cInfo.m_strExchg.c_str()))
			{
				//jComms[commInfo.m_strExchg] = Json::Value(Json::objectValue);
				jContracts.AddMember(rj::Value(cInfo.m_strExchg.c_str(), allocator), rj::Value(rj::kObjectType), allocator);
			}

			rj::Value jcInfo(rj::kObjectType);

			jcInfo.AddMember("name", rj::Value(cInfo.m_strName.c_str(), allocator), allocator);
			jcInfo.AddMember("code", rj::Value(cInfo.m_strCode.c_str(), allocator), allocator);
			jcInfo.AddMember("altcode", rj::Value(cInfo.m_strAltCode.c_str(), allocator), allocator);
			jcInfo.AddMember("exchg", rj::Value(cInfo.m_strExchg.c_str(), allocator), allocator);
			jcInfo.AddMember("product", rj::Value(cInfo.m_strProduct.c_str(), allocator), allocator);

			jcInfo.AddMember("maxlimitqty", cInfo.m_maxLmtQty, allocator);
			jcInfo.AddMember("maxmarketqty", cInfo.m_maxMktQty, allocator);
			jcInfo.AddMember("minlimitqty", cInfo.m_minLmtQty, allocator);
			jcInfo.AddMember("minmarketqty", cInfo.m_minMktQty, allocator);

			jcInfo.AddMember("opendate", cInfo.m_uOpenDate, allocator);
			jcInfo.AddMember("expiredate", cInfo.m_uExpireDate, allocator);

			jcInfo.AddMember("longmarginratio", cInfo.m_dLongMarginRatio, allocator);
			jcInfo.AddMember("shortmarginratio", cInfo.m_dShortMarginRatio, allocator);

			if (cInfo.m_optType != OT_None)
			{
				//OptionType	m_optType;
				//std::string m_strUnderlying;
				//double		m_strikePrice;
				//double		m_dUnderlyingScale;
				rj::Value optInfo(rj::kObjectType);
				optInfo.AddMember("optiontype", (uint32_t)cInfo.m_optType, allocator);
				optInfo.AddMember("underlying", rj::Value(cInfo.m_strUnderlying.c_str(), allocator), allocator);
				optInfo.AddMember("strikeprice", cInfo.m_strikePrice, allocator);
				optInfo.AddMember("underlyingscale", cInfo.m_dUnderlyingScale, allocator);

				jcInfo.AddMember("option", optInfo, allocator);
			}

			jContracts[cInfo.m_strExchg.c_str()].AddMember(rj::Value(cInfo.m_strCode.c_str(), allocator), jcInfo, allocator);
		}
	}

	std::ofstream ofs;
	std::string path = SAVEPATH;
	path += COMM_FILE;
	ofs.open(path);
	{
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		jComms.Accept(writer);
		ofs << sb.GetString();
	}
	ofs.close();
	std::cerr << "--->>> " << _commodities.size() << " commodities dumped into : " <<  path << std::endl;

	path = SAVEPATH;
	path += CONT_FILE;
	ofs.open(path);
	{
		rj::StringBuffer sb;
		rj::PrettyWriter<rj::StringBuffer> writer(sb);
		jContracts.Accept(writer);
		ofs << sb.GetString();
	}
	ofs.close();
	std::cerr << "--->>> " << _contracts.size() << " contracts dumped into : " << path << std::endl;
}


void CTraderSpi::OnFrontDisconnected(int nReason)
{
	std::cerr << "--->>> " << "OnFrontDisconnected" << std::endl;
	std::cerr << "--->>> Reason = " << nReason << std::endl;
}


void CTraderSpi::OnRspError(CThostFtdcRspInfoField *pRspInfo, int /*nRequestID*/, bool /*bIsLast*/)
{
	std::cerr << "--->>> " << "OnRspError" << std::endl;
	IsErrorRspInfo(pRspInfo);
}

bool CTraderSpi::IsErrorRspInfo(CThostFtdcRspInfoField *pRspInfo)
{
	// 如果ErrorID != 0, 说明收到了错误的响应
	bool bResult = ((pRspInfo) && (pRspInfo->ErrorID != 0));
	if (bResult)
		std::cerr << "--->>> ErrorID=" << pRspInfo->ErrorID << ", ErrorMsg=" << encode_text(pRspInfo->ErrorMsg) << std::endl;
	return bResult;
}
