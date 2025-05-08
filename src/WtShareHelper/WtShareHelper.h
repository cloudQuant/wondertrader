/**
 * @file WtShareHelper.h
 * @brief WonderTrader共享内存帮助模块头文件
 * @details 提供跨进程共享内存管理的接口，实现进程间数据共享和通信
 */

#pragma once

#include <stdint.h>
#include "../Includes/WTSMarcos.h"

/**
 * @brief 获取分区回调函数类型
 * @details 用于处理获取到的分区名称
 * @param 分区名称
 */
typedef void(PORTER_FLAG *FuncGetSections)(const char*);

/**
 * @brief 获取键回调函数类型
 * @details 用于处理获取到的键名称及其类型
 * @param 键名称
 * @param 键类型
 */
typedef void(PORTER_FLAG *FuncGetKeys)(const char*, uint64_t);

#ifdef __cplusplus
extern "C"
{
#endif

	/**
	 * @brief 初始化主进程共享内存
	 * @details 在主进程中初始化共享内存块，用于进程间数据共享
	 * @param id 共享内存标识符
	 * @param path 共享内存文件路径，默认为空字符串
	 * @return 初始化是否成功
	 */
	EXPORT_FLAG	bool	init_master(const char* id, const char* path = "");

	/**
	 * @brief 初始化从进程共享内存
	 * @details 在从进程中初始化共享内存块，连接到主进程创建的共享内存
	 * @param id 共享内存标识符，需要与主进程中的标识符相同
	 * @param path 共享内存文件路径，默认为空字符串
	 * @return 初始化是否成功
	 */
	EXPORT_FLAG	bool	init_slave(const char* id, const char* path = "");

	/**
	 * @brief 更新从进程共享内存
	 * @details 在从进程中更新共享内存块，同步主进程的最新数据
	 * @param id 共享内存标识符
	 * @param bForce 是否强制更新，默认为false
	 * @return 更新是否成功
	 */
	EXPORT_FLAG	bool	update_slave(const char* id, bool bForce = false);

	/**
	 * @brief 释放从进程共享内存
	 * @details 在从进程中释放共享内存块，断开与主进程的连接
	 * @param name 共享内存名称
	 * @return 释放是否成功
	 */
	EXPORT_FLAG bool	release_slave(const char* name);

	/**
	 * @brief 获取指定域下的所有分区
	 * @details 获取共享内存中指定域下的所有分区，并通过回调函数返回分区名称
	 * @param domain 域名称
	 * @param cb 回调函数，用于处理每个分区名称
	 * @return 分区数量
	 */
	EXPORT_FLAG	uint32_t	get_sections(const char* domain, FuncGetSections cb);

	/**
	 * @brief 获取指定域和分区下的所有键
	 * @details 获取共享内存中指定域和分区下的所有键，并通过回调函数返回键名称及类型
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param cb 回调函数，用于处理每个键的名称和类型
	 * @return 键的数量
	 */
	EXPORT_FLAG	uint32_t	get_keys(const char* domain, const char* section, FuncGetKeys cb);

	/**
	 * @brief 获取分区的更新时间
	 * @details 获取共享内存中指定域和分区的最后更新时间
	 * @param domain 域名称
	 * @param section 分区名称
	 * @return 更新时间的时间戳
	 */
	EXPORT_FLAG uint64_t	get_section_updatetime(const char* domain, const char* section);

	/**
	 * @brief 提交分区的变更
	 * @details 将共享内存中指定域和分区的变更提交到存储介质，并更新时间戳
	 * @param domain 域名称
	 * @param section 分区名称
	 * @return 提交是否成功
	 */
	EXPORT_FLAG bool		commit_section(const char* domain, const char* section);

	/**
	 * @brief 删除分区
	 * @details 删除共享内存中指定域和分区的所有数据
	 * @param domain 域名称
	 * @param section 分区名称
	 * @return 删除是否成功
	 */
	EXPORT_FLAG bool		delete_section(const char* domain, const char*section);

	/**
	 * @brief 分配字符串类型的共享内存
	 * @details 在共享内存中分配字符串类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的字符串指针
	 */
	EXPORT_FLAG const char*	allocate_string(const char* domain, const char* section, const char* key, const char* initVal, bool bForceWrite = false);

	/**
	 * @brief 分配32位整数类型的共享内存
	 * @details 在共享内存中分配32位整数类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的整数指针
	 */
	EXPORT_FLAG int32_t*	allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal, bool bForceWrite = false);

	/**
	 * @brief 分配64位整数类型的共享内存
	 * @details 在共享内存中分配64位整数类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的整数指针
	 */
	EXPORT_FLAG int64_t*	allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal, bool bForceWrite = false);

	/**
	 * @brief 分配无符32位整数类型的共享内存
	 * @details 在共享内存中分配无符32位整数类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的无符号整数指针
	 */
	EXPORT_FLAG uint32_t*	allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal, bool bForceWrite = false);

	/**
	 * @brief 分配无符64位整数类型的共享内存
	 * @details 在共享内存中分配无符64位整数类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的无符号整数指针
	 */
	EXPORT_FLAG uint64_t*	allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal, bool bForceWrite = false);

	/**
	 * @brief 分配双精度浮点数类型的共享内存
	 * @details 在共享内存中分配双精度浮点数类型的内存空间，并设置初始值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param initVal 初始值
	 * @param bForceWrite 是否强制写入，即使键已存在，默认为false
	 * @return 分配的浮点数指针
	 */
	EXPORT_FLAG double*		allocate_double(const char* domain, const char* section, const char* key, double initVal, bool bForceWrite = false);

	/**
	 * @brief 设置字符串类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的字符串值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的字符串值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_string(const char* domain, const char* section, const char* key, const char* val);

	/**
	 * @brief 设置32位整数类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的32位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的整数值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_int32(const char* domain, const char* section, const char* key, int32_t val);

	/**
	 * @brief 设置64位整数类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的64位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的整数值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_int64(const char* domain, const char* section, const char* key, int64_t val);

	/**
	 * @brief 设置无符32位整数类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的无符32位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的无符号整数值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_uint32(const char* domain, const char* section, const char* key, uint32_t val);

	/**
	 * @brief 设置无符64位整数类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的无符64位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的无符号整数值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_uint64(const char* domain, const char* section, const char* key, uint64_t val);

	/**
	 * @brief 设置双精度浮点数类型的共享内存值
	 * @details 设置共享内存中指定域、分区和键的双精度浮点数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param val 要设置的浮点数值
	 * @return 设置是否成功
	 */
	EXPORT_FLAG bool	set_double(const char* domain, const char* section, const char* key, double val);

	/**
	 * @brief 获取字符串类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的字符串值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为空字符串
	 * @return 字符串值
	 */
	EXPORT_FLAG const char*	get_string(const char* domain, const char* section, const char* key, const char* defVal = "");

	/**
	 * @brief 获取32位整数类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的32位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为0
	 * @return 32位整数值
	 */
	EXPORT_FLAG int32_t		get_int32(const char* domain, const char* section, const char* key, int32_t defVal = 0);

	/**
	 * @brief 获取64位整数类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的64位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为0
	 * @return 64位整数值
	 */
	EXPORT_FLAG int64_t		get_int64(const char* domain, const char* section, const char* key, int64_t defVal = 0);

	/**
	 * @brief 获取无符32位整数类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的无符32位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为0
	 * @return 无符32位整数值
	 */
	EXPORT_FLAG uint32_t	get_uint32(const char* domain, const char* section, const char* key, uint32_t defVal = 0);

	/**
	 * @brief 获取无符64位整数类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的无符64位整数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为0
	 * @return 无符64位整数值
	 */
	EXPORT_FLAG uint64_t	get_uint64(const char* domain, const char* section, const char* key, uint64_t defVal = 0);

	/**
	 * @brief 获取双精度浮点数类型的共享内存值
	 * @details 获取共享内存中指定域、分区和键的双精度浮点数值
	 * @param domain 域名称
	 * @param section 分区名称
	 * @param key 键名称
	 * @param defVal 默认值，当键不存在时返回此值，默认为0
	 * @return 双精度浮点数值
	 */
	EXPORT_FLAG double		get_double(const char* domain, const char* section, const char* key, double defVal = 0);


	/**
	 * @brief 初始化命令器
	 * @details 初始化命令器，用于进程间命令传递
	 * @param name 命令器名称
	 * @param isCmder 是否为命令发送方，默认为false
	 * @param path 共享内存文件路径，默认为空字符串
	 * @return 初始化是否成功
	 */
	EXPORT_FLAG bool		init_cmder(const char* name, bool isCmder = false, const char* path = "");

	/**
	 * @brief 添加命令
	 * @details 向命令器中添加一条命令
	 * @param name 命令器名称
	 * @param cmd 要添加的命令
	 * @return 添加是否成功
	 */
	EXPORT_FLAG bool		add_cmd(const char* name, const char* cmd);

	/**
	 * @brief 获取命令
	 * @details 从命令器中获取一条命令
	 * @param name 命令器名称
	 * @param lastIdx 上次获取的命令索引，会被更新为当前获取的命令索引
	 * @return 命令字符串，如果没有新命令则返回null
	 */
	EXPORT_FLAG const char*	get_cmd(const char* name, uint32_t& lastIdx);
	
#ifdef __cplusplus
}
#endif