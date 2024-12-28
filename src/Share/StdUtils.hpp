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

//////////////////////////////////////////////////////////////////////////
//std线程类
typedef std::thread StdThread;
typedef std::shared_ptr<StdThread> StdThreadPtr;

//////////////////////////////////////////////////////////////////////////
//std互斥量和锁
typedef std::recursive_mutex	StdRecurMutex;
typedef std::mutex				StdUniqueMutex;
typedef std::condition_variable_any	StdCondVariable;

typedef std::unique_lock<StdUniqueMutex>	StdUniqueLock;

template<typename T>
class StdLocker
{
public:
	StdLocker(T& mtx)
	{
		mtx.lock();
		_mtx = &mtx;
	}

	~StdLocker(){
		_mtx->unlock();
	}

private:
	T* _mtx;
};

//////////////////////////////////////////////////////////////////////////
//文件辅助类
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
	static inline void write_file_content(const char* filename, const std::string& content)
	{
		FILE* f = fopen(filename, "wb");
		fwrite((void*)content.data(), sizeof(char), content.size(), f);
		fclose(f);
	}

	static inline void write_file_content(const char* filename, const void* data, std::size_t length)
	{
		FILE* f = fopen(filename, "wb");
		fwrite(data, sizeof(char), length, f);
		fclose(f);
	}

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
