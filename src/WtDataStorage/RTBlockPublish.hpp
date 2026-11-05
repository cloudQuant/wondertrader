/*!
 * \file RTBlockPublish.hpp
 * \brief Cross-process multi-writer publish protocol for RT mmap blocks
 *        (iteration 03, decision card 13: 13a=A prepare-then-publish, 13b=B multi-writer).
 *
 * Protocol summary (see src/TestUnits/fixtures/i03_storage_multiwriter_20261104/README.md):
 *  - Every RT block file `X.dmb` has a sidecar `X.dmb.wlk`. A cooperating writer takes a
 *    blocking exclusive lock on the sidecar for the duration of one prepare+publish step.
 *    A second writer waits; it is never rejected (13b=B).
 *  - Prepare: under the lock the writer first makes its own mapping cover the published
 *    capacity (another writer may have grown the file), grows the block if needed, and
 *    writes the record payload into the first unpublished slot.
 *  - Publish: a single aligned 32-bit release store of the header `_size`. Capacity is
 *    published only after the file has been extended and remapped. A writer that dies
 *    before the size store leaves an invisible slot that the next writer overwrites.
 *  - Structural changes (create/initialise/resize/day reset) are bracketed by a journal
 *    record in the sidecar (state PREPARING -> IDLE, generation++). A sidecar found in
 *    PREPARING state by the next lock holder means the previous holder died mid-change;
 *    the holder reconciles the header against the physical file before continuing.
 *  - Readers never take the lock. They read `_size`/`_capacity` with acquire loads and
 *    clamp the visible count to the records covered by *their own* mapping.
 *
 * Out of protocol: writers built before this change, non-cooperating processes, power
 * loss (no fsync), network file systems, and end-of-day maintenance (transHisData).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wt_rtpub
{
	// RTBlockHeader is #pragma pack(1): BlockHeader(8 flag + 2 type + 2 version) = 12 bytes,
	// followed by uint32 _size and uint32 _capacity. Mappings are page aligned, so both
	// fields are naturally 4-byte aligned in memory.
	static const std::size_t RT_SIZE_OFFSET = 12;
	static const std::size_t RT_CAPACITY_OFFSET = 16;

	inline uint32_t loadAcquire(const void* base, std::size_t offset)
	{
		const uint32_t* p = reinterpret_cast<const uint32_t*>(static_cast<const char*>(base) + offset);
#ifdef _MSC_VER
		return (uint32_t)InterlockedCompareExchange((volatile LONG*)p, 0, 0);
#else
		return __atomic_load_n(p, __ATOMIC_ACQUIRE);
#endif
	}

	inline void storeRelease(void* base, std::size_t offset, uint32_t value)
	{
		uint32_t* p = reinterpret_cast<uint32_t*>(static_cast<char*>(base) + offset);
#ifdef _MSC_VER
		InterlockedExchange((volatile LONG*)p, (LONG)value);
#else
		__atomic_store_n(p, value, __ATOMIC_RELEASE);
#endif
	}

	inline uint32_t publishedSize(const void* base) { return loadAcquire(base, RT_SIZE_OFFSET); }
	inline uint32_t publishedCapacity(const void* base) { return loadAcquire(base, RT_CAPACITY_OFFSET); }
	inline void publishSize(void* base, uint32_t v) { storeRelease(base, RT_SIZE_OFFSET, v); }
	inline void publishCapacity(void* base, uint32_t v) { storeRelease(base, RT_CAPACITY_OFFSET, v); }

	/// Number of whole records covered by a mapping (0 when the mapping is shorter than the header).
	inline uint32_t mappedRecords(uint64_t mappedBytes, uint64_t headerBytes, uint64_t recordBytes)
	{
		if (recordBytes == 0 || mappedBytes < headerBytes)
			return 0;
		const uint64_t n = (mappedBytes - headerBytes) / recordBytes;
		return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
	}

	/// Reader-side visibility rule: published records that are also inside the caller's mapping.
	inline uint32_t visibleRecords(const void* base, uint64_t mappedBytes, uint64_t headerBytes, uint64_t recordBytes)
	{
		if (base == nullptr || mappedBytes < headerBytes || headerBytes < RT_CAPACITY_OFFSET + sizeof(uint32_t))
			return 0;
		uint32_t n = publishedSize(base);
		const uint32_t cap = publishedCapacity(base);
		const uint32_t mapped = mappedRecords(mappedBytes, headerBytes, recordBytes);
		if (n > cap) n = cap;
		if (n > mapped) n = mapped;
		return n;
	}

	enum JournalState : uint32_t
	{
		JS_IDLE = 0,
		JS_PREPARING = 1
	};

#pragma pack(push, 1)
	struct Journal
	{
		char		_magic[8];		///< "WTRTPUB"
		uint32_t	_version;		///< 1
		uint32_t	_state;			///< JournalState
		uint64_t	_generation;	///< incremented by every structural begin and recovery
		int64_t		_owner_pid;		///< pid of the last structural writer
		uint64_t	_target;		///< capacity targeted by the pending structural change
	};
#pragma pack(pop)

	static const char JOURNAL_MAGIC[8] = { 'W','T','R','T','P','U','B','\0' };

	/*!
	 * Blocking, cross-process, per-file publish lock with a crash journal.
	 * Not copyable. Locks are released automatically by the OS when the holder dies.
	 */
	class PublishLock
	{
	public:
		PublishLock() = default;
		~PublishLock() { release(); }
		PublishLock(const PublishLock&) = delete;
		PublishLock& operator=(const PublishLock&) = delete;

		static std::string sidecarPath(const std::string& dataPath) { return dataPath + ".wlk"; }

		/// Blocks until the lock is held. Returns false (fail closed) on I/O errors or on a
		/// sidecar with unknown content; error() then describes the reason.
		bool acquire(const std::string& dataPath)
		{
			release();
			_recovered = false;
			_recovered_pid = 0;
			const std::string lockPath = sidecarPath(dataPath);
#ifdef _WIN32
			_h = CreateFileA(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (_h == INVALID_HANDLE_VALUE)
			{
				_err = "cannot open publish lock " + lockPath;
				return false;
			}
			OVERLAPPED ov;
			memset(&ov, 0, sizeof(ov));
			ov.Offset = 0x10000;	// lock a byte beyond the journal so journal I/O is not blocked
			if (!LockFileEx(_h, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov))
			{
				_err = "cannot lock publish lock " + lockPath;
				CloseHandle(_h);
				_h = INVALID_HANDLE_VALUE;
				return false;
			}
#else
			_fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
			if (_fd < 0)
			{
				_err = "cannot open publish lock " + lockPath + ": " + strerror(errno);
				return false;
			}
			int rc;
			do { rc = ::flock(_fd, LOCK_EX); } while (rc != 0 && errno == EINTR);
			if (rc != 0)
			{
				_err = "cannot lock publish lock " + lockPath + ": " + strerror(errno);
				::close(_fd);
				_fd = -1;
				return false;
			}
#endif
			_held = true;

			Journal j;
			memset(&j, 0, sizeof(j));
			const long got = readJournal(j);
			if (got < 0)
			{
				_err = "cannot read publish journal " + lockPath;
				release();
				return false;
			}
			if (got == 0)
			{
				memcpy(j._magic, JOURNAL_MAGIC, sizeof(j._magic));
				j._version = 1;
				j._state = JS_IDLE;
				if (!writeJournal(j))
				{
					_err = "cannot initialise publish journal " + lockPath;
					release();
					return false;
				}
			}
			else if (got != (long)sizeof(Journal) || memcmp(j._magic, JOURNAL_MAGIC, sizeof(j._magic)) != 0
				|| j._version != 1 || (j._state != JS_IDLE && j._state != JS_PREPARING))
			{
				// 13c=A: unknown sidecar content is never guessed; the block is quarantined.
				_err = "publish journal " + lockPath + " has unknown format; refusing to write";
				release();
				return false;
			}
			_journal = j;
			if (j._state == JS_PREPARING)
			{
				// We hold the lock, so the previous structural writer is gone.
				_recovered = true;
				_recovered_pid = j._owner_pid;
			}
			return true;
		}

		bool held() const { return _held; }
		bool recovered() const { return _recovered; }
		int64_t recoveredPid() const { return _recovered_pid; }
		uint64_t generation() const { return _journal._generation; }
		const std::string& error() const { return _err; }

		/// Marks a structural change as in progress (crash-detectable).
		bool beginStructural(uint64_t target)
		{
			if (!_held)
				return false;
			Journal j = _journal;
			j._state = JS_PREPARING;
			j._generation += 1;
			j._owner_pid = currentPid();
			j._target = target;
			if (!writeJournal(j))
				return false;
			_journal = j;
			return true;
		}

		/// Marks the structural change (or a crash recovery) as complete.
		bool endStructural()
		{
			if (!_held)
				return false;
			Journal j = _journal;
			j._state = JS_IDLE;
			if (!writeJournal(j))
				return false;
			_journal = j;
			_recovered = false;
			return true;
		}

		void release()
		{
#ifdef _WIN32
			if (_h != INVALID_HANDLE_VALUE)
			{
				if (_held)
				{
					OVERLAPPED ov;
					memset(&ov, 0, sizeof(ov));
					ov.Offset = 0x10000;
					UnlockFileEx(_h, 0, 1, 0, &ov);
				}
				CloseHandle(_h);
				_h = INVALID_HANDLE_VALUE;
			}
#else
			if (_fd >= 0)
			{
				// Closing the descriptor drops the flock.
				::close(_fd);
				_fd = -1;
			}
#endif
			_held = false;
		}

	private:
		static int64_t currentPid()
		{
#ifdef _WIN32
			return (int64_t)GetCurrentProcessId();
#else
			return (int64_t)::getpid();
#endif
		}

		long readJournal(Journal& j)
		{
#ifdef _WIN32
			OVERLAPPED ov;
			memset(&ov, 0, sizeof(ov));
			DWORD got = 0;
			if (!ReadFile(_h, &j, sizeof(j), &got, &ov))
			{
				if (GetLastError() == ERROR_HANDLE_EOF)
					return 0;
				return -1;
			}
			return (long)got;
#else
			std::size_t total = 0;
			char* p = reinterpret_cast<char*>(&j);
			while (total < sizeof(j))
			{
				const ssize_t n = ::pread(_fd, p + total, sizeof(j) - total, (off_t)total);
				if (n < 0)
				{
					if (errno == EINTR) continue;
					return -1;
				}
				if (n == 0)
					break;
				total += (std::size_t)n;
			}
			return (long)total;
#endif
		}

		bool writeJournal(const Journal& j)
		{
#ifdef _WIN32
			OVERLAPPED ov;
			memset(&ov, 0, sizeof(ov));
			DWORD put = 0;
			return WriteFile(_h, &j, sizeof(j), &put, &ov) && put == sizeof(j);
#else
			std::size_t total = 0;
			const char* p = reinterpret_cast<const char*>(&j);
			while (total < sizeof(j))
			{
				const ssize_t n = ::pwrite(_fd, p + total, sizeof(j) - total, (off_t)total);
				if (n < 0)
				{
					if (errno == EINTR) continue;
					return false;
				}
				if (n == 0)
					return false;
				total += (std::size_t)n;
			}
			return true;
#endif
		}

	private:
#ifdef _WIN32
		HANDLE		_h = INVALID_HANDLE_VALUE;
#else
		int			_fd = -1;
#endif
		bool		_held = false;
		bool		_recovered = false;
		int64_t		_recovered_pid = 0;
		Journal		_journal{};
		std::string	_err;
	};

	/// Brackets a structural change; marks the journal IDLE again on every exit path of a
	/// live process. Only a process that dies inside the scope leaves PREPARING behind.
	class StructuralGuard
	{
	public:
		StructuralGuard(PublishLock& lock, uint64_t target) : _lock(lock), _begun(lock.beginStructural(target)) {}
		~StructuralGuard() { if (_begun) _lock.endStructural(); }
		StructuralGuard(const StructuralGuard&) = delete;
		StructuralGuard& operator=(const StructuralGuard&) = delete;
		bool begun() const { return _begun; }
	private:
		PublishLock&	_lock;
		bool			_begun;
	};

	/// Prepare-then-publish creation of a new block file: the fully initialised image is
	/// written to a private temporary file and renamed into place (caller holds the lock).
	/// A crash leaves either no block file or a complete one, never a half-initialised header.
	inline bool publishNewBlockFile(const std::string& path, const void* header, std::size_t headerBytes,
		uint64_t totalBytes, std::string& err)
	{
		if (totalBytes < headerBytes || totalBytes > (uint64_t)0x7FFFFFFF)
		{
			err = "invalid block size for " + path;
			return false;
		}
#ifdef _WIN32
		const std::string tmp = path + ".wtnew." + std::to_string((unsigned long)GetCurrentProcessId());
		HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			err = "cannot create " + tmp;
			return false;
		}
		std::string buf((std::size_t)totalBytes, '\0');
		memcpy(&buf[0], header, headerBytes);
		DWORD put = 0;
		const bool ok = WriteFile(h, buf.data(), (DWORD)buf.size(), &put, NULL) && put == buf.size();
		CloseHandle(h);
		if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		{
			DeleteFileA(tmp.c_str());
			err = "cannot publish new block file " + path;
			return false;
		}
		return true;
#else
		const std::string tmp = path + ".wtnew." + std::to_string((long)::getpid());
		const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		if (fd < 0)
		{
			err = "cannot create " + tmp + ": " + strerror(errno);
			return false;
		}
		std::string buf((std::size_t)totalBytes, '\0');
		memcpy(&buf[0], header, headerBytes);
		std::size_t total = 0;
		bool ok = true;
		while (total < buf.size())
		{
			const ssize_t n = ::write(fd, buf.data() + total, buf.size() - total);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
			{
				ok = false;
				break;
			}
			total += (std::size_t)n;
		}
		if (::close(fd) != 0)
			ok = false;
		if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0)
		{
			::unlink(tmp.c_str());
			err = "cannot publish new block file " + path;
			return false;
		}
		return true;
#endif
	}
}
