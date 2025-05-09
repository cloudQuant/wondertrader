/*!
 * \file WtLMDB.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief LMDB数据库封装类
 *
 * 本文件实现了对LMDB（Lightning Memory-Mapped Database）的封装，
 * 提供了高效的内存映射型键值数据库操作功能。
 * LMDB是一个高性能的事务性键值存储，在WonderTrader中用于存储各种交易数据。
 * 包含了两个主要类：WtLMDB（数据库基础类）和WtLMDBQuery（数据查询和操作类）。
 */
#pragma once
#include <stdint.h>
#include <string>
#include <functional>
#include <vector>
#include <algorithm>

#if _WIN32
#include <direct.h>
#include <io.h>
#else
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "../Includes/WTSMarcos.h"
#include "lmdb/lmdb.h"

NS_WTP_BEGIN

/**
 * @brief 字符串数组类型
 * 
 * 用于存储数据库查询结果中的键或值的字符串数组
 */
typedef std::vector<std::string> ValueArray;

/**
 * @brief LMDB查询回调函数类型
 * 
 * 用于定义查询结果的回调函数，接收两个ValueArray参数，分别表示键数组和值数组
 */
typedef std::function<void(const ValueArray&, const ValueArray&)> LMDBQueryCallback;

/**
 * @brief LMDB数据库封装基础类
 * 
 * 封装了LMDB数据库的基本操作，包括创建环境、打开数据库、管理数据库句柄等功能
 * 支持只读模式和读写模式，并提供错误处理机制
 * 作为WtLMDBQuery类的底层基础，不直接进行数据操作
 */
class WtLMDB
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化LMDB对象，设置是否为只读模式
	 * 
	 * @param bReadOnly 是否为只读模式，默认为false（读写模式）
	 */
	WtLMDB(bool bReadOnly = false)
		: _env(NULL)
		, _dbi(0)
		, _errno(0)
		, _readonly(bReadOnly)
	{}

	/**
	 * @brief 析构函数
	 * 
	 * 关闭数据库句柄和环境，释放LMDB相关资源
	 */
	~WtLMDB()
	{
		if (_dbi != 0)
			mdb_dbi_close(_env, _dbi);

		if (_env != NULL)
			mdb_env_close(_env);
	}

public:
	/**
	 * @brief 获取LMDB环境句柄
	 * @return MDB_env* LMDB环境句柄
	 */
	inline MDB_env* env() const{ return _env; }

	/**
	 * @brief 获取LMDB数据库句柄
	 * @return MDB_dbi LMDB数据库句柄
	 */
	inline MDB_dbi	dbi() const { return _dbi; }

	/**
	 * @brief 更新数据库句柄
	 * 
	 * 如果数据库句柄还未初始化，则打开数据库并创建句柄
	 * 
	 * @param txn 事务对象指针
	 * @return MDB_dbi 数据库句柄
	 */
	inline MDB_dbi update_dbi(MDB_txn* txn)
	{
		if (_dbi != 0)
			return _dbi;

		_errno = mdb_dbi_open(txn, NULL, 0, &_dbi);
		return _dbi;
	}

	/**
	 * @brief 打开数据库
	 * 
	 * 创建并打开LMDB数据库，如果指定路径不存在则自动创建
	 * 设置数据库的映射大小和权限
	 * 
	 * @param path 数据库文件路径
	 * @param mapsize 内存映射大小，默认为16MB
	 * @return bool 是否成功打开数据库
	 */
	bool open(const char* path, std::size_t mapsize = 16*1024*1024)
	{
#if _MSC_VER
        int ret = _access(path, 0);
#else
        int ret = access(path, 0);
#endif
		if(ret != 0)
		{
#if _WIN32
			_mkdir(path);
#else
			mkdir(path, 777);
#endif
		}

		int _errno = mdb_env_create(&_env);
		if (_errno != MDB_SUCCESS)
			return false;

		_errno = mdb_env_open(_env, path, 0, 0664);
		if (_errno != MDB_SUCCESS)
			return false;

		_errno = mdb_env_set_mapsize(_env, mapsize);

		return true;
	}

	/**
	 * @brief 更新错误码
	 * @param error LMDB操作返回的错误码
	 */
	inline void update_errno(int error) { _errno = error; }

	/**
	 * @brief 检查是否有错误
	 * @return bool 是否发生错误
	 */
	inline bool has_error() const { return _errno != MDB_SUCCESS; }

	/**
	 * @brief 检查是否为只读模式
	 * @return bool 是否为只读模式
	 */
	inline bool is_readonly() const { return _readonly; }

	/**
	 * @brief 获取错误信息
	 * @return const char* 错误信息字符串
	 */
	inline const char* errmsg()
	{
		return mdb_strerror(_errno);
	}

private:
	MDB_env*	_env;
	MDB_dbi		_dbi;
	int			_errno;
	bool		_readonly;
};

/**
 * @brief LMDB数据库查询和操作类
 * 
 * 基于WtLMDB实现的数据库查询和操作类，提供了各种数据库操作功能
 * 支持事务操作、数据写入、单条查询、区间查询、范围查询等功能
 * 自动管理事务生命周期，并提供提交和回滚操作
 */
class WtLMDBQuery
{
public:
	/**
	 * @brief 构造函数
	 * 
	 * 初始化LMDB查询对象，创建事务并连接到已存在的数据库
	 * 设置只读模式，并设置数据库句柄
	 * 
	 * @param db WtLMDB数据库对象引用
	 */
	WtLMDBQuery(WtLMDB& db)
		: _txn(NULL)
		, _commited(false)
		, _db(db)
	{
		_readonly = db.is_readonly();
		_db.update_errno(mdb_txn_begin(_db.env(), NULL, (_readonly ? MDB_RDONLY : 0), &_txn));
		_dbi = _db.update_dbi(_txn);
	}
	
	/**
	 * @brief 析构函数
	 * 
	 * 根据不同情况自动处理事务：
	 * - 只读模式下中止事务
	 * - 非只读模式且未提交时自动提交事务
	 */
	~WtLMDBQuery()
	{
		if (_readonly)
			mdb_txn_abort(_txn);
		else if(!_commited)
			_db.update_errno(mdb_txn_commit(_txn));
	}

public:
	/**
	 * @brief 回滚事务
	 * 
	 * 中止当前事务，取消所有未提交的修改
	 * 如果事务已提交，则不执行任何操作
	 */
	inline void	rollback()
	{
		if (_commited)
			return;

		mdb_txn_abort(_txn);
		_commited = true;
	}

	/**
	 * @brief 提交事务
	 * 
	 * 提交当前事务，将所有修改写入数据库
	 * 如果事务已提交或在只读模式下，则不执行任何操作
	 */
	inline void	commit()
	{
		if (_commited || _readonly)
			return;

		_db.update_errno(mdb_txn_commit(_txn));
		_commited = true;
	}

	/**
	 * @brief 写入数据（字符串版本）
	 * 
	 * 将字符串键值对写入数据库，不自动提交事务
	 * 
	 * @param key 键字符串
	 * @param val 值字符串
	 * @return bool 操作是否成功
	 */
	bool put(const std::string& key, const std::string& val)
	{
		return put((void*)key.data(), key.size(), (void*)val.data(), val.size());
	}

	/**
	 * @brief 写入数据并提交（字符串版本）
	 * 
	 * 将字符串键值对写入数据库，并自动提交事务
	 * 
	 * @param key 键字符串
	 * @param val 值字符串
	 * @return bool 操作是否成功
	 */
	bool put_and_commit(const std::string& key, const std::string& val)
	{
		return put_and_commit((void*)key.data(), key.size(), (void*)val.data(), val.size());
	}

	/**
	 * @brief 写入数据（原始数据版本）
	 * 
	 * 将原始二进制数据作为键值对写入数据库，不自动提交事务
	 * 
	 * @param key 键数据指针
	 * @param klen 键数据长度
	 * @param val 值数据指针
	 * @param vlen 值数据长度
	 * @return bool 操作是否成功
	 */
	bool put(void* key, std::size_t klen, void* val, std::size_t vlen)
	{
		MDB_val mKey, mData;
		mKey.mv_data = key;
		mKey.mv_size = klen;

		mData.mv_data = val;
		mData.mv_size = vlen;
		int _errno = mdb_put(_txn, _dbi, &mKey, &mData, 0);
		_db.update_errno(_errno);
		return (_errno == MDB_SUCCESS);
	}

	/**
	 * @brief 写入数据并提交（原始数据版本）
	 * 
	 * 将原始二进制数据作为键值对写入数据库，并自动提交事务
	 * 
	 * @param key 键数据指针
	 * @param klen 键数据长度
	 * @param val 值数据指针
	 * @param vlen 值数据长度
	 * @return bool 操作是否成功，写入和提交都成功时返回true
	 */
	bool put_and_commit(void* key, std::size_t klen, void* val, std::size_t vlen)
	{
		MDB_val mKey, mData;
		mKey.mv_data = key;
		mKey.mv_size = klen;

		mData.mv_data = val;
		mData.mv_size = vlen;
		int _errno = mdb_put(_txn, _dbi, &mKey, &mData, 0);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return false;

		_errno = mdb_txn_commit(_txn);
		_db.update_errno(_errno);
		_commited = true;
		return (_errno == MDB_SUCCESS);
	}

	/**
	 * @brief 获取指定键的数据（字符串版本）
	 * 
	 * 根据字符串键来获取对应的值
	 * 
	 * @param key 要查询的键字符串
	 * @return std::string 查询到的值字符串，如果不存在则返回空字符串
	 */
	std::string get(const std::string& key)
	{
		return get((void*)key.data(), key.size());
	}
	
	/**
	 * @brief 获取指定键的数据（原始数据版本）
	 * 
	 * 使用数据库游标查找并返回指定键对应的值
	 * 
	 * @param key 要查询的键数据指针
	 * @param klen 键数据长度
	 * @return std::string 查询到的值字符串，如果不存在或出错则返回空字符串
	 */
	std::string get(void* key, std::size_t klen)
	{
		MDB_cursor* cursor;
		int _errno = mdb_cursor_open(_txn, _dbi, &cursor);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return std::move(std::string());

		MDB_val mKey, mData;
		mKey.mv_data = key;
		mKey.mv_size = klen;

		_errno = mdb_cursor_get(cursor, &mKey, &mData, MDB_NEXT);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return std::move(std::string());

		auto ret = std::string((char*)mData.mv_data, mData.mv_size);
		mdb_cursor_close(cursor);
		return std::move(ret);
	}
	
	/**
	 * @brief 获取指定区间内的数据
	 * 
	 * 查询指定键区间[lower_key, upper_key]内的所有键值对数据
	 * 通过回调函数返回结果集
	 * 
	 * @param lower_key 区间下限键
	 * @param upper_key 区间上限键
	 * @param cb 查询结果回调函数
	 * @return int 返回符合条件的数据条数
	 */
	int get_range(const std::string& lower_key, const std::string& upper_key, LMDBQueryCallback cb)
	{
		MDB_cursor* cursor;
		int _errno = mdb_cursor_open(_txn, _dbi, &cursor);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return 0;

		MDB_val lKey, rKey, mData;
		lKey.mv_data = (void*)lower_key.data();
		lKey.mv_size = lower_key.size();

		rKey.mv_data = (void*)upper_key.data();
		rKey.mv_size = upper_key.size();
		
		if (_errno != MDB_SUCCESS)
			return 0;

		int cnt = 0;
		MDB_cursor_op op = MDB_SET_RANGE;
		std::vector<std::string> ayKeys, ayVals;
		for(; (_errno = mdb_cursor_get(cursor, &lKey, &mData, op))==MDB_SUCCESS;)
		{
			_db.update_errno(_errno);
			if(_errno == MDB_NOTFOUND)
				break;

			if(memcmp(lKey.mv_data, rKey.mv_data, lKey.mv_size) > 0)
				break;

			//回调
			//cb(std::string((char*)lKey.mv_data, lKey.mv_size), std::string((char*)mData.mv_data, mData.mv_size), false);
			ayKeys.emplace_back(std::string((char*)lKey.mv_data, lKey.mv_size));
			ayVals.emplace_back(std::string((char*)mData.mv_data, mData.mv_size));
			cnt++;
			op = MDB_NEXT;
		} 

		cb(ayKeys, ayVals);
		mdb_cursor_close(cursor);
		return cnt;
	}

	/**
	 * @brief 获取限定数量的区间上限之前的数据
	 * 
	 * 从upperKey往前查找指定数量的数据，然后进行倒序返回。
	 * 主要用于获取指定时间点之前的有限数量的历史数据。
	 * 
	 * @param lower_key 区间下限键，防止读取到其他合约的数据
	 * @param upper_key 区间上限键，从这个键开始向前查找
	 * @param count 要返回的数据条数
	 * @param cb 查询结果回调函数
	 * @return int 返回实际获取的数据条数
	 */
	int get_lowers(const std::string& lower_key, const std::string& upper_key, int count, LMDBQueryCallback cb)
	{
		MDB_cursor* cursor;
		int _errno = mdb_cursor_open(_txn, _dbi, &cursor);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return 0;

		MDB_val rKey, mData;
		rKey.mv_data = (void*)upper_key.data();
		rKey.mv_size = upper_key.size();

		int cnt = 0;
		std::vector<std::string> ayKeys, ayVals;
		_errno = mdb_cursor_get(cursor, &rKey, &mData, MDB_SET_RANGE);
		_db.update_errno(_errno);

		if (_errno == MDB_NOTFOUND)
		{
			_errno = mdb_cursor_get(cursor, &rKey, &mData, MDB_LAST);
			_db.update_errno(_errno);
		}

		for (; _errno != MDB_NOTFOUND;)
		{
			//往前查找，所以如果拿到的key，比右边界大，则直接往前退回一条
			if (memcmp(rKey.mv_data, upper_key.data(), upper_key.size()) > 0)
			{
				_errno = mdb_cursor_get(cursor, &rKey, &mData, MDB_PREV);
				_db.update_errno(_errno);
				continue;
			}

			if (memcmp(rKey.mv_data, lower_key.data(), lower_key.size()) < 0)
				break;

			//回调
			ayKeys.emplace_back(std::string((char*)rKey.mv_data, rKey.mv_size));
			ayVals.emplace_back(std::string((char*)mData.mv_data, mData.mv_size));
			cnt++;

			//如果找到目标数量，则退出
			if(cnt == count)
				break;
			
			_errno = mdb_cursor_get(cursor, &rKey, &mData, MDB_PREV);
			_db.update_errno(_errno);
		}

		//向前查找，是逆序的，需要做一个reverse
		std::reverse(ayKeys.begin(), ayKeys.end());
		std::reverse(ayVals.begin(), ayVals.end());
		
		cb(ayKeys, ayVals);
		mdb_cursor_close(cursor);
		return cnt;
	}

	/**
	 * @brief 获取限定数量的区间下限之后的数据
	 * 
	 * 从lowerKey往后查找指定数量的数据，按照键的升序返回。
	 * 主要用于获取指定时间点之后的有限数量的数据。
	 * 
	 * @param lower_key 区间下限键，从这个键开始向后查找
	 * @param upper_key 区间上限键，防止读取到其他合约的数据
	 * @param count 要返回的数据条数
	 * @param cb 查询结果回调函数
	 * @return int 返回实际获取的数据条数
	 */
	int get_uppers(const std::string& lower_key, const std::string& upper_key, int count, LMDBQueryCallback cb)
	{
		MDB_cursor* cursor;
		int _errno = mdb_cursor_open(_txn, _dbi, &cursor);
		if (_errno != MDB_SUCCESS)
			return 0;

		MDB_val bKey, mData;
		bKey.mv_data = (void*)lower_key.data();
		bKey.mv_size = lower_key.size();

		int cnt = 0;
		std::vector<std::string> ayKeys, ayVals;
		_errno = mdb_cursor_get(cursor, &bKey, &mData, MDB_SET_RANGE);
		_db.update_errno(_errno);
		for (; _errno != MDB_NOTFOUND;)
		{
			if (memcmp(bKey.mv_data, upper_key.data(), upper_key.size()) > 0)
				break;

			//回调
			ayKeys.emplace_back(std::string((char*)bKey.mv_data, bKey.mv_size));
			ayVals.emplace_back(std::string((char*)mData.mv_data, mData.mv_size));
			cnt++;

			//如果找到目标数量，则退出
			if (cnt == count)
				break;

			_errno = mdb_cursor_get(cursor, &bKey, &mData, MDB_NEXT);
			_db.update_errno(_errno);
		}

		cb(ayKeys, ayVals);
		mdb_cursor_close(cursor);
		return cnt;
	}

	/**
	 * @brief 获取数据库中的所有数据
	 * 
	 * 读取数据库中的所有键值对数据，通过回调函数返回结果集
	 * 
	 * @param cb 查询结果回调函数
	 * @return int 返回数据库中的数据条数
	 */
	inline int get_all(LMDBQueryCallback cb)
	{
		MDB_cursor* cursor;
		int _errno = mdb_cursor_open(_txn, _dbi, &cursor);
		if (_errno != MDB_SUCCESS)
			return 0;

		MDB_val bKey, mData;
		std::vector<std::string> ayKeys, ayVals;
		for (; _errno != MDB_NOTFOUND;)
		{
			_errno = mdb_cursor_get(cursor, &bKey, &mData, MDB_NEXT);
			_db.update_errno(_errno);

			ayKeys.emplace_back(std::string((const char*)bKey.mv_data, bKey.mv_size));
			ayVals.emplace_back(std::string((const char*)mData.mv_data, mData.mv_size));
		}
		cb(ayKeys, ayVals);
		return (int)ayVals.size();
	}

private:
	WtLMDB&		_db;
	MDB_txn*	_txn;
	MDB_dbi		_dbi;
	bool		_readonly;
	bool		_commited;
};

NS_WTP_END
