/*!   
 * \file WtKVCache.hpp
 * \project	WonderTrader
 * 
 * \brief 键值对快速缓存实现
 * 
 * 该文件实现了一个简单的基于内存映射文件的键值对缓存机制，
 * 支持快速的键值存取操作以及持久化存储。
 *
 */

#pragma once
#include "SpinMutex.hpp"
#include "BoostFile.hpp"
#include "BoostMappingFile.hpp"
#include "../Includes/FasterDefs.h"

/**
 * @brief 缓存初始容量递增步长
 * @details 创建新缓存时的初始容量，也是扩容时的最小增量单位
 */
#define SIZE_STEP 200

/**
 * @brief 缓存文件标识符
 * @details 用于标记缓存文件的特殊字符串，验证文件有效性
 */
#define CACHE_FLAG "&^%$#@!\0"

/**
 * @brief 标识符长度
 * @details 缓存文件标识符的字节长度
 */
#define FLAG_SIZE 8

/**
 * @brief BoostMappingFile的智能指针类型
 * @details 用于安全管理内存映射文件对象
 */
typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

#pragma warning(disable:4200)

NS_WTP_BEGIN

/**
 * @brief 缓存日志记录器类型
 * @details 定义一个函数对象类型，用于记录缓存操作的日志信息
 */
typedef std::function<void(const char*)> CacheLogger;

/**
 * @brief 键值对缓存实现类
 * 
 * @details 基于内存映射文件的持久化键值对缓存实现，
 * 支持高效的键值存取、日期检查及自动恢复功能
 */
class WtKVCache
{
public:
	/**
	 * @brief 默认构造函数
	 */
	WtKVCache() {}
	
	/**
	 * @brief 复制构造函数（已删除）
	 * @details 不允许复制构造，避免多个对象操作同一个缓存文件
	 */
	WtKVCache(const WtKVCache&) = delete;
	
	/**
	 * @brief 赋值运算符（已删除）
	 * @details 不允许对象赋值，避免资源冲突
	 */
	WtKVCache& operator=(const WtKVCache&) = delete;

private:
	
	/**
	 * @brief 缓存项结构体
	 * @details 存储单个键值对的结构
	 */
	typedef struct _CacheItem
	{
		/**
		 * @brief 键名字符数组，最大支持64字节
		 */
		char	_key[64] = { 0 };
		
		/**
		 * @brief 值字符数组，最大支持64字节
		 */
		char	_val[64] = { 0 };
	} CacheItem;

	/**
	 * @brief 缓存块结构体
	 * @details 存储所有缓存项和元数据的存储单元，直接映射到文件
	 */
	typedef struct CacheBlock
	{
		/**
		 * @brief 块标识符，用于验证缓存文件合法性
		 */
		char		_blk_flag[FLAG_SIZE];
		
		/**
		 * @brief 当前实际存储的缓存项数量
		 */
		uint32_t	_size;
		
		/**
		 * @brief 当前缓存块的总容量
		 */
		uint32_t	_capacity;
		
		/**
		 * @brief 缓存日期，用于验证缓存是否过期
		 */
		uint32_t	_date;
		
		/**
		 * @brief 可变长度的缓存项数组
		 * @note 使用C99的柱式数组实现可变长度结构
		 */
		CacheItem	_items[0];
	} CacheBlock;

	/**
	 * @brief 缓存块和文件的关联结构
	 * @details 将内存中的缓存块与持久化存储文件关联起来
	 */
	typedef struct _CacheBlockPair
	{
		/**
		 * @brief 指向内存映射的缓存块的指针
		 */
		CacheBlock*		_block;
		
		/**
		 * @brief 指向内存映射文件的智能指针
		 */
		BoostMFPtr		_file;

		/**
		 * @brief 默认构造函数
		 * @details 初始化成员变量为空
		 */
		_CacheBlockPair()
		{
			_block = NULL;
			_file = NULL;
		}
	} CacheBlockPair;

	/**
	 * @brief 当前缓存块和文件对象
	 */
	CacheBlockPair	_cache;
	
	/**
	 * @brief 内部索引更新锁
	 * @details 使用自旋锁保护资源写入时的线程安全
	 */
	SpinMutex		_lock;
	
	/**
	 * @brief 键值索引哈希表
	 * @details 存储键与缓存项索引的映射关系，加速查找
	 */
	wt_hashmap<std::string, uint32_t> _indice;

private:
	/**
	 * @brief 调整缓存容量
	 * 
	 * @param newCap 新的期望容量
	 * @param logger 日志记录回调函数，用于记录扩容过程中的错误信息
	 * @return bool 扩容是否成功
	 * @note 调用此函数前应保证线程安全
	 */
	bool	resize(uint32_t newCap, CacheLogger logger = nullptr)
	{
		if (_cache._file == NULL)
			return false;

		//调用该函数之前,应该保证线程安全了
		CacheBlock* cBlock = _cache._block;
		if (cBlock->_capacity >= newCap)
			return _cache._file->addr();

		std::string filename = _cache._file->filename();
		uint64_t uOldSize = sizeof(CacheBlock) + sizeof(CacheItem)*cBlock->_capacity;
		uint64_t uNewSize = sizeof(CacheBlock) + sizeof(CacheItem)*newCap;
		std::string data;
		data.resize((std::size_t)(uNewSize - uOldSize), 0);
		try
		{
			BoostFile f;
			f.open_existing_file(filename.c_str());
			f.seek_to_end();
			f.write_file(data.c_str(), data.size());
			f.close_file();
		}
		catch (std::exception&)
		{
			if (logger) logger("Got an exception while resizing cache file");
			return false;
		}


		_cache._file.reset();
		BoostMappingFile* pNewMf = new BoostMappingFile();
		try
		{
			if (!pNewMf->map(filename.c_str()))
			{
				delete pNewMf;
				if (logger) logger("Mapping cache file failed");
				return false;
			}
		}
		catch (std::exception&)
		{
			if (logger) logger("Got an exception while mapping cache file");
			return false;
		}

		_cache._file.reset(pNewMf);

		_cache._block = (CacheBlock*)_cache._file->addr();
		_cache._block->_capacity = newCap;
		return true;
	}

public:
	/**
	 * @brief 初始化缓存
	 * 
	 * @param filename 缓存文件路径
	 * @param uDate 当前日期，用于检查缓存是否过期
	 * @param logger 日志记录回调函数
	 * @return bool 初始化是否成功
	 * @details 初始化缓存对象，如果文件不存在则创建新文件，如果存在则映射到内存
	 * 并检查日期是否一致，如果不一致则重置缓存
	 */
	bool	init(const char* filename, uint32_t uDate, CacheLogger logger = nullptr)
	{
		bool isNew = false;
		if (!BoostFile::exists(filename))
		{
			uint64_t uSize = sizeof(CacheBlock) + sizeof(CacheItem) * SIZE_STEP;
			BoostFile bf;
			bf.create_new_file(filename);
			bf.truncate_file((uint32_t)uSize);
			bf.close_file();

			isNew = true;
		}

		_cache._file.reset(new BoostMappingFile);
		if (!_cache._file->map(filename))
		{
			_cache._file.reset();
			if (logger) logger("Mapping cache file failed");
			return false;
		}
		_cache._block = (CacheBlock*)_cache._file->addr();

		if (!isNew &&  _cache._block->_date != uDate)
		{
			 _cache._block->_size = 0;
			 _cache._block->_date = uDate;

			memset(& _cache._block->_items, 0, sizeof(CacheItem)* _cache._block->_capacity);

			if (logger) logger("Cache file reset due to a different date");
		}

		if (isNew)
		{
			 _cache._block->_capacity = SIZE_STEP;
			 _cache._block->_size = 0;
			 _cache._block->_date = uDate;
			strcpy( _cache._block->_blk_flag, CACHE_FLAG);
		}
		else
		{
			//检查缓存文件是否有问题,要自动恢复
			do
			{
				uint64_t uSize = sizeof(CacheBlock) + sizeof(CacheItem) *  _cache._block->_capacity;
				uint64_t realSz =  _cache._file->size();
				if (realSz != uSize)
				{
					uint32_t realCap = (uint32_t)((realSz - sizeof(CacheBlock)) / sizeof(CacheItem));
					uint32_t markedCap =  _cache._block->_capacity;
					//文件大小不匹配,一般是因为capacity改了,但是实际没扩容
					//这是做一次扩容即可
					 _cache._block->_capacity = realCap;
					 _cache._block->_size = (realCap < markedCap) ? realCap : markedCap;
				}

			} while (false);
		}

		//这里把索引加到hashmap中
		for (uint32_t i = 0; i < _cache._block->_size; i++)
			_indice[_cache._block->_items[i]._key] = i;

		return true;
	}

	/**
	 * @brief 清空缓存
	 * 
	 * @details 清空缓存内所有键值对和索引，线程安全
	 */
	inline void clear()
	{
		_lock.lock();

		if (_cache._block == NULL)
			return;

		_indice.clear();

		memset(_cache._block->_items, 0, sizeof(CacheItem)*_cache._block->_capacity);
		_cache._block->_size = 0;

		_lock.unlock();
	}

	/**
	 * @brief 获取指定键的值
	 * 
	 * @param key 要查询的键
	 * @return const char* 键对应的值，如果键不存在则返回空字符串
	 */
	inline const char*	get(const char* key) const
	{
		auto it = _indice.find(key);
		if (it == _indice.end())
			return "";

		return _cache._block->_items[it->second]._val;
	}

	/**
	 * @brief 添加或更新键值对
	 * 
	 * @param key 要添加或更新的键
	 * @param val 要设置的值
	 * @param len 值的长度，为0时使用值的字符串长度
	 * @param logger 日志记录回调函数
	 * @details 如果键已存在，则更新其值；如果键不存在，则添加新的键值对
	 * 当添加新键值对时，如果缓存已满，会自动扩容
	 */
	void	put(const char* key, const char*val, std::size_t len = 0, CacheLogger logger = nullptr)
	{
		auto it = _indice.find(key);
		if (it != _indice.end())
		{
			wt_strcpy(_cache._block->_items[it->second]._val, val, len);
		}
		else
		{
			_lock.lock();
			if(_cache._block->_size == _cache._block->_capacity)
				resize(_cache._block->_capacity*2, logger);

			_indice[key] = _cache._block->_size;
			wt_strcpy(_cache._block->_items[_cache._block->_size]._key, key);
			wt_strcpy(_cache._block->_items[_cache._block->_size]._val, val, len);
			_cache._block->_size += 1;
			_lock.unlock();
		}
	}

	/**
	 * @brief 检查缓存中是否存在指定的键
	 * 
	 * @param key 要检查的键
	 * @return bool 如果键存在返回true，否则返回false
	 */
	inline bool	has(const char* key) const 
	{
		return (_indice.find(key) != _indice.end());
	}

	/**
	 * @brief 获取当前缓存中的键值对数量
	 * 
	 * @return uint32_t 当前缓存中存储的键值对数量
	 */
	inline uint32_t size() const
	{
		if (_cache._block == 0)
			return 0;

		return _cache._block->_size;
	}

	/**
	 * @brief 获取当前缓存的总容量
	 * 
	 * @return uint32_t 当前缓存可容纳的键值对最大数量
	 */
	inline uint32_t capacity() const
	{
		if (_cache._block == 0)
			return 0;

		return _cache._block->_capacity;
	}
};

NS_WTP_END