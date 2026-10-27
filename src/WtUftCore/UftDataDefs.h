/*!
 * \file UftDataDefs.h
 * \project WonderTrader
 *
 * \author Wesley
 * \date Unknown
 * 
 * \brief UFT数据定义头文件
 * \details 定义了UFT模块使用的各种数据结构，包括数据块头、持仓详情、订单、成交和交易轮次等
 */
#pragma once
#include <stdint.h>
#include <string.h>
#include "../Includes/WTSMarcos.h"

#pragma warning(disable:4200)

namespace uft {

#pragma pack(push, 1)

	const char BLK_FLAG[] = "&^%$#@!\0";

	const int FLAG_SIZE = 8;

	/*!
	 * \brief 数据块头部结构
	 * \details 所有数据块的基础结构，包含块标识、类型、日期、容量和大小信息
	 */
	typedef struct _BlockHeader
	{
		char		_blk_flag[FLAG_SIZE];  /*!< 数据块标识符 */
		uint32_t	_type;                 /*!< 数据块类型 */
		uint32_t	_date;                 /*!< 数据日期 */

		uint32_t	_capacity;             /*!< 数据块容量 */
		uint32_t	_size;                 /*!< 当前数据大小 */
	} BlockHeader;

	/*!
	 * \brief 持仓详情结构体
	 * \details 记录单个合约的持仓详细信息，包括交易所、合约代码、方向、数量、开仓价格和时间、持仓盈亏等
	 */
	typedef struct _DetailStruct
	{
		char		_exchg[MAX_EXCHANGE_LENGTH];  /*!< 交易所代码 */
		char		_code[MAX_INSTRUMENT_LENGTH]; /*!< 合约代码 */
		uint32_t	_direct;	/*!< 方向：0-多，1-空 */
		double		_volume;                    /*!< 持仓量 */
		double		_open_price;                /*!< 开仓均价 */
		uint64_t	_open_time;                 /*!< 开仓时间 */
		uint32_t	_open_tdate;                /*!< 开仓交易日 */
		double		_position_profit;            /*!< 持仓盈亏 */

		double		_closed_volume;              /*!< 平仓量 */
		double		_closed_profit;              /*!< 平仓盈亏 */

		/*!
		 * \brief 构造函数
		 * \details 初始化所有成员变量为0
		 */
		_DetailStruct()
		{
			memset(this, 0, sizeof(_DetailStruct));
		}

	}DetailStruct;

	/*!
	 * \brief 持仓数据块
	 * \details 包含多个持仓详情记录的数据块，继承自BlockHeader
	 */
	typedef struct _PositionBlock : public BlockHeader
	{
		DetailStruct	_details[0];  /*!< 可变长度的持仓详情数组 */
	}PositionBlock;

	/*!
	 * \brief 订单结构体
	 * \details 记录单个订单的详细信息，包括交易所、合约代码、方向、开平仓标志、数量、价格、成交状态等
	 */
	typedef struct _OrderStruct
	{
		char		_exchg[MAX_EXCHANGE_LENGTH];  /*!< 交易所代码 */
		char		_code[MAX_INSTRUMENT_LENGTH]; /*!< 合约代码 */
		uint32_t	_direct;                    /*!< 方向：0-多，1-空 */
		uint32_t	_offset;                    /*!< 开平仓标志：0-开仓，1-平仓，2-平今 */

		double		_volume;                    /*!< 委托数量 */
		double		_price;                     /*!< 委托价格 */

		double		_traded;                    /*!< 已成交数量 */
		double		_left;                      /*!< 剩余数量 */
		uint32_t	_state;                     /*!< 订单状态：0-有效，1-全部成交，2-已撤单 */
		uint64_t	_oder_time;                 /*!< 委托时间 */
	} OrderStruct;

	/*!
	 * \brief 订单数据块
	 * \details 包含多个订单记录的数据块，继承自BlockHeader
	 */
	typedef struct _OrderBlock : public BlockHeader
	{
		OrderStruct	_orders[0];  /*!< 可变长度的订单数组 */
	} OrderBlock;

	/*!
	 * \brief 成交结构体
	 * \details 记录单个成交的详细信息，包括交易所、合约代码、方向、开平仓标志、数量、价格、成交日期和时间
	 */
	typedef struct _TradeStruct
	{
		char		_exchg[MAX_EXCHANGE_LENGTH];  /*!< 交易所代码 */
		char		_code[MAX_INSTRUMENT_LENGTH]; /*!< 合约代码 */
		uint32_t	_direct;                    /*!< 方向：0-多，1-空 */
		uint32_t	_offset;                    /*!< 开平仓标志：0-开仓，1-平仓，2-平今 */
		double		_volume;                    /*!< 成交数量 */
		double		_price;                     /*!< 成交价格 */
		uint32_t	_trading_date;              /*!< 成交日期 */
		uint64_t	_trading_time;              /*!< 成交时间 */
	} TradeStruct;

	/*!
	 * \brief 成交数据块
	 * \details 包含多个成交记录的数据块，继承自BlockHeader
	 */
	typedef struct _TradeBlock : public BlockHeader
	{
		TradeStruct	_trades[0];  /*!< 可变长度的成交数组 */
	} TradeBlock;

	/*!
 * \brief 交易轮次结构体
 * \details 记录一次完整交易（开仓到平仓）的详细信息，包括交易所、合约代码、方向、开平仓价格和时间、交易量和盈亏
 */
typedef struct _RoundStruct
	{
		char		_exchg[MAX_EXCHANGE_LENGTH];
		char		_code[MAX_INSTRUMENT_LENGTH];
		uint32_t	_direct;
		double		_open_price;
		uint64_t	_open_time;
		double		_close_price;
		uint64_t	_close_time;
		double		_volume;
		double		_profit;
	} RoundStruct;

	/*!
 * \brief 交易轮次数据块
 * \details 包含多个交易轮次记录的数据块，继承自BlockHeader
 */
typedef struct _RoundBlock : public BlockHeader
	{
		RoundStruct	_rounds[0];
	} RoundBlock;

#pragma pack(pop)

} //namespace uft