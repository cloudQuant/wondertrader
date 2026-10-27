/*!
 * \file DataDefine.h
 * \project WonderTrader
 *
 * \author Wesley
 * 
 * \brief WonderTrader数据存储系统数据结构定义文件
 * \details 该文件定义了所有与数据存储相关的数据结构，包括：
 *          1. 数据块类型枚举
 *          2. 数据块头部结构
 *          3. 实时数据块定义（K线、Tick、逐笔成交等）
 *          4. 历史数据块定义
 *          用于支持WonderTrader的各类行情数据的存储与读取
 */

#pragma once
#include "../Includes/WTSStruct.h"

USING_NS_WTP;

#pragma pack(push, 1)

/**
 * @brief 数据块标志常量
 * @details 用于标记数据块的起始位置，作为数据文件的预设标识
 */
const char BLK_FLAG[] = "&^%$#@!\0";

/**
 * @brief 数据块标志大小常量
 * @details 标记字符串的字节数
 */
const int FLAG_SIZE = 8;

/**
 * @brief 数据块类型枚举
 * @details 定义了所有支持的实时和历史数据类型
 *          实时数据类型从1开始编号
 *          历史数据类型从21开始编号
 */
typedef enum tagBlockType
{
	BT_RT_Minute1		= 1,	//实时1分钟线
	BT_RT_Minute5		= 2,	//实时5分钟线
	BT_RT_Ticks			= 3,	//实时tick数据
	BT_RT_Cache			= 4,	//实时缓存
	BT_RT_Trnsctn		= 5,	//实时逐笔成交
	BT_RT_OrdDetail		= 6,	//实时逐笔委托
	BT_RT_OrdQueue		= 7,	//实时委托队列

	BT_HIS_Minute1		= 21,	//历史1分钟线
	BT_HIS_Minute5		= 22,	//历史5分钟线
	BT_HIS_Day			= 23,	//历史日线
	BT_HIS_Ticks		= 24,	//历史tick
	BT_HIS_Trnsctn		= 25,	//历史逐笔成交
	BT_HIS_OrdDetail	= 26,	//历史逐笔委托
	BT_HIS_OrdQueue		= 27	//历史委托队列
} BlockType;

/**
 * @brief 数据块版本定义
 * @details 定义了不同版本的数据块格式
 */
#define BLOCK_VERSION_RAW		0x01	//老结构体未压缩
#define BLOCK_VERSION_CMP		0x02	//老结构体压缩
#define BLOCK_VERSION_RAW_V2	0x03	//新结构体未压缩
#define BLOCK_VERSION_CMP_V2	0x04	//新结构体压缩

/**
 * @brief 数据块头部结构
 * @details 定义了所有数据块的基础头部结构，包含标识、类型和版本信息
 */
typedef struct _BlockHeader
{
	char		_blk_flag[FLAG_SIZE]; /**< 数据块标识符 */
	uint16_t	_type;             /**< 数据块类型，对应BlockType枚举 */
	uint16_t	_version;          /**< 数据块版本号 */

	/**
	 * @brief 检查是否为旧版本数据块
	 * @return 如果是旧版本返回true，否则返回false
	 */
	inline bool is_old_version() const {
		return (_version == BLOCK_VERSION_CMP || _version == BLOCK_VERSION_RAW);
	}

	/**
	 * @brief 检查数据块是否经过压缩
	 * @return 如果数据已压缩返回true，否则返回false
	 */
	inline bool is_compressed() const {
		return (_version == BLOCK_VERSION_CMP || _version == BLOCK_VERSION_CMP_V2);
	}
} BlockHeader;

/**
 * @brief 数据块头部结构版本2
 * @details 新版数据块头部结构，在基础头部结构上增加了数据大小信息
 */
typedef struct _BlockHeaderV2
{
	char		_blk_flag[FLAG_SIZE]; /**< 数据块标识符 */
	uint16_t	_type;             /**< 数据块类型 */
	uint16_t	_version;          /**< 数据块版本号 */

	uint64_t	_size;		/**< 压缩后的数据大小，方便内存分配 */

	/**
	 * @brief 检查是否为旧版本数据块
	 * @return 如果是旧版本返回true，否则返回false
	 */
	inline bool is_old_version() const {
		return (_version == BLOCK_VERSION_CMP || _version == BLOCK_VERSION_RAW);
	}

	/**
	 * @brief 检查数据块是否经过压缩
	 * @return 如果数据已压缩返回true，否则返回false
	 */
	inline bool is_compressed() const {
		return (_version == BLOCK_VERSION_CMP || _version == BLOCK_VERSION_CMP_V2);
	}
} BlockHeaderV2;

/**
 * @brief 常量：原始数据块头部大小
 */
#define BLOCK_HEADER_SIZE	sizeof(BlockHeader)

/**
 * @brief 常量：版本2数据块头部大小
 */
#define BLOCK_HEADERV2_SIZE sizeof(BlockHeaderV2)

/**
 * @brief 实时数据块头部基础结构
 * @details 继承自基础头部结构，添加了实时数据的大小和容量信息
 */
typedef struct _RTBlockHeader : BlockHeader
{
	uint32_t _size;      /**< 当前实际数据项数量 */
	uint32_t _capacity;  /**< 数据块可容纳的最大数据项数量 */
} RTBlockHeader;

/**
 * @brief 每日实时数据块头部结构
 * @details 继承自实时数据块头部，添加了交易日期信息
 */
typedef struct _RTDayBlockHeader : RTBlockHeader
{
	uint32_t		_date;  /**< 数据对应的交易日期，格式为YYYYMMDD */
} RTDayBlockHeader;

/**
 * @brief 实时K线数据块结构
 * @details 存储实时K线数据，继承自每日实时数据块头部
 *          包含一个可变长度的WTSBarStruct数组
 */
typedef struct _RTKlineBlock : _RTDayBlockHeader
{
	WTSBarStruct	_bars[0];  /**< K线数据数组，可变长度 */
} RTKlineBlock;

/**
 * @brief 实时Tick数据块结构
 * @details 存储实时Tick数据，继承自每日实时数据块头部
 *          包含一个可变长度的WTSTickStruct数组
 * @note By Wesley @ 2021.12.30
 *       实时tick缓存，直接用新版本的tick结构
 *       切换程序一定要在盘后进行！！！
 */
typedef struct _RTTickBlock : RTDayBlockHeader
{
	WTSTickStruct	_ticks[0];  /**< Tick数据数组，可变长度 */
} RTTickBlock;

/**
 * @brief 实时逐笔成交数据块结构
 * @details 存储实时逐笔成交数据，继承自每日实时数据块头部
 *          包含一个可变长度的WTSTransStruct数组
 */
typedef struct _RTTransBlock : RTDayBlockHeader
{
	WTSTransStruct	_trans[0];  /**< 逐笔成交数据数组，可变长度 */
} RTTransBlock;

/**
 * @brief 实时逐笔委托数据块结构
 * @details 存储实时逐笔委托数据，继承自每日实时数据块头部
 *          包含一个可变长度的WTSOrdDtlStruct数组
 */
typedef struct _RTOrdDtlBlock : RTDayBlockHeader
{
	WTSOrdDtlStruct	_details[0];  /**< 逐笔委托数据数组，可变长度 */
} RTOrdDtlBlock;

/**
 * @brief 实时委托队列数据块结构
 * @details 存储实时委托队列数据，继承自每日实时数据块头部
 *          包含一个可变长度的WTSOrdQueStruct数组
 */
typedef struct _RTOrdQueBlock : RTDayBlockHeader
{
	WTSOrdQueStruct	_queues[0];  /**< 委托队列数据数组，可变长度 */
} RTOrdQueBlock;

/**
 * @brief Tick缓存项结构
 * @details 存储单个合约的最新Tick数据及其对应的交易日期
 */
typedef struct _TickCacheItem
{
	uint32_t		_date;  /**< 数据对应的交易日期，格式为YYYYMMDD */
	WTSTickStruct	_tick;  /**< Tick数据内容 */
} TickCacheItem;

/**
 * @brief 实时Tick缓存块结构
 * @details 存储多个合约的最新Tick数据，继承自实时数据块头部
 *          与普通RTTickBlock不同，它存储的是不同合约的最新行情
 */
typedef struct _RTTickCache : RTBlockHeader
{
	TickCacheItem	_ticks[0];  /**< Tick缓存项数组，可变长度 */
} RTTickCache;


/**
 * @brief 历史Tick数据块结构（旧版）
 * @details 存储历史Tick数据，继承自基础头部结构
 *          使用原始的WTSTickStruct数组存储数据
 */
typedef struct _HisTickBlock : BlockHeader
{
	WTSTickStruct	_ticks[0];  /**< Tick数据数组，可变长度 */
} HisTickBlock;

/**
 * @brief 历史Tick数据块结构（新版）
 * @details 存储历史Tick数据，继承自新版头部结构
 *          使用字节数组存储数据，支持数据压缩
 */
typedef struct _HisTickBlockV2 : BlockHeaderV2
{
	char			_data[0];  /**< 原始数据字节数组，包含压缩或非压缩数据 */
} HisTickBlockV2;

/**
 * @brief 历史逐笔成交数据块结构（旧版）
 * @details 存储历史逐笔成交数据，继承自基础头部结构
 *          使用原始的WTSTransStruct数组存储数据
 */
typedef struct _HisTransBlock : BlockHeader
{
	WTSTransStruct	_items[0];  /**< 逐笔成交数据数组，可变长度 */
} HisTransBlock;

/**
 * @brief 历史逐笔成交数据块结构（新版）
 * @details 存储历史逐笔成交数据，继承自新版头部结构
 *          使用字节数组存储数据，支持数据压缩
 */
typedef struct _HisTransBlockV2 : BlockHeaderV2
{
	char			_data[0];  /**< 原始数据字节数组，包含压缩或非压缩数据 */
} HisTransBlockV2;

/**
 * @brief 历史逐笔委托数据块结构（旧版）
 * @details 存储历史逐笔委托数据，继承自基础头部结构
 *          使用原始的WTSOrdDtlStruct数组存储数据
 */
typedef struct _HisOrdDtlBlock : BlockHeader
{
	WTSOrdDtlStruct	_items[0];  /**< 逐笔委托数据数组，可变长度 */
} HisOrdDtlBlock;

/**
 * @brief 历史逐笔委托数据块结构（新版）
 * @details 存储历史逐笔委托数据，继承自新版头部结构
 *          使用字节数组存储数据，支持数据压缩
 */
typedef struct _HisOrdDtlBlockV2 : BlockHeaderV2
{
	char			_data[0];  /**< 原始数据字节数组，包含压缩或非压缩数据 */
} HisOrdDtlBlockV2;

/**
 * @brief 历史委托队列数据块结构（旧版）
 * @details 存储历史委托队列数据，继承自基础头部结构
 *          使用原始的WTSOrdQueStruct数组存储数据
 */
typedef struct _HisOrdQueBlock : BlockHeader
{
	WTSOrdQueStruct	_items[0];  /**< 委托队列数据数组，可变长度 */
} HisOrdQueBlock;

/**
 * @brief 历史委托队列数据块结构（新版）
 * @details 存储历史委托队列数据，继承自新版头部结构
 *          使用字节数组存储数据，支持数据压缩
 */
typedef struct _HisOrdQueBlockV2 : BlockHeaderV2
{
	char			_data[0];  /**< 原始数据字节数组，包含压缩或非压缩数据 */
} HisOrdQueBlockV2;

/**
 * @brief 历史K线数据块结构（标准版）
 * @details 存储历史K线数据，继承自基础头部结构
 *          使用原始的WTSBarStruct数组存储数据
 */
typedef struct _HisKlineBlock : BlockHeader
{
	WTSBarStruct	_bars[0];  /**< K线数据数组，可变长度 */
} HisKlineBlock;

/**
 * @brief 历史K线数据块结构（新版）
 * @details 存储历史K线数据，继承自新版头部结构
 *          使用字节数组存储数据，支持数据压缩
 */
typedef struct _HisKlineBlockV2 : BlockHeaderV2
{
	char			_data[0];  /**< 原始数据字节数组，包含压缩或非压缩数据 */
} HisKlineBlockV2;

/**
 * @brief 历史K线数据块结构（旧版）
 * @details 存储历史K线数据，继承自基础头部结构
 *          使用旧版的WTSBarStructOld数组存储数据
 *          主要用于兼容旧版本的数据文件
 */
typedef struct _HisKlineBlockOld : BlockHeader
{
	WTSBarStructOld	_bars[0];  /**< 旧版K线数据数组，可变长度 */
} HisKlineBlockOld;

#pragma pack(pop)
