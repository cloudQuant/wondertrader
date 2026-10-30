#pragma once
#include "SpinMutex.hpp"
#include "BoostFile.hpp"
#include "StdUtils.hpp"
#include "BoostMappingFile.hpp"
#include "../Includes/FasterDefs.h"
#include <cstring>
#include <limits>
#include <utility>

#define SIZE_STEP 200
#define CACHE_FLAG "&^%$#@!\0"
#define FLAG_SIZE 8

typedef std::shared_ptr<BoostMappingFile> BoostMFPtr;

#ifdef _MSC_VER
#pragma warning(disable:4200)
#endif

NS_WTP_BEGIN

typedef std::function<void(const char*)> CacheLogger;

class WtKVCache
{
public:
	WtKVCache() {}
	WtKVCache(const WtKVCache&) = delete;
	WtKVCache& operator=(const WtKVCache&) = delete;

private:
	friend struct WtKVCacheTestAccess;
	

	typedef struct _CacheItem
	{
		char	_key[64] = { 0 };
		char	_val[64] = { 0 };
	} CacheItem;

	typedef struct CacheBlock
	{
		char		_blk_flag[FLAG_SIZE];
		volatile uint32_t	_size;
		uint32_t	_capacity;
		uint32_t	_date;
		CacheItem	_items[0];
	} CacheBlock;

	typedef struct _CacheBlockPair
	{
		CacheBlock*		_block;
		BoostMFPtr		_file;

		_CacheBlockPair()
		{
			_block = NULL;
			_file = NULL;
		}
	} CacheBlockPair;

	CacheBlockPair	_cache;
	SpinMutex		_lock;
	wt_hashmap<std::string, uint32_t> _indice;
	CacheLogger		_logger = nullptr;

private:
	static bool	get_slot_capacity(uint64_t fileSize, uint32_t& capacity) noexcept
	{
		if (fileSize < sizeof(CacheBlock))
			return false;

		const uint64_t itemBytes = fileSize - sizeof(CacheBlock);
		if (itemBytes % sizeof(CacheItem) != 0)
			return false;

		const uint64_t slotCount = itemBytes / sizeof(CacheItem);
		if (slotCount == 0 || slotCount > 0xFFFFFFFFULL)
			return false;

		capacity = (uint32_t)slotCount;
		return true;
	}

	static bool	get_string_length(const char* value, std::size_t fieldSize, std::size_t& length) noexcept
	{
		if (value == NULL)
			return false;

		for (std::size_t i = 0; i < fieldSize; i++)
		{
			if (value[i] == '\0')
			{
				length = i;
				return true;
			}
		}

		return false;
	}

	void	log_message(const char* message) noexcept
	{
		if (!_logger)
			return;
		try { _logger(message); } catch (...) {}
	}

	struct FileIdentity
	{
		uint64_t volume;
		uint64_t file;
		bool valid;

		FileIdentity() : volume(0), file(0), valid(false) {}
	};

	static bool get_file_identity(bip::file_handle_t handle, FileIdentity& identity) noexcept
	{
#if defined(_WIN32)
		bip::winapi::interprocess_by_handle_file_information info;
		if (!bip::winapi::get_file_information_by_handle(handle, &info))
			return false;
		identity.volume = info.dwVolumeSerialNumber;
		identity.file = (uint64_t(info.nFileIndexHigh) << 32) | uint64_t(info.nFileIndexLow);
#else
		struct stat info;
		if (::fstat(handle, &info) != 0)
			return false;
		identity.volume = (uint64_t)info.st_dev;
		identity.file = (uint64_t)info.st_ino;
#endif
		identity.valid = true;
		return true;
	}

	static bool same_file_identity(const FileIdentity& lhs, const FileIdentity& rhs) noexcept
	{
		return lhs.valid && rhs.valid && lhs.volume == rhs.volume && lhs.file == rhs.file;
	}

	static bool path_matches_identity(const char* filename, const FileIdentity& expected) noexcept
	{
		bip::file_handle_t handle = bip::ipcdetail::open_existing_file(filename, bip::read_only);
		if (handle == bip::ipcdetail::invalid_file())
			return false;
		FileIdentity actual;
		const bool identified = get_file_identity(handle, actual);
		const bool closed = bip::ipcdetail::close_file(handle);
		return identified && closed && same_file_identity(expected, actual);
	}

	struct FileCreateOps
	{
		bip::file_handle_t create_exclusive(const char* filename)
		{
			return bip::ipcdetail::create_new_file(filename, bip::read_write, bip::permissions());
		}
		bool truncate(bip::file_handle_t handle, std::size_t size) { return bip::ipcdetail::truncate_file(handle, size); }
		bool set_position(bip::file_handle_t handle, uint64_t offset)
		{
			return bip::ipcdetail::set_file_pointer(handle, (bip::offset_t)offset, bip::file_begin);
		}
		bool get_position(bip::file_handle_t handle, uint64_t& offset)
		{
			bip::offset_t position = 0;
			if (!bip::ipcdetail::get_file_pointer(handle, position) || position < 0)
				return false;
			offset = (uint64_t)position;
			return true;
		}
		bool get_size(bip::file_handle_t handle, uint64_t& size)
		{
			bip::offset_t fileSize = 0;
			if (!bip::ipcdetail::get_file_size(handle, fileSize) || fileSize < 0)
				return false;
			size = (uint64_t)fileSize;
			return true;
		}
		bool write(bip::file_handle_t handle, const void* data, std::size_t size)
		{
			return bip::ipcdetail::write_file(handle, data, size);
		}
		bool read(bip::file_handle_t handle, void* data, std::size_t size)
		{
#if defined(_WIN32)
			unsigned long bytesRead = 0;
			return bip::winapi::read_file(handle, data, (unsigned long)size, &bytesRead, 0) && bytesRead == size;
#else
			return ::read(handle, data, size) == (ssize_t)size;
#endif
		}
		bool close(bip::file_handle_t handle) { return bip::ipcdetail::close_file(handle); }
	};

	static void make_init_marker(const FileIdentity& identity, CacheItem& marker) noexcept
	{
		std::memset(&marker, 0, sizeof(marker));
		static const char keySignature[] = "WTKCID01";
		static const char valueSignature[] = "WTKVNEW1";
		std::memcpy(marker._key, keySignature, sizeof(keySignature) - 1);
		std::memcpy(marker._key + 8, &identity.volume, sizeof(identity.volume));
		std::memcpy(marker._key + 16, &identity.file, sizeof(identity.file));
		std::memcpy(marker._val, valueSignature, sizeof(valueSignature) - 1);
		const uint64_t check = identity.volume ^ identity.file ^ 0x57544B56494E4954ULL;
		std::memcpy(marker._val + 8, &check, sizeof(check));
	}

	static bool has_init_marker(const CacheItem& item) noexcept
	{
		static const char keySignature[] = "WTKCID01";
		static const char valueSignature[] = "WTKVNEW1";
		return std::memcmp(item._key, keySignature, sizeof(keySignature) - 1) == 0 &&
			std::memcmp(item._val, valueSignature, sizeof(valueSignature) - 1) == 0;
	}

	static void make_resize_marker(const FileIdentity& identity, uint64_t oldSize, uint64_t newSize, CacheItem& marker) noexcept
	{
		std::memset(&marker, 0, sizeof(marker));
		static const char keySignature[] = "WTKRID01";
		static const char valueSignature[] = "WTKRESZ1";
		std::memcpy(marker._key, keySignature, sizeof(keySignature) - 1);
		std::memcpy(marker._key + 8, &identity.volume, sizeof(identity.volume));
		std::memcpy(marker._key + 16, &identity.file, sizeof(identity.file));
		std::memcpy(marker._val, valueSignature, sizeof(valueSignature) - 1);
		std::memcpy(marker._val + 8, &oldSize, sizeof(oldSize));
		std::memcpy(marker._val + 16, &newSize, sizeof(newSize));
	}

	template <typename Ops>
	static bool create_initialized_file_with_ops(const char* filename, uint32_t date, FileIdentity& identity, Ops& ops) noexcept
	{
		bip::file_handle_t handle = ops.create_exclusive(filename);
		if (handle == bip::ipcdetail::invalid_file())
			return false;

		FileIdentity createdIdentity;
		bool initialized = get_file_identity(handle, createdIdentity);
		const uint64_t fileSize = sizeof(CacheBlock) + sizeof(CacheItem) * SIZE_STEP;
		initialized = initialized && ops.truncate(handle, (std::size_t)fileSize);

		CacheBlock header;
		CacheItem marker;
		if (initialized)
		{
			std::memset(&header, 0, sizeof(header));
			std::memcpy(header._blk_flag, CACHE_FLAG, FLAG_SIZE);
			header._size = 0;
			header._capacity = SIZE_STEP;
			header._date = date;
			make_init_marker(createdIdentity, marker);

			// Boost's Windows wrapper reports API success even when WriteFile wrote
			// fewer bytes. Verify the position, total size and bytes read back from
			// this exact handle after each write.
			char readback[sizeof(CacheItem)];
			auto writeAndVerify = [&](uint64_t offset, const void* expected, std::size_t length) -> bool
			{
				if (!ops.set_position(handle, offset) || !ops.write(handle, expected, length))
					return false;
				uint64_t position = 0;
				uint64_t currentSize = 0;
				if (!ops.get_position(handle, position) || position != offset + length ||
					!ops.get_size(handle, currentSize) || currentSize != fileSize ||
					!ops.set_position(handle, offset) || !ops.read(handle, readback, length))
					return false;
				return std::memcmp(readback, expected, length) == 0;
			};
			initialized = writeAndVerify(0, &header, sizeof(header)) &&
				writeAndVerify(sizeof(CacheBlock), &marker, sizeof(marker));
		}

		const bool closed = ops.close(handle);
		if (initialized && closed)
			identity = createdIdentity;
		return initialized && closed;
	}

	static bool create_initialized_file(const char* filename, uint32_t date, FileIdentity& identity) noexcept
	{
		FileCreateOps ops;
		return create_initialized_file_with_ops(filename, date, identity, ops);
	}

	struct InitOps
	{
		bool create_initialized_file(const char* filename, uint32_t date, FileIdentity& identity)
		{
			return WtKVCache::create_initialized_file(filename, date, identity);
		}
		bool map(BoostMappingFile& mapping, const char* filename) { return mapping.map(filename); }
		template <typename Index>
		void add_index(Index& index, const char* key, std::size_t keyLength, uint32_t value) { index[std::string(key, keyLength)] = value; }
	};

	struct ResizeOps
	{
		bip::file_handle_t handle = bip::ipcdetail::invalid_file();
		~ResizeOps() { close_file(); }

		bool open_existing_file(const char* filename)
		{
			close_file();
			handle = bip::ipcdetail::open_existing_file(filename, bip::read_write);
			return handle != bip::ipcdetail::invalid_file();
		}
		bool get_file_identity(FileIdentity& identity) { return WtKVCache::get_file_identity(handle, identity); }
		bool get_file_size(uint64_t& size)
		{
			bip::offset_t fileSize = 0;
			if (!bip::ipcdetail::get_file_size(handle, fileSize) || fileSize < 0)
				return false;
			size = (uint64_t)fileSize;
			return true;
		}
		bool seek_to_end() { return bip::ipcdetail::set_file_pointer(handle, 0, bip::file_end); }
		bool get_file_pointer(uint64_t& pos)
		{
			bip::offset_t filePos = 0;
			if (!bip::ipcdetail::get_file_pointer(handle, filePos) || filePos < 0)
				return false;
			pos = (uint64_t)filePos;
			return true;
		}
		bool set_file_pointer(uint64_t pos) { return bip::ipcdetail::set_file_pointer(handle, (bip::offset_t)pos, bip::file_begin); }
		bool write_file(const void* data, std::size_t size) { return bip::ipcdetail::write_file(handle, data, size); }
		bool read_file(void* data, std::size_t size)
		{
#if defined(_WIN32)
			unsigned long bytesRead = 0;
			return bip::winapi::read_file(handle, data, (unsigned long)size, &bytesRead, 0) && bytesRead == size;
#else
			return ::read(handle, data, size) == (ssize_t)size;
#endif
		}
		bool truncate_file(uint64_t size) { return bip::ipcdetail::truncate_file(handle, (std::size_t)size); }
		bool close_file()
		{
			if (handle == bip::ipcdetail::invalid_file())
				return true;
			if (!bip::ipcdetail::close_file(handle))
				return false;
			handle = bip::ipcdetail::invalid_file();
			return true;
		}
		bool map(BoostMappingFile& mapping, const char* filename) { return mapping.map(filename); }
	};

	static bool	calculate_doubled_capacity(uint32_t current, uint32_t& doubled) noexcept
	{
		if (current == 0 || current > (std::numeric_limits<uint32_t>::max)() / 2)
			return false;

		doubled = current * 2;
		return true;
	}

	// Compile-time operation seam for deterministic tests. It adds no object state
	// or virtual dispatch to WtKVCache.
	template <typename Ops>
	bool	resize_with_ops(uint32_t newCap, Ops& ops) noexcept
	{
		if (_cache._file == NULL || _cache._block == NULL)
			return false;

		//调用该函数之前,应该保证线程安全了
		CacheBlock* cBlock = _cache._block;
		if (cBlock->_capacity >= newCap)
			return true;
		if (newCap == 0)
			return false;

		const uint64_t uOldSize = sizeof(CacheBlock) + (uint64_t)sizeof(CacheItem) * cBlock->_capacity;
		const uint64_t uNewSize = sizeof(CacheBlock) + (uint64_t)sizeof(CacheItem) * newCap;
		const uint64_t mappedSize = _cache._file->size();
		if (mappedSize != uOldSize || uNewSize <= uOldSize ||
			uNewSize > (uint64_t)(std::numeric_limits<std::size_t>::max)())
		{
			log_message("Cache resize rejected because mapped size or capacity is invalid");
			return false;
		}

		const char* filename = _cache._file->filename();
		FileIdentity expectedIdentity;
		if (!_cache._file->get_file_identity(expectedIdentity.volume, expectedIdentity.file))
		{
			log_message("Cache resize rejected because original mapping identity is unavailable");
			return false;
		}
		expectedIdentity.valid = true;
		if (!path_matches_identity(filename, expectedIdentity))
		{
			log_message("Cache resize rejected because path no longer identifies original mapping");
			return false;
		}

		bool fileOpened = false;
		bool fileMayHaveChanged = false;
		bool fileReady = false;
		std::string data;
		BoostMFPtr pNewMf;
		try
		{
			if (!ops.open_existing_file(filename))
			{
				log_message("Opening cache file failed while resizing");
			}
			else
			{
				fileOpened = true;
				FileIdentity openedIdentity;
				uint64_t currentFileSize = 0;
				if (!ops.get_file_identity(openedIdentity) || !same_file_identity(expectedIdentity, openedIdentity))
				{
					log_message("Cache resize rejected because reopened handle is not the original mapping file");
				}
				else if (!ops.get_file_size(currentFileSize) || currentFileSize < uOldSize || currentFileSize > uNewSize)
				{
					log_message("Cache resize rejected because backing file size is inconsistent");
				}
				else
				{
					fileMayHaveChanged = currentFileSize > uOldSize;
					if (!ops.seek_to_end())
					{
						log_message("Seeking to end of cache file failed while resizing");
					}
					else
					{
						uint64_t filePos = 0;
						if (!ops.get_file_pointer(filePos) || filePos != currentFileSize)
						{
							log_message("Cache resize could not verify the end-of-file position");
						}
						else
						{
							const uint64_t bytesToWrite = uNewSize - currentFileSize;
							bool expanded = bytesToWrite == 0;
							if (bytesToWrite != 0)
							{
								data.resize((std::size_t)bytesToWrite, 0);
								fileMayHaveChanged = true;
								if (!ops.write_file(data.data(), data.size()))
								{
									log_message("Writing cache expansion failed");
								}
								else
									expanded = true;
							}
							uint64_t expandedSize = 0;
							if (expanded && ops.get_file_size(expandedSize) && expandedSize == uNewSize)
							{
								CacheItem marker;
								make_resize_marker(expectedIdentity, uOldSize, uNewSize, marker);
								char readback[sizeof(CacheItem)];
								const uint64_t markerOffset = uOldSize;
								uint64_t markerPosition = 0;
								uint64_t markerFileSize = 0;
								fileMayHaveChanged = true;
								if (ops.set_file_pointer(markerOffset) && ops.write_file(&marker, sizeof(marker)) &&
									ops.get_file_pointer(markerPosition) && markerPosition == markerOffset + sizeof(marker) &&
									ops.get_file_size(markerFileSize) && markerFileSize == uNewSize &&
									ops.set_file_pointer(markerOffset) && ops.read_file(readback, sizeof(readback)) &&
									std::memcmp(readback, &marker, sizeof(marker)) == 0)
									fileReady = true;
								else
									log_message("Cache resize identity marker write or verification failed");
							}
							else if (expanded)
								log_message("Cache file size did not match requested capacity after expansion");
						}
					}
				}
			}
		}
		catch (...)
		{
			log_message("Got an exception while resizing cache file");
		}
		if (fileReady)
		{
			try
			{
				if (!path_matches_identity(filename, expectedIdentity))
				{
					log_message("Cache path changed before expanded mapping; old mapping retained");
					fileReady = false;
				}
				else
				{
					pNewMf.reset(new BoostMappingFile());
					if (!ops.map(*pNewMf, filename) || pNewMf->size() != uNewSize || pNewMf->addr() == NULL)
					{
						log_message("Mapping expanded cache file failed or returned an unexpected size");
						fileReady = false;
					}
					else
					{
						FileIdentity mappedIdentity;
						CacheBlock* newBlock = (CacheBlock*)pNewMf->addr();
						CacheItem expectedMarker;
						make_resize_marker(expectedIdentity, uOldSize, uNewSize, expectedMarker);
						const bool hasMappedIdentity = pNewMf->get_file_identity(mappedIdentity.volume, mappedIdentity.file);
						mappedIdentity.valid = hasMappedIdentity;
						if (!same_file_identity(expectedIdentity, mappedIdentity) ||
							!path_matches_identity(filename, expectedIdentity) ||
							std::memcmp(newBlock->_blk_flag, CACHE_FLAG, FLAG_SIZE) != 0 ||
							newBlock->_size != cBlock->_size || newBlock->_capacity != cBlock->_capacity ||
							newBlock->_date != cBlock->_date ||
							std::memcmp(&newBlock->_items[cBlock->_capacity], &expectedMarker, sizeof(expectedMarker)) != 0)
						{
							log_message("Expanded mapping is not the original cache file");
							fileReady = false;
						}
						else
							std::memset(&newBlock->_items[cBlock->_capacity], 0, sizeof(CacheItem));
					}
				}
			}
			catch (...)
			{
				log_message("Got an exception while mapping cache file");
				fileReady = false;
			}
		}

		if (fileReady && fileOpened)
		{
			bool closed = false;
			try { closed = ops.close_file(); } catch (...) { closed = false; }
			if (closed)
				fileOpened = false;
			else
			{
				log_message("Cache resize could not close the original expansion handle");
				fileReady = false;
			}
		}

		if (!fileReady)
		{
			if (fileMayHaveChanged)
			{
				bool rolledBack = false;
				try
				{
					FileIdentity rollbackIdentity;
					if (fileOpened && ops.get_file_identity(rollbackIdentity) && same_file_identity(expectedIdentity, rollbackIdentity))
					{
						rolledBack = ops.truncate_file(uOldSize);
						uint64_t restoredSize = 0;
						rolledBack = rolledBack && ops.get_file_size(restoredSize) && restoredSize == uOldSize;
					}
				}
				catch (...)
				{
					rolledBack = false;
				}
				if (fileOpened)
				{
					try { ops.close_file(); } catch (...) {}
					fileOpened = false;
				}
				if (!rolledBack)
					log_message("Cache resize failed and rollback on original handle also failed; old mapping remains active and backing-file recovery is unverified");
			}
			else if (fileOpened)
			{
				try { ops.close_file(); } catch (...) {}
				fileOpened = false;
			}
			return false;
		}
		// Commit only after the expanded file has been mapped and verified. Keep
		// the old mapping alive through every failure path above.
		_cache._file = pNewMf;
		_cache._block = (CacheBlock*)pNewMf->addr();
		_cache._block->_capacity = newCap;
		return true;
	}

	bool	resize(uint32_t newCap) noexcept
	{
		ResizeOps ops;
		return resize_with_ops(newCap, ops);
	}

	bool	ensure_capacity_for_insert() noexcept
	{
		if (_cache._block == NULL || _cache._file == NULL)
		{
			log_message("Cache insertion rejected because cache is not initialized");
			return false;
		}

		if (_cache._block->_size < _cache._block->_capacity)
			return true;

		SpinLock lock(_lock);
		if (_cache._block->_size < _cache._block->_capacity)
			return true;

		uint32_t newCap = 0;
		if (!calculate_doubled_capacity(_cache._block->_capacity, newCap))
		{
			log_message("Cache capacity doubling would overflow uint32_t; insertion rejected");
			return false;
		}

		if (!resize(newCap))
		{
			log_message("Cache insertion rejected because capacity expansion failed");
			return false;
		}
		return _cache._block->_size < _cache._block->_capacity;
	}

private:
	template <typename Ops>
	bool	init_with_ops(const char* filename, uint32_t uDate, CacheLogger logger, Ops& ops) noexcept
	{
		_logger = std::move(logger);
		try
		{
			_indice.clear();
			_cache._block = NULL;
			_cache._file.reset();
			bool isNew = false;
			FileIdentity createdIdentity;
			if (!StdFile::exists(filename))
			{
				if (!ops.create_initialized_file(filename, uDate, createdIdentity))
				{
					log_message("Mapping cache file failed during exclusive initialization; path left fail-closed");
					return false;
				}

				isNew = true;
				if (!path_matches_identity(filename, createdIdentity))
				{
					log_message("New cache path no longer identifies the exclusively created file");
					return false;
				}
			}

			uint32_t actualCap = 0;
			if (!get_slot_capacity(BoostFile::get_file_size(filename), actualCap))
			{
				_cache._block = NULL;
				log_message("Cache file does not contain a complete header and slot layout");
				return false;
			}

			_cache._file.reset(new BoostMappingFile);
			if (!ops.map(*_cache._file, filename))
			{
				_cache._file.reset();
				_cache._block = NULL;
				log_message("Mapping cache file failed");
				return false;
			}
			_cache._block = (CacheBlock*)_cache._file->addr();
			if (isNew)
			{
				FileIdentity mappedIdentity;
				const bool mappedIdentityAvailable = _cache._file->get_file_identity(mappedIdentity.volume, mappedIdentity.file);
				mappedIdentity.valid = mappedIdentityAvailable;
				if (!same_file_identity(createdIdentity, mappedIdentity) || !path_matches_identity(filename, createdIdentity))
				{
					_cache._file.reset();
					_cache._block = NULL;
					log_message("New cache mapping is not the exclusively created file");
					return false;
				}
			}

			// Recheck the mapped size because the file may have changed between stat and mmap.
			if (!get_slot_capacity(_cache._file->size(), actualCap))
			{
				_cache._file.reset();
				_cache._block = NULL;
				log_message("Mapped cache file does not contain a complete header and slot layout");
				return false;
			}

			if (memcmp(_cache._block->_blk_flag, CACHE_FLAG, FLAG_SIZE) != 0)
			{
				_cache._file.reset();
				_cache._block = NULL;
				log_message("Cache file header flag is invalid");
				return false;
			}

			const uint32_t storedSize = _cache._block->_size;
			const uint32_t markedCap = _cache._block->_capacity;
			if (markedCap == 0 || storedSize > markedCap)
			{
				_cache._file.reset();
				_cache._block = NULL;
				log_message("Cache file header has an invalid capacity or size");
				return false;
			}

			if (isNew)
			{
				CacheItem expectedMarker;
				make_init_marker(createdIdentity, expectedMarker);
				if (_cache._block->_date != uDate || markedCap != actualCap || storedSize != 0 ||
					std::memcmp(&_cache._block->_items[0], &expectedMarker, sizeof(expectedMarker)) != 0)
				{
					_cache._file.reset();
					_cache._block = NULL;
					log_message("Mapped cache is not the newly initialized file");
					return false;
				}
				// Slot zero is not an entry while size is zero. Remove the temporary
				// identity marker only after the mapped bytes have been verified.
				std::memset(&_cache._block->_items[0], 0, sizeof(CacheItem));
			}
			else
			{
				if (storedSize == 0 && has_init_marker(_cache._block->_items[0]))
					std::memset(&_cache._block->_items[0], 0, sizeof(CacheItem));

				const bool resetForDate = _cache._block->_date != uDate;
				const uint32_t restoredSize = storedSize < actualCap ? storedSize : actualCap;
				// Validate every retained record before changing persistent header
				// metadata; construct index keys only from a bounded length.
				for (uint32_t i = 0; i < restoredSize; i++)
				{
					std::size_t keyLength = 0;
					std::size_t valueLength = 0;
					CacheItem& item = _cache._block->_items[i];
					if (!get_string_length(item._key, sizeof(item._key), keyLength) ||
						!get_string_length(item._val, sizeof(item._val), valueLength))
					{
						_indice.clear();
						_cache._block = NULL;
						_cache._file.reset();
						log_message("Cache record has a key or value without a terminator");
						return false;
					}
					(void)valueLength;
					if (!resetForDate)
						ops.add_index(_indice, item._key, keyLength, i);
				}

				// The mapped file is authoritative for available slots. Clamp the
				// persisted entry count, not to the old capacity, so spare slots are
				// never reported as entries after a truncation.
				_cache._block->_capacity = actualCap;
				if (resetForDate)
				{
					_cache._block->_size = 0;
					_cache._block->_date = uDate;
					memset(_cache._block->_items, 0, sizeof(CacheItem) * actualCap);
					log_message("Cache file reset due to a different date");
				}
				else
				{
					_cache._block->_size = restoredSize;
					if (actualCap != markedCap)
						log_message("Cache file capacity recovered from mapped file size");
				}
			}

			return true;
		}
		catch (...)
		{
			try { _indice.clear(); } catch (...) {}
			_cache._block = NULL;
			_cache._file.reset();
			log_message("Cache initialization failed due to an exception");
			return false;
		}
	}

public:
	bool	init(const char* filename, uint32_t uDate, CacheLogger logger = nullptr) noexcept
	{
		InitOps ops;
		return init_with_ops(filename, uDate, std::move(logger), ops);
	}

	inline void clear() noexcept
	{
		if (_cache._block == NULL)
			return;

		_lock.lock();
		_indice.clear();

		memset(_cache._block->_items, 0, sizeof(CacheItem)*_cache._block->_capacity);
		_cache._block->_size = 0;

		_lock.unlock();
	}

	inline const char*	get(const char* key) const  noexcept
	{
		auto it = _indice.find(key);
		if (it == _indice.end())
			return "";

		return _cache._block->_items[it->second]._val;
	}

	void	put(const char* key, const char*val, std::size_t len = 0) noexcept
	{
		(void)try_put(key, val, len);
	}

	// Returns false when validation or required capacity growth fails.
	bool	try_put(const char* key, const char* val, std::size_t len = 0) noexcept
	{
		std::size_t keyLen = 0;
		if (!get_string_length(key, sizeof(((CacheItem*)NULL)->_key), keyLen))
		{
			log_message("Rejected WtKVCache::put because key exceeds 63 bytes or is null");
			return false;
		}

		std::size_t valueLen = len;
		if (len == 0)
		{
			if (!get_string_length(val, sizeof(((CacheItem*)NULL)->_val), valueLen))
			{
				log_message("Rejected WtKVCache::put because value exceeds 63 bytes or is null");
				return false;
			}
		}
		else if (val == NULL || len >= sizeof(((CacheItem*)NULL)->_val))
		{
			log_message("Rejected WtKVCache::put because value length exceeds 63 bytes or is null");
			return false;
		}

		try
		{
			auto it = _indice.find(key);
			if (it != _indice.end())
			{
				wt_strcpy(_cache._block->_items[it->second]._val, val, valueLen);
				return true;
			}
		}
		catch (...)
		{
			log_message("Cache insertion rejected because key lookup failed");
			return false;
		}

		if (!ensure_capacity_for_insert())
			return false;

		const uint32_t idx = _cache._block->_size;
		CacheItem& item = _cache._block->_items[idx];
		wt_strcpy(item._key, key, keyLen);
		wt_strcpy(item._val, val, valueLen);
		try
		{
			_indice[key] = idx;
		}
		catch (...)
		{
			try { _indice.erase(key); } catch (...) {}
			memset(&item, 0, sizeof(item));
			log_message("Cache insertion rejected because index allocation failed");
			return false;
		}
		_cache._block->_size = idx + 1;
		return true;
	}

	void	put_if_none(const char* key, const char*val, std::size_t len = 0) noexcept
	{
		(void)try_put_if_none(key, val, len);
	}

	// Existing keys are a successful no-op; false reports a rejected insertion.
	bool	try_put_if_none(const char* key, const char* val, std::size_t len = 0) noexcept
	{
		std::size_t keyLen = 0;
		if (!get_string_length(key, sizeof(((CacheItem*)NULL)->_key), keyLen))
		{
			log_message("Rejected WtKVCache::put_if_none because key exceeds 63 bytes or is null");
			return false;
		}

		try
		{
			if (_indice.find(key) != _indice.end())
				return true;
		}
		catch (...)
		{
			log_message("Cache insertion rejected because key lookup failed");
			return false;
		}

		std::size_t valueLen = len;
		if (len == 0)
		{
			if (!get_string_length(val, sizeof(((CacheItem*)NULL)->_val), valueLen))
			{
				log_message("Rejected WtKVCache::put_if_none because value exceeds 63 bytes or is null");
				return false;
			}
		}
		else if (val == NULL || len >= sizeof(((CacheItem*)NULL)->_val))
		{
			log_message("Rejected WtKVCache::put_if_none because value length exceeds 63 bytes or is null");
			return false;
		}

		if (!ensure_capacity_for_insert())
			return false;

		const uint32_t idx = _cache._block->_size;
		CacheItem& item = _cache._block->_items[idx];
		wt_strcpy(item._key, key, keyLen);
		wt_strcpy(item._val, val, valueLen);
		try
		{
			_indice[key] = idx;
		}
		catch (...)
		{
			try { _indice.erase(key); } catch (...) {}
			memset(&item, 0, sizeof(item));
			log_message("Cache insertion rejected because index allocation failed");
			return false;
		}
		_cache._block->_size = idx + 1;
		return true;
	}

	inline bool	has(const char* key) const  noexcept
	{
		return (_indice.find(key) != _indice.end());
	}

	inline uint32_t size() const noexcept
	{
		if (_cache._block == NULL)
			return 0;

		return _cache._block->_size;
	}

	inline uint32_t capacity() const noexcept
	{
		if (_cache._block == NULL)
			return 0;

		return _cache._block->_capacity;
	}
};

NS_WTP_END
