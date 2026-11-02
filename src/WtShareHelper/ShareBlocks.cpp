#include "ShareBlocks.h"
#include "../Share/BoostFile.hpp"
#include "../Share/TimeUtils.hpp"
#include "../Share/StdUtils.hpp"
#include "../Includes/WTSTypes.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <KnownFolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace shareblock;

namespace
{
	struct PendingLog
	{
		ShareBlocks::FuncLogger logger;
		uint32_t level;
		std::string message;
	};

	thread_local std::size_t g_operationDepth = 0;
	thread_local std::vector<PendingLog> g_pendingLogs;
	thread_local bool g_dispatchingLogs = false;

	class OperationLogScope
	{
	public:
		OperationLogScope() noexcept { ++g_operationDepth; }
		~OperationLogScope() noexcept
		{
			if (--g_operationDepth != 0)
				return;

			std::vector<PendingLog> pending;
			pending.swap(g_pendingLogs);
			// A logger may call back into ShareBlocks and produce another error.
			// Drop those nested records instead of recursively invoking the logger.
			if (g_dispatchingLogs)
				return;
			g_dispatchingLogs = true;
			for (const PendingLog& item : pending)
			{
				if (item.logger == nullptr)
					continue;
				try { item.logger(item.level, item.message.c_str()); }
				catch (...) {}
			}
			g_dispatchingLogs = false;
		}
	};

	std::recursive_mutex& shareblocks_mutex()
	{
		static std::recursive_mutex mutex;
		return mutex;
	}

	struct FileIdentity
	{
		uint64_t volume = 0;
		uint64_t file = 0;

		bool operator<(const FileIdentity& other) const noexcept
		{
			return volume < other.volume || (volume == other.volume && file < other.file);
		}

		bool operator==(const FileIdentity& other) const noexcept
		{
			return volume == other.volume && file == other.file;
		}
	};

	uint64_t current_process_id() noexcept
	{
#ifdef _WIN32
		return static_cast<uint64_t>(::_getpid());
#else
		return static_cast<uint64_t>(::getpid());
#endif
	}

	bool mapped_file_identity(const BoostMappingFile* mapping, FileIdentity& identity) noexcept
	{
		if (mapping == nullptr)
			return false;
		return mapping->get_file_identity(identity.volume, identity.file);
	}

	class SharedFileLock
	{
	public:
		SharedFileLock() noexcept = default;
		explicit SharedFileLock(const char* filename, bool tryOnly = false) noexcept
		{
			acquire_path(filename, tryOnly, nullptr);
		}
		explicit SharedFileLock(const std::filesystem::path& filename, bool tryOnly = false)
		{
			acquire_path(filename.c_str(), tryOnly, nullptr);
		}
		explicit SharedFileLock(BoostMappingFile* mapping, bool tryOnly = false) noexcept
		{
			FileIdentity expected;
			if (mapped_file_identity(mapping, expected))
				acquire_path(mapping->filename(), tryOnly, &expected);
		}
		~SharedFileLock() { release(); }

		SharedFileLock(const SharedFileLock&) = delete;
		SharedFileLock& operator=(const SharedFileLock&) = delete;

		bool acquire(BoostMappingFile* mapping, bool tryOnly = false) noexcept
		{
			if (_locked)
				return true;
			FileIdentity expected;
			if (mapping == nullptr || !mapped_file_identity(mapping, expected))
				return false;
			return acquire_path(mapping->filename(), tryOnly, &expected);
		}

		bool locked() const noexcept { return _locked; }

		// flock locks are attached to the open-file description. After fork, the
		// child must close its inherited descriptor without issuing LOCK_UN, which
		// would release the parent's lease as well.
		void discard_inherited_lock() noexcept
		{
#ifndef _WIN32
			_locked = false;
			if (_fd >= 0)
			{
				int rc;
				do { rc = ::close(_fd); } while (rc != 0 && errno == EINTR);
				_fd = -1;
			}
#endif
		}

	private:
		template <typename PathChar>
		bool acquire_path(const PathChar* filename, bool tryOnly, const FileIdentity* expected) noexcept
		{
			if (_locked || filename == nullptr || filename[0] == PathChar{})
				return _locked;

#ifdef _WIN32
			const DWORD shareMode = expected == nullptr
				? (FILE_SHARE_READ | FILE_SHARE_WRITE)
				: (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
			const DWORD creationDisposition = expected == nullptr ? OPEN_ALWAYS : OPEN_EXISTING;
			const DWORD attributes = FILE_ATTRIBUTE_NORMAL |
				(expected == nullptr ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
			if constexpr (std::is_same_v<PathChar, wchar_t>)
				_handle = ::CreateFileW(filename, GENERIC_READ | GENERIC_WRITE,
					shareMode, nullptr, creationDisposition, attributes, nullptr);
			else
				_handle = ::CreateFileA(filename, GENERIC_READ | GENERIC_WRITE,
					shareMode, nullptr, creationDisposition, attributes, nullptr);
			if (_handle == INVALID_HANDLE_VALUE)
				return false;

			BY_HANDLE_FILE_INFORMATION info{};
			if (!::GetFileInformationByHandle(_handle, &info))
			{
				release();
				return false;
			}
			if (expected == nullptr &&
				((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
					info.nNumberOfLinks != 1))
			{
				release();
				return false;
			}
			const FileIdentity actual{
				static_cast<uint64_t>(info.dwVolumeSerialNumber),
				(static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow };
			if (expected != nullptr && !(actual == *expected))
			{
				release();
				return false;
			}

			_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (_event == nullptr)
			{
				release();
				return false;
			}
			_overlapped = OVERLAPPED{};
			_overlapped.hEvent = _event;
			DWORD flags = LOCKFILE_EXCLUSIVE_LOCK;
			if (tryOnly)
				flags |= LOCKFILE_FAIL_IMMEDIATELY;
			BOOL acquired = ::LockFileEx(_handle, flags, 0, 1, 0, &_overlapped);
			if (!acquired && !tryOnly && ::GetLastError() == ERROR_IO_PENDING)
			{
				if (::WaitForSingleObject(_event, INFINITE) == WAIT_OBJECT_0)
				{
					DWORD transferred = 0;
					acquired = ::GetOverlappedResult(_handle, &_overlapped, &transferred, FALSE);
				}
			}
			_locked = acquired != FALSE;
			if (!_locked)
				release();
#else
			int flags = O_RDWR;
			if (expected == nullptr)
				flags |= O_CREAT;
#ifdef O_CLOEXEC
			flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
			flags |= O_NOFOLLOW;
#endif
			_fd = ::open(filename, flags, S_IRUSR | S_IWUSR);
			if (_fd < 0)
				return false;

			struct stat info{};
			if (::fstat(_fd, &info) != 0)
			{
				release();
				return false;
			}
			const FileIdentity actual{ static_cast<uint64_t>(info.st_dev), static_cast<uint64_t>(info.st_ino) };
			if (expected == nullptr)
			{
				if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || info.st_nlink != 1 ||
					::fchmod(_fd, S_IRUSR | S_IWUSR) != 0)
				{
					release();
					return false;
				}
			}
			if (expected != nullptr && !(actual == *expected))
			{
				release();
				return false;
			}

			int operation = LOCK_EX;
			if (tryOnly)
				operation |= LOCK_NB;
			int rc;
			do { rc = ::flock(_fd, operation); } while (rc != 0 && errno == EINTR && !tryOnly);
			_locked = rc == 0;
			if (!_locked)
				release();
#endif
			return _locked;
		}

		void release() noexcept
		{
#ifdef _WIN32
			if (_locked && _handle != INVALID_HANDLE_VALUE)
				(void)::UnlockFileEx(_handle, 0, 1, 0, &_overlapped);
			_locked = false;
			if (_event != nullptr)
			{
				::CloseHandle(_event);
				_event = nullptr;
			}
			if (_handle != INVALID_HANDLE_VALUE)
			{
				::CloseHandle(_handle);
				_handle = INVALID_HANDLE_VALUE;
			}
#else
			if (_locked && _fd >= 0)
			{
				int rc;
				do { rc = ::flock(_fd, LOCK_UN); } while (rc != 0 && errno == EINTR);
			}
			_locked = false;
			if (_fd >= 0)
			{
				::close(_fd);
				_fd = -1;
			}
#endif
		}

#ifdef _WIN32
		HANDLE _handle = INVALID_HANDLE_VALUE;
		HANDLE _event = nullptr;
		OVERLAPPED _overlapped{};
#else
		int _fd = -1;
#endif
		bool _locked = false;
	};

	struct MasterLease
	{
		MappedFilePtr _mapping;
		std::unique_ptr<SharedFileLock> _lock;
	};

	struct MasterLeaseRegistry
	{
		uint64_t _processId = current_process_id();
		std::map<FileIdentity, MasterLease> _leases;
	};

	MasterLeaseRegistry& master_lease_registry()
	{
		static MasterLeaseRegistry registry;
		return registry;
	}

#ifndef _WIN32
	void discard_forked_master_lock_descriptors() noexcept
	{
		// This handler only closes inherited descriptors. It does not unlock them,
		// mutate the registry, or allocate; the parent retains its own fd copy.
		MasterLeaseRegistry& registry = master_lease_registry();
		for (auto& entry : registry._leases)
		{
			if (entry.second._lock)
				entry.second._lock->discard_inherited_lock();
		}
	}
#endif

	bool install_fork_handler()
	{
#ifndef _WIN32
		(void)master_lease_registry();
		static std::once_flag registered;
		static bool registrationSucceeded = false;
		std::call_once(registered, [] {
			registrationSucceeded = ::pthread_atfork(nullptr, nullptr,
				discard_forked_master_lock_descriptors) == 0;
		});
		return registrationSucceeded;
#else
		return true;
#endif
	}

	void reset_inherited_master_leases()
	{
		MasterLeaseRegistry& registry = master_lease_registry();
		const uint64_t processId = current_process_id();
		if (registry._processId != processId)
		{
			// Be safe even if a platform fork hook did not run: close the child's
			// inherited descriptor without LOCK_UN before destroying registry nodes.
			for (auto& entry : registry._leases)
			{
				if (entry.second._lock)
					entry.second._lock->discard_inherited_lock();
			}
			registry._leases.clear();
			registry._processId = processId;
		}
	}

	class ShareBlocksLockGuard
	{
	public:
		ShareBlocksLockGuard() : _guard(shareblocks_mutex())	{ reset_inherited_master_leases(); }
		ShareBlocksLockGuard(const ShareBlocksLockGuard&) = delete;
		ShareBlocksLockGuard& operator=(const ShareBlocksLockGuard&) = delete;

	private:
		std::lock_guard<std::recursive_mutex> _guard;
	};

	std::filesystem::path master_lease_path(const FileIdentity& identity)
	{
#ifndef _WIN32
		std::error_code ec;
		std::filesystem::path directory = std::filesystem::weakly_canonical("/tmp", ec);
		if (ec)
			return std::filesystem::path();
		directory /= std::string("wondertrader-shareblocks-") + std::to_string(static_cast<uint64_t>(::geteuid()));
		if (::mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST)
			return std::filesystem::path();
#else
		PWSTR localAppData = nullptr;
		if (FAILED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) ||
			localAppData == nullptr)
			return std::filesystem::path();
		std::filesystem::path directory(localAppData);
		::CoTaskMemFree(localAppData);
		directory /= L"WonderTrader";
		directory /= L"ShareBlocks";
		std::error_code ec;
		std::filesystem::create_directories(directory, ec);
		if (ec)
		{
			ec.clear();
			if (!std::filesystem::is_directory(directory, ec) || ec)
				return std::filesystem::path();
		}
#endif
#ifndef _WIN32
		struct stat dirInfo{};
		if (::lstat(directory.c_str(), &dirInfo) != 0 || !S_ISDIR(dirInfo.st_mode) ||
			dirInfo.st_uid != ::geteuid() || (dirInfo.st_mode & 0077) != 0)
			return std::filesystem::path();
#else
		const DWORD attributes = ::GetFileAttributesW(directory.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
			(attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
			return std::filesystem::path();
#endif
		return directory / (std::string("master-") + std::to_string(identity.volume) + "-" +
			std::to_string(identity.file) + ".lock");
	}

	bool owns_master_lease(const BoostMappingFile* mapping)
	{
		reset_inherited_master_leases();
		FileIdentity identity;
		if (!mapped_file_identity(mapping, identity))
			return false;
		const MasterLeaseRegistry& registry = master_lease_registry();
		return registry._leases.find(identity) != registry._leases.end();
	}

	bool is_current_master(bool mappedAsMaster, BoostMappingFile* mapping)
	{
		return mappedAsMaster && owns_master_lease(mapping);
	}

	std::unique_ptr<SharedFileLock> acquire_master_lease(const FileIdentity& identity)
	{
		if (!install_fork_handler())
			return nullptr;
		reset_inherited_master_leases();
		MasterLeaseRegistry& registry = master_lease_registry();
		if (registry._leases.find(identity) != registry._leases.end())
			return nullptr;

		const std::filesystem::path path = master_lease_path(identity);
		if (path.empty())
			return nullptr;
		std::unique_ptr<SharedFileLock> lock(new SharedFileLock(path, true));
		if (!lock->locked())
			return nullptr;
		return lock;
	}

	bool register_master_lease(const MappedFilePtr& mapping, std::unique_ptr<SharedFileLock>& lock)
	{
		reset_inherited_master_leases();
		FileIdentity identity;
		if (!mapped_file_identity(mapping.get(), identity) || !lock || !lock->locked())
			return false;

		MasterLease lease;
		lease._mapping = mapping;
		lease._lock = std::move(lock);
		return master_lease_registry()._leases.try_emplace(identity, std::move(lease)).second;
	}

	std::string canonical_file_path(const char* input)
	{
		if (input == nullptr || input[0] == '\0')
			return std::string();
		std::error_code ec;
		std::filesystem::path absolute = std::filesystem::absolute(input, ec);
		if (ec)
			return std::string();
		std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
		if (ec)
			return std::string();
		return canonical.string();
	}

	bool create_shared_file_if_missing(const std::string& filename)
	{
		if (StdFile::exists(filename.c_str()))
			return true;
		auto handle = boost::interprocess::ipcdetail::create_new_file(filename.c_str(),
			boost::interprocess::read_write, boost::interprocess::permissions());
		if (handle == boost::interprocess::ipcdetail::invalid_file())
			return StdFile::exists(filename.c_str());
		const bool resized = boost::interprocess::ipcdetail::truncate_file(handle, sizeof(ShmBlock));
		boost::interprocess::ipcdetail::close_file(handle);
		if (!resized)
		{
			(void)boost::interprocess::ipcdetail::delete_file(filename.c_str());
			return false;
		}
		return true;
	}

	bool next_block_version(uint64_t current, uint64_t& next) noexcept
	{
		if (current == (std::numeric_limits<uint64_t>::max)())
			return false;

		const uint64_t wallClock = static_cast<uint64_t>(TimeUtils::getLocalTimeNow());
		next = (std::max)(wallClock, current + 1);
		return true;
	}

	template <typename Map>
	constexpr bool unordered_dense_swap_components_are_nothrow()
	{
		// The vendored table::swap noexcept expression wraps trait values in
		// noexcept(...), which is always true. Check the concrete wrapper and the
		// actual value/hash/equal/allocator components used by its std::swap path.
		using ValueContainer = typename Map::value_container_type;
		using Hash = typename Map::hasher;
		using KeyEqual = typename Map::key_equal;
		using Allocator = typename Map::allocator_type;
		using BucketAllocator = typename std::allocator_traits<Allocator>::template rebind_alloc<typename Map::bucket_type>;

		return std::is_nothrow_move_constructible_v<Map> &&
			std::is_nothrow_move_assignable_v<Map> &&
			std::is_nothrow_swappable_v<ValueContainer> &&
			std::is_nothrow_move_constructible_v<ValueContainer> &&
			std::is_nothrow_move_assignable_v<ValueContainer> &&
			std::is_nothrow_swappable_v<Hash> &&
			std::is_nothrow_move_constructible_v<Hash> &&
			std::is_nothrow_move_assignable_v<Hash> &&
			std::is_nothrow_default_constructible_v<Hash> &&
			std::is_nothrow_swappable_v<KeyEqual> &&
			std::is_nothrow_move_constructible_v<KeyEqual> &&
			std::is_nothrow_move_assignable_v<KeyEqual> &&
			std::is_nothrow_default_constructible_v<KeyEqual> &&
			std::allocator_traits<Allocator>::is_always_equal::value &&
			std::is_nothrow_swappable_v<Allocator> &&
			std::allocator_traits<BucketAllocator>::is_always_equal::value &&
			std::is_nothrow_swappable_v<BucketAllocator>;
	}

	bool get_string_length(const char* value, std::size_t capacity, std::size_t& length)
	{
		if (value == nullptr || capacity == 0)
			return false;

		length = 0;
		while (length < capacity && value[length] != '\0')
			++length;
		return length < capacity;
	}

}

shareblock::detail::Logger shareblock::detail::snapshot_logger(Logger* target) noexcept
{
	if (target == nullptr)
		return nullptr;
	ShareBlocksLockGuard localGuard;
	return *target;
}

void shareblock::detail::update_logger(Logger* target, Logger logger) noexcept
{
	if (target == nullptr)
		return;
	ShareBlocksLockGuard localGuard;
	*target = logger;
}

void shareblock::detail::dispatch_log(Logger logger, uint32_t level, const char* message) noexcept
{
	if (logger == nullptr || message == nullptr)
		return;

	if (g_operationDepth != 0)
	{
		try
		{
			g_pendingLogs.push_back({ logger, level, message });
		}
		catch (...)
		{
			// Logging must not invoke a user callback while an operation still owns
			// the local mutex or backing-file lock.
		}
		return;
	}
	if (g_dispatchingLogs)
		return;

	g_dispatchingLogs = true;
	try { logger(level, message); }
	catch (...) {}
	g_dispatchingLogs = false;
}

#define SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domainName, domainIt, fileGuard, failureValue) \
	auto domainIt = _shm_blocks.find(domainName); \
	if (domainIt == _shm_blocks.end() || domainIt->second._block == nullptr || !domainIt->second._domain) \
		return failureValue; \
	SharedFileLock fileGuard(domainIt->second._domain.get()); \
	if (!fileGuard.locked()) \
	{ \
		write_log(LL_ERROR, "Unable to lock shared domain {}", domainName); \
		return failureValue; \
	}

#define SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domainName, domainIt, fileGuard, failureValue) \
	auto domainIt = _shm_blocks.find(domainName); \
	SharedFileLock fileGuard; \
	if (domainIt != _shm_blocks.end() && domainIt->second._block != nullptr && domainIt->second._domain && \
		!fileGuard.acquire(domainIt->second._domain.get())) \
	{ \
		write_log(LL_ERROR, "Unable to lock shared domain {}", domainName); \
		return failureValue; \
	}

bool ShareBlocks::init_master(const char* name, const char* path/* = ""*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	auto existing = _shm_blocks.find(name);
	if (existing != _shm_blocks.end() && existing->second._block != NULL)
		return existing->second._master && owns_master_lease(existing->second._domain.get());

	const std::string input = (path != nullptr && path[0] != '\0') ? path : (name == nullptr ? "" : name);
	const std::string filename = canonical_file_path(input.c_str());
	if (filename.empty())
	{
		write_log(LL_ERROR, "Unable to resolve shared domain path for {}", name);
		return false;
	}

	// Serialize first creation and mapping by canonical path. The lock file is
	// never truncated; the data file is resized only by its exclusive creator.
	const std::string initLockPath = filename + ".wtshareblocks.init.lock";
	SharedFileLock initGuard(initLockPath.c_str());
	if (!initGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain initialization {}", name);
		return false;
	}
	if (!create_shared_file_if_missing(filename))
	{
		write_log(LL_ERROR, "Unable to create shared domain file {}", filename);
		return false;
	}

	MappedFilePtr mapped;
	try
	{
		mapped.reset(new BoostMappingFile);
		if (!mapped->map(filename.c_str()) || mapped->size() < sizeof(ShmBlock))
		{
			write_log(LL_ERROR, "Unable to map a complete shared domain {}", name);
			return false;
		}
	}
	catch (...)
	{
		write_log(LL_ERROR, "Unable to map shared domain {}", name);
		return false;
	}
	FileIdentity identity;
	if (!mapped_file_identity(mapped.get(), identity))
	{
		write_log(LL_ERROR, "Unable to identify mapped shared domain {}", name);
		return false;
	}

	// This path+identity check makes replaced paths fail closed. The freshly
	// opened operation lock is bound to the mapped inode/file identity.
	SharedFileLock fileGuard(mapped.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", name);
		return false;
	}

	// The OS lease is keyed by actual file identity, so hard links and aliases
	// converge on one master regardless of the path used to open the mapping.
	auto masterLease = acquire_master_lease(identity);
	if (!masterLease)
	{
		write_log(LL_ERROR, "Unable to acquire shared domain master lease {}", name);
		return false;
	}
	ShmBlock* block = (ShmBlock*)mapped->addr();
	if (block->_count > MAX_SEC_CNT)
	{
		write_log(LL_ERROR, "Shared domain {} has an invalid section count", name);
		return false;
	}

	for (const auto& entry : _shm_blocks)
	{
		if (entry.first == name)
			continue;
		const ShmPair& candidate = entry.second;
		FileIdentity candidateIdentity;
		if (candidate._master && candidate._block != nullptr && candidate._domain &&
			owns_master_lease(candidate._domain.get()) &&
			mapped_file_identity(candidate._domain.get(), candidateIdentity) && candidateIdentity == identity)
		{
			write_log(LL_ERROR, "Domain {} already has a master for backing file {}", name, filename);
			return false;
		}
	}

	/*
	 *	By Wesley @ 2023.09.20
	 *	这里要做一个清理，如果state为0，则不再保留
	 */
	std::vector<SecInfo> aySecs;
	aySecs.reserve(block->_count);
	for (uint32_t i = 0; i < block->_count; i++)
	{
		SecInfo& secInfo = block->_sections[i];
		if (secInfo._count == 0 || secInfo._state != 1)
			continue;

		aySecs.emplace_back(secInfo);
	}

	const bool compact = aySecs.size() != block->_count;
	bool deactivate = false;
	for (const SecInfo& secInfo : aySecs)
	{
		if (secInfo._state != 0)
		{
			deactivate = true;
			break;
		}
	}
	const bool publishInitialization = compact || deactivate;
	uint64_t initializedVersion = block->_updatetime;
	if (publishInitialization && !next_block_version(block->_updatetime, initializedVersion))
	{
		write_log(LL_ERROR, "Shared domain {} version is exhausted", name);
		return false;
	}

	// Stage all potentially allocating index work before changing shared metadata.
	ShmPair::SectionMap stagedSections;
	static_assert(unordered_dense_swap_components_are_nothrow<ShmPair::SectionMap>(),
		"section map swap must not throw after shared block publication");
	for (std::size_t i = 0; i < aySecs.size(); ++i)
	{
		const SecInfo& source = aySecs[i];
		if (source._count > MAX_KEY_CNT || memchr(source._name, '\0', sizeof(source._name)) == nullptr)
		{
			write_log(LL_ERROR, "Shared domain {} contains malformed section metadata", name);
			return false;
		}

		ShmPair::KVPair kvPair;
		kvPair._index = static_cast<uint32_t>(i);
		for (uint32_t j = 0; j < source._count; ++j)
		{
			const KeyInfo& sourceKey = source._keys[j];
			if (memchr(sourceKey._key, '\0', sizeof(sourceKey._key)) == nullptr)
			{
				write_log(LL_ERROR, "Shared domain {} contains malformed key metadata", name);
				return false;
			}
			if (!kvPair._keys.try_emplace(sourceKey._key, &block->_sections[i]._keys[j]).second)
			{
				write_log(LL_ERROR, "Shared domain {} contains duplicate keys", name);
				return false;
			}
		}
		if (!stagedSections.try_emplace(source._name, std::move(kvPair)).second)
		{
			write_log(LL_ERROR, "Shared domain {} contains duplicate section names", name);
			return false;
		}
	}

	// Reserve the process-local domain entry before publishing any compaction or
	// state changes into the shared block. If this allocation throws, the mapped
	// file is still untouched and init_master can be retried safely.
	auto shmIt = _shm_blocks.find(name);
	if (shmIt == _shm_blocks.end())
		shmIt = _shm_blocks.try_emplace(name).first;
	ShmPair& shm = shmIt->second;

	// Insert the lease before touching shared metadata. If registry allocation
	// fails, the mapped bytes are unchanged and the caller may retry.
	if (!register_master_lease(mapped, masterLease))
	{
		write_log(LL_ERROR, "Unable to register shared domain master lease {}", name);
		return false;
	}

	if (compact)
	{
		write_log(LL_INFO, "Section count of domain {} changed: {} -> {}，cache resized",
			name, block->_count, aySecs.size());
		block->_count = static_cast<uint32_t>(aySecs.size());
		memset(block->_sections, 0, sizeof(SecInfo) * MAX_SEC_CNT);
		if (block->_count > 0)
			memcpy(block->_sections, aySecs.data(), sizeof(SecInfo) * block->_count);
	}

	for (uint32_t i = 0; i < block->_count; ++i)
		block->_sections[i]._state = 0;
	if (publishInitialization)
		block->_updatetime = initializedVersion;

	shm._domain = std::move(mapped);
	shm._master = true;
	shm._block = block;
	shm._blocktime = block->_updatetime;
	shm._sections.swap(stagedSections);

	return true;
}

bool ShareBlocks::init_slave(const char* name, const char* path/* = ""*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	ShmPair& shm = (ShmPair&)_shm_blocks[name];
	if (shm._block != NULL)
	{
		if (!shm._master || owns_master_lease(shm._domain.get()))
			return true;

		// A forked child inherited the parent's local mapping but not master
		// authority. Rebuild this copy as a slave before reporting initialization.
		SharedFileLock inheritedMappingLock(shm._domain.get());
		if (!inheritedMappingLock.locked())
			return false;
		ShmPair staged;
		staged._domain = shm._domain;
		staged._block = shm._block;
		staged._master = false;
		if (!update_slave_locked(name, staged, true))
			return false;
		shm._master = false;
		shm._blocktime = staged._blocktime;
		shm._sections.swap(staged._sections);
		return true;
	}

	const std::string input = (path != nullptr && path[0] != '\0') ? path : (name == nullptr ? "" : name);
	const std::string filename = canonical_file_path(input.c_str());
	if (filename.empty())
		return false;

	const std::string initLockPath = filename + ".wtshareblocks.init.lock";
	SharedFileLock initGuard(initLockPath.c_str());
	if (!initGuard.locked())
		return false;

	if (!StdFile::exists(filename.c_str()))
		return false;

	MappedFilePtr mapped;
	try
	{
		mapped.reset(new BoostMappingFile);
		if (!mapped->map(filename.c_str()) || mapped->size() < sizeof(ShmBlock))
		{
			write_log(LL_ERROR, "Unable to map a complete shared domain {}", name);
			return false;
		}
	}
	catch (...)
	{
		write_log(LL_ERROR, "Unable to map shared domain {}", name);
		return false;
	}

	SharedFileLock fileGuard(mapped.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", name);
		return false;
	}

	ShmPair staged;
	staged._domain = mapped;
	staged._block = (ShmBlock*)mapped->addr();
	staged._master = false;
	if (staged._block->_count > MAX_SEC_CNT || !update_slave_locked(name, staged, true))
	{
		write_log(LL_ERROR, "Unable to load shared domain {}", name);
		return false;
	}

	shm._domain = std::move(staged._domain);
	shm._master = false;
	shm._block = staged._block;
	shm._blocktime = staged._blocktime;
	shm._sections.swap(staged._sections);

	write_log(LL_INFO, "Initialize domain {} succeed", name);
	return true;
}

bool ShareBlocks::update_slave(const char* name, bool bForce)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	ShmPair& shm = (ShmPair&)_shm_blocks[name];
	if (shm._block == NULL)
		return false;

	SharedFileLock fileGuard(shm._domain.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", name);
		return false;
	}
	return update_slave_locked(name, shm, bForce);
}


bool ShareBlocks::update_slave_locked(const char* name, ShmPair& shm, bool bForce)
{
	if (shm._block == nullptr || is_current_master(shm._master, shm._domain.get()) ||
		shm._block->_count > MAX_SEC_CNT)
		return false;

	const uint64_t publishedVersion = shm._block->_updatetime;
	if (!bForce && shm._blocktime == publishedVersion)
		return false;

	ShmPair::SectionMap stagedSections;
	static_assert(unordered_dense_swap_components_are_nothrow<ShmPair::SectionMap>(),
		"section map swap must not throw after shared block publication");
	try
	{
		for (uint32_t i = 0; i < shm._block->_count; ++i)
		{
			SecInfo& secInfo = shm._block->_sections[i];
			if (secInfo._count == 0 || secInfo._state != 1)
				continue;
			if (secInfo._count > MAX_KEY_CNT || memchr(secInfo._name, '\0', sizeof(secInfo._name)) == nullptr)
				return false;

			ShmPair::KVPair kvPair;
			kvPair._index = i;
			for (uint32_t j = 0; j < secInfo._count; ++j)
			{
				KeyInfo& key = secInfo._keys[j];
				if (memchr(key._key, '\0', sizeof(key._key)) == nullptr ||
					!kvPair._keys.try_emplace(key._key, &key).second)
					return false;
			}
			if (!stagedSections.try_emplace(secInfo._name, std::move(kvPair)).second)
				return false;
		}
	}
	catch (...)
	{
		write_log(LL_ERROR, "Unable to rebuild shared domain {} index", name);
		return false;
	}

	shm._sections.swap(stagedSections);
	shm._blocktime = publishedVersion;

	write_log(LL_INFO, "Domain {} reloaded", name);
	return true;
}

bool ShareBlocks::release_slave(const char* name)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	auto it = _shm_blocks.find(name);
	if (it == _shm_blocks.end())
		return true;

	ShmPair& shm = it->second;

	//只有slave需要释放
	if (is_current_master(shm._master, shm._domain.get()))
		return false;

	shm._block = NULL;
	shm._sections.clear();
	shm._domain.reset();
	shm._blocktime = 0;

	_shm_blocks.erase(it);
	return true;
}

uint64_t ShareBlocks::get_section_updatetime(const char* domain, const char* section)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainIt, fileGuard, 0);
	auto sit = domainIt->second._sections.find(section);
	if (sit == domainIt->second._sections.end())
		return 0;

	const ShmPair& shm = domainIt->second;
	const uint32_t index = sit->second._index;
	if (index >= MAX_SEC_CNT || index >= shm._block->_count)
		return 0;
	const SecInfo& secInfo = shm._block->_sections[index];
	if (secInfo._state == 2 || std::strcmp(secInfo._name, section) != 0)
		return 0;
	return secInfo._updatetime;
}

bool ShareBlocks::commit_section(const char* domain, const char* section)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	auto domainIt = _shm_blocks.find(domain);
	if (domainIt == _shm_blocks.end() || domainIt->second._block == nullptr || !domainIt->second._domain)
	{
		write_log(LL_ERROR, "Domain {} not initialized", domain);
		return false;
	}
	SharedFileLock fileGuard(domainIt->second._domain.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", domain);
		return false;
	}
	ShmPair& shm = (ShmPair&)domainIt->second;
	if (!is_current_master(shm._master, shm._domain.get()))
	{
		write_log(LL_ERROR, "Domain {} is not in master mode", domain);
		return false;
	}
	auto sit = shm._sections.find(section);
	if (sit == shm._sections.end())
	{
		write_log(LL_ERROR, "Secion {}.{} not exists", domain, section);
		return false;
	}

	ShmPair::KVPair& kvPair = (ShmPair::KVPair&)sit->second;
	if (kvPair._index >= MAX_SEC_CNT || kvPair._index >= shm._block->_count ||
		shm._block->_sections[kvPair._index]._state == 2)
		return false;
	uint64_t publishedVersion = 0;
	if (!next_block_version(shm._block->_updatetime, publishedVersion))
	{
		write_log(LL_ERROR, "Shared domain {} version is exhausted", domain);
		return false;
	}
	SecInfo& secInfo = shm._block->_sections[kvPair._index];
	secInfo._updatetime = TimeUtils::getLocalTimeNow();
	shm._block->_updatetime = publishedVersion;

	write_log(LL_INFO, "Timestamp of section {}.{} updated", domain, section);
	return true;
}

bool ShareBlocks::delete_section(const char* domain, const char*section)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	auto domainIt = _shm_blocks.find(domain);
	if (domainIt == _shm_blocks.end() || domainIt->second._block == nullptr || !domainIt->second._domain)
	{
		write_log(LL_ERROR, "Domain {} not initialized", domain);
		return false;
	}
	SharedFileLock fileGuard(domainIt->second._domain.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", domain);
		return false;
	}
	ShmPair& shm = (ShmPair&)domainIt->second;
	if (!is_current_master(shm._master, shm._domain.get()))
	{
		write_log(LL_ERROR, "Domain {} is not in master mode", domain);
		return false;
	}
	auto sit = shm._sections.find(section);
	if (sit == shm._sections.end())
	{
		write_log(LL_WARN, "Secion {}.{} not exists", domain, section);
		return true;
	}

	uint32_t idx = sit->second._index;
	if (idx >= MAX_SEC_CNT || idx >= shm._block->_count)
		return false;
	uint64_t publishedVersion = 0;
	if (!next_block_version(shm._block->_updatetime, publishedVersion))
	{
		write_log(LL_ERROR, "Shared domain {} version is exhausted", domain);
		return false;
	}
	shm._block->_sections[idx]._state = 2;
	shm._block->_updatetime = publishedVersion;
	shm._sections.erase(sit);
	write_log(LL_INFO, "Section {}.{} marked as removed", domain, section);
	return true;
}

void* ShareBlocks::make_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)
{
	secInfo = nullptr;
	auto it = _shm_blocks.find(domain);
	if (it == _shm_blocks.end())
		return nullptr;

	if (section == NULL)
	{
		write_log(LL_ERROR, "Section name in domain {} is null", domain);
		return nullptr;
	}

	const std::size_t sectionNameSize = sizeof(((SecInfo*)nullptr)->_name);
	std::size_t sectionNameLen = 0;
	while (sectionNameLen < sectionNameSize && section[sectionNameLen] != '\0')
		sectionNameLen++;
	if (sectionNameLen == sectionNameSize)
	{
		write_log(LL_ERROR, "Section name in domain {} exceeds {} bytes", domain, sectionNameSize - 1);
		return nullptr;
	}

	const std::size_t keySize = sizeof(((KeyInfo*)nullptr)->_key);
	std::size_t keyLen = 0;
	if (key == nullptr)
	{
		write_log(LL_ERROR, "Key name in {}.{} is null or exceeds {} bytes", domain, section, keySize - 1);
		return nullptr;
	}
	while (keyLen < keySize && key[keyLen] != '\0')
		++keyLen;
	if (keyLen == keySize)
	{
		write_log(LL_ERROR, "Key name in {}.{} is null or exceeds {} bytes", domain, section, keySize - 1);
		return nullptr;
	}

	std::size_t len = SMVT_SIZES[vType];

	ShmPair& shm = (ShmPair&)it->second;
	if (shm._block == nullptr)
	{
		write_log(LL_ERROR, "Domain {} is not mapped", domain);
		return nullptr;
	}
	if (shm._master && !owns_master_lease(shm._domain.get()))
	{
		write_log(LL_ERROR, "Domain {} master lease is not owned by this process", domain);
		return nullptr;
	}

	const std::size_t dataSize = sizeof(((SecInfo*)nullptr)->_data);
	if (len > dataSize)
	{
		write_log(LL_ERROR, "Value type in {}.{} exceeds section data capacity", domain, section);
		return nullptr;
	}

	auto sit = shm._sections.find(section);
	const bool newSection = sit == shm._sections.end();
	ShmPair::KVPair* kvPair = nullptr;
	SecInfo* targetSection = nullptr;
	if (newSection)
	{
		//如果不是master，就不能创建
		if (!shm._master)
		{
			write_log(LL_ERROR, "Domain {} is not in master mode，cannot add new section", domain);
			return nullptr;
		}

		if (shm._block->_count >= MAX_SEC_CNT)
		{
			//已经没有额外的空间可以分配了
			write_log(LL_ERROR, "No spare slot for new section in domain {}", domain);
			return nullptr;
		}
		// The unused section starts at offset zero; len was already bounded by dataSize.
	}
	else
	{
		kvPair = (ShmPair::KVPair*)&sit->second;
		if (kvPair->_index >= MAX_SEC_CNT || kvPair->_index >= shm._block->_count)
		{
			write_log(LL_ERROR, "Section index for {}.{} is outside the shared block", domain, section);
			return nullptr;
		}
		targetSection = &shm._block->_sections[kvPair->_index];

		if (strcmp(targetSection->_name, section) != 0)
		{
			/*
			 *	By Wesley @ 2024.04.10
			 *	如果之前存的索引位置的section和目标section名字不同
			 *	说明master那边做了修改，需要强制update才行
			 */
			write_log(LL_ERROR, "Cache zone {}.{} overwrote, cannot be modified", domain, section);
			return nullptr;
		}
		// A slave may still hold the pre-delete key index until its next refresh.
		// Never let an allocate/set call through that stale index reactivate a tombstone.
		if (targetSection->_state == 2)
		{
			write_log(LL_ERROR, "Section {}.{} is deleted", domain, section);
			return nullptr;
		}

		auto kit = kvPair->_keys.find(key);
		if (kit != kvPair->_keys.end())
		{
			secInfo = targetSection;
			secInfo->_state = 1;
			return kit->second;
		}
	}

	// All checks that can reject a new key happen before changing the section state,
	// key count, offset, section count, or either lookup map.
	if (!shm._master)
	{
		write_log(LL_ERROR, "Domnain {} is not in master mode，cannot create kv pair", domain);
		return nullptr;
	}

	if (targetSection != nullptr && targetSection->_count >= MAX_KEY_CNT)
	{
		write_log(LL_ERROR, "Secion {}.{} has no spare slot for new kv pair", domain, section);
		return nullptr;
	}

	if (targetSection != nullptr && targetSection->_offset > dataSize - len)
	{
		write_log(LL_ERROR, "Secion {}.{} has no enough bytes for new kv pair", domain, section);
		return nullptr;
	}

	const uint32_t sectionIndex = newSection ? shm._block->_count : kvPair->_index;
	if (newSection)
		targetSection = &shm._block->_sections[sectionIndex];
	const uint16_t keyIndex = newSection ? 0 : targetSection->_count;
	KeyInfo* keyInfo = &targetSection->_keys[keyIndex];
	const uint64_t sectionUpdateTime = newSection ? TimeUtils::getLocalTimeNow() : 0;
	const uint64_t keyUpdateTime = TimeUtils::getLocalTimeNow();

	// Build the private lookup changes off to the side. unordered_dense may
	// reallocate its bucket storage while inserting, so a failed allocation must
	// never leave a half-inserted entry in the live maps or a published block slot.
	ShmPair::SectionMap stagedSections;
	ShmPair::KVMap stagedKeys;
	static_assert(unordered_dense_swap_components_are_nothrow<ShmPair::SectionMap>(),
		"section map swap requires nothrow value/hash/equal and always-equal allocators");
	static_assert(unordered_dense_swap_components_are_nothrow<ShmPair::KVMap>(),
		"key map swap requires nothrow value/hash/equal and always-equal allocators");
	if (newSection)
	{
		ShmPair::KVPair stagedPair;
		stagedPair._index = sectionIndex;
		stagedPair._keys.try_emplace(key, keyInfo);

		stagedSections = shm._sections;
		auto inserted = stagedSections.try_emplace(section, std::move(stagedPair));
		if (!inserted.second)
		{
			write_log(LL_ERROR, "Section {} already exists in the private map", section);
			return nullptr;
		}
	}
	else
	{
		stagedKeys = kvPair->_keys;
		auto inserted = stagedKeys.try_emplace(key, keyInfo);
		if (!inserted.second)
		{
			write_log(LL_ERROR, "Key {}.{}.{} already exists in the private map", domain, section, key);
			return nullptr;
		}
	}

	// Every potentially allocating map operation completed above. From here to
	// publication, only fixed-size mapped writes and noexcept map swaps remain.
	if (newSection)
	{
		memset(targetSection, 0, sizeof(SecInfo));
		wt_strcpy(targetSection->_name, section);
		targetSection->_updatetime = sectionUpdateTime;
	}
	wt_strcpy(keyInfo->_key, key);
	keyInfo->_updatetime = keyUpdateTime;
	keyInfo->_offset = targetSection->_offset;

	//字符串固定最大长度为64
	targetSection->_count++;
	targetSection->_offset += (uint32_t)len;
	targetSection->_state = 1;
	if (newSection)
		shm._block->_count++;
	secInfo = targetSection;

	if (newSection)
	{
		shm._sections.swap(stagedSections);
	}
	else
	{
		kvPair->_keys.swap(stagedKeys);
	}

	return keyInfo;
}

void* ShareBlocks::check_valid(const char* domain, const char* section, const char* key, ValueType vType, SecInfo* &secInfo)
{
	auto it = _shm_blocks.find(domain);
	if (it == _shm_blocks.end())
		return nullptr;

	ShmPair& shm = (ShmPair&)it->second;
	KeyInfo* keyInfo = nullptr;
	ShmPair::KVPair* kvPair = nullptr;
	auto sit = shm._sections.find(section);
	if (sit == shm._sections.end())
	{
		return nullptr;
	}
	else
	{
		kvPair = (ShmPair::KVPair*)&sit->second;
	}
	if (shm._block == nullptr || kvPair->_index >= MAX_SEC_CNT || kvPair->_index >= shm._block->_count)
		return nullptr;

	secInfo = &shm._block->_sections[kvPair->_index];
	if (std::strcmp(secInfo->_name, section) != 0)
	{
		secInfo = nullptr;
		return nullptr;
	}
	if (secInfo->_state == 2 || (!is_current_master(shm._master, shm._domain.get()) && secInfo->_state != 1))
	{
		secInfo = nullptr;
		return nullptr;
	}

	auto kit = kvPair->_keys.find(key);
	if (kit == kvPair->_keys.end())
	{
		return nullptr;
	}
	else
	{
		keyInfo = kit->second;
		if (keyInfo->_type != vType)
			return nullptr;

		return keyInfo;
	}
}

std::vector<std::string> ShareBlocks::get_sections(const char* domain)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	static std::vector<std::string> emptyRet;
	auto domainIt = _shm_blocks.find(domain);
	if (domainIt == _shm_blocks.end() || domainIt->second._block == nullptr || !domainIt->second._domain)
	{
		write_log(LL_ERROR, "Domain {} not initialized", domain);
		return emptyRet;
	}
	SharedFileLock fileGuard(domainIt->second._domain.get());
	if (!fileGuard.locked())
	{
		write_log(LL_ERROR, "Unable to lock shared domain {}", domain);
		return emptyRet;
	}
	const ShmPair& shm = domainIt->second;
	if (shm._block->_count > MAX_SEC_CNT)
	{
		write_log(LL_ERROR, "Shared domain {} has an invalid section count", domain);
		return emptyRet;
	}

	std::vector<std::string> ret;
	for (uint32_t i = 0; i < shm._block->_count; i++)
	{
		if (shm._block->_sections[i]._state != 1)
			continue;

		ret.emplace_back(shm._block->_sections[i]._name);
	}

	return ret;
}

std::vector<KeyInfo*> ShareBlocks::get_keys(const char* domain, const char* section)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	static std::vector<KeyInfo*> emptyRet;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainIt, fileGuard, emptyRet);
	ShmPair& shm = domainIt->second;
	if (!is_current_master(shm._master, shm._domain.get()))
	{
		const bool versionChanged = shm._blocktime != shm._block->_updatetime;
		if (versionChanged && !update_slave_locked(domain, shm, false))
			return emptyRet;
	}

	auto sit = shm._sections.find(section);
	if (sit == shm._sections.end())
		return emptyRet;
	if (sit->second._index >= MAX_SEC_CNT || sit->second._index >= shm._block->_count)
		return emptyRet;
	const SecInfo& sectionInfo = shm._block->_sections[sit->second._index];
	const uint16_t sectionState = sectionInfo._state;
	if (std::strcmp(sectionInfo._name, section) != 0)
		return emptyRet;
	if (sectionState == 2 || (!is_current_master(shm._master, shm._domain.get()) && sectionState != 1))
		return emptyRet;

	std::vector<KeyInfo*> ret;
	const ShmPair::KVPair& kvPair = sit->second;
	for (auto& v : kvPair._keys)
	{
		ret.emplace_back(v.second);
	}

	return ret;
}

const char* ShareBlocks::allocate_string(const char* domain, const char* section, const char* key, const char* initVal /* = "" */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	const std::size_t stringCapacity = SMVT_SIZES[SMVT_STRING];
	std::size_t valueLength = 0;
	if (!get_string_length(initVal, stringCapacity, valueLength))
	{
		write_log(LL_ERROR, "String value for {}.{}.{} is null or exceeds {} bytes", domain, section, key, stringCapacity - 1);
		return nullptr;
	}
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);

	// Check existing entries before make_valid: it marks the section active, and
	// it does not verify an existing key's type or reserved data range.
	if (section != nullptr && key != nullptr)
	{
		auto domainIt = _shm_blocks.find(domain);
		if (domainIt != _shm_blocks.end() && domainIt->second._block != nullptr)
		{
			auto sectionIt = domainIt->second._sections.find(section);
			if (sectionIt != domainIt->second._sections.end())
			{
				const uint32_t sectionIndex = sectionIt->second._index;
				if (sectionIndex >= MAX_SEC_CNT || sectionIndex >= domainIt->second._block->_count)
				{
					write_log(LL_ERROR, "Section index for {}.{} is outside the shared block", domain, section);
					return nullptr;
				}

				auto keyIt = sectionIt->second._keys.find(key);
				if (keyIt != sectionIt->second._keys.end())
				{
					const KeyInfo* existingKey = keyIt->second;
					if (existingKey == nullptr || existingKey->_type != SMVT_STRING)
					{
						write_log(LL_ERROR, "Existing key {}.{}.{} is not a string", domain, section, key);
						return nullptr;
					}

					const std::size_t dataSize = sizeof(domainIt->second._block->_sections[sectionIndex]._data);
					if (stringCapacity > dataSize || existingKey->_offset > dataSize - stringCapacity)
					{
						write_log(LL_ERROR, "String slot {}.{}.{} is outside the section data", domain, section, key);
						return nullptr;
					}
				}
			}
		}
	}

	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_STRING, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if (keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_STRING;
		std::memmove(secInfo->_data + keyInfo->_offset, initVal, valueLength + 1);
	}

	return (secInfo->_data + keyInfo->_offset);
}

int32_t* ShareBlocks::allocate_int32(const char* domain, const char* section, const char* key, int32_t initVal /* = 0 */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_INT32, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if (keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_INT32;
		*secInfo->get<int32_t>(keyInfo->_offset) = initVal;
	}

	return secInfo->get<int32_t>(keyInfo->_offset);
}

int64_t* ShareBlocks::allocate_int64(const char* domain, const char* section, const char* key, int64_t initVal /* = 0 */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_INT64, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if (keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_INT64;
		*secInfo->get<int64_t>(keyInfo->_offset) = initVal;
	}

	return secInfo->get<int64_t>(keyInfo->_offset);
}

uint32_t* ShareBlocks::allocate_uint32(const char* domain, const char* section, const char* key, uint32_t initVal /* = 0 */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_UINT32, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if (keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_UINT32;
		*secInfo->get<uint32_t>(keyInfo->_offset) = initVal;
	}

	return secInfo->get<uint32_t>(keyInfo->_offset);
}

uint64_t* ShareBlocks::allocate_uint64(const char* domain, const char* section, const char* key, uint64_t initVal /* = 0 */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_UINT64, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if (keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_UINT64;
		*secInfo->get<uint64_t>(keyInfo->_offset) = initVal;
	}

	return secInfo->get<uint64_t>(keyInfo->_offset);
}

double* ShareBlocks::allocate_double(const char* domain, const char* section, const char* key, double initVal /* = 0 */, bool bForceWrite/* = false*/)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, nullptr);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_DOUBLE, secInfo);
	if (keyInfo == nullptr)
		return NULL;

	if(keyInfo->_type == 0 || bForceWrite)
	{
		//如果type为0，说明是新分配的，则用初始值填充
		keyInfo->_type = SMVT_DOUBLE;
		*secInfo->get<double>(keyInfo->_offset) = initVal;
	}

	return secInfo->get<double>(keyInfo->_offset);
}

bool ShareBlocks::set_string(const char* domain, const char* section, const char* key, const char* val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	const std::size_t stringCapacity = SMVT_SIZES[SMVT_STRING];
	std::size_t valueLength = 0;
	if (!get_string_length(val, stringCapacity, valueLength))
	{
		write_log(LL_ERROR, "String value for {}.{}.{} is null or exceeds {} bytes", domain, section, key, stringCapacity - 1);
		return false;
	}
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);

	// Reject corrupt existing entries before make_valid can change section state.
	if (section != nullptr && key != nullptr)
	{
		auto domainIt = _shm_blocks.find(domain);
		if (domainIt != _shm_blocks.end() && domainIt->second._block != nullptr)
		{
			auto sectionIt = domainIt->second._sections.find(section);
			if (sectionIt != domainIt->second._sections.end())
			{
				const uint32_t sectionIndex = sectionIt->second._index;
				if (sectionIndex >= MAX_SEC_CNT || sectionIndex >= domainIt->second._block->_count)
				{
					write_log(LL_ERROR, "Section index for {}.{} is outside the shared block", domain, section);
					return false;
				}

				auto keyIt = sectionIt->second._keys.find(key);
				if (keyIt != sectionIt->second._keys.end())
				{
					const KeyInfo* existingKey = keyIt->second;
					if (existingKey == nullptr || existingKey->_type != SMVT_STRING)
					{
						write_log(LL_ERROR, "Existing key {}.{}.{} is not a string", domain, section, key);
						return false;
					}

					const std::size_t dataSize = sizeof(domainIt->second._block->_sections[sectionIndex]._data);
					if (stringCapacity > dataSize || existingKey->_offset > dataSize - stringCapacity)
					{
						write_log(LL_ERROR, "String slot {}.{}.{} is outside the section data", domain, section, key);
						return false;
					}
				}
			}
		}
	}

	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_STRING, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_STRING;
	std::memmove(secInfo->_data + keyInfo->_offset, val, valueLength + 1);

	return true;
}

bool ShareBlocks::set_int32(const char* domain, const char* section, const char* key, int32_t val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_INT32, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_INT32;
	*secInfo->get<int32_t>(keyInfo->_offset) = val;

	return true;
}

bool ShareBlocks::set_int64(const char* domain, const char* section, const char* key, int64_t val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_INT64, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_INT64;
	*secInfo->get<int64_t>(keyInfo->_offset) = val;

	return true;
}

bool ShareBlocks::set_uint32(const char* domain, const char* section, const char* key, uint32_t val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_UINT32, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_UINT32;
	*secInfo->get<uint32_t>(keyInfo->_offset) = val;

	return true;
}

bool ShareBlocks::set_uint64(const char* domain, const char* section, const char* key, uint64_t val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_UINT64, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_UINT64;
	*secInfo->get<uint64_t>(keyInfo->_offset) = val;

	return true;
}

bool ShareBlocks::set_double(const char* domain, const char* section, const char* key, double val)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_LOCK_IF_MAPPED_DOMAIN(domain, domainPairIt, fileGuard, false);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)make_valid(domain, section, key, SMVT_DOUBLE, secInfo);
	if (keyInfo == nullptr)
		return false;

	keyInfo->_type = SMVT_DOUBLE;
	*secInfo->get<double>(keyInfo->_offset) = val;

	return true;
}

const char* ShareBlocks::get_string(const char* domain, const char* section, const char* key, const char* defVal /* = "" */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_STRING, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return (const char*)(secInfo->_data + keyInfo->_offset);
}

int32_t ShareBlocks::get_int32(const char* domain, const char* section, const char* key, int32_t defVal /* = 0 */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_INT32, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return *(int32_t*)(secInfo->_data + keyInfo->_offset);
}

uint32_t ShareBlocks::get_uint32(const char* domain, const char* section, const char* key, uint32_t defVal /* = 0 */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_UINT32, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return *(uint32_t*)(secInfo->_data + keyInfo->_offset);
}

int64_t ShareBlocks::get_int64(const char* domain, const char* section, const char* key, int64_t defVal /* = 0 */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_INT64, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return *(int64_t*)(secInfo->_data + keyInfo->_offset);
}

uint64_t ShareBlocks::get_uint64(const char* domain, const char* section, const char* key, uint64_t defVal /* = 0 */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_UINT64, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return *(uint64_t*)(secInfo->_data + keyInfo->_offset);
}

double ShareBlocks::get_double(const char* domain, const char* section, const char* key, double defVal /* = 0 */)
{
	OperationLogScope logScope;
	ShareBlocksLockGuard localGuard;
	SHAREBLOCK_ACQUIRE_DOMAIN_LOCK(domain, domainPairIt, fileGuard, defVal);
	SecInfo* secInfo = nullptr;
	KeyInfo* keyInfo = (KeyInfo*)check_valid(domain, section, key, SMVT_DOUBLE, secInfo);
	if (keyInfo == nullptr)
		return defVal;

	return *(double*)(secInfo->_data + keyInfo->_offset);
}

bool ShareBlocks::init_cmder(const char* name, bool isCmder /* = false */, const char* path /* = "" */)
{
	ShareBlocksLockGuard localGuard;
	CmdPair& cmdPair = _cmd_blocks[name];
	if (cmdPair._block != NULL)
		return true;

	std::string filename = path;
	if (filename.empty())
		filename = ".cmd";
	filename = canonical_file_path(filename.c_str());
	if (filename.empty())
		return false;

	// Serialize first creation and cold initialization across processes. Only
	// initializers that honor this sidecar lock cooperate; it does not make
	// rolling upgrades with legacy initializers safe. This protects initialization
	// only, not concurrent command writers. Active attachments must not rewrite
	// the shared monotonic command cursors.
	const std::string initLockPath = filename + ".wtshareblocks.init.lock";
	SharedFileLock initGuard(initLockPath.c_str());
	if (!initGuard.locked())
		return false;

	if (!StdFile::exists(filename.c_str()))
	{
		BoostFile bf;
		if (!bf.create_new_file(filename.c_str()) || !bf.truncate_file(sizeof(CmdBlock)))
			return false;
		bf.close_file();
	}

	cmdPair._domain.reset(new BoostMappingFile);
	if (!cmdPair._domain->map(filename.c_str()) || cmdPair._domain->size() < sizeof(CmdBlock))
	{
		cmdPair._domain.reset();
		return false;
	}
	cmdPair._cmder = isCmder;
	cmdPair._block = (CmdBlock*)cmdPair._domain->addr();
	if(cmdPair._block->_capacity == 0)
		new(cmdPair._domain->addr()) CmdBlock();

	if(cmdPair._cmder)
#ifdef _MSC_VER
		cmdPair._block->_cmdpid = _getpid();
#else
		cmdPair._block->_cmdpid = getpid();
#endif
	return true;
}

bool ShareBlocks::add_cmd(const char* name, const char* cmd)
{
	if (name == nullptr || cmd == nullptr)
		return false;

	// _command includes its terminating NUL, so at most MAX_CMD_SIZE - 1
	// payload bytes can be published. Validate before touching the shared cursor
	// or the target slot.
	constexpr std::size_t cmdCapacity = static_cast<std::size_t>(MAX_CMD_SIZE);
	std::size_t cmdSize = 0;
	while (cmdSize < cmdCapacity && cmd[cmdSize] != '\0')
		++cmdSize;
	if (cmdSize == cmdCapacity)
		return false;

	ShareBlocksLockGuard localGuard;
	auto it = _cmd_blocks.find(name);
	if (it == _cmd_blocks.end())
		return false;

	CmdPair& cmdPair = it->second;

	if (cmdPair._block == NULL)
		return false;

#ifdef _MSC_VER
    if (cmdPair._block->_cmdpid != _getpid())
#else
	if (cmdPair._block->_cmdpid != (uint32_t)getpid())
#endif
	
		return false;

	/*
	 *	先移动写的下标，然后写入数据
	 *	写完了以后，再移动读的下标
	 */
	uint32_t wIdx = cmdPair._block->_writable++;
	uint32_t realIdx = wIdx % cmdPair._block->_capacity;
	cmdPair._block->_commands[realIdx]._state = 0;
	strcpy(cmdPair._block->_commands[realIdx]._command, cmd);
	cmdPair._block->_readable = wIdx;
	return true;
}

const char* ShareBlocks::get_cmd(const char* name, uint32_t& lastIdx)
{
	ShareBlocksLockGuard localGuard;
	auto it = _cmd_blocks.find(name);
	if (it == _cmd_blocks.end())
		return "";

	CmdPair& cmdPair = it->second;

	if (cmdPair._block == NULL)
		return "";

	//指令下达者就不需要获取指令了
	if (cmdPair._cmder)
		return "";

	//说明刚启动，之前的命令全部作废
	if (cmdPair._block->_readable == UINT32_MAX)
	{
		lastIdx = 999999;
		return "";
	}
	else if (lastIdx == UINT32_MAX && cmdPair._block->_readable != UINT32_MAX)
	{
		lastIdx = cmdPair._block->_readable;
		return "";
	}
	else if(lastIdx == 999999 && cmdPair._block->_readable != UINT32_MAX)
	{
		lastIdx = 0;
		return cmdPair._block->_commands[lastIdx]._command;
	}
	else if(lastIdx >= cmdPair._block->_readable)
	{
		return "";
	}
	else
	{
		lastIdx++;
		return cmdPair._block->_commands[lastIdx % cmdPair._block->_capacity]._command;
	}
}
