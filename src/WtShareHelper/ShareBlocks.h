/**
 * @file ShareBlocks.h
 * @brief WonderTrader共享内存块管理模块头文件
 * @details 定义了共享内存块的数据结构和管理类，用于进程间数据共享和通信
 */

#pragma once
#include <stdint.h>
#include <memory>

#include "../Share/BoostMappingFile.hpp"
#include "../Includes/FasterDefs.h"

USING_NS_WTP;

/**
 * @brief 内存映射文件的智能指针类型
 * @details 使用智能指针管理BoostMappingFile对象，确保资源正确释放
 */
typedef std::shared_ptr<BoostMappingFile> MappedFilePtr;

/**
 * @brief 共享内存块管理命名空间
 * @details 定义了与共享内存相关的数据结构、常量和管理类
 */
namespace shareblock
{
	/**
	 * @brief 共享内存块标识符
	 * @details 用于标识共享内存块的有效性和类型
	 */
	const char BLK_FLAG[] = "&^%$#@!\0";

	/**
	 * @brief 标识符大小
	 */
	const int FLAG_SIZE = 8;
	/**
	 * @brief 最大小节数量
	 * @details 每个共享内存块最多可包含的小节数量
	 */
	const int MAX_SEC_CNT = 64;
	/**
	 * @brief 每个小节最大键值对数量
	 */
	const int MAX_KEY_CNT = 64;
	/**
	 * @brief 命令最大长度
	 */
	const int MAX_CMD_SIZE = 64;

	/**
	 * @brief 值类型定义
	 * @details 用于标识共享内存中存储的数据类型
	 */
	typedef uint64_t ValueType;
	/**
	 * @brief 32位有符号整型
	 */
	const ValueType	SMVT_INT32 = 1;
	/**
	 * @brief 32位无符号整型
	 */
	const ValueType	SMVT_UINT32 = 2;
	/**
	 * @brief 64位有符号整型
	 */
	const ValueType	SMVT_INT64 = 3;
	/**
	 * @brief 64位无符号整型
	 */
	const ValueType	SMVT_UINT64 = 4;
	/**
	 * @brief 双精度浮点型
	 */
	const ValueType	SMVT_DOUBLE = 5;
	/**
	 * @brief 字符串类型
	 */
	const ValueType	SMVT_STRING = 6;

	/**
	 * @brief 各数据类型的大小（字节）
	 * @details 数组索引对应ValueType的值，内容为对应类型的字节大小
	 */
	const std::size_t SMVT_SIZES[] = { 0,4,4,8,8,8,64 };

	#pragma pack(push, 1)
	/**
	 * @brief 键信息结构体
	 * @details 定义共享内存中键的属性信息
	 */
	typedef struct _KeyInfo
	{
		char		_key[32];      /**< 键名，最大长度32字节 */
		ValueType	_type;       /**< 值类型，对应SMVT_*常量 */
		uint32_t	_offset;     /**< 值在数据区的偏移量 */
		uint64_t	_updatetime; /**< 最后更新时间 */
	} KeyInfo;

	/**
	 * @brief 小节信息结构体
	 * @details 定义共享内存中的小节（段落）信息，每个小节包含多个键值对
	 */
	typedef struct _SectionInfo
	{
		char		_name[32];        /**< 小节名称，最大长度32字节 */
		KeyInfo		_keys[MAX_KEY_CNT]; /**< 键信息数组，最多包含MAX_KEY_CNT个键 */
		uint16_t	_count;			/**< 数据条数，即key的个数 */
		uint16_t	_state;			/**< 状态：0-无效，1-生效 */
		uint32_t	_offset;		/**< 记录下一个可分配地址的偏移量 */
		uint64_t	_updatetime;     /**< 最后更新时间 */
		char		_data[1024];      /**< 数据存储区域，大小为1024字节 */

		/**
		 * @brief 获取指定偏移量处的数据指针
		 * @tparam T 数据类型
		 * @param offset 偏移量
		 * @return 转换为T类型的指针
		 */
		template<typename T>
		T* get(uint32_t offset)
		{
			return (T*)(_data + offset);
		}

		/**
		 * @brief 构造函数
		 * @details 初始化小节信息，将所有成员置零
		 */
		_SectionInfo()
		{
			memset(this, 0, sizeof(_SectionInfo));
		}
	} SecInfo;

	/**
	 * @brief 共享内存块结构体
	 * @details 定义共享内存的整体结构，包含多个小节
	 */
	typedef struct _ShmBlock
	{
		char		_flag[8];         /**< 内存块标识符，用于验证内存块有效性 */
		char		_name[32];        /**< 内存块名称，最大长度32字节 */
		SecInfo		_sections[MAX_SEC_CNT]; /**< 小节数组，最多包含MAX_SEC_CNT个小节 */
		uint64_t	_updatetime;     /**< 内存块最后更新时间 */
		uint32_t	_count;          /**< 小节数量 */

		/**
		 * @brief 构造函数
		 * @details 初始化内存块，将所有成员置零
		 */
		_ShmBlock()
		{
			memset(this, 0, sizeof(_ShmBlock));
		}
	} ShmBlock;

	/**
	 * @brief 命令信息结构体
	 * @details 定义共享内存中的命令信息
	 */
	typedef struct _CmdInfo
	{
		uint32_t	_state;              /**< 命令状态 */
		char		_command[MAX_CMD_SIZE]; /**< 命令内容，最大长度为MAX_CMD_SIZE */

		/**
		 * @brief 构造函数
		 * @details 初始化命令信息，将所有成员置零
		 */
		_CmdInfo() { memset(this, 0, sizeof(_CmdInfo)); }
	} CmdInfo;

	/**
	 * @brief 命令块模板结构体
	 * @details 定义共享内存中的命令块，用于进程间命令传递
	 * @tparam N 命令容量，默认为128
	 */
	template <int N = 128>
	struct _CmdBlock
	{
		uint32_t	_capacity = N;      /**< 命令容量 */
	 	volatile uint32_t	_readable;   /**< 可读索引，使用volatile确保多进程可见性 */
		volatile uint32_t	_writable;   /**< 可写索引，使用volatile确保多进程可见性 */
		uint32_t	_cmdpid;             /**< 命令处理进程的PID */
		CmdInfo		_commands[N];        /**< 命令数组 */

		/**
		 * @brief 构造函数
		 * @details 初始化命令块，设置初始状态
		 */
		_CmdBlock():_readable(UINT32_MAX),_writable(0),_cmdpid(0){}
	};

	/**
	 * @brief 默认命令块类型
	 * @details 使用128个命令容量的命令块
	 */
	typedef _CmdBlock<128>	CmdBlock;

	#pragma pack(pop)


	/**
	 * @brief 共享内存块管理类
	 * @details 实现进程间共享内存的创建、访问和管理，采用单例模式
	 */
	class ShareBlocks
	{
	private:
		/**
		 * @brief 私有构造函数
		 * @details 实现单例模式，防止外部直接创建实例
		 */
		ShareBlocks(){}

	public:
		/**
		 * @brief 获取单例对象
		 * @return 返回共享内存块管理类的单例
		 */
		static ShareBlocks& one()
		{
			static ShareBlocks inst;
			return inst;
		}

		/**
		 * @brief 初始化主进程共享内存
		 * @param name 共享内存名称
		 * @param path 共享内存文件路径，默认为空
		 * @return 初始化是否成功
		 */
		bool	init_master(const char* name, const char* path = "");
		
		/**
		 * @brief 初始化从进程共享内存
		 * @param name 共享内存名称，需要与主进程一致
		 * @param path 共享内存文件路径，默认为空
		 * @return 初始化是否成功
		 */
		bool	init_slave(const char* name, const char* path = "");

		/**
		 * @brief 更新从进程共享内存
		 * @param name 共享内存名称
		 * @param bForce 是否强制更新
		 * @return 更新是否成功
		 */
		bool	update_slave(const char* name, bool bForce);
		
		/**
		 * @brief 释放从进程共享内存
		 * @param name 共享内存名称
		 * @return 释放是否成功
		 */
		bool	release_slave(const char* name);

		/**
		 * @brief 获取指定域下的所有小节
		 * @param domain 域名称
		 * @return 小节名称列表
		 */
		std::vector<std::string>	get_sections(const char* domain);
		
		/**
		 * @brief 获取指定域和小节下的所有键
		 * @param domain 域名称
		 * @param section 小节名称
		 * @return 键信息指针列表
		 */
		std::vector<KeyInfo*>		get_keys(const char* domain, const char* section);

		/**
		 * @brief 获取小节的最后更新时间
		 * @param domain 域名称
		 * @param section 小节名称
		 * @return 最后更新时间戳
		 */
		uint64_t get_section_updatetime(const char* domain, const char* section);
		
		/**
		 * @brief 提交小节更改
		 * @param domain 域名称
		 * @param section 小节名称
		 * @return 提交是否成功
		 */
		bool	commit_section(const char* domain, const char* section);

		/**
		 * @brief 删除指定小节
		 * @param domain 域名称
		 * @param section 小节名称
		 * @return 删除是否成功
		 */
		bool	delete_section(const char* domain, const char*section);

		/**
		 * @brief 分配字符串类型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为空字符串
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的字符串指针
		 */
		const char* allocate_string(const char* domain, const char* section, const char* key, const char* initVal = "", bool bForceWrite = false);
		
		/**
		 * @brief 分配32位有符号整型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为0
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的int32_t指针
		 */
		int32_t*	allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal = 0, bool bForceWrite = false);
		
		/**
		 * @brief 分配64位有符号整型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为0
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的int64_t指针
		 */
		int64_t*	allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal = 0, bool bForceWrite = false);
		
		/**
		 * @brief 分配32位无符号整型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为0
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的uint32_t指针
		 */
		uint32_t*	allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal = 0, bool bForceWrite = false);
		
		/**
		 * @brief 分配64位无符号整型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为0
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的uint64_t指针
		 */
		uint64_t*	allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal = 0, bool bForceWrite = false);
		
		/**
		 * @brief 分配双精度浮点型的共享内存
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param initVal 初始值，默认为0
		 * @param bForceWrite 是否强制写入，默认为false
		 * @return 分配的double指针
		 */
		double*		allocate_double(const char* domain, const char* section, const char* key, double initVal = 0, bool bForceWrite = false);

		/**
		 * @brief 设置字符串类型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_string(const char* domain, const char* section, const char* key, const char* val);
		
		/**
		 * @brief 设置32位有符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_int32(const char* domain, const char* section, const char* key, int32_t val);
		
		/**
		 * @brief 设置64位有符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_int64(const char* domain, const char* section, const char* key, int64_t val);
		
		/**
		 * @brief 设置32位无符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_uint32(const char* domain, const char* section, const char* key, uint32_t val);
		
		/**
		 * @brief 设置64位无符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_uint64(const char* domain, const char* section, const char* key, uint64_t val);
		
		/**
		 * @brief 设置双精度浮点型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param val 要设置的值
		 * @return 设置是否成功
		 */
		bool	set_double(const char* domain, const char* section, const char* key, double val);

		/**
		 * @brief 获取字符串类型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为空字符串
		 * @return 字符串值
		 */
		const char*	get_string(const char* domain, const char* section, const char* key, const char* defVal = "");
		
		/**
		 * @brief 获取32位有符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为0
		 * @return 32位有符号整型值
		 */
		int32_t		get_int32(const char* domain, const char* section, const char* key, int32_t defVal = 0);
		
		/**
		 * @brief 获取64位有符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为0
		 * @return 64位有符号整型值
		 */
		int64_t		get_int64(const char* domain, const char* section, const char* key, int64_t defVal = 0);
		
		/**
		 * @brief 获取32位无符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为0
		 * @return 32位无符号整型值
		 */
		uint32_t	get_uint32(const char* domain, const char* section, const char* key, uint32_t defVal = 0);
		
		/**
		 * @brief 获取64位无符号整型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为0
		 * @return 64位无符号整型值
		 */
		uint64_t	get_uint64(const char* domain, const char* section, const char* key, uint64_t defVal = 0);
		
		/**
		 * @brief 获取双精度浮点型的共享内存值
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param defVal 默认值，当键不存在时返回，默认为0
		 * @return 双精度浮点型值
		 */
		double		get_double(const char* domain, const char* section, const char* key, double defVal = 0);

	public:
		/**
		 * @brief 初始化命令器
		 * @param name 命令器名称
		 * @param isCmder 是否为命令发送者，默认为false
		 * @param path 命令器文件路径，默认为空
		 * @return 初始化是否成功
		 */
		bool	init_cmder(const char* name, bool isCmder = false, const char* path = "");
		
		/**
		 * @brief 添加命令
		 * @param name 命令器名称
		 * @param cmd 命令内容
		 * @return 添加是否成功
		 */
		bool	add_cmd(const char* name, const char* cmd);
		
		/**
		 * @brief 获取命令
		 * @param name 命令器名称
		 * @param lastIdx 上次读取的索引，传入参数，会被更新
		 * @return 命令内容，如果没有新命令则返回空
		 */
		const char*	get_cmd(const char* name, uint32_t& lastIdx);

	private:
		/**
		 * @brief 创建或验证共享内存中的键值对
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param vType 值类型
		 * @param secInfo 输出参数，返回小节信息指针
		 * @return 值的指针，如果失败则返回nullptr
		 */
		void*	make_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo);
		
		/**
		 * @brief 检查共享内存中的键值对是否有效
		 * @param domain 域名称
		 * @param section 小节名称
		 * @param key 键名称
		 * @param vType 值类型
		 * @param secInfo 输出参数，返回小节信息指针
		 * @return 值的指针，如果不存在或类型不匹配则返回nullptr
		 */
		void*	check_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo);

	private:
		/**
		 * @brief 共享内存块对结构体
		 * @details 管理单个共享内存块的映射和缓存信息
		 */
		typedef struct _ShmPair
		{
			MappedFilePtr	_domain;    /**< 内存映射文件指针 */
			ShmBlock*		_block;     /**< 内存块指针 */
			bool			_master;    /**< 是否为主进程 */
			uint64_t		_blocktime; /**< 内存块时间戳 */

			/**
			 * @brief 键值映射类型
			 * @details 存储键名称到键信息的映射
			 */
			typedef wt_hashmap<std::string, KeyInfo*>	KVMap;
			
			/**
			 * @brief 键值对结构体
			 * @details 存储小节中的键值对信息
			 */
			typedef struct _KVPair
			{
				uint32_t	_index;  /**< 小节索引 */
				KVMap		_keys;   /**< 键值映射 */
			} KVPair;
			
			/**
			 * @brief 小节映射类型
			 * @details 存储小节名称到键值对的映射
			 */
			typedef wt_hashmap<std::string, KVPair>	SectionMap;
			SectionMap	_sections;  /**< 小节映射 */

			/**
			 * @brief 构造函数
			 * @details 初始化共享内存块对
			 */
			_ShmPair() :_block(nullptr),_master(false)
			{
			}
		}ShmPair;
		/**
		 * @brief 共享内存块映射类型
		 * @details 存储域名称到共享内存块对的映射
		 */
		typedef wt_hashmap<std::string, ShmPair>	ShmBlockMap;
		ShmBlockMap		_shm_blocks;  /**< 共享内存块映射集合 */

		/**
		 * @brief 命令块对结构体
		 * @details 管理单个命令块的映射和状态信息
		 */
		typedef struct _CmdPair
		{
			MappedFilePtr	_domain;  /**< 内存映射文件指针 */
			CmdBlock*		_block;   /**< 命令块指针 */
			bool			_cmder;   /**< 是否为命令发送者 */
		} CmdPair;
		
		/**
		 * @brief 命令块映射类型
		 * @details 存储命令器名称到命令块对的映射
		 */
		typedef wt_hashmap<std::string, CmdPair>	CmdBlockMap;
		CmdBlockMap		_cmd_blocks;  /**< 命令块映射集合 */
	};
}