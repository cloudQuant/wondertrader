/*!
 * \file DataDefineAD.h
 * \brief 数据存储模块的数据结构定义文件
 * 
 * 该文件定义了数据存储模块中使用的各种数据结构，包括块头、缓存结构等
 * 这些结构用于存储和管理实时行情数据
 * 
 * \author Wesley
 */

#pragma once
#include "../Includes/WTSStruct.h"

USING_NS_WTP;

/**
 * \brief 字节对齐指令，确保结构体在内存中紧凑排列，没有填充字节
 */
#pragma pack(push, 1)

/**
 * \brief 数据块标志，用于标识数据块的开始
 * 
 * 该常量定义了一个特殊的字符序列，用于标识数据块的开始位置
 */
const char BLK_FLAG[] = "&^%$#@!\0";

/**
 * \brief 数据块标志的大小（字节数）
 */
const int FLAG_SIZE = 8;

/**
 * \brief 数据块类型枚举
 * 
 * 定义了不同类型的数据块，用于区分不同用途的数据存储
 */
typedef enum tagBlockType
{
	/**
	 * \brief 实时缓存块类型，值为4
	 */
	BT_RT_Cache			= 4		//实时缓存
} BlockType;

/**
 * \brief 数据块的原始版本号
 * 
 * 定义了普通版本的数据块，值为1
 */
#define BLOCK_VERSION_RAW	1	//普通版本

/**
 * \brief 数据块头部结构
 * 
 * 定义了数据块的基本头部信息，包括标志、类型和版本号
 */
typedef struct _BlockHeader
{
	/**
	 * \brief 数据块标志，用于识别数据块的开始
	 */
	char		_blk_flag[FLAG_SIZE];
	
	/**
	 * \brief 数据块类型，对应BlockType枚举
	 */
	uint16_t	_type;
	
	/**
	 * \brief 数据块版本号
	 */
	uint16_t	_version;
} BlockHeader;

/**
 * \brief 数据块头部结构的大小（字节数）
 * 
 * 定义了BlockHeader结构体的字节大小，用于内存分配和偏移计算
 */
#define BLOCK_HEADER_SIZE	sizeof(BlockHeader)

/**
 * \brief 实时数据块头部结构
 * 
 * 继承自BlockHeader，扩展了实时数据块的大小和容量信息
 */
typedef struct _RTBlockHeader : BlockHeader
{
	/**
	 * \brief 当前实际存储的数据项数量
	 */
	uint32_t _size;

	/**
	 * \brief 数据块的总容量，即可存储的最大数据项数量
	 */
	uint32_t _capacity;
} RTBlockHeader;

/**
 * \brief Tick数据缓存项结构
 * 
 * 定义了单个Tick数据的缓存项，包含日期和Tick数据结构
 */
typedef struct _TickCacheItem
{
	/**
	 * \brief 交易日期，格式为YYYYMMDD
	 */
	uint32_t		_date;

	/**
	 * \brief Tick数据结构，包含完整的Tick行情数据
	 */
	WTSTickStruct	_tick;
} TickCacheItem;

/**
 * \brief 实时Tick数据缓存结构
 * 
 * 继承自RTBlockHeader，用于存储多个Tick数据项的实时缓存
 */
typedef struct _RTTickCache : RTBlockHeader
{
	/**
	 * \brief Tick数据项数组，使用柱状数组实现可变长度
	 * 
	 * 数组大小为0表示这是一个柱状数组，实际大小由_capacity决定
	 */
	TickCacheItem	_items[0];
} RTTickCache;

/**
 * \brief K线数据缓存项结构
 * 
 * 定义了单个K线数据的缓存项，包含交易所、合约代码和K线数据结构
 */
typedef struct _BarCacheItem
{
	/**
	 * \brief 交易所代码，最外16个字符
	 */
	char			_exchg[16];

	/**
	 * \brief 合约代码，最外32个字符
	 */
	char			_code[32];

	/**
	 * \brief K线数据结构，包含完整的K线行情数据
	 */
	WTSBarStruct	_bar;
} BarCacheItem;

/**
 * \brief 实时K线数据缓存结构
 * 
 * 继承自RTBlockHeader，用于存储多个K线数据项的实时缓存
 */
typedef struct _RTBarCache : RTBlockHeader
{
	/**
	 * \brief K线数据项数组，使用柱状数组实现可变长度
	 * 
	 * 数组大小为0表示这是一个柱状数组，实际大小由_capacity决定
	 */
	BarCacheItem	_items[0];
} RTBarCache;

#pragma pack(pop)
