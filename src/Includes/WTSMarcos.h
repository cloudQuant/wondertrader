/*!
 * \file WTSMarcos.h
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief WonderTrader基础宏定义文件
 * \details 该文件定义了WonderTrader框架中使用的基础宏定义、类型别名和通用工具函数，包括长度定义、操作系统兼容性宏、命名空间定义等
 */
#pragma once
#include <limits.h>
#include <string.h>
#include <cstddef>   // size_t
#include <stddef.h>  // size_t

/**
 * @brief 防止Windows平台上max/min宏与C++标准库冲突
 * @details 在Windows平台上，windows.h中定义了min和max宏，会与c++标准库中的std::min和std::max产生冲突
 *          定义NOMINMAX可以禁止Windows平台上的这些宏定义
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif

/**
 * @brief 合约代码最大长度限制
 * @details 定义了合约代码字符串的最大长度为32个字符
 */
#define MAX_INSTRUMENT_LENGTH	32

/**
 * @brief 交易所代码最大长度限制
 * @details 定义了交易所代码字符串的最大长度为16个字符
 */
#define MAX_EXCHANGE_LENGTH		16

/**
 * @brief 静态类型转换宏
 * @details 封装了C++的static_cast提供更简洁的类型转换语法
 */
#define STATIC_CONVERT(x,T)		static_cast<T>(x)

#ifndef DBL_MAX
#define DBL_MAX 1.7976931348623158e+308
#endif

#ifndef FLT_MAX
#define FLT_MAX 3.402823466e+38F        /* max value */
#endif

/**
 * @brief 无效值宏定义
 * @details 定义了各种数据类型的无效值，用于表示空值或错误状态
 *          对不同的编译器平台提供兼容性定义
 */
#ifdef _MSC_VER
/* 微软编译器平台上的无效值定义 */
#define INVALID_DOUBLE		DBL_MAX      ///< 无效的双精度浮点数值
#define INVALID_INT32		INT_MAX      ///< 无效的有符号32位整数值
#define INVALID_UINT32		UINT_MAX     ///< 无效的无符号32位整数值
#define INVALID_INT64		_I64_MAX     ///< 无效的有符号64位整数值
#define INVALID_UINT64		_UI64_MAX    ///< 无效的无符号64位整数值
#else
/* 非微软编译器平台上的无效值定义 */
#define INVALID_DOUBLE		1.7976931348623158e+308 /* max value */  ///< 无效的双精度浮点数值
#define INVALID_INT32		2147483647                 ///< 无效的有符号32位整数值
#define INVALID_UINT32		0xffffffffUL              ///< 无效的无符号32位整数值
#define INVALID_INT64		9223372036854775807LL      ///< 无效的有符号64位整数值
#define INVALID_UINT64		0xffffffffffffffffULL     ///< 无效的无符号64位整数值
#endif

#ifndef NULL
#ifdef __cplusplus
#define NULL 0
#else
#define NULL ((void *)0)
#endif
#endif

/**
 * @brief 命名空间定义宏
 * @details 简化命名空间的声明和使用，定义了WonderTrader的命名空间wtp
 */
#define NS_WTP_BEGIN	namespace wtp{    ///< 命名空间开始
#define NS_WTP_END	}//namespace wtp     ///< 命名空间结束
#define	USING_NS_WTP	using namespace wtp  ///< 使用wtp命名空间

/**
 * @brief 导出函数标记宏
 * @details 用于处理不同编译器平台上的DLL函数导出语法差异
 *          在Windows平台上使用__declspec(dllexport)
 *          在非Windows平台上使用__attribute__((__visibility__("default")))
 */
#ifndef EXPORT_FLAG
#ifdef _MSC_VER
#	define EXPORT_FLAG __declspec(dllexport)  ///< Windows平台函数导出宏
#else
#	define EXPORT_FLAG __attribute__((__visibility__("default")))  ///< 非Windows平台函数导出宏
#endif
#endif

/**
 * @brief 函数调用约定宏
 * @details 用于处理不同编译器平台上的函数调用约定
 *          在Windows平台上使用_cdecl调用约定
 *          在非Windows平台上不需要额外的调用约定
 */
#ifndef PORTER_FLAG
#ifdef _MSC_VER
#	define PORTER_FLAG _cdecl  ///< Windows平台函数调用约定
#else
#	define PORTER_FLAG         ///< 非Windows平台函数调用约定（空）
#endif
#endif

/**
 * @brief WonderTrader类型别名定义
 * @details 为常用类型定义简洁的别名，提高代码可读性和类型一致性
 */
typedef unsigned int		WtUInt32;   ///< 32位无符号整数类型

typedef unsigned long long	WtUInt64;   ///< 64位无符号整数类型

typedef const char*			WtString;    ///< 字符串类型（常量字符指针）

/**
 * @brief 字符串大小写不敏感比较函数
 * @details 封装了不同平台上的字符串大小写不敏感比较函数
 *          在Windows平台上使用_stricmp，在非Windows平台上使用strcasecmp
 */
#ifdef _MSC_VER
#define wt_stricmp _stricmp     ///< Windows平台字符串大小写不敏感比较函数
#else
#define wt_stricmp strcasecmp   ///< 非Windows平台字符串大小写不敏感比较函数
#endif

/**
 * @brief 字符串拷贝函数
 * @details 重写的字符串拷贝函数，直接使用memcpy实现字符串拷贝功能
 *          最初设计目的是为了提高性能，但最新测试结果显示性能提升不显著
 *
 * @param des 目标字符串缓冲区
 * @param src 源字符串
 * @param len 要拷贝的字符串长度，默认为0，表示自动计算源字符串长度
 * @return 拷贝的字符串长度
 *
 * @note By Wesley @ 2022.03.17
 *       核心要点就是使用memcpy替代strcpy，原本认为对于长字符串会有性能提升
 *
 * @note By Wesley @ 2023.10.09
 *       重新和strcpy进行了性能测试，发现性能提升不显著，甚至有一些下降
 *       可能与早期测试环境有关，由于用到的地方很多，目前保留不变
 */
inline size_t wt_strcpy(char* des, const char* src, size_t len = 0)
{
	len = (len == 0) ? strlen(src) : len;  ///< 如果长度为0，自动计算源字符串长度
	memcpy(des, src, len);                  ///< 使用memcpy拷贝内存
	des[len] = '\0';                       ///< 手动添加字符串结束符
	return len;                            ///< 返回拷贝的字符串长度
}