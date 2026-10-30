/*!
 * \file BoostFile.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief boost库文件操作的辅助对象
 */
#pragma once
#include <boost/shared_ptr.hpp>
#include <boost/interprocess/detail/os_file_functions.hpp>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

#if _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

namespace bip = boost::interprocess;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

//struct OVERLAPPED;
//extern "C" __declspec(dllimport) int __stdcall ReadFile(void *hnd, void *buffer, unsigned long bytes_to_write,unsigned long *bytes_written, OVERLAPPED* overlapped);
namespace boost_file_detail
{
// The callback receives the destination offset and a request no larger than
// max_chunk. It may return a short read; zero, a negative value, or a value
// larger than the request is an error.
template <typename ReadChunk>
bool read_exact_chunks(std::size_t total, std::size_t max_chunk, ReadChunk&& read_chunk)
{
	if (total == 0)
		return true;
	if (max_chunk == 0)
		return false;

	std::size_t offset = 0;
	while (offset < total)
	{
		const std::size_t chunk_end = offset + (std::min)(max_chunk, total - offset);
		while (offset < chunk_end)
		{
			const std::size_t requested = chunk_end - offset;
			const auto read_count = read_chunk(offset, requested);
			if (read_count <= 0 || static_cast<std::size_t>(read_count) > requested)
				return false;
			offset += static_cast<std::size_t>(read_count);
		}
	}
	return true;
}

inline std::size_t max_native_read_size()
{
	std::size_t max_size = static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)());
#ifdef _WIN32
	max_size = (std::min)(max_size, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()));
#else
	max_size = (std::min)(max_size, static_cast<std::size_t>((std::numeric_limits<ssize_t>::max)()));
#endif
	return max_size;
}
} // namespace boost_file_detail

class BoostFile
{
public:
	BoostFile()
	{
		_handle=bip::ipcdetail::invalid_file(); 
	}
	~BoostFile()
	{
		close_file();
	}

	bool create_new_file(const char *name, bip::mode_t mode = bip::read_write, bool temporary = false)
	{
		_handle=bip::ipcdetail::create_or_open_file(name,mode,bip::permissions(),temporary);

		if (valid())
			return truncate_file(0);
		return false;
	}

	bool create_or_open_file(const char *name, bip::mode_t mode = bip::read_write, bool temporary = false)
	{
		_handle=bip::ipcdetail::create_or_open_file(name,mode,bip::permissions(),temporary);

		return valid();
	}

	bool open_existing_file(const char *name, bip::mode_t mode = bip::read_write, bool temporary = false)
	{
		_handle=bip::ipcdetail::open_existing_file(name,mode,temporary);
		return valid();
	}

	bool is_invalid_file()
	{  
		return _handle==bip::ipcdetail::invalid_file();  
	}

	bool valid()
	{
		return _handle!=bip::ipcdetail::invalid_file();
	}

	void close_file()
	{
		if(!is_invalid_file())
		{
			bip::ipcdetail::close_file(_handle);
			_handle=bip::ipcdetail::invalid_file();
		}
	}

	bool truncate_file (std::size_t size)
	{
		return bip::ipcdetail::truncate_file(_handle,size);
	}

	bool get_file_size(bip::offset_t &size)
	{
		return bip::ipcdetail::get_file_size(_handle,size);
	}

	unsigned long long get_file_size()
	{
		bip::offset_t size=0;
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

	bool set_file_pointer(bip::offset_t off, bip::file_pos_t pos)
	{
		return bip::ipcdetail::set_file_pointer(_handle,off,pos);
	}

	bool seek_to_begin(int offsize=0)
	{
		return set_file_pointer(offsize,bip::file_begin);
	}

	bool seek_current(int offsize=0)
	{
		return set_file_pointer(offsize,bip::file_current);
	}

	bool seek_to_end(int offsize=0)
	{
		return set_file_pointer(offsize,bip::file_end);
	}

	bool get_file_pointer(bip::offset_t &off)
	{
		return bip::ipcdetail::get_file_pointer(_handle,off);
	}

	unsigned long long get_file_pointer()
	{
		bip::offset_t off=0;
		if(!get_file_pointer(off))
			return 0;
		return off;
	}

	bool write_file(const void *data, std::size_t numdata)
	{
		return bip::ipcdetail::write_file(_handle,data,numdata);
	}

	bool write_file(const std::string& data)
	{
		return bip::ipcdetail::write_file(_handle, data.data(), data.size());
	}

	bool read_file(void *data, std::size_t numdata)
	{
		if (!valid())
			return false;
		if (numdata == 0)
			return true;
		if (data == nullptr)
			return false;

		return boost_file_detail::read_exact_chunks(numdata,
			boost_file_detail::max_native_read_size(),
			[this, data](std::size_t offset, std::size_t requested) -> std::ptrdiff_t
			{
#ifdef _WIN32
				DWORD readbytes = 0;
				if (!ReadFile(_handle, static_cast<char*>(data) + offset,
					static_cast<DWORD>(requested), &readbytes, NULL))
					return -1;
				return static_cast<std::ptrdiff_t>(readbytes);
#else
				const auto readbytes = read(_handle, static_cast<char*>(data) + offset, requested);
				return static_cast<std::ptrdiff_t>(readbytes);
#endif
			});
	}

	int read_file_length(void *data, std::size_t numdata)
	{
		unsigned long readbytes = 0;
#ifdef _WIN32
		if (!ReadFile(_handle, data, (DWORD)numdata, &readbytes, NULL))
			return -1;
#else
		const auto result = read(_handle, data, (std::size_t)numdata);
		if (result < 0)
			return -1;
		readbytes = static_cast<unsigned long>(result);
#endif
		return static_cast<int>(readbytes);
	}

private:
	bip::file_handle_t _handle;

public:
	static bool delete_file(const char *name)
	{
		return bip::ipcdetail::delete_file(name);
	}

	static bool read_file_contents(const char *filename,std::string &buffer)
	{
		BoostFile bf;
		if(!bf.open_existing_file(filename,bip::read_only))
			return false;
		const unsigned long long filesize = bf.get_file_size();
		if(filesize == 0 || filesize > (std::numeric_limits<std::size_t>::max)() ||
			filesize > buffer.max_size())
			return false;
		// Keep the caller's buffer unchanged when the read fails. C++17's mutable
		// data() overload provides the writable destination expected by read_file.
		std::string contents(static_cast<std::size_t>(filesize), '\0');
		if (!bf.read_file(contents.data(), contents.size()))
			return false;

		buffer.swap(contents);
		return true;
	}

	static bool write_file_contents(const char *filename,const void *pdata,uint32_t datalen)
	{
		BoostFile bf;
		if(!bf.create_new_file(filename))
			return false;
		return bf.write_file(pdata,datalen);
	}
};

typedef boost::shared_ptr<BoostFile> BoostFilePtr;
