/*!
 * \file BoostFile.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief boost库文件操作的辅助对象
 * 
 * \details 本文件封装了基于boost库的文件操作功能，提供了以下主要功能：
 * - 文件的创建、打开、关闭和删除
 * - 文件内容的读取和写入
 * - 文件指针的操作（移动、获取位置）
 * - 文件大小的获取和设置（截断）
 * - 静态工具方法（文件内容的读取与写入、文件删除、获取文件大小）
 *
 * 这个类主要基于boost::interprocess::ipcdetail命名空间下的文件操作函数，
 * 并考虑了Windows和Unix/Linux系统的兼容性。
 */
#pragma once
#include <boost/version.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/filesystem.hpp>
#include <boost/interprocess/detail/os_file_functions.hpp>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

//struct OVERLAPPED;
//extern "C" __declspec(dllimport) int __stdcall ReadFile(void *hnd, void *buffer, unsigned long bytes_to_write,unsigned long *bytes_written, OVERLAPPED* overlapped);
/**
 * @brief boost库文件操作的封装类
 * 
 * @details BoostFile类提供了一组基于boost::interprocess的文件操作方法，
 * 包括创建、打开、关闭、读取、写入和操作文件指针等功能。
 * 该类封装了底层的文件操作函数，设计为跨平台可用，支持Windows和Unix/Linux系统。
 * 类中包含实例方法和静态方法，实例方法用于操作指定的文件，静态方法提供了便捷的文件操作工具。
 */
class BoostFile
{
public:
	/**
	 * @brief 默认构造函数
	 * 
	 * @details 创建一个新的BoostFile实例，并将文件句柄初始化为无效值。
	 * 构造后的对象需要调用创建或打开文件的方法才能进行文件操作。
	 */
	BoostFile()
	{
		_handle=boost::interprocess::ipcdetail::invalid_file(); 
	}
	/**
	 * @brief 析构函数
	 * 
	 * @details 析构对象时自动关闭文件句柄，确保文件正确关闭。
	 * 调用close_file()方法来完成文件关闭操作，防止资源泄漏。
	 */
	~BoostFile()
	{
		close_file();
	}

	/**
	 * @brief 创建新文件
	 * 
	 * @param name 文件名称（包含路径）
	 * @param mode 文件访问模式，默认为读写模式（read_write）
	 * @param temporary 是否为临时文件，默认为false
	 * @return bool 创建成功返回true，失败返回false
	 * 
	 * @details 创建一个新文件并将其打开以进行操作。如果文件已存在，会将其截断为零长度，
	 * 相当于清空文件内容。如果成功创建文件，方法会调用truncate_file(0)将文件大小设置为0。
	 * 如果文件创建失败或截断失败，则返回false。
	 */
	bool create_new_file(const char *name, boost::interprocess::mode_t mode = boost::interprocess::read_write, bool temporary = false)
	{
		_handle=boost::interprocess::ipcdetail::create_or_open_file(name,mode,boost::interprocess::permissions(),temporary);

		if (valid())
			return truncate_file(0);
		return false;
	}

	/**
	 * @brief 创建或打开文件
	 * 
	 * @param name 文件名称（包含路径）
	 * @param mode 文件访问模式，默认为读写模式（read_write）
	 * @param temporary 是否为临时文件，默认为false
	 * @return bool 创建或打开成功返回true，失败返回false
	 * 
	 * @details 创建新文件或打开现有文件。如果文件不存在，则创建新文件；如果文件已存在，则直接打开现有文件。
	 * 与 create_new_file 不同的是，此方法不会截断或清空现有文件的内容。
	 * 返回值通过调用 valid() 方法来确定文件是否成功打开。
	 */
	bool create_or_open_file(const char *name, boost::interprocess::mode_t mode = boost::interprocess::read_write, bool temporary = false)
	{
		_handle=boost::interprocess::ipcdetail::create_or_open_file(name,mode,boost::interprocess::permissions(),temporary);

		return valid();
	}

	/**
	 * @brief 打开现有文件
	 * 
	 * @param name 文件名称（包含路径）
	 * @param mode 文件访问模式，默认为读写模式（read_write）
	 * @param temporary 是否为临时文件，默认为false
	 * @return bool 打开成功返回true，失败返回false
	 * 
	 * @details 打开一个现有文件，如果文件不存在，则返回false。与 create_or_open_file 方法不同，
	 * 该方法只会打开现有文件，而不会创建不存在的文件。
	 * 返回值通过调用 valid() 方法来确定文件是否成功打开。
	 */
	bool open_existing_file(const char *name, boost::interprocess::mode_t mode = boost::interprocess::read_write, bool temporary = false)
	{
		_handle=boost::interprocess::ipcdetail::open_existing_file(name,mode,temporary);
		return valid();
	}

	/**
	 * @brief 检查文件句柄是否无效
	 * 
	 * @return bool 文件句柄无效返回true，有效返回false
	 * 
	 * @details 检查当前文件句柄是否为无效值。这个方法可用于判断文件是否已经打开或创建成功。
	 * 内部实现是将当前文件句柄与Boost库中定义的无效文件句柄进行比较。
	 */
	bool is_invalid_file()
	{  
		return _handle==boost::interprocess::ipcdetail::invalid_file();  
	}

	/**
	 * @brief 检查文件句柄是否有效
	 * 
	 * @return bool 文件句柄有效返回true，无效返回false
	 * 
	 * @details 检查当前文件句柄是否有效。这个方法是 is_invalid_file() 的反向方法，
	 * 用于判断文件是否已经打开或创建成功。当文件打开成功时返回true，否则返回false。
	 * 内部实现是将当前文件句柄与Boost库中定义的无效文件句柄进行比较。
	 */
	bool valid()
	{
		return _handle!=boost::interprocess::ipcdetail::invalid_file();
	}

	/**
	 * @brief 关闭文件
	 * 
	 * @details 关闭当前打开的文件，并将文件句柄设置为无效值。
	 * 该方法首先检查文件句柄是否有效，如果有效，才会进行关闭操作，
	 * 这样可以避免尝试关闭已经关闭或从未打开的文件。
	 * 关闭文件后，将文件句柄重置为无效状态，这样可以防止对已关闭文件的误操作。
	 * 在析构函数中会自动调用此方法以确保文件在对象销毁时被正确关闭。
	 */
	void close_file()
	{
		if(!is_invalid_file())
		{
			boost::interprocess::ipcdetail::close_file(_handle);
			_handle=boost::interprocess::ipcdetail::invalid_file();
		}
	}

	bool truncate_file (std::size_t size)
	{
		return boost::interprocess::ipcdetail::truncate_file(_handle,size);
	}

	bool get_file_size(boost::interprocess::offset_t &size)
	{
		return boost::interprocess::ipcdetail::get_file_size(_handle,size);
	}

	unsigned long long get_file_size()
	{
		boost::interprocess::offset_t size=0;
		if(!get_file_size(size))
			size=0;
		return size;
	}

	static unsigned long long get_file_size(const char *name)
	{
		BoostFile bf;
		if (!bf.open_existing_file(name))
			return 0;

		auto ret = bf.get_file_size();
		bf.close_file();
		return ret;
	}

	bool set_file_pointer(boost::interprocess::offset_t off, boost::interprocess::file_pos_t pos)
	{
		return boost::interprocess::ipcdetail::set_file_pointer(_handle,off,pos);
	}

	bool seek_to_begin(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_begin);
	}

	bool seek_current(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_current);
	}

	bool seek_to_end(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_end);
	}

	bool get_file_pointer(boost::interprocess::offset_t &off)
	{
		return boost::interprocess::ipcdetail::get_file_pointer(_handle,off);
	}

	unsigned long long get_file_pointer()
	{
		boost::interprocess::offset_t off=0;
		if(!get_file_pointer(off))
			return 0;
		return off;
	}

	bool write_file(const void *data, std::size_t numdata)
	{
		return boost::interprocess::ipcdetail::write_file(_handle,data,numdata);
	}

	bool write_file(const std::string& data)
	{
		return boost::interprocess::ipcdetail::write_file(_handle, data.data(), data.size());
	}

	bool read_file(void *data, std::size_t numdata)
	{
		unsigned long readbytes = 0;
#ifdef _WIN32
		int ret = ReadFile(_handle, data, (DWORD)numdata, &readbytes, NULL);
#else
		readbytes = read(_handle, data, (std::size_t)numdata);
#endif
		return numdata == readbytes;
	}

	int read_file_length(void *data, std::size_t numdata)
	{
		unsigned long readbytes = 0;
#ifdef _WIN32
		int ret = ReadFile(_handle, data, (DWORD)numdata, &readbytes, NULL);
#else
		readbytes = read(_handle, data, (std::size_t)numdata);
#endif
		return readbytes;
	}

private:
	boost::interprocess::file_handle_t _handle;

public:
	static bool delete_file(const char *name)
	{
		return boost::interprocess::ipcdetail::delete_file(name);
	}

	static bool read_file_contents(const char *filename,std::string &buffer)
	{
		BoostFile bf;
		if(!bf.open_existing_file(filename,boost::interprocess::read_only))
			return false;
		unsigned int filesize=(unsigned int)bf.get_file_size();
		if(filesize==0)
			return false;
		buffer.resize(filesize);
		return bf.read_file((void *)buffer.c_str(),filesize);
	}

	static bool write_file_contents(const char *filename,const void *pdata,uint32_t datalen)
	{
		BoostFile bf;
		if(!bf.create_new_file(filename))
			return false;
		return bf.write_file(pdata,datalen);
	}

	static bool create_directory(const char *name)
	{
		if(exists(name))
			return true;

		return boost::filesystem::create_directory(boost::filesystem::path(name));
	}

	static bool create_directories(const char *name)
	{
		if(exists(name))
			return true;

		return boost::filesystem::create_directories(boost::filesystem::path(name));
	}

	static bool exists(const char* name)
	{
		return boost::filesystem::exists(boost::filesystem::path(name));
	}
};

typedef boost::shared_ptr<BoostFile> BoostFilePtr;