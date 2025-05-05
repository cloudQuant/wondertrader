/*!
 * \file StdUtils.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief C++标准库一些定义的简单封装,方便调用
 */
#pragma once

#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <stdint.h>
#include <cstdio>    // for FILE, fopen, fseek, ftell, fread, fclose
#include <string>    // for std::string
#include <stdexcept> // for std::runtime_error

#if _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * @defgroup ThreadUtils 线程工具
 * @brief 对C++标准库线程相关类的封装
 * @{*/

/**
 * @brief std::thread的别名
 * @details 对std::thread的重命名，方便统一管理和替换实现
 */
typedef std::thread StdThread;

/**
 * @brief 线程智能指针类型
 * @details 使用std::shared_ptr管理StdThread对象的生命周期
 */
typedef std::shared_ptr<StdThread> StdThreadPtr;

/** @} */

/**
 * @defgroup MutexUtils 互斥锁工具
 * @brief 对C++标准库互斥量和锁相关类的封装
 * @{*/

/**
 * @brief 递归互斥量类型
 * @details 对std::recursive_mutex的重命名，允许同一线程多次获取该锁
 */
typedef std::recursive_mutex	StdRecurMutex;

/**
 * @brief 普通互斥量类型
 * @details 对std::mutex的重命名，基本的互斥同步原语
 */
typedef std::mutex				StdUniqueMutex;

/**
 * @brief 条件变量类型
 * @details 对std::condition_variable_any的重命名，可以配合任意锁类型使用的条件变量
 */
typedef std::condition_variable_any	StdCondVariable;

/**
 * @brief 唯一锁类型
 * @details 基于StdUniqueMutex的std::unique_lock封装，提供RAII风格的锁管理
 */
typedef std::unique_lock<StdUniqueMutex>	StdUniqueLock;

/** @} */

/**
 * @brief 通用锁包装器模板类
 * @tparam T 互斥量类型，需要支持lock()和unlock()方法
 * 
 * @details 提供RAII风格的锁管理，构造时自动加锁，析构时自动解锁
 * 主要用于确保在作用域结束时自动释放锁，防止因异常或提前返回导致的锁未释放
 */
template<typename T>
class StdLocker
{
public:
	/**
	 * @brief 构造函数
	 * @param mtx 要锁定的互斥量引用
	 *
	 * @details 构造时自动调用互斥量的lock()方法加锁
	 */
	StdLocker(T& mtx)
	{
		mtx.lock();
		_mtx = &mtx;
	}

	/**
	 * @brief 析构函数
	 *
	 * @details 在对象销毁时自动调用互斥量的unlock()方法解锁
	 */
	~StdLocker(){
		_mtx->unlock();
	}

private:
	/**
	 * @brief 互斥量指针
	 * @details 存储指向被锁定的互斥量的指针，用于在析构时解锁
	 */
	T* _mtx;
};

/** @} */

/**
 * @defgroup FileUtils 文件工具
 * @brief 提供文件操作相关的辅助功能
 * @{*/

/**
 * @brief 文件操作辅助类
 * @details 提供静态方法用于常见的文件操作，如读取文件内容、写入文件、检查文件是否存在等
 */
class StdFile
{
public:
//	static inline uint64_t read_file_content(const char* filename, std::string& content)
//	{
//		FILE* f = fopen(filename, "rb");
//		fseek(f, 0, SEEK_END);
//		uint32_t length = ftell(f);
//		content.resize(length);   // allocate memory for a buffer of appropriate dimension
//		fseek(f, 0, 0);
//		fread((void*)content.data(), sizeof(char), length, f);
//		fclose(f);
//		return length;
//	}

	/**
	 * @brief 读取文件内容到字符串
	 * @param filename 要读取的文件路径
	 * @param content 输出参数，用于存储读取的文件内容
	 * @return uint64_t 读取的字节数
	 * @throw std::runtime_error 当文件打开失败、获取文件大小失败或读取内容失败时抛出
	 * 
	 * @details 使用C文件IO读取整个文件内容到std::string中，支持二进制文件
	 */
	static inline uint64_t read_file_content(const char* filename, std::string& content)
	{
		FILE* f = fopen(filename, "rb");
		if (!f) {
			// 文件打开失败，抛出异常并显示文件名
			throw std::runtime_error("Failed to open file: " + std::string(filename));
		}

		// 获取文件大小
		fseek(f, 0, SEEK_END);
		long length = ftell(f);
		if (length < 0) {
			fclose(f);
			// 获取文件大小失败，抛出异常并显示文件名
			throw std::runtime_error("Failed to get file size for file: " + std::string(filename));
		}
		fseek(f, 0, SEEK_SET);

		// 分配缓冲区
		content.resize(length);

		// 读取文件内容
		std::size_t readSize = fread((void*)content.data(), sizeof(char), length, f);
		fclose(f);

		// 检查是否读取成功
		if (readSize != static_cast<std::size_t>(length)) {
			// 读取失败，抛出异常并显示文件名
			throw std::runtime_error("Failed to read file content for file: " + std::string(filename));
		}

		return length;
	}
	/**
	 * @brief 将字符串内容写入文件
	 * @param filename 目标文件路径
	 * @param content 要写入的字符串内容
	 * 
	 * @details 使用C文件IO将std::string内容写入文件，支持二进制数据
	 */
	static inline void write_file_content(const char* filename, const std::string& content)
	{
		FILE* f = fopen(filename, "wb");
		fwrite((void*)content.data(), sizeof(char), content.size(), f);
		fclose(f);
	}

	/**
	 * @brief 将内存数据写入文件
	 * @param filename 目标文件路径
	 * @param data 要写入的内存数据指针
	 * @param length 要写入的数据长度（字节数）
	 * 
	 * @details 使用C文件IO将内存中的数据块写入文件，适用于任意二进制数据
	 */
	static inline void write_file_content(const char* filename, const void* data, std::size_t length)
	{
		FILE* f = fopen(filename, "wb");
		fwrite(data, sizeof(char), length, f);
		fclose(f);
	}

	/**
	 * @brief 检查文件是否存在
	 * @param filename 要检查的文件路径
	 * @return bool 文件存在返回true，否则返回false
	 * 
	 * @details 使用平台特定的API检查文件是否存在，Windows下使用_access，非Windows系统使用access
	 */
	static inline bool exists(const char* filename)
	{
#if _WIN32
		int ret = _access(filename, 0);
#else
		int ret = access(filename, 0);
#endif
		return ret == 0;
	}
};

/** @} */
