#pragma once
#include <stdint.h>
#include <string>
#include <functional>
#include <vector>
#include <algorithm>
#include <memory>
#include <cstring>

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

typedef std::vector<std::string> ValueArray;
typedef std::function<void(const ValueArray&, const ValueArray&)> LMDBQueryCallback;
typedef int (*LMDBRangeCursorGetOverride)(void*, MDB_cursor*, MDB_val*, MDB_val*, MDB_cursor_op);

inline int compare_lmdb_keys(const void* lhs, std::size_t lhs_size, const void* rhs, std::size_t rhs_size)
{
	const std::size_t prefix_size = (std::min)(lhs_size, rhs_size);
	if (prefix_size != 0)
	{
		const int prefix_order = std::memcmp(lhs, rhs, prefix_size);
		if (prefix_order != 0)
			return prefix_order;
	}
	if (lhs_size < rhs_size)
		return -1;
	if (lhs_size > rhs_size)
		return 1;
	return 0;
}

class WtLMDB
{
public:
	WtLMDB(bool bReadOnly = false)
		: _env(NULL)
		, _dbi(0)
		, _errno(0)
		, _readonly(bReadOnly)
	{}

	~WtLMDB()
	{
		if (_dbi != 0)
			mdb_dbi_close(_env, _dbi);

		if (_env != NULL)
			mdb_env_close(_env);
	}

public:
	inline MDB_env* env() const{ return _env; }
	inline MDB_dbi	dbi() const { return _dbi; }

	inline MDB_dbi update_dbi(MDB_txn* txn)
	{
		if (_dbi != 0)
			return _dbi;

		_errno = mdb_dbi_open(txn, NULL, 0, &_dbi);
		return _dbi;
	}

	bool open(const char* path, std::size_t mapsize = 16*1024*1024)
	{
		return open_impl(path, mapsize, &mdb_env_set_mapsize);
	}

	inline void update_errno(int error) { _errno = error; }

	inline bool has_error() const { return _errno != MDB_SUCCESS; }

	inline bool is_readonly() const { return _readonly; }

	inline const char* errmsg()
	{
		return mdb_strerror(_errno);
	}

private:
	friend struct WtLMDBTestAccess;
	typedef int (*MapSizeSetter)(MDB_env*, std::size_t);

	bool open_impl(const char* path, std::size_t mapsize, MapSizeSetter set_mapsize)
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
			mkdir(path, 0777);
#endif
		}

		_errno = mdb_env_create(&_env);
		if (_errno != MDB_SUCCESS)
		{
			if (_env != NULL)
				mdb_env_close(_env);
			_env = NULL;
			_dbi = 0;
			return false;
		}

		_errno = mdb_env_open(_env, path, 0, 0664);
		if (_errno != MDB_SUCCESS)
		{
			mdb_env_close(_env);
			_env = NULL;
			_dbi = 0;
			return false;
		}

		_errno = set_mapsize(_env, mapsize);
		if (_errno != MDB_SUCCESS)
		{
			mdb_env_close(_env);
			_env = NULL;
			_dbi = 0;
			return false;
		}

		return true;
	}

	MDB_env*	_env;
	MDB_dbi		_dbi;
	int			_errno;
	bool		_readonly;
};

class WtLMDBQuery
{
public:
	WtLMDBQuery(WtLMDB& db)
		: _db(db)
	, _txn(NULL)
		, _commited(false)
		, _range_cursor_get_override(NULL)
		, _range_cursor_get_context(NULL)
		{
		_readonly = db.is_readonly();
		int status = mdb_txn_begin(_db.env(), NULL, (_readonly ? MDB_RDONLY : 0), &_txn);
		_db.update_errno(status);
		_dbi = status == MDB_SUCCESS ? _db.update_dbi(_txn) : _db.dbi();
	}
	
	~WtLMDBQuery()
	{
		if (_txn == NULL)
			return;
		if (_readonly)
			mdb_txn_abort(_txn);
		else if(!_commited)
			_db.update_errno(mdb_txn_commit(_txn));
	}

public:
	inline void	rollback()
	{
		if (_commited || _txn == NULL)
			return;

		mdb_txn_abort(_txn);
		_commited = true;
	}

	inline void	commit()
	{
		if (_commited || _readonly || _txn == NULL)
			return;

		_db.update_errno(mdb_txn_commit(_txn));
		_commited = true;
	}

	bool put(const std::string& key, const std::string& val)
	{
		return put((void*)key.data(), key.size(), (void*)val.data(), val.size());
	}

	bool put_and_commit(const std::string& key, const std::string& val)
	{
		return put_and_commit((void*)key.data(), key.size(), (void*)val.data(), val.size());
	}

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

	std::string get(const std::string& key)
	{
		return get((void*)key.data(), key.size());
	}

	/*
	 *	读取指定key的数据
	 */
	std::string get(void* key, std::size_t klen)
	{
		MDB_cursor* raw_cursor = NULL;
		int _errno = mdb_cursor_open(_txn, _dbi, &raw_cursor);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return std::string();
		auto close_cursor = [](MDB_cursor* cursor) { if (cursor != NULL) mdb_cursor_close(cursor); };
		std::unique_ptr<MDB_cursor, decltype(close_cursor)> cursor(raw_cursor, close_cursor);

		MDB_val mKey, mData;
		mKey.mv_data = key;
		mKey.mv_size = klen;

		_errno = mdb_cursor_get(cursor.get(), &mKey, &mData, MDB_SET);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return std::string();

		auto ret = mData.mv_size == 0 ? std::string() : std::string((char*)mData.mv_data, mData.mv_size);
		return ret;
	}

	/*
	 *	读取区间数据（兼容旧接口）。LMDB 迭代异常时仍按旧接口返回已读取数量
	 *	并将已读部分传给回调；新代码应使用 get_range_checked。
	 */
	int get_range(const std::string& lower_key, const std::string& upper_key, LMDBQueryCallback cb)
	{
		if (upper_key.empty())
		{
			_db.update_errno(MDB_SUCCESS);
			cb(ValueArray(), ValueArray());
			return 0;
		}

		MDB_cursor* cursor = NULL;
		int status = mdb_cursor_open(_txn, _dbi, &cursor);
		_db.update_errno(status);
		if (status != MDB_SUCCESS)
			return 0;

		MDB_val lKey = { 0, 0 }, rKey = { 0, 0 }, mData = { 0, 0 };
		lKey.mv_data = (void*)lower_key.data();
		lKey.mv_size = lower_key.size();

		rKey.mv_data = (void*)upper_key.data();
		rKey.mv_size = upper_key.size();

		int cnt = 0;
		MDB_cursor_op op = lower_key.empty() ? MDB_FIRST : MDB_SET_RANGE;
		std::vector<std::string> ayKeys, ayVals;
		for (; (status = mdb_cursor_get(cursor, &lKey, &mData, op)) == MDB_SUCCESS;)
		{
			_db.update_errno(status);

			if (compare_lmdb_keys(lKey.mv_data, lKey.mv_size, rKey.mv_data, rKey.mv_size) > 0)
				break;

			ayKeys.emplace_back(lKey.mv_size == 0 ? std::string() : std::string((const char*)lKey.mv_data, lKey.mv_size));
			ayVals.emplace_back(mData.mv_size == 0 ? std::string() : std::string((const char*)mData.mv_data, mData.mv_size));
			cnt++;
			op = MDB_NEXT;
		}

		cb(ayKeys, ayVals);
		mdb_cursor_close(cursor);
		return cnt;
	}

	/*
	 *	完整扫描后的安全区间读取。正常返回记录数；游标打开或扫描失败返回 -1，
	 *	并将 LMDB 错误保存在 WtLMDB。只有扫描完整成功后才调用回调。
	 */
	int get_range_checked(const std::string& lower_key, const std::string& upper_key, LMDBQueryCallback cb)
	{
		if (_txn == NULL)
			return -1;
		if (upper_key.empty())
		{
			_db.update_errno(MDB_SUCCESS);
			cb(ValueArray(), ValueArray());
			return 0;
		}

		MDB_cursor* raw_cursor = NULL;
		int status = mdb_cursor_open(_txn, _dbi, &raw_cursor);
		_db.update_errno(status);
		if (status != MDB_SUCCESS)
		{
			if (raw_cursor != NULL)
				mdb_cursor_close(raw_cursor);
			return -1;
		}
		auto close_cursor = [](MDB_cursor* cursor) { if (cursor != NULL) mdb_cursor_close(cursor); };
		std::unique_ptr<MDB_cursor, decltype(close_cursor)> cursor(raw_cursor, close_cursor);

		MDB_val lKey = { 0, 0 }, rKey = { 0, 0 }, mData = { 0, 0 };
		lKey.mv_data = (void*)lower_key.data();
		lKey.mv_size = lower_key.size();
		rKey.mv_data = (void*)upper_key.data();
		rKey.mv_size = upper_key.size();

		int cnt = 0;
		MDB_cursor_op op = lower_key.empty() ? MDB_FIRST : MDB_SET_RANGE;
		std::vector<std::string> ayKeys, ayVals;
		for (;;)
		{
			status = _range_cursor_get_override == NULL
				? mdb_cursor_get(cursor.get(), &lKey, &mData, op)
				: _range_cursor_get_override(_range_cursor_get_context, cursor.get(), &lKey, &mData, op);
			if (status == MDB_NOTFOUND)
			{
				// End of the key space is a complete scan, not a query failure.
				status = MDB_SUCCESS;
				break;
			}
			if (status != MDB_SUCCESS)
				break;

			if (compare_lmdb_keys(lKey.mv_data, lKey.mv_size, rKey.mv_data, rKey.mv_size) > 0)
				break;

			ayKeys.emplace_back(lKey.mv_size == 0 ? std::string() : std::string((const char*)lKey.mv_data, lKey.mv_size));
			ayVals.emplace_back(mData.mv_size == 0 ? std::string() : std::string((const char*)mData.mv_data, mData.mv_size));
			cnt++;
			op = MDB_NEXT;
		}

		cursor.reset();
		if (status != MDB_SUCCESS)
		{
			_db.update_errno(status);
			return -1;
		}

		_db.update_errno(MDB_SUCCESS);
		cb(ayKeys, ayVals);
		return cnt;
	}

	/*
	 *	读取upper_key之前的数据，从upper_key往前找，找到以后在做一个reverse
	 *	@lower_key	下边界，这个必须要有，因为如果多个合约存一个库的话，不加的话可能会读到别的合约的数据
	 *	@upper_key	上边界
	 *	@count		目标数据条数
	 *	@cb			回调函数
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
			if (compare_lmdb_keys(rKey.mv_data, rKey.mv_size, upper_key.data(), upper_key.size()) > 0)
			{
				_errno = mdb_cursor_get(cursor, &rKey, &mData, MDB_PREV);
				_db.update_errno(_errno);
				continue;
			}

			if (compare_lmdb_keys(rKey.mv_data, rKey.mv_size, lower_key.data(), lower_key.size()) < 0)
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

	/*
	 *	带失败判定的限量向前读取。只在 cursor 完整达到 count、下边界或
	 *	LMDB 末端后调用回调；正常空结果返回 0，任何游标错误返回 -1。
	 */
	int get_lowers_checked(const std::string& lower_key, const std::string& upper_key, int count, LMDBQueryCallback cb)
	{
		if (_txn == NULL)
			return -1;
		if (upper_key.empty() || count <= 0)
		{
			_db.update_errno(MDB_SUCCESS);
			cb(ValueArray(), ValueArray());
			return 0;
		}

		MDB_cursor* raw_cursor = NULL;
		int status = mdb_cursor_open(_txn, _dbi, &raw_cursor);
		_db.update_errno(status);
		if (status != MDB_SUCCESS)
		{
			if (raw_cursor != NULL)
				mdb_cursor_close(raw_cursor);
			return -1;
		}
		auto close_cursor = [](MDB_cursor* cursor) { if (cursor != NULL) mdb_cursor_close(cursor); };
		std::unique_ptr<MDB_cursor, decltype(close_cursor)> cursor(raw_cursor, close_cursor);

		auto cursor_get = [this, &cursor](MDB_val* key, MDB_val* data, MDB_cursor_op op) {
			return _range_cursor_get_override == NULL
				? mdb_cursor_get(cursor.get(), key, data, op)
				: _range_cursor_get_override(_range_cursor_get_context, cursor.get(), key, data, op);
		};

		MDB_val key = { 0, 0 }, value = { 0, 0 };
		key.mv_data = (void*)upper_key.data();
		key.mv_size = upper_key.size();

		int cnt = 0;
		std::vector<std::string> ayKeys, ayVals;
		bool positioned = true;
		status = cursor_get(&key, &value, MDB_SET_RANGE);
		if (status == MDB_NOTFOUND)
		{
			status = cursor_get(&key, &value, MDB_LAST);
			if (status == MDB_NOTFOUND)
			{
				status = MDB_SUCCESS;
				positioned = false;
			}
		}
		if (status != MDB_SUCCESS)
		{
			_db.update_errno(status);
			return -1;
		}

		while (positioned && compare_lmdb_keys(key.mv_data, key.mv_size, upper_key.data(), upper_key.size()) > 0)
		{
			status = cursor_get(&key, &value, MDB_PREV);
			if (status == MDB_NOTFOUND)
			{
				status = MDB_SUCCESS;
				positioned = false;
				break;
			}
			if (status != MDB_SUCCESS)
				break;
		}

		while (positioned && status == MDB_SUCCESS)
		{
			if (compare_lmdb_keys(key.mv_data, key.mv_size, lower_key.data(), lower_key.size()) < 0)
				break;

			ayKeys.emplace_back(key.mv_size == 0 ? std::string() : std::string((const char*)key.mv_data, key.mv_size));
			ayVals.emplace_back(value.mv_size == 0 ? std::string() : std::string((const char*)value.mv_data, value.mv_size));
			cnt++;
			if (cnt >= count)
				break;

			status = cursor_get(&key, &value, MDB_PREV);
			if (status == MDB_NOTFOUND)
			{
				status = MDB_SUCCESS;
				positioned = false;
			}
		}

		cursor.reset();
		if (status != MDB_SUCCESS)
		{
			_db.update_errno(status);
			return -1;
		}

		std::reverse(ayKeys.begin(), ayKeys.end());
		std::reverse(ayVals.begin(), ayVals.end());
		_db.update_errno(MDB_SUCCESS);
		cb(ayKeys, ayVals);
		return cnt;
	}

	/*
	 *	读取lower_key之后的数据，从lower_key往后找
	 *	@lower_key	下边界
	 *	@upper_key	上边界，这个必须要有，因为如果多个合约存一个库的话，不加的话可能会读到别的合约的数据
	 *	@count		目标数据条数
	 *	@cb			回调函数
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
			if (compare_lmdb_keys(bKey.mv_data, bKey.mv_size, upper_key.data(), upper_key.size()) > 0)
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

	inline int get_all(LMDBQueryCallback cb)
	{
		MDB_cursor* raw_cursor = NULL;
		int _errno = mdb_cursor_open(_txn, _dbi, &raw_cursor);
		_db.update_errno(_errno);
		if (_errno != MDB_SUCCESS)
			return 0;
		auto close_cursor = [](MDB_cursor* cursor) { if (cursor != NULL) mdb_cursor_close(cursor); };
		std::unique_ptr<MDB_cursor, decltype(close_cursor)> cursor(raw_cursor, close_cursor);

		MDB_val bKey = { 0, 0 }, mData = { 0, 0 };
		std::vector<std::string> ayKeys, ayVals;
		_errno = mdb_cursor_get(cursor.get(), &bKey, &mData, MDB_FIRST);
		_db.update_errno(_errno);
		while (_errno == MDB_SUCCESS)
		{
			ayKeys.emplace_back(bKey.mv_size == 0 ? std::string() : std::string((const char*)bKey.mv_data, bKey.mv_size));
			ayVals.emplace_back(mData.mv_size == 0 ? std::string() : std::string((const char*)mData.mv_data, mData.mv_size));
			_errno = mdb_cursor_get(cursor.get(), &bKey, &mData, MDB_NEXT);
			_db.update_errno(_errno);
		}
		cursor.reset();
		if (_errno != MDB_NOTFOUND)
			return 0;

		_db.update_errno(MDB_SUCCESS);
		cb(ayKeys, ayVals);
		return (int)ayVals.size();
	}

private:
	friend struct WtLMDBRangeTestAccess;
	friend struct WtLMDBCheckedLowersTestAccess;
	friend struct WtLMDBReaderTestAccess;
	typedef LMDBRangeCursorGetOverride RangeCursorGetOverride;

	WtLMDB&		_db;
	MDB_txn*	_txn;
	MDB_dbi		_dbi;
	bool		_readonly;
	bool		_commited;
	RangeCursorGetOverride _range_cursor_get_override;
	void* _range_cursor_get_context;
};

NS_WTP_END
