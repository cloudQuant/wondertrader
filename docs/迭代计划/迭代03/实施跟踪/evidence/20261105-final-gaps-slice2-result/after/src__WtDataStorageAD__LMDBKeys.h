/*!
 * \file LMDBKeys.h
 * \brief LMDB数据库键定义文件
 * 
 * 该文件定义了LMDB数据库中使用的键结构，包括HFT(高频交易)Tick数据和K线数据的键结构，
 * 以及字节序转换函数。
 * 
 * \author Wesley
 */

#pragma once
#include <stdint.h>
#include <string.h>
#include <stdexcept>
#include "../Includes/WTSMarcos.h"

// BUG-L04-004: the on-disk key layout is fixed (exchange 16 + code 32 bytes,
// NUL padded). Each field must fit including its terminating NUL. Inspect no
// more than the field capacity, so a non-terminated fixed-size input is
// rejected without reading past it. Empty fields still fit (the historical
// strcpy accepted them); the stricter entry check below rejects them.
inline bool lmdbKeyFieldLengths(const char* exchg, const char* code,
	size_t& exchangeLen, size_t& codeLen)
{
	if (!exchg || !code)
		return false;
	exchangeLen = 0;
	codeLen = 0;
	while (exchangeLen < MAX_EXCHANGE_LENGTH && exchg[exchangeLen]) ++exchangeLen;
	while (codeLen < MAX_INSTRUMENT_LENGTH && code[codeLen]) ++codeLen;
	return exchangeLen < MAX_EXCHANGE_LENGTH && codeLen < MAX_INSTRUMENT_LENGTH;
}

// Reader/writer entry check, applied before any cache, directory, database or
// queue side effect: a usable storage identity is non-empty and fits the key.
inline bool lmdbKeyIdentityLengths(const char* exchg, const char* code,
	size_t& exchangeLen, size_t& codeLen)
{
	return lmdbKeyFieldLengths(exchg, code, exchangeLen, codeLen) &&
		exchangeLen != 0 && codeLen != 0;
}

inline bool lmdbKeyIdentityValid(const char* exchg, const char* code)
{
	size_t exchangeLen, codeLen;
	return lmdbKeyIdentityLengths(exchg, code, exchangeLen, codeLen);
}

/**
 * \brief 16位无符号整数的字节序转换函数
 * 
 * 将一个16位无符号整数的字节序从小端转换为大端或从大端转换为小端
 * 
 * \param src 输入的原始16位无符号整数
 * \return 返回字节序转换后的结果
 */
[[maybe_unused]] static uint16_t reverseEndian(uint16_t src)
{
	uint16_t up = (src & 0x00FF) << 8;
	uint16_t low = (src & 0xFF00) >> 8;
	return up + low;
}

/**
 * \brief 32位无符号整数的字节序转换函数
 * 
 * 将一个32位无符号整数的字节序从小端转换为大端或从大端转换为小端
 * 
 * \param src 输入的原始32位无符号整数
 * \return 返回字节序转换后的结果
 */
static uint32_t reverseEndian(uint32_t src)
{
	uint32_t x = (src & 0x000000FF) << 24;
	uint32_t y = (src & 0x0000FF00) << 8;
	uint32_t z = (src & 0x00FF0000) >> 8;
	uint32_t w = (src & 0xFF000000) >> 24;
	return x + y + z + w;
}

/**
 * \brief 字节对齐指令，确保结构体在内存中紧凑排列，没有填充字节
 */
#pragma pack(push, 1)
/**
 * \brief LMDB高频交易(HFT)Tick数据的键结构
 * 
 * 用于在LMDB数据库中存储和检索Tick数据的键结构，包含交易所、合约代码、日期和时间信息
 */
typedef struct _LMDBHftKey
{
	/**
	 * \brief 交易所代码
	 */
	char		_exchg[MAX_EXCHANGE_LENGTH];
	
	/**
	 * \brief 合约代码
	 */
	char		_code[MAX_INSTRUMENT_LENGTH];
	
	/**
	 * \brief 交易日期，格式为YYYYMMDD，存储时会进行字节序转换
	 */
	uint32_t	_date;
	
	/**
	 * \brief 交易时间，格式为HHMMSSmmm，存储时会进行字节序转换
	 */
	uint32_t	_time;

	/**
	 * \brief 构造函数
	 * 
	 * 初始化高频交易Tick数据的键结构，并对日期和时间进行字节序转换
	 * 
	 * \param exchg 交易所代码
	 * \param code 合约代码
	 * \param date 交易日期，格式为YYYYMMDD
	 * \param time 交易时间，格式为HHMMSSmmm
	 */
	_LMDBHftKey(const char* exchg, const char* code, uint32_t date, uint32_t time)
	{
		// A constructor has no status channel: an identity that cannot be
		// represented is reported by exception instead of being copied past
		// the fixed field. Product entries pre-check with lmdbKeyIdentityValid.
		size_t exchangeLen, codeLen;
		if (!lmdbKeyFieldLengths(exchg, code, exchangeLen, codeLen))
			throw std::invalid_argument("invalid LMDB key identity");
		memset(this, 0, sizeof(_LMDBHftKey));
		memcpy(_exchg, exchg, exchangeLen);
		memcpy(_code, code, codeLen);
		_date = reverseEndian(date);
		_time = reverseEndian(time);
	}
} LMDBHftKey;

/**
 * \brief LMDB K线数据的键结构
 * 
 * 用于在LMDB数据库中存储和检索K线数据的键结构，包含交易所、合约代码和K线时间信息
 */
typedef struct  _LMDBBarKey
{
public:
	/**
	 * \brief 交易所代码
	 */
	char		_exchg[MAX_EXCHANGE_LENGTH];
	
	/**
	 * \brief 合约代码
	 */
	char		_code[MAX_INSTRUMENT_LENGTH];
	
	/**
	 * \brief K线时间，格式取决于K线周期，存储时会进行字节序转换
	 *
	 * 日线为交易日 YYYYMMDD；分钟线为 (日期-19900000)*10000+HHMM。V1 键为 32 位，
	 * 分钟线可表示的最后一个交易日是 2032-12-31（K33=A）：2033 年起读写两端均明确拒绝
	 * 并记 ERROR，不得截断回绕。上限说明见 docs/data_engine.md（data_lmdb_minute_limit）。
	 */
	uint32_t	_bartime;

	/**
	 * \brief 构造函数
	 * 
	 * 初始化K线数据的键结构，并对K线时间进行字节序转换
	 * 
	 * \param exchg 交易所代码
	 * \param code 合约代码
	 * \param bartime K线时间，格式取决于K线周期
	 */
	_LMDBBarKey(const char* exchg, const char* code, uint32_t bartime)
	{
		size_t exchangeLen, codeLen;
		if (!lmdbKeyFieldLengths(exchg, code, exchangeLen, codeLen))
			throw std::invalid_argument("invalid LMDB key identity");
		memset(this, 0, sizeof(_LMDBBarKey));
		memcpy(_exchg, exchg, exchangeLen);
		memcpy(_code, code, codeLen);
		_bartime = reverseEndian(bartime);
	}
} LMDBBarKey;
#pragma pack(pop)

// The key bytes are the persisted LMDB ABI; bounded copying must not move them.
static_assert(sizeof(LMDBHftKey) == MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 8,
	"LMDBHftKey on-disk layout changed");
static_assert(sizeof(LMDBBarKey) == MAX_EXCHANGE_LENGTH + MAX_INSTRUMENT_LENGTH + 4,
	"LMDBBarKey on-disk layout changed");
