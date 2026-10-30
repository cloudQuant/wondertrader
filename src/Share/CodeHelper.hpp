/*!
 * \file CodeHelper.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief 代码辅助类,封装到一起方便使用
 */
#pragma once
#include "fmtlib.h"
#include "StrUtil.hpp"
#include "../Includes/WTSTypes.h"
#include "../Includes/IHotMgr.h"

#include <boost/xpressive/xpressive_dynamic.hpp>


USING_NS_WTP;

//主力合约后缀
static const char* SUFFIX_HOT = ".HOT";

//次主力合约后缀
static const char* SUFFIX_2ND = ".2ND";

//前复权合约代码后缀
static const char SUFFIX_QFQ = '-';

//后复权合约代码后缀
static const char SUFFIX_HFQ = '+';

//前复权合约代码后缀
static const char* SUFFIX_QFQ_S = "-";

//后复权合约代码后缀
static const char* SUFFIX_HFQ_S = "+";

class CodeHelper
{
public:
	typedef struct _CodeInfo
	{
		char _code[MAX_INSTRUMENT_LENGTH];		//合约代码
		char _exchg[MAX_INSTRUMENT_LENGTH];		//交易所代码
		char _product[MAX_INSTRUMENT_LENGTH];	//品种代码
		char _ruletag[MAX_INSTRUMENT_LENGTH];	//
		char _fullpid[MAX_INSTRUMENT_LENGTH];	//

		//By Wesley @ 2021.12.25
		//去掉合约类型，这里不再进行判断
		//整个CodeHelper会重构
		//ContractCategory	_category;		//合约类型
		//union
		//{
		//	uint8_t	_hotflag;	//主力标记，0-非主力，1-主力，2-次主力
		//	uint8_t	_exright;	//是否是复权代码,如SH600000Q: 0-不复权, 1-前复权, 2-后复权
		//};

		/*
		 *	By Wesley @ 2022.03.07
		 *	取消原来的union
		 *	要把主力标记和复权标记分开处理
		 *	因为后面要对主力合约做复权处理了
		 */
		uint8_t	_exright;	//是否是复权代码,如SH600000Q: 0-不复权, 1-前复权, 2-后复权

		//是否是复权代码
		inline bool isExright() const { return _exright != 0; }

		//是否前复权代码
		inline bool isForwardAdj() const { return _exright == 1; }

		//是否后复权代码
		inline bool isBackwardAdj() const { return _exright == 2; }

		//标准品种ID
		inline const char* stdCommID()
		{
			if (strlen(_fullpid) == 0)
				fmtutil::format_to(_fullpid, "{}.{}", _exchg, _product);

			return _fullpid;
		}

		_CodeInfo()
		{
			memset(this, 0, sizeof(_CodeInfo));
			//_category = CC_Future;
		}

		inline void clear()
		{
			memset(this, 0, sizeof(_CodeInfo));
		}

		inline bool hasRule() const
		{
			return strlen(_ruletag) > 0;
		}
	} CodeInfo;

private:
	static inline std::size_t find(const char* src, char symbol = '.', bool bReverse = false)
	{
		std::size_t len = strlen(src);
		if (len != 0)
		{
			if (bReverse)
			{
				for (std::size_t idx = len; idx > 0; idx--)
				{
					const std::size_t pos = idx - 1;
					if (src[pos] == symbol)
						return pos;
				}
			}
			else
			{
				for (std::size_t idx = 0; idx < len; idx++)
				{
					if (src[idx] == symbol)
						return idx;
				}
			}
		}


		return std::string::npos;
	}

public:
	/*
	 *	是否是期货期权合约代码
	 *	CFFEX.IO2007.C.4000
	 */
	static bool	isStdChnFutOptCode(const char* code)
	{
		if (code == nullptr)
			return false;
		const bool isLegacyCzce = strncmp(code, "CZCE.", 5) == 0 || strncmp(code, "ZCE.", 4) == 0;

		/* 定义正则表达式 */
		//static cregex reg_stk = cregex::compile("^[A-Z]+.[A-z]+\\d{4}.(C|P).\\d+$");	//CFFEX.IO2007.C.4000
		//return 	regex_match(code, reg_stk);
		char state = 0;
		std::size_t i = 0;
		for(; ; i++)
		{
			char ch = code[i];
			if(ch == '\0')
				break;

			if(state == 0)
			{
				if (!('A' <= ch && ch <= 'Z'))
					return false;

				state += 1;
			}
			else if (state == 1)
			{
				if ('A' <= ch && ch <= 'Z')
					continue;

				if (ch == '.')
					state += 1;
				else
					return false;
			}
			else if (state == 2)
			{
				if (!('A' <= ch && ch <= 'z'))
					return false;

				state += 1;
			}
			else if (state == 3)
			{
				if ('A' <= ch && ch <= 'z')
					continue;

				if ('0' <= ch && ch <= '9')
					state += 1;
				else
					return false;
			}
			else if (state == 4 || state == 5)
			{
				if ('0' <= ch && ch <= '9')
					state += 1;
				else
					return false;
			}
			else if (state == 6)
			{
				if ('0' <= ch && ch <= '9')
					state = 7;
				else if (ch == '.' && isLegacyCzce)
					state = 8; // Legacy CZCE/ZCE options also use three-digit months.
				else
					return false;
			}
			else if (state == 7)
			{
				if (ch == '.')
					state = 8;
				else
					return false;
			}
			else if (state == 8)
			{
				if (ch == 'C' || ch == 'P')
					state += 1;
				else
					return false;
			}
			else if (state == 9)
			{
				if (ch == '.')
					state += 1;
				else
					return false;
			}
			else if (state == 10)
			{
				if ('0' <= ch && ch <= '9')
					state += 1;
				else
					return false;
			}
			else if (state == 11)
			{
				if ('0' <= ch && ch <= '9')
					continue;
				else
					return false;
			}
		}

		return (state == 11);
	}

	/*
	 *	是否是标准分月期货合约代码
	 *	//CFFEX.IF.2007
	 */
	static inline bool	isStdMonthlyFutCode(const char* code)
	{
		using namespace boost::xpressive;
		/* 定义正则表达式 */
		static cregex reg_stk = cregex::compile("^[A-Z]+.[A-z]+.\\d{4}$");	//CFFEX.IO.2007
		return 	regex_match(code, reg_stk);
	}

	/*
	 *	标准代码转标准品种ID
	 *	如SHFE.ag.1912->SHFE.ag
	 *	如果是简化的股票代码，如SSE.600000，则转成SSE.STK
	 */
	static inline std::string stdCodeToStdCommID2(const char* stdCode)
	{
		auto idx = find(stdCode, '.', true);
		auto idx2 = find(stdCode, '.', false);
		if (idx != idx2)
		{
			//前后两个.不是同一个，说明是三段的代码
			//提取前两段作为品种代码
			return std::string(stdCode, idx);
		}
		else
		{
			//两段的代码，直接返回
			//主要针对某些交易所，每个合约的交易规则都不同的情况
			//这种情况，就把合约直接当成品种来用
			return stdCode;
		}		
	}

	/*
	 *	从基础分月合约代码提取基础品种代码
	 *	如ag1912 -> ag
	 *	这个只有分月期货品种才有意义
	 *	这个不会有永续合约的代码传到这里来，如果有的话就是调用的地方有Bug!
	 */
	static inline std::string rawMonthCodeToRawCommID(const char* code)
	{
		int nLen = 0;
		while ('A' <= code[nLen] && code[nLen] <= 'z')
			nLen++;

		return std::string(code, nLen);
	}

	/*
	 *	基础分月合约代码转标准码
	 *	如ag1912转成全码
	 *	这个不会有永续合约的代码传到这里来，如果有的话就是调用的地方有Bug!
	 */
	static inline std::string rawMonthCodeToStdCode(const char* code, const char* exchg, bool isComm = false)
	{
		if (code == nullptr || exchg == nullptr || code[0] == '\0' || exchg[0] == '\0')
			return "";

		std::string result(exchg);
		result.push_back('.');
		if (isComm)
		{
			result.append(code);
			return result;
		}

		const std::string rawCode(code);
		const std::string pid = rawMonthCodeToRawCommID(rawCode.c_str());
		if (pid.empty() || pid.size() >= rawCode.size())
			return "";

		const std::size_t monthLen = rawCode.size() - pid.size();
		const char* month = rawCode.c_str() + pid.size();
		if (monthLen == 4)
		{
			result.append(pid);
			result.push_back('.');
			result.append(month, 4);
		}
		else if (monthLen >= 3)
		{
			result.append(pid);
			result.push_back('.');
			result.push_back(month[0] > '5' ? '1' : '2');
			result.append(month, 3);
		}
		else
		{
			return "";
		}

		return result;
	}

	/*
	 *	原始常规代码转标准代码
	 *	这种主要针对非分月合约而言
	 */
	static inline std::string rawFlatCodeToStdCode(const char* code, const char* exchg, const char* pid)
	{
		thread_local static char buffer[64] = { 0 };
		auto len = strlen(exchg);
		memcpy(buffer, exchg, len);
		buffer[len] = '.';
		len += 1;

		auto plen = strlen(pid);
		auto clen = strlen(code);

		if (strcmp(code, pid) == 0 || plen == 0)
		{
			memcpy(buffer + len, code, clen);
			len += clen;
			buffer[len] = '\0';
		}
		else
		{
			memcpy(buffer + len, pid, plen);
			len += plen;
			buffer[len] = '.';
			len += 1;

			memcpy(buffer + len, code, clen);
			len += clen;
			buffer[len] = '\0';
		}

		return buffer;
	}

	static inline bool isMonthlyCode(const char* code)
	{
		//using namespace boost::xpressive;
		//最后3-6位都是数字，才是分月合约
		//static cregex reg_stk = cregex::compile("^.*[A-z|-]\\d{3,6}$");	//CFFEX.IO.2007
		//return 	regex_match(code, reg_stk);
		auto len = strlen(code);
		char state = 0;
		for (std::size_t i = 0; i < len; i++)
		{
			char ch = code[len - i - 1];
			if (0 <= state && state < 3)
			{
				if (!('0' <= ch && ch <= '9'))
					return false;

				state += 1;
			}
			else if (3 == state || 4 == state)
			{
				if ('0' <= ch && ch <= '9')
					state += 1;

				else if ('A' <= ch && ch <= 'z')
				{
					state = 7;
					break;
				}
			}
			else if (4 == state)
			{
				if ('0' <= ch && ch <= '9')
					state += 1;

				else if (('A' <= ch && ch <= 'z') || ch == '-')
				{
					state = 7;
					break;
				}
			}
			else if (state < 6)
			{
				if ('0' <= ch && ch <= '9')
					state += 1;
				else
					return false;
			}
			else if (state >= 6)
			{
				if (('A' <= ch && ch <= 'z') || ch == '-')
				{
					state = 7;
					break;
				}
				else
				{
					return false;
				}
			}
		}

		return state == 7;
	}

	/*
	 *	期货期权代码标准化
	 *	标准期货期权代码格式为CFFEX.IO2008.C.4300
	 *	-- 暂时没有地方调用 --
	 */
	static inline bool tryRawFutOptCodeToStdCode(const char* code, const char* exchg, std::string& out)
	{
		out.clear();
		if (code == nullptr || exchg == nullptr)
			return false;

		// Contract codes are stored in MAX_INSTRUMENT_LENGTH-sized fields. Stop
		// at that bound rather than asking strlen/regex to scan arbitrary input.
		std::size_t codeLen = 0;
		while (codeLen < MAX_INSTRUMENT_LENGTH && code[codeLen] != '\0')
			++codeLen;
		if (codeLen == 0 || codeLen >= MAX_INSTRUMENT_LENGTH)
			return false;

		std::size_t exchgLen = 0;
		const auto isAsciiAlpha = [](unsigned char ch) {
			return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
		};
		const auto isAsciiDigit = [](unsigned char ch) {
			return ch >= '0' && ch <= '9';
		};
		while (exchgLen < MAX_INSTRUMENT_LENGTH && exchg[exchgLen] != '\0')
		{
			const unsigned char ch = static_cast<unsigned char>(exchg[exchgLen]);
			if (!isAsciiAlpha(ch) && !isAsciiDigit(ch) && ch != '_' && ch != '-')
				return false;
			++exchgLen;
		}
		if (exchgLen == 0 || exchgLen >= MAX_INSTRUMENT_LENGTH)
			return false;

		std::size_t productLen = 0;
		while (productLen < codeLen && isAsciiAlpha(static_cast<unsigned char>(code[productLen])))
			++productLen;
		if (productLen == 0 || productLen == codeLen)
			return false;

		std::size_t monthLen = 0;
		std::size_t directionPos = 0;
		std::size_t strikePos = 0;
		std::size_t monthEnd = productLen;
		while (monthEnd < codeLen && isAsciiDigit(static_cast<unsigned char>(code[monthEnd])))
			++monthEnd;
		monthLen = monthEnd - productLen;
		if (monthEnd >= codeLen)
			return false;

		if (code[monthEnd] == '-')
		{
			if (monthLen != 4 || monthEnd + 3 >= codeLen)
				return false;
			directionPos = monthEnd + 1;
			if ((code[directionPos] != 'C' && code[directionPos] != 'P') ||
				code[monthEnd + 2] != '-')
				return false;
			strikePos = monthEnd + 3;
		}
		else
		{
			if ((monthLen != 3 && monthLen != 4) ||
				(code[monthEnd] != 'C' && code[monthEnd] != 'P'))
				return false;
			directionPos = monthEnd;
			strikePos = directionPos + 1;
		}

		if (strikePos >= codeLen)
			return false;
		for (std::size_t idx = strikePos; idx < codeLen; ++idx)
		{
			if (!isAsciiDigit(static_cast<unsigned char>(code[idx])))
				return false;
		}

		try
		{
			std::string parsed;
			parsed.reserve(exchgLen + codeLen + 5);
			parsed.append(exchg, exchgLen);
			parsed.push_back('.');
			parsed.append(code, productLen + monthLen);
			parsed.push_back('.');
			parsed.push_back(code[directionPos]);
			parsed.push_back('.');
			parsed.append(code + strikePos, codeLen - strikePos);
			out.swap(parsed);
			return true;
		}
		catch (...)
		{
			out.clear();
			return false;
		}
	}

	static inline std::string rawFutOptCodeToStdCode(const char* code, const char* exchg)
	{
		std::string result;
		tryRawFutOptCodeToStdCode(code, exchg, result);
		return result;
	}

	/*
	 *	标准合约代码转主力代码
	 */
	static inline std::string stdCodeToStdHotCode(const char* stdCode)
	{
		std::size_t idx = find(stdCode, '.', true);
		if (idx == std::string::npos)
			return "";		
		
		std::string stdWrappedCode(stdCode, idx);
		stdWrappedCode.append(SUFFIX_HOT);
		return stdWrappedCode;
	}

	/*
	 *	标准合约代码转次主力代码
	 */
	static inline std::string stdCodeToStd2ndCode(const char* stdCode)
	{
		std::size_t idx = find(stdCode, '.', true);
		if (idx == std::string::npos)
			return "";

		std::string stdWrappedCode(stdCode, idx);
		stdWrappedCode.append(SUFFIX_2ND);
		return stdWrappedCode;
	}

	/*
	 *	标准期货期权代码转原代码
	 *	-- 暂时没有地方调用 --
	 */
	static inline std::string stdFutOptCodeToRawCode(const char* stdCode)
	{
		std::string ret = stdCode;
		auto pos = ret.find(".");
		ret = ret.substr(pos + 1);
		if (strncmp(stdCode, "CFFEX", 5) == 0 || strncmp(stdCode, "DCE", 3) == 0)
			StrUtil::replace(ret, ".", "-");
		else
			StrUtil::replace(ret, ".", "");
		return ret;
	}

	static inline int indexCodeMonth(const char* code)
	{
		if (strlen(code) == 0)
			return -1;

		std::size_t idx = 0;
		std::size_t len = strlen(code);
		while(idx < len)
		{
			if (isdigit(code[idx]))
				return (int)idx;

			idx++;
		}
		return -1;
	}

	/*
	 *	提取标准期货期权代码的信息
	 */
	static CodeInfo extractStdChnFutOptCode(const char* stdCode)
	{
		CodeInfo codeInfo;

		StringVector ay = StrUtil::split(stdCode, ".");
		wt_strcpy(codeInfo._exchg, ay[0].c_str());
		const bool isCzce = strcmp(codeInfo._exchg, "CZCE") == 0 || strcmp(codeInfo._exchg, "ZCE") == 0;
		if(strcmp(codeInfo._exchg, "SHFE") == 0 || strcmp(codeInfo._exchg, "INE") == 0 || isCzce)
		{
			fmt::format_to(codeInfo._code, "{}{}{}", ay[1], ay[2], ay[3]);
		}
		// By Wesley @ 2024.04.12
		// 郑商所代码通过altcode来处理，因子在wt内部和其他交易所一样了
		//else if (strcmp(codeInfo._exchg, "CZCE") == 0)
		//{
		//	std::string& s = ay[1];
		//	fmt::format_to(codeInfo._code, "{}{}{}{}", s.substr(0, s.size()-4), s.substr(s.size()-4), ay[2], ay[3]);
		//}
		else
		{
			fmt::format_to(codeInfo._code, "{}-{}-{}", ay[1], ay[2], ay[3]);
		}

		int mpos = indexCodeMonth(ay[1].c_str());

		if(isCzce)
		{
			memcpy(codeInfo._product, ay[1].c_str(), mpos);
			strcat(codeInfo._product, ay[2].c_str());
		}
		else if (strcmp(codeInfo._exchg, "CFFEX") == 0)
		{
			memcpy(codeInfo._product, ay[1].c_str(), mpos);
		}
		else
		{
			memcpy(codeInfo._product, ay[1].c_str(), mpos);
			strcat(codeInfo._product, "_o");
		}

		return codeInfo;
	}

	/*
	 *	提起标准代码的信息
	 */
	static CodeInfo extractStdCode(const char* stdCode, IHotMgr *hotMgr)
	{
		if (stdCode == NULL || stdCode[0] == '\0')
			return CodeInfo();

		// Standard codes contain two fields (exchange.contract), three fields
		// (exchange.product.contract), or four fields for a recognized option.
		// Validate every field before passing pointers or lengths to the legacy
		// fixed-size CodeInfo fields below.
		const char* fields[4] = {};
		std::size_t lengths[4] = {};
		std::size_t fieldCount = 0;
		auto fieldEquals = [](const char* field, std::size_t length, const char* value) {
			return strlen(value) == length && memcmp(field, value, length) == 0;
		};
		auto fullPidFits = [](std::size_t exchangeLen, std::size_t productLen) {
			// stdCommID() formats "exchange.product" into a fixed-size array.
			return exchangeLen + 1 + productLen < MAX_INSTRUMENT_LENGTH;
		};
		const char* fieldStart = stdCode;
		for (const char* p = stdCode; ; ++p)
		{
			if (*p == '.' || *p == '\0')
			{
				if (fieldCount >= 4 || p == fieldStart ||
					static_cast<std::size_t>(p - fieldStart) >= MAX_INSTRUMENT_LENGTH)
					return CodeInfo();

				fields[fieldCount] = fieldStart;
				lengths[fieldCount] = static_cast<std::size_t>(p - fieldStart);
				++fieldCount;
				if (*p == '\0')
					break;
				fieldStart = p + 1;
			}
		}

		//期权的代码规则和其他都不一样，所以单独判断
		if(isStdChnFutOptCode(stdCode))
		{
			if (fieldCount != 4)
				return CodeInfo();

			const bool isCzce = fieldEquals(fields[0], lengths[0], "CZCE") ||
				fieldEquals(fields[0], lengths[0], "ZCE");
			const bool isCompactOption = fieldEquals(fields[0], lengths[0], "SHFE") ||
				fieldEquals(fields[0], lengths[0], "INE") || isCzce;
			const std::size_t optionCodeLen =
				(lengths[1] + lengths[3] +
				 (isCompactOption ? 1 : 3));
			const std::string optionContract(fields[1], lengths[1]);
			const int monthIndex = indexCodeMonth(optionContract.c_str());
			if (monthIndex < 0)
				return CodeInfo();
			const std::size_t monthPos = static_cast<std::size_t>(monthIndex);
			std::size_t productLen = monthPos;
			if (isCzce)
				productLen += 1;
			else if (!fieldEquals(fields[0], lengths[0], "CFFEX"))
				productLen += 2;
			if (optionCodeLen >= MAX_INSTRUMENT_LENGTH || monthPos >= lengths[1] ||
				productLen >= MAX_INSTRUMENT_LENGTH || !fullPidFits(lengths[0], productLen))
				return CodeInfo();

			return extractStdChnFutOptCode(stdCode);
		}
		if (fieldCount != 2 && fieldCount != 3)
			return CodeInfo();
		if (!fullPidFits(lengths[0], lengths[1]))
			return CodeInfo();
		{
			/*
			 *	By Wesley @ 2021.12.25
			 *	1、先看是不是Q和H结尾的，如果是复权标记确认以后，最后一段长度-1，复制到code，如SSE.STK.600000Q
			 *	2、再看是不是分月合约，如果是，则将product字段拼接月份给code（郑商所特殊处理），如CFFEX.IF.2112
			 *	3、最后看看是不是HOT和2ND结尾的，如果是，则将product拷贝给code，如DCE.m.HOT
			 *	4、如果都不是，则原样复制第三段，如BINANCE.DC.BTCUSDT/SSE.STK.600000
			 */
			thread_local static CodeInfo codeInfo;
			codeInfo.clear();
			wt_strcpy(codeInfo._exchg, fields[0], lengths[0]);

			if (fieldCount == 2)
			{
				wt_strcpy(codeInfo._product, fields[1], lengths[1]);

				//By Wesley @ 2021.12.29
				//如果是两段的合约代码，如OKEX.BTC-USDT
				//则品种代码和合约代码一致
				wt_strcpy(codeInfo._code, fields[1], lengths[1]);
			}
			else
			{
				wt_strcpy(codeInfo._product, fields[1], lengths[1]);
				const char* ext = fields[2];
				std::size_t extlen = lengths[2];
				char lastCh = ext[extlen - 1];
				if (lastCh == SUFFIX_QFQ || lastCh == SUFFIX_HFQ)
				{
					codeInfo._exright = (lastCh == SUFFIX_QFQ) ? 1 : 2;

					extlen--;
					if (extlen == 0)
					{
						codeInfo.clear();
						return codeInfo;
					}
					lastCh = ext[extlen - 1];
				}
				
				if (extlen == 4 && '0' <= lastCh && lastCh <= '9')
				{
					//如果最后一段是4位数字，说明是分月合约
					//TODO: 这样的判断存在一个假设，最后一位是数字的一定是期货分月合约，以后可能会有问题，先注释一下
					//那么code得加上品种id
					//郑商所得单独处理一下，这个只能hardcode了
					if (lengths[1] + extlen >= MAX_INSTRUMENT_LENGTH)
					{
						codeInfo.clear();
						return codeInfo;
					}
					auto i = wt_strcpy(codeInfo._code, codeInfo._product, lengths[1]);
					wt_strcpy(codeInfo._code + i, ext, extlen);
				}
				else
				{
					const char* ruleTag = (hotMgr != NULL) ? hotMgr->getRuleTag(ext) :"";
					if (ruleTag == NULL)
						ruleTag = "";
					const std::size_t ruleLen = strlen(ruleTag);
					if (ruleLen >= MAX_INSTRUMENT_LENGTH)
					{
						codeInfo.clear();
						return codeInfo;
					}
					if (ruleLen == 0)
						wt_strcpy(codeInfo._code, ext, extlen);
					else
					{
						wt_strcpy(codeInfo._code, codeInfo._product, lengths[1]);
						wt_strcpy(codeInfo._ruletag, ruleTag);
					}
				}
			}			

			return codeInfo;
		}
	}
};

