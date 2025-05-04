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

	/**
	 * @brief 截断文件到指定大小
	 * 
	 * @param size 文件要截断到的大小（字节数）
	 * @return bool 截断成功返回true，失败返回false
	 * 
	 * @details 将当前打开的文件截断到指定的大小。如果指定大小小于当前文件大小，
	 * 则文件将被裁剪；如果指定大小大于当前文件大小，则文件将被扩展，新增的部分用零填充。
	 * 当size为0时，文件将被清空。这个方法在create_new_file中被调用，以确保新创建的文件是空的。
	 */
	bool truncate_file (std::size_t size)
	{
		return boost::interprocess::ipcdetail::truncate_file(_handle,size);
	}

	/**
	 * @brief 获取文件大小
	 * 
	 * @param[out] size 输出参数，用于存储文件大小
	 * @return bool 获取成功返回true，失败返回false
	 * 
	 * @details 获取当前打开文件的大小，并将结果存储在size参数中。
	 * 这个方法使用boost::interprocess::ipcdetail命名空间中的get_file_size函数来获取文件大小。
	 * 如果文件句柄无效或获取大小时发生错误，则返回false。
	 */
	bool get_file_size(boost::interprocess::offset_t &size)
	{
		return boost::interprocess::ipcdetail::get_file_size(_handle,size);
	}

	/**
	 * @brief 获取文件大小
	 * 
	 * @return unsigned long long 文件大小，单位为字节，如果获取失败则返回0
	 * 
	 * @details 获取当前打开文件的大小并返回。这是另一个get_file_size方法的简化版本，
	 * 其内部调用了带引用参数的get_file_size方法来获取文件大小。
	 * 如果获取文件大小失败（例如文件句柄无效），则返回0。
	 * 这个方法更方便使用，当不需要知道是否成功获取了文件大小时可以使用此方法。
	 */
	unsigned long long get_file_size()
	{
		boost::interprocess::offset_t size=0;
		if(!get_file_size(size))
			size=0;
		return size;
	}

	/**
	 * @brief 获取指定文件的大小（静态方法）
	 * 
	 * @param name 文件名称（包含路径）
	 * @return unsigned long long 文件大小，单位为字节，如果文件不存在或无法打开则返回0
	 * 
	 * @details 该静态方法用于获取指定文件的大小，无需首先创建 BoostFile 对象。
	 * 内部实现是创建一个临时的 BoostFile 对象，打开指定文件，获取其大小，然后关闭文件并返回结果。
	 * 如果文件不存在或无法打开，则返回0。
	 * 这个方法提供了一个便捷的方式来检查文件大小，而无需手动管理文件的打开和关闭。
	 */
	static unsigned long long get_file_size(const char *name)
	{
		BoostFile bf;
		if (!bf.open_existing_file(name))
			return 0;

		auto ret = bf.get_file_size();
		bf.close_file();
		return ret;
	}

	/**
	 * @brief 设置文件指针位置
	 * 
	 * @param off 偏移量，相对于pos参数指定的位置
	 * @param pos 文件位置基准，可以是文件开头（file_begin）、当前位置（file_current）或文件结尾（file_end）
	 * @return bool 设置成功返回true，失败返回false
	 * 
	 * @details 设置当前文件的读写指针位置。pos参数指定了基准位置（文件开头、当前位置或文件结尾），
	 * off参数指定了相对于该基准位置的偏移量。此方法直接调用boost::interprocess::ipcdetail命名空间中的
	 * set_file_pointer函数来实现文件指针的移动。
	 * 这个方法是低级别的文件指针操作函数，通常使用seek_to_begin、seek_current或seek_to_end这样的高级别函数更方便。
	 */
	bool set_file_pointer(boost::interprocess::offset_t off, boost::interprocess::file_pos_t pos)
	{
		return boost::interprocess::ipcdetail::set_file_pointer(_handle,off,pos);
	}

	/**
	 * @brief 将文件指针移动到文件开头
	 * 
	 * @param offsize 相对于文件开头的偏移量，默认为0（即文件的第一个字节）
	 * @return bool 移动成功返回true，失败返回false
	 * 
	 * @details 将当前文件的读写指针移动到文件开头加上指定的偏移量。
	 * 这是set_file_pointer方法的一个封装，直接设置了位置参数为file_begin（文件开头）。
	 * 当offsize为0时，指针将移动到文件的第一个字节；当offsize为正数时，指针将移动到文件开头往后偏移指定字节数的位置。
	 */
	bool seek_to_begin(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_begin);
	}

	/**
	 * @brief 将文件指针相对于当前位置移动
	 * 
	 * @param offsize 相对于当前位置的偏移量，可正可负，默认为0（保持当前位置不变）
	 * @return bool 移动成功返回true，失败返回false
	 * 
	 * @details 将当前文件的读写指针相对于当前位置移动指定的偏移量。
	 * 这是set_file_pointer方法的一个封装，直接设置了位置参数为file_current（当前位置）。
	 * 当offsize为0时，指针位置保持不变；当offsize为正数时，指针向前移动（向文件结尾方向）；
	 * 当offsize为负数时，指针向后移动（向文件开头方向）。
	 */
	bool seek_current(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_current);
	}

	/**
	 * @brief 将文件指针移动到文件结尾
	 * 
	 * @param offsize 相对于文件结尾的偏移量，默认为0（正好在文件结尾），通常为负数以定位到文件结尾之前的位置
	 * @return bool 移动成功返回true，失败返回false
	 * 
	 * @details 将当前文件的读写指针移动到文件结尾加上指定的偏移量。
	 * 这是set_file_pointer方法的一个封装，直接设置了位置参数为file_end（文件结尾）。
	 * 当offsize为0时，指针将移动到文件的最后一个字节之后，这在追加写入文件时非常有用。
	 * 当offsize为负数时，指针将移动到文件结尾往前偏移指定字节数的位置。
	 */
	bool seek_to_end(int offsize=0)
	{
		return set_file_pointer(offsize,boost::interprocess::file_end);
	}

	/**
	 * @brief 获取当前文件指针位置
	 * 
	 * @param[out] off 输出参数，用于存储当前文件指针的位置
	 * @return bool 获取成功返回true，失败返回false
	 * 
	 * @details 获取当前文件的读写指针位置，并将结果存储在off参数中。
	 * 这个方法使用boost::interprocess::ipcdetail命名空间中的get_file_pointer函数来获取指针位置。
	 * 如果文件句柄无效或获取读写指针位置时发生错误，则返回false。
	 */
	bool get_file_pointer(boost::interprocess::offset_t &off)
	{
		return boost::interprocess::ipcdetail::get_file_pointer(_handle,off);
	}

	/**
	 * @brief 获取当前文件指针位置
	 * 
	 * @return unsigned long long 当前文件指针的位置，如果获取失败则返回0
	 * 
	 * @details 获取当前文件的读写指针位置并返回。这是另一个get_file_pointer方法的简化版本，
	 * 其内部调用了带引用参数的get_file_pointer方法来获取指针位置。
	 * 如果获取文件指针位置失败（例如文件句柄无效），则返回0。
	 * 这个方法更方便使用，当不需要知道是否成功获取了文件指针位置时可以使用此方法。
	 */
	unsigned long long get_file_pointer()
	{
		boost::interprocess::offset_t off=0;
		if(!get_file_pointer(off))
			return 0;
		return off;
	}

	/**
	 * @brief 写入数据到文件
	 * 
	 * @param data 要写入的数据的指针
	 * @param numdata 要写入的数据字节数
	 * @return bool 写入成功返回true，失败返回false
	 * 
	 * @details 将指定的数据写入到当前文件中的当前文件指针位置。
	 * 写入成功后，文件指针会自动前移写入的字节数。
	 * 这个方法使用boost::interprocess::ipcdetail命名空间中的write_file函数来实现文件写入操作。
	 * 如果文件句柄无效或写入过程中发生错误，则返回false。
	 */
	bool write_file(const void *data, std::size_t numdata)
	{
		return boost::interprocess::ipcdetail::write_file(_handle,data,numdata);
	}

	/**
	 * @brief 写入字符串数据到文件
	 * 
	 * @param data 要写入的字符串数据
	 * @return bool 写入成功返回true，失败返回false
	 * 
	 * @details 将指定的字符串数据写入到当前文件中的当前文件指针位置。
	 * 这是一个重载write_file方法，提供了一个更便捷的方式来写入std::string类型的数据。
	 * 内部实现通过调用boost::interprocess::ipcdetail::write_file函数，传入字符串的内部数据指针和大小来实现。
	 * 如同基本版本的write_file方法，写入成功后文件指针会自动前移写入的字节数。
	 */
	bool write_file(const std::string& data)
	{
		return boost::interprocess::ipcdetail::write_file(_handle, data.data(), data.size());
	}

	/**
	 * @brief 从文件读取数据
	 * 
	 * @param data 读取数据的目标缓冲区指针
	 * @param numdata 要读取的数据字节数
	 * @return bool 如果成功读取了指定数量的字节返回true，否则返回false
	 * 
	 * @details 从当前文件的当前指针位置读取指定数量的数据到目标缓冲区。
	 * 该方法在Windows和Linux/Unix平台上有不同的实现：Windows使用ReadFile API，
	 * 而Linux/Unix使用read系统调用。读取成功后，文件指针会自动前移读取的字节数。
	 * 这个方法只在完全读取了请求的字节数时才会返回true，如果实际读取的字节数少于请求的
	 * 字节数（例如达到文件结尾或发生错误），则返回false。
	 */
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

	/**
	 * @brief 从文件读取数据并返回实际读取到的字节数
	 * 
	 * @param data 读取数据的目标缓冲区指针
	 * @param numdata 要读取的最大数据字节数
	 * @return int 实际读取到的字节数，可能小于请求的字节数
	 * 
	 * @details 从当前文件的当前指针位置读取数据到目标缓冲区，并返回实际读取到的字节数。
	 * 这个方法与 read_file 的主要区别是返回类型和行为：read_file 返回布尔值表示是否读取了全部请求的字节，
	 * 而 read_file_length 方法返回实际读取到的字节数。当需要知道实际读取到多少数据时，
	 * 例如读取可能不完整的数据块或者需要处理文件结尾情况时，这个方法很有用。
	 * 该方法同样在Windows和Linux/Unix平台上有不同的实现。
	 */
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
	/**
	 * @brief 文件句柄
	 * 
	 * @details 存储当前打开文件的文件句柄，这是一个由Boost.Interprocess库提供的平台无关的文件句柄类型。
	 * 在Windows上，它是一个HANDLE；在POSIX系统上，它是一个文件描述符（int）。
	 * 该句柄由各种文件操作方法使用，如读、写、定位等。
	 * 当文件关闭时，该句柄将被设置为无效值。
	 */
	boost::interprocess::file_handle_t _handle;

public:
	/**
	 * @brief 删除指定文件（静态方法）
	 * 
	 * @param name 要删除的文件名称（包含路径）
	 * @return bool 删除成功返回true，失败返回false
	 * 
	 * @details 删除指定路径的文件。这是一个静态方法，可以直接通过类名调用，而不需要创建 BoostFile 对象。
	 * 该方法内部调用 boost::interprocess::ipcdetail::delete_file 函数来实现文件删除操作。
	 * 如果文件不存在，或者没有足够的权限删除文件，则返回false。
	 * 该方法提供了一个便捷的方式来删除文件，而无需手动管理 BoostFile 对象的创建和销毁。
	 */
	static bool delete_file(const char *name)
	{
		return boost::interprocess::ipcdetail::delete_file(name);
	}

	/**
	 * @brief 读取文件全部内容到字符串（静态方法）
	 * 
	 * @param filename 要读取的文件名称（包含路径）
	 * @param[out] buffer 输出参数，用于存储读取到的文件内容
	 * @return bool 读取成功返回true，失败返回false
	 * 
	 * @details 该静态方法用于将指定文件的全部内容读取到一个字符串中。
	 * 内部实现是创建一个临时的 BoostFile 对象，以只读模式打开指定文件，获取文件大小，
	 * 调整字符串缓冲区大小，然后读取文件内容到该缓冲区。
	 * 如果文件不存在、无法打开或文件大小为0，则返回false。
	 * 这个方法提供了一个便捷的方式来读取文件内容，而无需手动管理文件的打开、读取和关闭。
	 */
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

	/**
	 * @brief 写入数据到文件（静态方法）
	 * 
	 * @param filename 目标文件名称（包含路径）
	 * @param pdata 要写入的数据的指针
	 * @param datalen 要写入的数据字节数
	 * @return bool 写入成功返回true，失败返回false
	 * 
	 * @details 该静态方法用于将指定数据写入到文件中。
	 * 内部实现是创建一个临时的 BoostFile 对象，创建一个新文件（如果文件已存在，则会被截断为空），
	 * 然后将指定的数据写入到该文件中。
	 * 如果文件无法创建（例如由于权限问题或文件已被锁定）或写入操作失败，则返回false。
	 * 这个方法提供了一个便捷的方式来写入数据到文件，而无需手动管理文件的创建、写入和关闭。
	 */
	static bool write_file_contents(const char *filename,const void *pdata,uint32_t datalen)
	{
		BoostFile bf;
		if(!bf.create_new_file(filename))
			return false;
		return bf.write_file(pdata,datalen);
	}

	/**
	 * @brief 创建目录（静态方法）
	 * 
	 * @param name 要创建的目录名称（包含路径）
	 * @return bool 创建成功或目录已存在返回true，失败返回false
	 * 
	 * @details 该静态方法用于创建指定的目录。
	 * 首先使用exists方法检查目录是否已存在，如果已存在则直接返回true。
	 * 如果目录不存在，则使用boost::filesystem::create_directory函数创建目录。
	 * 该方法只能创建一级目录，如果需要创建多级目录，应使用create_directories方法。
	 * 如果目录无法创建（例如由于权限问题或路径错误），则返回false。
	 */
	static bool create_directory(const char *name)
	{
		if(exists(name))
			return true;

		return boost::filesystem::create_directory(boost::filesystem::path(name));
	}

	/**
	 * @brief 创建多级目录（静态方法）
	 * 
	 * @param name 要创建的目录路径（可以包含多级目录）
	 * @return bool 创建成功或目录已存在返回true，失败返回false
	 * 
	 * @details 该静态方法用于创建指定的目录，包括中间的所有父级目录。
	 * 首先使用exists方法检查目录是否已存在，如果已存在则直接返回true。
	 * 如果目录不存在，则使用boost::filesystem::create_directories函数创建目录。
	 * 与 create_directory 方法的区别是，该方法可以创建多级目录结构，如果路径中的父目录不存在，也会自动创建。
	 * 例如，可以创建如 "a/b/c" 这样的目录结构，即使 a 和 b 目录不存在。
	 * 如果目录无法创建（例如由于权限问题或路径错误），则返回false。
	 */
	static bool create_directories(const char *name)
	{
		if(exists(name))
			return true;

		return boost::filesystem::create_directories(boost::filesystem::path(name));
	}

	/**
	 * @brief 检查文件或目录是否存在（静态方法）
	 * 
	 * @param name 要检查的文件或目录路径
	 * @return bool 如果文件或目录存在返回true，不存在返回false
	 * 
	 * @details 该静态方法用于检查指定路径的文件或目录是否存在。
	 * 内部调用boost::filesystem::exists函数来实现这一功能。
	 * 该方法可以检查普通文件、目录、符号链接等各种文件系统对象的存在性。
	 * 该方法在create_directory和create_directories方法中被调用，用于检查目标目录是否已存在。
	 */
	static bool exists(const char* name)
	{
		return boost::filesystem::exists(boost::filesystem::path(name));
	}
};

/**
 * @brief BoostFile类的智能指针类型
 * 
 * @details 使用boost::shared_ptr封装的BoostFile智能指针类型，提供了自动内存管理功能。
 * 当BoostFilePtr对象不再被引用时，内部的BoostFile对象将会自动析构，从而关闭文件句柄。
 * 这种智能指针类型的使用可以避免资源泄漏，特别是在异常处理或复杂流程控制中非常有用。
 */
typedef boost::shared_ptr<BoostFile> BoostFilePtr;