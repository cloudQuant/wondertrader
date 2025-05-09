/**
 * @file ShareManager.h
 * @brief 共享数据管理器头文件
 * 
 * 定义了ShareManager类，用于管理多进程间的共享数据。
 * 支持不同类型的参数存取和分配，并提供监控参数变更的机制。
 */
#pragma once
#include <stdint.h>
#include <string>

#include "../Share/StdUtils.hpp"
#include "../Includes/FasterDefs.h"
#include "../Share/DLLHelper.hpp"

NS_WTP_BEGIN
class WtUftEngine;
NS_WTP_END
USING_NS_WTP;

/**
 * @brief 初始化主域的函数指针类型
 * @param domain 域名称
 * @param suffix 后缀
 * @return 是否初始化成功
 */
typedef bool (*func_init_master)(const char*, const char*);

/**
 * @brief 获取指定图片更新时间的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @return 更新时间戳
 */
typedef uint64_t(*func_get_section_updatetime)(const char*, const char*);

/**
 * @brief 提交图片的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @return 是否提交成功
 */
typedef bool(*func_commit_section)(const char*, const char*);

/**
 * @brief 分配字符串类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的字符串指针
 */
typedef const char*(*func_allocate_string)(const char*, const char*, const char*, const char*, bool);

/**
 * @brief 分配32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的整型指针
 */
typedef int32_t* (*func_allocate_int32)(const char*, const char*, const char*, int32_t, bool);

/**
 * @brief 分配64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的整型指针
 */
typedef int64_t* (*func_allocate_int64)(const char*, const char*, const char*, int64_t, bool);

/**
 * @brief 分配无符32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的整型指针
 */
typedef uint32_t* (*func_allocate_uint32)(const char*, const char*, const char*, uint32_t, bool);

/**
 * @brief 分配无符64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的整型指针
 */
typedef uint64_t* (*func_allocate_uint64)(const char*, const char*, const char*, uint64_t, bool);

/**
 * @brief 分配浮点型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param initVal 初始值
 * @param bForceWrite 是否强制写入
 * @return 分配的浮点型指针
 */
typedef double*	(*func_allocate_double)(const char*, const char*, const char*, double, bool);

/**
 * @brief 设置字符串类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool (*func_set_string)(const char*, const char*, const char*, const char*);

/**
 * @brief 设置32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool (*func_set_int32)(const char*, const char*, const char*, int32_t);

/**
 * @brief 设置64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool (*func_set_int64)(const char*, const char*, const char*, int64_t);

/**
 * @brief 设置无符32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool (*func_set_uint32)(const char*, const char*, const char*, uint32_t);

/**
 * @brief 设置无符64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool(*func_set_uint64)(const char*, const char*, const char*, uint64_t);

/**
 * @brief 设置浮点型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param val 要设置的值
 * @return 是否设置成功
 */
typedef bool(*func_set_double)(const char*, const char*, const char*, double);
/**
 * @brief 获取字符串类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的字符串值
 */
typedef const char* (*func_get_string)(const char*, const char*, const char*, const char*);

/**
 * @brief 获取32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的整型值
 */
typedef int32_t (*func_get_int32)(const char*, const char*, const char*, int32_t);

/**
 * @brief 获取64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的整型值
 */
typedef int64_t (*func_get_int64)(const char*, const char*, const char*, int64_t);

/**
 * @brief 获取无符32位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的整型值
 */
typedef uint32_t (*func_get_uint32)(const char*, const char*, const char*, uint32_t);

/**
 * @brief 获取无符64位整型类型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的整型值
 */
typedef uint64_t (*func_get_uint64)(const char*, const char*, const char*, uint64_t);

/**
 * @brief 获取浮点型值的函数指针类型
 * @param domain 域名称
 * @param section 图片名称
 * @param key 键名
 * @param defVal 默认值
 * @return 获取的浮点数值
 */
typedef double (*func_get_double)(const char*, const char*, const char*, double);

/**
 * @brief 共享数据管理器类
 * 
 * 管理多进程间的共享数据，支持各种数据类型的存取和分配。
 * 实现了单例模式，提供数据监控和自动更新机制。
 */
class ShareManager
{
private:
	/**
	 * @brief 私有构造函数
	 * 
	 * 初始化基本属性值，实现单例模式
	 */
	ShareManager():_inited(false), _stopped(false), _engine(nullptr), _sync("sync"){}
	
	/**
	 * @brief 析构函数
	 * 
	 * 清理资源，停止并等待监控线程完成
	 */
	~ShareManager()
	{
		_stopped = true;
		if (_worker)
			_worker->join();
	}

public:
	/**
	 * @brief 获取共享数据管理器实例的全局访问点
	 * 
	 * 实现单例模式，确保在整个程序中只有一个实例
	 * 
	 * @return ShareManager& 共享数据管理器的唯一实例
	 */
	static ShareManager& self()
	{
		static ShareManager inst;
		return inst;
	}

	/**
	 * @brief 设置交易引擎对象
	 * 
	 * @param engine 交易引擎指针
	 */
	void	set_engine(WtUftEngine* engine) { _engine = engine; }

	/**
	 * @brief 初始化共享数据管理器
	 * 
	 * 加载指定的共享库模块，并获取相关函数指针
	 * 
	 * @param module 共享模块完整路径
	 * @return bool 初始化是否成功
	 */
	bool	initialize(const char* module);

	/**
	 * @brief 开始监控共享数据变化
	 * 
	 * 启动监控线程定期检查参数更新情况
	 * 
	 * @param microsecs 检查间隔微秒数，如果设为0则进入无限循环检查
	 * @return bool 是否成功启动监控
	 */
	bool	start_watching(uint32_t microsecs);

	/**
	 * @brief 初始化共享域
	 * 
	 * 使用指定标识符初始化共享域和同步域
	 * 
	 * @param id 共享域标识符
	 * @return bool 初始化是否成功
	 */
	bool	init_domain(const char* id);

	/**
	 * @brief 提交参数监控器
	 * 
	 * 提交特定图片区域的更新，并记录更新时间
	 * 
	 * @param section 图片区域名称
	 * @return bool 提交是否成功
	 */
	bool	commit_param_watcher(const char* section);

	/**
	 * @brief 设置字符串类型值
	 * 
	 * 在指定的图片区域设置字符串类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的字符串值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, const char* val);

	/**
	 * @brief 设置32位整型值
	 * 
	 * 在指定的图片区域设置32位整型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的整型值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, int32_t val);

	/**
	 * @brief 设置64位整型值
	 * 
	 * 在指定的图片区域设置64位整型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的整型值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, int64_t val);

	/**
	 * @brief 设置无符32位整型值
	 * 
	 * 在指定的图片区域设置无符32位整型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的整型值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, uint32_t val);

	/**
	 * @brief 设置无符64位整型值
	 * 
	 * 在指定的图片区域设置无符64位整型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的整型值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, uint64_t val);

	/**
	 * @brief 设置浮点型值
	 * 
	 * 在指定的图片区域设置浮点型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param val 要设置的浮点值
	 * @return bool 设置是否成功
	 */
	bool	set_value(const char* section, const char* key, double val);

	/**
	 * @brief 获取字符串类型值
	 * 
	 * 从指定的图片区域获取字符串类型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return const char* 获取到的字符串值
	 */
	const char*	get_value(const char* section, const char* key, const char* defVal = "");

	/**
	 * @brief 获取32位整型值
	 * 
	 * 从指定的图片区域获取32位整型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return int32_t 获取到的整型值
	 */
	int32_t		get_value(const char* section, const char* key, int32_t defVal = 0);

	/**
	 * @brief 获取64位整型值
	 * 
	 * 从指定的图片区域获取64位整型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return int64_t 获取到的整型值
	 */
	int64_t		get_value(const char* section, const char* key, int64_t defVal = 0);

	/**
	 * @brief 获取无符32位整型值
	 * 
	 * 从指定的图片区域获取无符32位整型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return uint32_t 获取到的整型值
	 */
	uint32_t	get_value(const char* section, const char* key, uint32_t defVal = 0);

	/**
	 * @brief 获取无符64位整型值
	 * 
	 * 从指定的图片区域获取无符64位整型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return uint64_t 获取到的整型值
	 */
	uint64_t	get_value(const char* section, const char* key, uint64_t defVal = 0);

	/**
	 * @brief 获取浮点型值
	 * 
	 * 从指定的图片区域获取浮点型的键值
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param defVal 默认值，当键不存在时返回
	 * @return double 获取到的浮点值
	 */
	double		get_value(const char* section, const char* key, double defVal = 0);

	/**
	 * @brief 分配共享字符串类型值
	 * 
	 * 在单向同步区或交换区分配字符串类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为空字符串
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return const char* 分配的字符串指针
	 */
	const char*	allocate_value(const char* section, const char* key, const char* initVal = "", bool bForceWrite = false, bool isExchg = false);
	
	/**
	 * @brief 分配共享32位整型类型值
	 * 
	 * 在单向同步区或交换区分配32位整型类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为0
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return int32_t* 分配的整型指针
	 */
	int32_t*	allocate_value(const char* section, const char* key, int32_t initVal = 0, bool bForceWrite = false, bool isExchg = false);
	
	/**
	 * @brief 分配共享64位整型类型值
	 * 
	 * 在单向同步区或交换区分配64位整型类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为0
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return int64_t* 分配的整型指针
	 */
	int64_t*	allocate_value(const char* section, const char* key, int64_t initVal = 0, bool bForceWrite = false, bool isExchg = false);
	
	/**
	 * @brief 分配共享无符32位整型类型值
	 * 
	 * 在单向同步区或交换区分配无符32位整型类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为0
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return uint32_t* 分配的整型指针
	 */
	uint32_t*	allocate_value(const char* section, const char* key, uint32_t initVal = 0, bool bForceWrite = false, bool isExchg = false);
	
	/**
	 * @brief 分配共享无符64位整型类型值
	 * 
	 * 在单向同步区或交换区分配无符64位整型类型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为0
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return uint64_t* 分配的整型指针
	 */
	uint64_t*	allocate_value(const char* section, const char* key, uint64_t initVal = 0, bool bForceWrite = false, bool isExchg = false);
	
	/**
	 * @brief 分配共享浮点型值
	 * 
	 * 在单向同步区或交换区分配浮点型的键值对
	 * 
	 * @param section 图片区域名称
	 * @param key 键名
	 * @param initVal 初始值，默认为0
	 * @param bForceWrite 是否强制写入，默认为false
	 * @param isExchg 是否在交换区分配，默认为false（即在同步区分配）
	 * @return double* 分配的浮点型指针
	 */
	double*		allocate_value(const char* section, const char* key, double initVal = 0, bool bForceWrite = false, bool isExchg = false);
private:
	bool			_inited;		/**< 是否已初始化标志 */
	std::string		_exchg;		/**< 交换区域标识符 */
	std::string		_sync;		/**< 同步区域标识符 */

	wt_hashmap<std::string, uint64_t>	_secnames;	/**< 图片区及其更新时间映射表 */

	bool			_stopped;	/**< 是否已停止标志 */
	StdThreadPtr	_worker;	/**< 监控线程指针 */
	WtUftEngine*	_engine;	/**< 交易引擎指针 */

	DllHandle		_inst;		/**< 动态库句柄 */
	std::string		_module;	/**< 模块路径 */

	func_init_master _init_master;				/**< 初始化主域的函数指针 */
	func_get_section_updatetime _get_section_updatetime;	/**< 获取图片区更新时间的函数指针 */
	func_commit_section _commit_section;			/**< 提交图片区的函数指针 */

	func_set_double _set_double;	/**< 设置浮点值的函数指针 */
	func_set_int32 _set_int32;		/**< 设置32位整型值的函数指针 */
	func_set_int64 _set_int64;		/**< 设置64位整型值的函数指针 */
	func_set_uint32 _set_uint32;	/**< 设置无符32位整型值的函数指针 */
	func_set_uint64 _set_uint64;	/**< 设置无符64位整型值的函数指针 */
	func_set_string _set_string;	/**< 设置字符串值的函数指针 */

	func_get_double _get_double;	/**< 获取浮点值的函数指针 */
	func_get_int32 _get_int32;		/**< 获取32位整型值的函数指针 */
	func_get_int64 _get_int64;		/**< 获取64位整型值的函数指针 */
	func_get_uint32 _get_uint32;	/**< 获取无符32位整型值的函数指针 */
	func_get_uint64 _get_uint64;	/**< 获取无符64位整型值的函数指针 */
	func_get_string _get_string;	/**< 获取字符串值的函数指针 */

	func_allocate_double _allocate_double;	/**< 分配浮点值的函数指针 */
	func_allocate_int32 _allocate_int32;		/**< 分配32位整型值的函数指针 */
	func_allocate_int64 _allocate_int64;		/**< 分配64位整型值的函数指针 */
	func_allocate_uint32 _allocate_uint32;	/**< 分配无符32位整型值的函数指针 */
	func_allocate_uint64 _allocate_uint64;	/**< 分配无符64位整型值的函数指针 */
	func_allocate_string _allocate_string;	/**< 分配字符串值的函数指针 */
};

