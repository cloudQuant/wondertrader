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
#include "../Includes/WTSMarcos.h"

/**
 * \brief 16位无符号整数的字节序转换函数
 * 
 * 将一个16位无符号整数的字节序从小端转换为大端或从大端转换为小端
 * 
 * \param src 输入的原始16位无符号整数
 * \return 返回字节序转换后的结果
 */
static uint16_t reverseEndian(uint16_t src)
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
		memset(this, 0, sizeof(_LMDBHftKey));
		strcpy(_exchg, exchg);
		strcpy(_code, code);
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
		memset(this, 0, sizeof(_LMDBBarKey));
		strcpy(_exchg, exchg);
		strcpy(_code, code);
		_bartime = reverseEndian(bartime);
	}
} LMDBBarKey;
#pragma pack(pop)