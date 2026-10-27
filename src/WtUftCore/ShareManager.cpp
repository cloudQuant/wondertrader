/**
 * @file ShareManager.cpp
 * @brief 共享数据管理器实现文件
 * 
 * 实现了ShareManager类，用于管理多进程间的共享数据。
 * 支持各种数据类型的读写和监控，并提供参数变更通知机制。
 */
#include "ShareManager.h"
#include "WtUftEngine.h"
#include "../WTSTools/WTSLogger.h"

#include "../Share/StdUtils.hpp"

#include "../Share/TimeUtils.hpp"


/**
 * @brief 初始化共享数据管理器
 * 
 * 加载指定的共享库模块，并获取各种操作共享数据的函数指针。
 * 如果模块不存在或加载失败，则返回false。
 * 
 * @param module 共享库模块的完整路径
 * @return bool 初始化是否成功
 */
bool ShareManager::initialize(const char* module)
{
	if (_inited)
		return true;

	if(!StdFile::exists(module))
	{
		WTSLogger::warn("WtShareHelper {} not exist", module);
		return false;
	}

	_module = module;
	_inst = DLLHelper::load_library(_module.c_str());
	_inited = (_inst != NULL);

	_init_master = (func_init_master)DLLHelper::get_symbol(_inst, "init_master");
	_get_section_updatetime = (func_get_section_updatetime)DLLHelper::get_symbol(_inst, "get_section_updatetime");
	_commit_section = (func_commit_section)DLLHelper::get_symbol(_inst, "commit_section");

	_set_double = (func_set_double)DLLHelper::get_symbol(_inst, "set_double");
	_set_int32 = (func_set_int32)DLLHelper::get_symbol(_inst, "set_int32");
	_set_int64 = (func_set_int64)DLLHelper::get_symbol(_inst, "set_int64");
	_set_uint32 = (func_set_uint32)DLLHelper::get_symbol(_inst, "set_uint32");
	_set_uint64 = (func_set_uint64)DLLHelper::get_symbol(_inst, "set_uint64");
	_set_string = (func_set_string)DLLHelper::get_symbol(_inst, "set_string");

	_get_double = (func_get_double)DLLHelper::get_symbol(_inst, "get_double");
	_get_int32 = (func_get_int32)DLLHelper::get_symbol(_inst, "get_int32");
	_get_int64 = (func_get_int64)DLLHelper::get_symbol(_inst, "get_int64");
	_get_uint32 = (func_get_uint32)DLLHelper::get_symbol(_inst, "get_uint32");
	_get_uint64 = (func_get_uint64)DLLHelper::get_symbol(_inst, "get_uint64");
	_get_string = (func_get_string)DLLHelper::get_symbol(_inst, "get_string");

	_allocate_double = (func_allocate_double)DLLHelper::get_symbol(_inst, "allocate_double");
	_allocate_int32 = (func_allocate_int32)DLLHelper::get_symbol(_inst, "allocate_int32");
	_allocate_int64 = (func_allocate_int64)DLLHelper::get_symbol(_inst, "allocate_int64");
	_allocate_uint32 = (func_allocate_uint32)DLLHelper::get_symbol(_inst, "allocate_uint32");
	_allocate_uint64 = (func_allocate_uint64)DLLHelper::get_symbol(_inst, "allocate_uint64");
	_allocate_string = (func_allocate_string)DLLHelper::get_symbol(_inst, "allocate_string");

	return _inited;
}

/**
 * @brief 开始监控共享数据变化
 * 
 * 启动一个后台线程，定期检查共享域中指定图片区域的变化。
 * 当检测到图片区域更新时，触发引擎的通知回调。
 * 
 * @param microsecs 监控线程的睡眠间隔（微秒），如果为0则持续不间断检查
 * @return bool 启动监控是否成功
 */
bool ShareManager::start_watching(uint32_t microsecs)
{
	if (!_inited)
		return false;

	if (_inited && !_stopped && _worker == nullptr)
	{
		_worker.reset(new StdThread([this, microsecs]() {
			while (!_stopped)
			{
				for(auto& v : _secnames)
				{
					if(_stopped)
						break;

					const char* section = v.first.c_str();
					uint64_t& udtTime = (uint64_t&)v.second;

					uint64_t lastUdtTime = _get_section_updatetime(_exchg.c_str(), section);
					if(lastUdtTime > v.second)
					{
						//触发通知
						_engine->notify_params_update(section);
						udtTime = lastUdtTime;
					}
				}

				//如果等待时间为0，则进入无限循环的检查中
				if(microsecs > 0 && !_stopped)
					std::this_thread::sleep_for(std::chrono::microseconds(microsecs));
			}
		}));

		WTSLogger::info("Share domain is on watch");
	}

	return true;
}

/**
 * @brief 初始化共享数据域
 * 
 * 使用指定的标识符初始化共享域和同步域。
 * 共享域使用指定的ID，而同步域固定使用"sync"作为标识符。
 * 
 * @param id 共享域的标识符
 * @return bool 初始化是否成功
 */
bool ShareManager::init_domain(const char* id)
{
	if (!_inited)
		return false;

	bool ret = _init_master(id, ".share");
	_exchg = id;
	WTSLogger::info("Share domain [{}] initialing {}", id, ret ? "succeed" : "failed");

	//初始化同步区
	ret = _init_master("sync", ".sync");
	WTSLogger::info("Sync domain [sync] initialing {}", ret ? "succeed" : "failed");

	return ret;
}

/**
 * @brief 提交参数监控器
 * 
 * 将指定图片区域的更新提交到共享域，并更新当前的时间戳记录。
 * 这使得其他进程可以获知参数变更。
 * 
 * @param section 要提交的图片区域名称
 * @return bool 提交是否成功
 */
bool ShareManager::commit_param_watcher(const char* section)
{
	if (!_inited)
		return false;

	bool ret = _commit_section(_exchg.c_str(), section);
	_secnames[section] = TimeUtils::getLocalTimeNow();
	return ret;
}

/**
 * @brief 设置浮点型共享数据值
 * 
 * 在指定的图片区域内设置一个浮点型的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的浮点值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, double val)
{
	if (!_inited)
		return false;

	return _set_double(_exchg.c_str(), section, key, val);
}

/**
 * @brief 设置无符64位整型共享数据值
 * 
 * 在指定的图片区域内设置一个无符64位整型的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的无符64位整型值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, uint64_t val)
{
	if (!_inited)
		return false;

	return _set_uint64(_exchg.c_str(), section, key, val);
}

/**
 * @brief 设置无符32位整型共享数据值
 * 
 * 在指定的图片区域内设置一个无符32位整型的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的无符32位整型值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, uint32_t val)
{
	if (!_inited)
		return false;

	return _set_uint32(_exchg.c_str(), section, key, val);
}

/**
 * @brief 设置64位整型共享数据值
 * 
 * 在指定的图片区域内设置一个64位整型的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的64位整型值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, int64_t val)
{
	if (!_inited)
		return false;

	return _set_int64(_exchg.c_str(), section, key, val);
}

/**
 * @brief 设置32位整型共享数据值
 * 
 * 在指定的图片区域内设置一个32位整型的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的32位整型值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, int32_t val)
{
	if (!_inited)
		return false;

	return _set_int32(_exchg.c_str(), section, key, val);
}

/**
 * @brief 设置字符串型共享数据值
 * 
 * 在指定的图片区域内设置一个字符串的键值对。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param val 要设置的字符串值
 * @return bool 设置是否成功
 */
bool ShareManager::set_value(const char* section, const char* key, const char* val)
{
	if (!_inited)
		return false;

	return _set_string(_exchg.c_str(), section, key, val);
}

/**
 * @brief 获取字符串型共享数据值
 * 
 * 从指定的图片区域内获取一个字符串类型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return const char* 获取到的字符串值
 */
const char* ShareManager::get_value(const char* section, const char* key, const char* defVal /* = "" */)
{
	if (!_inited)
		return defVal;

	return _get_string(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 获取32位整型共享数据值
 * 
 * 从指定的图片区域内获取一个32位整型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return int32_t 获取到的32位整型值
 */
int32_t ShareManager::get_value(const char* section, const char* key, int32_t defVal /* = 0 */)
{
	if (!_inited)
		return defVal;

	return _get_int32(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 获取64位整型共享数据值
 * 
 * 从指定的图片区域内获取一个64位整型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return int64_t 获取到的64位整型值
 */
int64_t ShareManager::get_value(const char* section, const char* key, int64_t defVal /* = 0 */)
{
	if (!_inited)
		return defVal;

	return _get_int64(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 获取无符32位整型共享数据值
 * 
 * 从指定的图片区域内获取一个无符32位整型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return uint32_t 获取到的无符32位整型值
 */
uint32_t ShareManager::get_value(const char* section, const char* key, uint32_t defVal /* = 0 */)
{
	if (!_inited)
		return defVal;

	return _get_uint32(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 获取无符64位整型共享数据值
 * 
 * 从指定的图片区域内获取一个无符64位整型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return uint64_t 获取到的无符64位整型值
 */
uint64_t ShareManager::get_value(const char* section, const char* key, uint64_t defVal /* = 0 */)
{
	if (!_inited)
		return defVal;

	return _get_uint64(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 获取浮点型共享数据值
 * 
 * 从指定的图片区域内获取一个浮点型的键值对。
 * 如果该键不存在或共享管理器未初始化，则返回默认值。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param defVal 默认值，当键不存在或管理器未初始化时返回
 * @return double 获取到的浮点型值
 */
double ShareManager::get_value(const char* section, const char* key, double defVal /* = 0 */)
{
	if (!_inited)
		return defVal;

	return _get_double(_exchg.c_str(), section, key, defVal);
}

/**
 * @brief 分配字符串型共享数据值
 * 
 * 在指定的图片区域内分配一个字符串型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为空字符串
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return const char* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
const char* ShareManager::allocate_value(const char* section, const char* key, const char* initVal/* = ""*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_string(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}

/**
 * @brief 分配32位整型共享数据值
 * 
 * 在指定的图片区域内分配一个32位整型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为0
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return int32_t* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
int32_t* ShareManager::allocate_value(const char* section, const char* key, int32_t initVal/* = 0*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_int32(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}

/**
 * @brief 分配64位整型共享数据值
 * 
 * 在指定的图片区域内分配一个64位整型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为0
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return int64_t* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
int64_t* ShareManager::allocate_value(const char* section, const char* key, int64_t initVal/* = 0*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_int64(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}

/**
 * @brief 分配无符32位整型共享数据值
 * 
 * 在指定的图片区域内分配一个无符32位整型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为0
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return uint32_t* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
uint32_t* ShareManager::allocate_value(const char* section, const char* key, uint32_t initVal/* = 0*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_uint32(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}

/**
 * @brief 分配无符64位整型共享数据值
 * 
 * 在指定的图片区域内分配一个无符64位整型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为0
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return uint64_t* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
uint64_t* ShareManager::allocate_value(const char* section, const char* key, uint64_t initVal/* = 0*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_uint64(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}

/**
 * @brief 分配浮点型共享数据值
 * 
 * 在指定的图片区域内分配一个浮点型的键值对，并返回指向分配内存的指针。
 * 分配的内存可以在同步区或交换区中，由isExchg参数控制。
 * 
 * @param section 图片区域名称
 * @param key 键名
 * @param initVal 初始化值，默认为0
 * @param bForceWrite 是否强制写入，默认为false
 * @param isExchg 是否在交换区分配，默认为false（则在同步区分配）
 * @return double* 指向分配内存的指针，如果管理器未初始化则返回nullptr
 */
double* ShareManager::allocate_value(const char* section, const char* key, double initVal/* = 0*/, bool bForceWrite/* = false*/, bool isExchg/* = false*/)
{
	if (!_inited)
		return nullptr;

	return _allocate_double(isExchg ? _exchg.c_str() : _sync.c_str(), section, key, initVal, bForceWrite);
}