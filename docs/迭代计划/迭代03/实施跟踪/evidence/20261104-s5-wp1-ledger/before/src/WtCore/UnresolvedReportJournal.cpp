#include "UnresolvedReportJournal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/acl.h>
#elif defined(__linux__)
#include <sys/xattr.h>
#endif
#endif

NS_WTP_BEGIN

#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
void wtJournalBootstrapTestHook(int stage);
#endif

namespace
{
const uint8_t kPrimaryMagic[8] = { 'W', 'T', 'U', 'R', 'J', 'N', 'L', '2' };
const uint8_t kGuardMagic[8] = { 'W', 'T', 'U', 'G', 'A', 'R', 'D', '1' };
const uint8_t kMarkerMagic[8] = { 'W', 'T', 'U', 'I', 'N', 'F', 'L', '1' };
const uint8_t kCleanupMagic[8] = { 'W', 'T', 'U', 'C', 'L', 'E', 'A', '1' };
const uint16_t kPrimaryVersion = 2;
const uint16_t kGuardVersion = 1;
const uint16_t kMarkerVersion = 1;
const uint32_t kMaxReportBytes = 1024 * 1024;
const uint32_t kMaxFramePayloadBytes = kMaxReportBytes + 16 * 1024;
const uint32_t kMaxAuditTextBytes = 4096;
const size_t kFrameHeaderBytes = 28;

void putU16(std::string& out, uint16_t value)
{
	out.push_back(static_cast<char>(value & 0xff));
	out.push_back(static_cast<char>((value >> 8) & 0xff));
}

void putU32(std::string& out, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

void putU64(std::string& out, uint64_t value)
{
	for (unsigned i = 0; i < 8; ++i)
		out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

uint16_t getU16(const char* data)
{
	return static_cast<uint16_t>(static_cast<uint8_t>(data[0])) |
		(static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint32_t getU32(const char* data)
{
	uint32_t value = 0;
	for (unsigned i = 0; i < 4; ++i)
		value |= static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << (i * 8);
	return value;
}

uint64_t getU64(const char* data)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i)
		value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (i * 8);
	return value;
}

uint32_t crc32(const char* data, size_t size, uint32_t crc = 0xffffffffu)
{
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= static_cast<uint8_t>(data[i]);
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return crc;
}

uint32_t frameChecksum(const std::string& frame)
{
	uint32_t crc = crc32(frame.data() + 8, 16);
	crc = crc32(frame.data() + kFrameHeaderBytes,
		frame.size() - kFrameHeaderBytes, crc);
	return crc ^ 0xffffffffu;
}

std::string makeFrame(const uint8_t magic[8], uint16_t version,
	uint16_t type, const std::string& payload, uint64_t sequence)
{
	std::string frame;
	frame.append(reinterpret_cast<const char*>(magic), 8);
	putU16(frame, version);
	putU16(frame, type);
	putU32(frame, static_cast<uint32_t>(payload.size()));
	putU64(frame, sequence);
	putU32(frame, 0);
	frame.append(payload);
	const uint32_t checksum = frameChecksum(frame);
	for (unsigned i = 0; i < 4; ++i)
		frame[24 + i] = static_cast<char>((checksum >> (i * 8)) & 0xff);
	return frame;
}

bool validAuditText(const std::string& text)
{
	return !text.empty() && text.size() <= kMaxAuditTextBytes &&
		text.find('\0') == std::string::npos;
}

void putString(std::string& out, const std::string& value)
{
	putU32(out, static_cast<uint32_t>(value.size()));
	out.append(value);
}

class PayloadReader
{
public:
	explicit PayloadReader(const std::string& bytes) : _bytes(bytes), _offset(0) {}

	bool readU16(uint16_t& value)
	{
		if (_bytes.size() - _offset < 2)
			return false;
		value = getU16(_bytes.data() + _offset);
		_offset += 2;
		return true;
	}

	bool readU32(uint32_t& value)
	{
		if (_bytes.size() - _offset < 4)
			return false;
		value = getU32(_bytes.data() + _offset);
		_offset += 4;
		return true;
	}

	bool readU64(uint64_t& value)
	{
		if (_bytes.size() - _offset < 8)
			return false;
		value = getU64(_bytes.data() + _offset);
		_offset += 8;
		return true;
	}

	bool readBytes(std::string& value, uint32_t maximum, bool allow_empty = true)
	{
		uint32_t length = 0;
		if (!readU32(length) || length > maximum || _bytes.size() - _offset < length ||
			(!allow_empty && length == 0))
			return false;
		value.assign(_bytes.data() + _offset, length);
		_offset += length;
		return true;
	}

	bool readString(std::string& value, uint32_t maximum)
	{
		return readBytes(value, maximum, false) &&
			value.find('\0') == std::string::npos;
	}

	bool finished() const { return _offset == _bytes.size(); }

private:
	const std::string& _bytes;
	size_t _offset;
};

struct ParsedFrame
{
	uint16_t type = 0;
	uint64_t sequence = 0;
	uint64_t offset = 0;
	uint32_t checksum = 0;
	std::string payload;
	std::string raw;
};

struct FrameScan
{
	std::vector<ParsedFrame> frames;
	std::string valid_prefix;
	uint64_t bad_offset = 0;
	std::string issue;
};

#ifndef _WIN32
bool syncFd(int fd)
{
	int result = -1;
	do
	{
		result = fsync(fd);
	} while (result != 0 && errno == EINTR);
	return result == 0;
}

bool readExactAt(int fd, uint64_t offset, char* output, size_t size)
{
	while (size != 0)
	{
		const ssize_t got = pread(fd, output, size, static_cast<off_t>(offset));
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			return false;
		offset += static_cast<uint64_t>(got);
		output += got;
		size -= static_cast<size_t>(got);
	}
	return true;
}

bool writeAll(int fd, const char* data, size_t size, size_t max_chunk = 0,
	size_t fail_after_bytes = std::numeric_limits<size_t>::max())
{
	size_t total = 0;
	while (total < size)
	{
		if (total >= fail_after_bytes)
		{
			errno = EIO;
			return false;
		}
		size_t request = size - total;
		if (max_chunk != 0)
			request = std::min(request, max_chunk);
		request = std::min(request, fail_after_bytes - total);
		if (request == 0)
		{
			errno = EIO;
			return false;
		}
		const ssize_t written = ::write(fd, data + total, request);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		total += static_cast<size_t>(written);
	}
	return true;
}

bool fileSize(int fd, uint64_t& size)
{
	struct stat info;
	int result = -1;
	do
	{
		result = fstat(fd, &info);
	} while (result != 0 && errno == EINTR);
	if (result != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
		return false;
	size = static_cast<uint64_t>(info.st_size);
	return true;
}

FrameScan scanFrames(int fd, const uint8_t magic[8], uint16_t version,
	uint16_t min_type, uint16_t max_type, uint32_t max_payload)
{
	FrameScan scan;
	uint64_t size = 0;
	if (!fileSize(fd, size))
	{
		scan.issue = "unable to stat journal file";
		return scan;
	}
	if (size == 0)
	{
		scan.issue = "journal file is empty";
		return scan;
	}
	uint64_t offset = 0;
	uint64_t expected_sequence = 1;
	while (offset < size)
	{
		if (size - offset < kFrameHeaderBytes)
		{
			scan.issue = "truncated frame header";
			scan.bad_offset = offset;
			break;
		}
		std::string raw(kFrameHeaderBytes, '\0');
		if (!readExactAt(fd, offset, &raw[0], raw.size()))
		{
			scan.issue = "read failure in frame header";
			scan.bad_offset = offset;
			break;
		}
		if (std::memcmp(raw.data(), magic, 8) != 0)
		{
			scan.issue = "frame magic mismatch";
			scan.bad_offset = offset;
			break;
		}
		const uint16_t frame_version = getU16(raw.data() + 8);
		const uint16_t type = getU16(raw.data() + 10);
		const uint32_t payload_length = getU32(raw.data() + 12);
		const uint64_t sequence = getU64(raw.data() + 16);
		const uint32_t checksum = getU32(raw.data() + 24);
		if (frame_version != version || type < min_type || type > max_type ||
			payload_length > max_payload || sequence != expected_sequence ||
			sequence == std::numeric_limits<uint64_t>::max())
		{
			scan.issue = "unsupported frame or invalid type, length, or sequence";
			scan.bad_offset = offset;
			break;
		}
		if (size - offset - kFrameHeaderBytes < payload_length)
		{
			scan.issue = "truncated frame payload";
			scan.bad_offset = offset;
			break;
		}
		raw.resize(kFrameHeaderBytes + payload_length);
		if (payload_length != 0 && !readExactAt(fd,
			offset + kFrameHeaderBytes, &raw[kFrameHeaderBytes], payload_length))
		{
			scan.issue = "read failure in frame payload";
			scan.bad_offset = offset;
			break;
		}
		if (frameChecksum(raw) != checksum)
		{
			scan.issue = "frame checksum mismatch";
			scan.bad_offset = offset;
			break;
		}
		ParsedFrame frame;
		frame.type = type;
		frame.sequence = sequence;
		frame.offset = offset;
		frame.checksum = checksum;
		frame.payload.assign(raw.data() + kFrameHeaderBytes, payload_length);
		frame.raw = raw;
		scan.frames.push_back(frame);
		scan.valid_prefix.append(raw);
		offset += raw.size();
		if (sequence == std::numeric_limits<uint64_t>::max() - 1)
		{
			scan.issue = "journal sequence space is exhausted";
			scan.bad_offset = offset;
			break;
		}
		expected_sequence = sequence + 1;
	}
	return scan;
}

bool splitStablePath(const std::string& input, std::string& directory,
	std::string& basename, std::string& full_path)
{
	if (input.empty() || input.find('\0') != std::string::npos)
		return false;
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(
		std::filesystem::u8path(input), error).lexically_normal();
	if (error)
		return false;
	const std::filesystem::path name = absolute.filename();
	if (name.empty() || name == "." || name == ".." || name.has_parent_path())
		return false;
	const std::filesystem::path canonical_directory =
		std::filesystem::canonical(absolute.parent_path(), error);
	if (error)
		return false;
	directory = canonical_directory.u8string();
	basename = name.u8string();
	full_path = (canonical_directory / name).u8string();
	return !directory.empty() && !basename.empty();
}

bool noExtendedAcl(int fd, bool directory)
{
#if defined(__APPLE__)
	(void)directory;
	errno = 0;
	acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
	if (acl != NULL)
	{
		acl_free(acl);
		return false;
	}
	return errno == ENOENT || errno == ENODATA || errno == ENOATTR;
#elif defined(__linux__)
	const char* names[] = { "system.posix_acl_access", "system.posix_acl_default" };
	const size_t count = directory ? 2 : 1;
	for (size_t i = 0; i < count; ++i)
	{
		const ssize_t length = fgetxattr(fd, names[i], NULL, 0);
		bool no_attribute = errno == ENODATA;
#ifdef ENOATTR
		no_attribute = no_attribute || errno == ENOATTR;
#endif
		if (length >= 0 || !no_attribute)
			return false;
	}
	return true;
#else
	(void)fd;
	(void)directory;
	return false;
#endif
}

bool validatePrivateDirectory(int fd, uint64_t& device, uint64_t& inode)
{
	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
		(info.st_mode & 07777) != 0700 || !noExtendedAcl(fd, true))
		return false;
	device = static_cast<uint64_t>(info.st_dev);
	inode = static_cast<uint64_t>(info.st_ino);
	return true;
}

bool validateContainingDirectory(const std::string& directory_path)
{
	const std::filesystem::path parent_path =
		std::filesystem::u8path(directory_path).parent_path();
	if (parent_path.empty())
		return true;
	int flags = O_RDONLY;
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int parent_fd = -1;
	do
	{
		parent_fd = ::open(parent_path.c_str(), flags);
	} while (parent_fd < 0 && errno == EINTR);
	if (parent_fd < 0)
		return false;
	struct stat info;
	const bool stat_ok = fstat(parent_fd, &info) == 0 && S_ISDIR(info.st_mode);
	const bool owner_ok = stat_ok && (info.st_uid == 0 || info.st_uid == geteuid());
	const bool writable_ok = stat_ok &&
		((info.st_mode & 0022) == 0 || (info.st_mode & S_ISVTX) != 0);
	const bool acl_ok = stat_ok && noExtendedAcl(parent_fd, true);
	const bool result = owner_ok && writable_ok && acl_ok;
	::close(parent_fd);
	return result;
}

bool validatePrivateFile(int fd, uint64_t& device, uint64_t& inode)
{
	struct stat info;
	if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		info.st_nlink != 1 || (info.st_mode & 07777) != 0600 ||
		!noExtendedAcl(fd, false))
		return false;
	device = static_cast<uint64_t>(info.st_dev);
	inode = static_cast<uint64_t>(info.st_ino);
	return true;
}

bool pathIdentityAt(int directory_fd, const std::string& basename,
	uint64_t& device, uint64_t& inode)
{
	struct stat info;
	int result = -1;
	do
	{
		result = fstatat(directory_fd, basename.c_str(), &info, AT_SYMLINK_NOFOLLOW);
	} while (result != 0 && errno == EINTR);
	if (result != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
		info.st_nlink != 1 || (info.st_mode & 07777) != 0600)
		return false;
	device = static_cast<uint64_t>(info.st_dev);
	inode = static_cast<uint64_t>(info.st_ino);
	return true;
}

bool directoryIdentityAtPath(const std::string& path, uint64_t expected_device,
	uint64_t expected_inode)
{
	int flags = O_RDONLY;
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int fd = -1;
	do
	{
		fd = ::open(path.c_str(), flags);
	} while (fd < 0 && errno == EINTR);
	if (fd < 0)
		return false;
	struct stat info;
	const bool same = fstat(fd, &info) == 0 && S_ISDIR(info.st_mode) &&
		static_cast<uint64_t>(info.st_dev) == expected_device &&
		static_cast<uint64_t>(info.st_ino) == expected_inode;
	::close(fd);
	return same;
}

enum class ComponentState
{
	Present,
	Absent,
	Error
};

ComponentState componentStateAt(int directory_fd, const std::string& basename,
	bool inject_error = false)
{
	if (inject_error)
	{
		errno = EIO;
		return ComponentState::Error;
	}
	struct stat info;
	int result = -1;
	do
	{
		result = fstatat(directory_fd, basename.c_str(), &info, AT_SYMLINK_NOFOLLOW);
	} while (result != 0 && errno == EINTR);
	if (result == 0)
		return ComponentState::Present;
	return errno == ENOENT ? ComponentState::Absent : ComponentState::Error;
}

bool captureInFlightMarker(int directory_fd, const std::string& basename,
	UnresolvedReportJournal::RecoveryInspection& inspection)
{
	struct stat info;
	int result = -1;
	do
	{
		result = fstatat(directory_fd, basename.c_str(), &info, AT_SYMLINK_NOFOLLOW);
	} while (result != 0 && errno == EINTR);
	if (result != 0)
	{
		if (errno != ENOENT)
		{
			inspection.has_in_flight_marker = true;
			inspection.requires_manual_recovery = true;
			inspection.guard_issue = "unable to inspect journal marker path";
			return true;
		}
		return false;
	}
	inspection.has_in_flight_marker = true;
	inspection.requires_manual_recovery = true;
	if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || info.st_nlink != 1 ||
		(info.st_mode & 07777) != 0600)
	{
		inspection.guard_issue = "journal marker is not a private regular file";
		return true;
	}
	int marker_fd = -1;
	do
	{
		marker_fd = openat(directory_fd, basename.c_str(), O_RDONLY
#ifdef O_CLOEXEC
			| O_CLOEXEC
#endif
#ifdef O_NOFOLLOW
			| O_NOFOLLOW
#endif
		);
	} while (marker_fd < 0 && errno == EINTR);
	if (marker_fd < 0)
	{
		inspection.guard_issue = "unable to open journal marker without following links";
		return true;
	}
	uint64_t device = 0, inode = 0;
	if (!validatePrivateFile(marker_fd, device, inode))
	{
		inspection.guard_issue = "journal marker failed owner, mode, link, or ACL checks";
		::close(marker_fd);
		return true;
	}
	struct stat marker_info;
	if (fstat(marker_fd, &marker_info) != 0 || marker_info.st_size < 0 ||
		static_cast<uint64_t>(marker_info.st_size) > kMaxFramePayloadBytes + kFrameHeaderBytes)
		inspection.guard_issue = "journal marker size is invalid";
	else
	{
		inspection.in_flight_marker_bytes.resize(static_cast<size_t>(marker_info.st_size));
		if (!inspection.in_flight_marker_bytes.empty() &&
			!readExactAt(marker_fd, 0, &inspection.in_flight_marker_bytes[0],
				inspection.in_flight_marker_bytes.size()))
			inspection.guard_issue = "unable to read journal marker";
	}
	::close(marker_fd);
	return true;
}

bool captureCleanupMarker(int directory_fd, const std::string& basename,
	UnresolvedReportJournal::RecoveryInspection& inspection)
{
	UnresolvedReportJournal::RecoveryInspection marker;
	if (!captureInFlightMarker(directory_fd, basename, marker))
		return false;
	inspection.has_cleanup_marker = true;
	inspection.cleanup_uncertain = true;
	inspection.cleanup_marker_bytes = marker.in_flight_marker_bytes;
	inspection.requires_manual_recovery = true;
	if (inspection.guard_issue.empty())
		inspection.guard_issue = marker.guard_issue;
	return true;
}

bool syncDirectory(int directory_fd)
{
	return syncFd(directory_fd);
}

bool appendAuditAckPayload(const std::string& payload,
	UnresolvedReportJournal::Acknowledgement& record)
{
	PayloadReader reader(payload);
	return reader.readU64(record.acknowledged_high_water) &&
		reader.readString(record.operator_id, kMaxAuditTextBytes) &&
		reader.readString(record.reason, kMaxAuditTextBytes) && reader.finished();
}

std::string makeAckPayload(uint64_t high_water, const std::string& operator_id,
	const std::string& reason)
{
	std::string payload;
	putU64(payload, high_water);
	putString(payload, operator_id);
	putString(payload, reason);
	return payload;
}

std::string makePreparePayload(uint16_t primary_type, uint64_t primary_sequence,
	const std::string& primary_payload)
{
	std::string payload;
	putU16(payload, primary_type);
	putU64(payload, primary_sequence);
	putString(payload, primary_payload);
	return payload;
}

bool parsePreparePayload(const std::string& payload, uint16_t& primary_type,
	uint64_t& primary_sequence, std::string& primary_payload)
{
	PayloadReader reader(payload);
	return reader.readU16(primary_type) && reader.readU64(primary_sequence) &&
		reader.readBytes(primary_payload, kMaxReportBytes + kMaxAuditTextBytes) &&
		reader.finished();
}

std::string makeCommitPayload(uint64_t prepare_sequence, uint64_t primary_sequence,
	uint32_t primary_checksum)
{
	std::string payload;
	putU64(payload, prepare_sequence);
	putU64(payload, primary_sequence);
	putU32(payload, primary_checksum);
	return payload;
}

bool parseCommitPayload(const std::string& payload, uint64_t& prepare_sequence,
	uint64_t& primary_sequence, uint32_t& primary_checksum)
{
	PayloadReader reader(payload);
	return reader.readU64(prepare_sequence) && reader.readU64(primary_sequence) &&
		reader.readU32(primary_checksum) && reader.finished();
}

#endif // !_WIN32
} // namespace

enum class UnresolvedReportJournal::RecordType : uint16_t
{
	Bootstrap = 1,
	RawReport = 2,
	Acknowledgement = 3
};

enum class UnresolvedReportJournal::GuardRecordType : uint16_t
{
	Prepare = 1,
	Commit = 2
};

UnresolvedReportJournal::UnresolvedReportJournal()
	: _last_sequence(0)
	, _acknowledged_high_water(0)
	, _opened(false)
	, _healthy(false)
	, _created_new(false)
	, _has_recovery_inspection(false)
	, _guard_sequence(0)
	, _fd(-1)
	, _guard_fd(-1)
	, _lock_fd(-1)
	, _directory_fd(-1)
	, _guard_device(0)
	, _guard_inode(0)
	, _directory_device(0)
	, _directory_inode(0)
	, _file_device(0)
	, _file_inode(0)
	, _lock_device(0)
	, _lock_inode(0)
	, _inflight_device(0)
	, _inflight_inode(0)
	, _cleanup_device(0)
	, _cleanup_inode(0)
	, _test_fault_point(TestFaultPoint::None)
	, _test_write_chunk(0)
	, _test_fail_after_write_bytes(0)
{
}

UnresolvedReportJournal::~UnresolvedReportJournal()
{
	close();
}

UnresolvedReportJournal::Status UnresolvedReportJournal::createNew(
	const std::string& stable_path, const std::string& bootstrap_evidence_reference,
	std::string* error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!validAuditText(bootstrap_evidence_reference))
	{
		if (error) *error = "explicit bootstrap evidence reference is required";
		return Status::InvalidArgument;
	}
	return openLocked(stable_path, true, bootstrap_evidence_reference, error);
}

UnresolvedReportJournal::Status UnresolvedReportJournal::open(
	const std::string& stable_path, std::string* error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	return openLocked(stable_path, false, std::string(), error);
}

UnresolvedReportJournal::Status UnresolvedReportJournal::openLocked(
	const std::string& stable_path, bool create_new,
	const std::string& bootstrap_evidence_reference, std::string* error)
{
#ifdef _WIN32
	(void)stable_path;
	(void)create_new;
	(void)bootstrap_evidence_reference;
	if (error)
		*error = "Windows journal I/O is disabled pending native ACL and crash-durability validation";
	return Status::PlatformUnsupported;
#else
	if (_opened)
	{
		if (error) *error = "journal instance is already open";
		return Status::Busy;
	}
	std::string directoryPath, basename, fullPath;
	if (!splitStablePath(stable_path, directoryPath, basename, fullPath))
	{
		if (error) *error = "journal path must name a file in an existing directory";
		return Status::InvalidArgument;
	}
	_path = fullPath;
	_directory_path = directoryPath;
	_wal_name = basename;
	_lock_name = basename + ".lock";
	_guard_name = basename + ".guard";
	_inflight_name = basename + ".inflight";
	_cleanup_name = basename + ".cleanup";
	_lock_path = (_directory_path + "/" + _lock_name);
	_guard_path = (_directory_path + "/" + _guard_name);
	_inflight_path = (_directory_path + "/" + _inflight_name);
	_cleanup_path = (_directory_path + "/" + _cleanup_name);
	_pending.clear();
	_acknowledgements.clear();
	_last_sequence = 0;
	_acknowledged_high_water = 0;
	_guard_sequence = 0;
	_inflight_device = 0;
	_inflight_inode = 0;
	_cleanup_device = 0;
	_cleanup_inode = 0;
	_last_error.clear();
	_created_new = create_new;
	_healthy = false;
	_has_recovery_inspection = false;
	_recovery_inspection = RecoveryInspection();
	if (!validateContainingDirectory(_directory_path))
	{
		if (error) *error = "journal directory parent is not a trusted, ACL-free directory";
		return Status::UnsafePath;
	}

	int directoryFlags = O_RDONLY;
#ifdef O_DIRECTORY
	directoryFlags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
	directoryFlags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	directoryFlags |= O_NOFOLLOW;
#endif
	do
	{
		_directory_fd = ::open(_directory_path.c_str(), directoryFlags);
	} while (_directory_fd < 0 && errno == EINTR);
	if (_directory_fd < 0)
	{
		if (error) *error = "unable to open journal directory without following links";
		return Status::UnsafePath;
	}
	if (!validatePrivateDirectory(_directory_fd, _directory_device, _directory_inode))
	{
		if (error) *error = "journal directory must be owned by this user, mode 0700, and have no extended ACL";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	if (create_new)
	{
		// A known pre-existing component must not be accompanied by freshly
		// created lock/WAL files. This preflight does not replace O_EXCL or
		// claim atomicity against a concurrent external creator.
		const ComponentState lock_state = componentStateAt(_directory_fd, _lock_name);
		const ComponentState wal_state = componentStateAt(_directory_fd, _wal_name,
			_test_fault_point == TestFaultPoint::FailComponentProbe);
		const ComponentState guard_state = componentStateAt(_directory_fd, _guard_name);
		const ComponentState inflight_state = componentStateAt(_directory_fd, _inflight_name);
		const ComponentState cleanup_state = componentStateAt(_directory_fd, _cleanup_name);
		if (lock_state == ComponentState::Error || wal_state == ComponentState::Error ||
			guard_state == ComponentState::Error || inflight_state == ComponentState::Error ||
			cleanup_state == ComponentState::Error)
		{
			if (error) *error = "unable to inspect journal components before bootstrap";
			releaseHandlesLocked();
			return Status::StorageFailed;
		}
		if (lock_state == ComponentState::Present)
		{
			if (error) *error = "journal lock already exists";
			releaseHandlesLocked();
			return Status::AlreadyExists;
		}
		if (inflight_state == ComponentState::Present || cleanup_state == ComponentState::Present)
		{
			captureInFlightMarker(_directory_fd, _inflight_name, _recovery_inspection);
			captureCleanupMarker(_directory_fd, _cleanup_name, _recovery_inspection);
			_recovery_inspection.requires_manual_recovery = true;
			_has_recovery_inspection = true;
			if (error) *error = "orphan journal marker prevents explicit bootstrap";
			releaseHandlesLocked();
			return Status::RecoveryRequired;
		}
		if (wal_state == ComponentState::Present || guard_state == ComponentState::Present)
		{
			if (error) *error = "journal WAL or guard already exists";
			releaseHandlesLocked();
			return Status::AlreadyExists;
		}
	}

#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (create_new) wtJournalBootstrapTestHook(0);
#endif

	int commonFlags = O_RDWR;
#ifdef O_CLOEXEC
	commonFlags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	commonFlags |= O_NOFOLLOW;
#endif
	int createFlags = commonFlags;
	if (create_new)
		createFlags |= O_CREAT | O_EXCL;
	const int lockFlags = create_new ? createFlags : commonFlags;
	do
	{
		_lock_fd = openat(_directory_fd, _lock_name.c_str(), lockFlags, 0600);
	} while (_lock_fd < 0 && errno == EINTR);
	if (_lock_fd < 0)
	{
		const int osError = errno;
		if (error) *error = "unable to open required journal lock file: " + std::string(std::strerror(osError));
		const ComponentState wal_state = componentStateAt(_directory_fd, _wal_name);
		const ComponentState guard_state = componentStateAt(_directory_fd, _guard_name);
		const bool marker_exists = captureInFlightMarker(_directory_fd,
			_inflight_name, _recovery_inspection);
		const bool cleanup_exists = captureCleanupMarker(_directory_fd,
			_cleanup_name, _recovery_inspection);
		if (!create_new && (wal_state != ComponentState::Absent ||
			guard_state != ComponentState::Absent || marker_exists || cleanup_exists))
		{
			_recovery_inspection.requires_manual_recovery = true;
			_recovery_inspection.guard_issue = "journal data exists but its required lock file is missing or unsafe";
			_has_recovery_inspection = true;
			releaseHandlesLocked();
			if (error) *error = _recovery_inspection.guard_issue;
			return Status::RecoveryRequired;
		}
		releaseHandlesLocked();
		if (create_new && osError == EEXIST) return Status::AlreadyExists;
		if (!create_new && osError == ENOENT) return Status::NotFound;
		return osError == ELOOP ? Status::UnsafePath : Status::StorageFailed;
	}
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (create_new) wtJournalBootstrapTestHook(1);
#endif
	if (create_new && fchmod(_lock_fd, 0600) != 0)
	{
		if (error) *error = "unable to set private journal lock permissions";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	int lockResult = -1;
	do
	{
		lockResult = flock(_lock_fd, LOCK_EX | LOCK_NB);
	} while (lockResult != 0 && errno == EINTR);
	if (lockResult != 0)
	{
		const int osError = errno;
		if (error) *error = "journal is already locked by another process";
		releaseHandlesLocked();
		return (osError == EWOULDBLOCK || osError == EAGAIN) ? Status::Busy : Status::StorageFailed;
	}
	if (!validatePrivateFile(_lock_fd, _lock_device, _lock_inode))
	{
		if (error) *error = "journal lock file must be owned by this user, mode 0600, single-link, and ACL-free";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	if (create_new)
	{
		const ComponentState inflight_state = componentStateAt(_directory_fd, _inflight_name);
		const ComponentState cleanup_state = componentStateAt(_directory_fd, _cleanup_name);
		if (inflight_state == ComponentState::Error || cleanup_state == ComponentState::Error)
		{
			if (error) *error = "unable to inspect journal markers during bootstrap";
			releaseHandlesLocked();
			return Status::StorageFailed;
		}
		if (inflight_state == ComponentState::Present || cleanup_state == ComponentState::Present)
		{
			captureInFlightMarker(_directory_fd, _inflight_name, _recovery_inspection);
			captureCleanupMarker(_directory_fd, _cleanup_name, _recovery_inspection);
			_recovery_inspection.requires_manual_recovery = true;
			_has_recovery_inspection = true;
			if (error) *error = "orphan journal marker prevents explicit bootstrap";
			releaseHandlesLocked();
			return Status::RecoveryRequired;
		}
	}
	if (!create_new)
	{
		captureInFlightMarker(_directory_fd, _inflight_name, _recovery_inspection);
		captureCleanupMarker(_directory_fd, _cleanup_name, _recovery_inspection);
	}

	const int fileFlags = createFlags | O_APPEND;
	do
	{
		_fd = openat(_directory_fd, _wal_name.c_str(), fileFlags, 0600);
	} while (_fd < 0 && errno == EINTR);
	if (_fd < 0)
	{
		const int osError = errno;
		if (error) *error = "unable to open journal WAL: " + std::string(std::strerror(osError));
		if (!create_new && (osError == ENOENT ||
				_recovery_inspection.has_in_flight_marker ||
				_recovery_inspection.has_cleanup_marker ||
				componentStateAt(_directory_fd, _guard_name) != ComponentState::Absent))
		{
			_recovery_inspection.requires_manual_recovery = true;
			_recovery_inspection.primary_issue = "required journal WAL is missing or unavailable";
			_has_recovery_inspection = true;
			releaseHandlesLocked();
			if (error) *error = _recovery_inspection.primary_issue;
			return Status::RecoveryRequired;
		}
		releaseHandlesLocked();
		if (create_new && osError == EEXIST) return Status::AlreadyExists;
		if (!create_new && osError == ENOENT) return Status::NotFound;
		return osError == ELOOP ? Status::UnsafePath : Status::StorageFailed;
	}
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (create_new) wtJournalBootstrapTestHook(2);
#endif
	if (create_new && fchmod(_fd, 0600) != 0)
	{
		if (error) *error = "unable to set private journal WAL permissions";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	if (!validatePrivateFile(_fd, _file_device, _file_inode))
	{
		if (error) *error = "journal WAL must be owned by this user, mode 0600, single-link, and ACL-free";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	lockResult = -1;
	do
	{
		lockResult = flock(_fd, LOCK_EX | LOCK_NB);
	} while (lockResult != 0 && errno == EINTR);
	if (lockResult != 0)
	{
		const int osError = errno;
		if (error) *error = "journal WAL is already locked by another process";
		releaseHandlesLocked();
		return (osError == EWOULDBLOCK || osError == EAGAIN) ? Status::Busy : Status::StorageFailed;
	}

	do
	{
		_guard_fd = openat(_directory_fd, _guard_name.c_str(), fileFlags, 0600);
	} while (_guard_fd < 0 && errno == EINTR);
	if (_guard_fd < 0)
	{
		const int osError = errno;
		if (error) *error = "unable to open journal guard WAL: " + std::string(std::strerror(osError));
		if (!create_new && osError == ENOENT)
		{
			FrameScan primary_prefix = scanFrames(_fd, kPrimaryMagic, kPrimaryVersion,
				1, 3, kMaxReportBytes);
			_recovery_inspection.primary_valid_prefix = primary_prefix.valid_prefix;
			_recovery_inspection.primary_bad_offset = primary_prefix.bad_offset;
			_recovery_inspection.primary_issue = primary_prefix.issue;
			_recovery_inspection.guard_issue = "required guard WAL is missing";
			_recovery_inspection.requires_manual_recovery = true;
			_has_recovery_inspection = true;
			releaseHandlesLocked();
			if (error) *error = _recovery_inspection.guard_issue;
			return Status::RecoveryRequired;
		}
		releaseHandlesLocked();
		if (create_new && osError == EEXIST) return Status::AlreadyExists;
		if (!create_new && osError == ENOENT) return Status::NotFound;
		return osError == ELOOP ? Status::UnsafePath : Status::StorageFailed;
	}
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (create_new) wtJournalBootstrapTestHook(3);
#endif
	if (create_new && fchmod(_guard_fd, 0600) != 0)
	{
		if (error) *error = "unable to set private journal guard permissions";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	if (!validatePrivateFile(_guard_fd, _guard_device, _guard_inode))
	{
		if (error) *error = "journal guard must be owned by this user, mode 0600, single-link, and ACL-free";
		releaseHandlesLocked();
		return Status::UnsafePath;
	}
	lockResult = -1;
	do
	{
		lockResult = flock(_guard_fd, LOCK_EX | LOCK_NB);
	} while (lockResult != 0 && errno == EINTR);
	if (lockResult != 0)
	{
		const int osError = errno;
		if (error) *error = "journal guard WAL is already locked by another process";
		releaseHandlesLocked();
		return (osError == EWOULDBLOCK || osError == EAGAIN) ? Status::Busy : Status::StorageFailed;
	}

	if (create_new)
	{
		if (!syncFd(_lock_fd) || !syncFd(_fd) || !syncFd(_guard_fd) ||
			!syncDirectory(_directory_fd))
		{
			if (error) *error = "unable to durably create the journal directory entries";
			releaseHandlesLocked();
			return Status::StorageFailed;
		}
	}
	_opened = true;
	// A present marker is evidence for scanLocked, not an unsafe base path.
	// Preserve strict absence checks for explicit bootstrap and all later
	// healthy operations, while allowing existing journals to expose recovery
	// evidence before closing their descriptors.
	if (!validatePathsLocked(create_new))
	{
		if (error) *error = "journal path identity or private permissions changed during open";
		releaseHandlesLocked();
		_opened = false;
		return Status::UnsafePath;
	}

	if (create_new)
	{
		_healthy = true;
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
		wtJournalBootstrapTestHook(4);
#endif
		uint64_t sequence = 0;
		const Status status = appendGuardedFrameLocked(RecordType::Bootstrap,
			bootstrap_evidence_reference, sequence);
		if (status != Status::Ok && error) *error = _last_error;
		return status;
	}

	const Status status = scanLocked(error);
	if (status != Status::Ok)
	{
		_healthy = false;
		_opened = false;
		releaseHandlesLocked();
		return status;
	}
	_healthy = true;
	return Status::Ok;
#endif
}

void UnresolvedReportJournal::close()
{
	std::lock_guard<std::mutex> lock(_mutex);
	closeLocked();
}

UnresolvedReportJournal::Status UnresolvedReportJournal::closeLocked()
{
	releaseHandlesLocked();
	_opened = false;
	_healthy = false;
	_created_new = false;
	_path.clear();
	_directory_path.clear();
	_wal_name.clear();
	_guard_name.clear();
	_lock_name.clear();
	_inflight_name.clear();
	_cleanup_name.clear();
	_lock_path.clear();
	_guard_path.clear();
	_inflight_path.clear();
	_cleanup_path.clear();
	_pending.clear();
	_acknowledgements.clear();
	_last_sequence = 0;
	_acknowledged_high_water = 0;
	_guard_sequence = 0;
	_inflight_device = 0;
	_inflight_inode = 0;
	_cleanup_device = 0;
	_cleanup_inode = 0;
	return Status::Ok;
}

void UnresolvedReportJournal::releaseHandlesLocked()
{
#ifndef _WIN32
	if (_fd >= 0)
	{
		flock(_fd, LOCK_UN);
		::close(_fd);
		_fd = -1;
	}
	if (_guard_fd >= 0)
	{
		flock(_guard_fd, LOCK_UN);
		::close(_guard_fd);
		_guard_fd = -1;
	}
	if (_lock_fd >= 0)
	{
		flock(_lock_fd, LOCK_UN);
		::close(_lock_fd);
		_lock_fd = -1;
	}
	if (_directory_fd >= 0)
	{
		::close(_directory_fd);
		_directory_fd = -1;
	}
#endif
}

bool UnresolvedReportJournal::isOpen() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _opened && _healthy && validatePathsLocked();
}

bool UnresolvedReportJournal::isHealthy() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _opened && _healthy && validatePathsLocked();
}

std::string UnresolvedReportJournal::lastError() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (_opened && _healthy && !validatePathsLocked())
		return "journal identity or private permissions changed; refusing access";
	return _last_error;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::append(
	const std::string& raw_payload, uint64_t& sequence)
{
	std::lock_guard<std::mutex> lock(_mutex);
	sequence = 0;
	if (!_opened)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
	if (!_healthy)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::StorageFailed;
	if (raw_payload.empty() || raw_payload.size() > kMaxReportBytes)
		return Status::InvalidArgument;
	if (!validatePathsLocked())
		return storageFailureLocked("journal identity or private permissions changed; refusing append");
	return appendGuardedFrameLocked(RecordType::RawReport, raw_payload, sequence);
}

UnresolvedReportJournal::Status UnresolvedReportJournal::pending(
	std::vector<Report>& reports) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	reports.clear();
	if (!_opened)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
	if (!_healthy || !validatePathsLocked())
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::StorageFailed;
	reports = _pending;
	return Status::Ok;
}

uint64_t UnresolvedReportJournal::pendingHighWater() const
{
	uint64_t high_water = 0;
	queryPendingHighWater(high_water);
	return high_water;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::queryPendingHighWater(
	uint64_t& high_water) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	high_water = 0;
	if (!_opened)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
	if (!_healthy || !validatePathsLocked())
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::StorageFailed;
	if (!_pending.empty())
		high_water = _pending.back().sequence;
	return Status::Ok;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::acknowledgements(
	std::vector<Acknowledgement>& records) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	records.clear();
	if (!_opened)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
	if (!_healthy || !validatePathsLocked())
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::StorageFailed;
	records = _acknowledgements;
	return Status::Ok;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::recoveryInspection(
	RecoveryInspection& inspection) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	inspection = _recovery_inspection;
	return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::acknowledge(
	uint64_t expected_high_water, const std::string& operator_id,
	const std::string& reason)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::NotOpen;
	if (!_healthy)
		return _has_recovery_inspection ? Status::RecoveryRequired : Status::StorageFailed;
	if (!validAuditText(operator_id) || !validAuditText(reason))
		return Status::InvalidArgument;
	if (!validatePathsLocked())
		return storageFailureLocked("journal identity or private permissions changed; refusing acknowledgement");
	if (_pending.empty() || expected_high_water != _pending.back().sequence)
		return Status::StaleHighWater;
	uint64_t committed_sequence = 0;
	return appendGuardedFrameLocked(RecordType::Acknowledgement,
		makeAckPayload(expected_high_water, operator_id, reason), committed_sequence);
}

UnresolvedReportJournal::Status UnresolvedReportJournal::appendRawFrameLocked(
	int fd, const uint8_t magic[8], uint16_t version, uint16_t type,
	const std::string& payload, uint64_t sequence, TestFaultPoint flush_fault,
	std::string& frame, uint64_t& offset)
{
#ifdef _WIN32
	(void)fd; (void)magic; (void)version; (void)type; (void)payload;
	(void)sequence; (void)flush_fault; (void)frame; (void)offset;
	return Status::PlatformUnsupported;
#else
	if (payload.size() > kMaxFramePayloadBytes || sequence == 0 ||
		sequence == std::numeric_limits<uint64_t>::max())
		return Status::InvalidArgument;
	frame = makeFrame(magic, version, type, payload, sequence);
	uint64_t file_size = 0;
	if (!fileSize(fd, file_size) || file_size > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
		return storageFailureLocked("journal file size is unavailable or exhausted");
	offset = file_size;
	if (_test_fault_point == TestFaultPoint::FailPrimaryPartialWrite &&
		flush_fault == TestFaultPoint::FailPrimaryFlush)
	{
		if (!writeAll(fd, frame.data(), frame.size(), _test_write_chunk,
			_test_fail_after_write_bytes))
			return storageFailureLocked("injected partial primary WAL write failure");
	}
	else if (!writeAll(fd, frame.data(), frame.size(), _test_write_chunk))
		return storageFailureLocked("failed to append a complete journal frame");
	if (_test_fault_point == flush_fault)
		return storageFailureLocked("injected journal durability flush failure");
	if (!syncFd(fd))
		return storageFailureLocked("journal durability flush failed");
	return Status::Ok;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::writeInFlightMarkerLocked(
	RecordType type, uint64_t primary_sequence, const std::string& prepared_payload)
{
#ifdef _WIN32
	(void)type; (void)primary_sequence; (void)prepared_payload;
	return Status::PlatformUnsupported;
#else
	int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int marker_fd = -1;
	do
	{
		marker_fd = openat(_directory_fd, _inflight_name.c_str(), flags, 0600);
	} while (marker_fd < 0 && errno == EINTR);
	if (marker_fd < 0)
	{
		if (errno == EEXIST)
		{
			captureInFlightMarker(_directory_fd, _inflight_name, _recovery_inspection);
			_has_recovery_inspection = true;
			return Status::RecoveryRequired;
		}
		return storageFailureLocked("unable to create exclusive in-flight marker");
	}
	// Once creation succeeds, even a later permission/identity check failure
	// leaves an on-disk artifact that requires inspection after this call.
	// Do not claim its contents are valid until the marker has been prepared.
	_recovery_inspection.has_in_flight_marker = true;
	_recovery_inspection.requires_manual_recovery = true;
	_recovery_inspection.in_flight_marker_bytes.clear();
	_has_recovery_inspection = true;
	if (_test_fault_point == TestFaultPoint::FailMarkerPermissions || fchmod(marker_fd, 0600) != 0)
	{
		::close(marker_fd);
		return storageFailureLocked("unable to set in-flight marker permissions");
	}
	uint64_t marker_device = 0, marker_inode = 0;
	if (!validatePrivateFile(marker_fd, marker_device, marker_inode))
	{
		::close(marker_fd);
		return storageFailureLocked("in-flight marker is not a private single-link file");
	}
	_inflight_device = marker_device;
	_inflight_inode = marker_inode;
	const std::string frame = makeFrame(kMarkerMagic, kMarkerVersion,
		static_cast<uint16_t>(type), prepared_payload, primary_sequence);
	_recovery_inspection.has_in_flight_marker = true;
	_recovery_inspection.requires_manual_recovery = true;
	_recovery_inspection.in_flight_marker_bytes = frame;
	_has_recovery_inspection = true;
	if (!writeAll(marker_fd, frame.data(), frame.size(), _test_write_chunk))
	{
		::close(marker_fd);
		return storageFailureLocked("failed to write in-flight marker completely");
	}
	if (_test_fault_point == TestFaultPoint::FailMarkerFlush || !syncFd(marker_fd))
	{
		::close(marker_fd);
		return storageFailureLocked("in-flight marker flush failed; primary WAL was not touched");
	}
	::close(marker_fd);
	if (_test_fault_point == TestFaultPoint::FailMarkerCreateDirectoryFlush ||
		!syncDirectory(_directory_fd))
		return storageFailureLocked("in-flight marker directory flush failed; primary WAL was not touched");
	return Status::Ok;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::writeCleanupMarkerLocked(
	RecordType type, uint64_t primary_sequence, const std::string& prepared_payload)
{
#ifdef _WIN32
	(void)type; (void)primary_sequence; (void)prepared_payload;
	return Status::PlatformUnsupported;
#else
	int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
	flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
	flags |= O_NOFOLLOW;
#endif
	int marker_fd = -1;
	do
	{
		marker_fd = openat(_directory_fd, _cleanup_name.c_str(), flags, 0600);
	} while (marker_fd < 0 && errno == EINTR);
	if (marker_fd < 0)
	{
		if (errno == EEXIST)
		{
			captureCleanupMarker(_directory_fd, _cleanup_name, _recovery_inspection);
			_has_recovery_inspection = true;
			return Status::RecoveryRequired;
		}
		return storageFailureLocked("unable to create exclusive cleanup uncertainty marker");
	}
	if (fchmod(marker_fd, 0600) != 0)
	{
		::close(marker_fd);
		return storageFailureLocked("unable to set cleanup marker permissions");
	}
	uint64_t marker_device = 0, marker_inode = 0;
	if (!validatePrivateFile(marker_fd, marker_device, marker_inode))
	{
		::close(marker_fd);
		return storageFailureLocked("cleanup marker is not a private single-link file");
	}
	_cleanup_device = marker_device;
	_cleanup_inode = marker_inode;
	const std::string frame = makeFrame(kCleanupMagic, kMarkerVersion,
		static_cast<uint16_t>(type), prepared_payload, primary_sequence);
	_recovery_inspection.has_cleanup_marker = true;
	_recovery_inspection.cleanup_uncertain = true;
	_recovery_inspection.requires_manual_recovery = true;
	_recovery_inspection.cleanup_marker_bytes = frame;
	_has_recovery_inspection = true;
	if (!writeAll(marker_fd, frame.data(), frame.size(), _test_write_chunk))
	{
		::close(marker_fd);
		return storageFailureLocked("failed to write cleanup uncertainty marker completely");
	}
	if (_test_fault_point == TestFaultPoint::FailCleanupMarkerFlush || !syncFd(marker_fd))
	{
		::close(marker_fd);
		return storageFailureLocked("cleanup marker flush failed while in-flight marker remains");
	}
	::close(marker_fd);
	if (_test_fault_point == TestFaultPoint::FailCleanupMarkerCreateDirectoryFlush ||
		!syncDirectory(_directory_fd))
		return storageFailureLocked("cleanup marker directory flush failed while in-flight marker remains");
	return Status::Ok;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::clearInFlightMarkerLocked()
{
#ifdef _WIN32
	return Status::PlatformUnsupported;
#else
	uint64_t marker_device = 0, marker_inode = 0;
	if (!pathIdentityAt(_directory_fd, _inflight_name, marker_device, marker_inode) ||
		marker_device != _inflight_device || marker_inode != _inflight_inode)
	{
		_last_error = "in-flight marker identity changed; manual recovery is required";
		_recovery_inspection.cleanup_uncertain = true;
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		return Status::RecoveryRequired;
	}
	int unlink_result = -1;
	do
	{
		unlink_result = unlinkat(_directory_fd, _inflight_name.c_str(), 0);
	} while (unlink_result != 0 && errno == EINTR);
	if (unlink_result != 0)
	{
		_last_error = "unable to remove committed in-flight marker; manual recovery is required";
		_recovery_inspection.cleanup_uncertain = true;
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		return Status::RecoveryRequired;
	}
	if (_test_fault_point == TestFaultPoint::FailMarkerClearDirectoryFlush ||
		!syncDirectory(_directory_fd))
	{
		_last_error = "in-flight marker removal directory flush failed; cleanup uncertainty requires manual recovery";
		_recovery_inspection.has_in_flight_marker = false;
		_recovery_inspection.in_flight_marker_bytes.clear();
		_recovery_inspection.has_cleanup_marker = true;
		_recovery_inspection.cleanup_uncertain = true;
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		return Status::RecoveryRequired;
	}
	_recovery_inspection.has_in_flight_marker = false;
	_recovery_inspection.in_flight_marker_bytes.clear();
	_inflight_device = 0;
	_inflight_inode = 0;
	return Status::Ok;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::clearCleanupMarkerLocked()
{
#ifdef _WIN32
	return Status::PlatformUnsupported;
#else
	uint64_t marker_device = 0, marker_inode = 0;
	if (!pathIdentityAt(_directory_fd, _cleanup_name, marker_device, marker_inode) ||
		marker_device != _cleanup_device || marker_inode != _cleanup_inode)
	{
		_last_error = "cleanup marker identity changed; manual recovery is required";
		_recovery_inspection.cleanup_uncertain = true;
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		return Status::RecoveryRequired;
	}
	int unlink_result = -1;
	if (_test_fault_point != TestFaultPoint::FailCleanupMarkerRemove)
	{
		do
		{
			unlink_result = unlinkat(_directory_fd, _cleanup_name.c_str(), 0);
		} while (unlink_result != 0 && errno == EINTR);
	}
	if (unlink_result != 0)
	{
		_last_error = "unable to remove cleanup marker; manual recovery is required";
		_recovery_inspection.cleanup_uncertain = true;
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		return Status::RecoveryRequired;
	}
	// The preceding directory fsync durably removed the in-flight marker. Do not
	// fsync after removing this final conservative marker: if that unlink is
	// resurrected by a crash it only forces extra manual recovery, never a clean
	// state after an unconfirmed in-flight-marker removal.
	_recovery_inspection.has_cleanup_marker = false;
	_recovery_inspection.cleanup_uncertain = false;
	_recovery_inspection.cleanup_marker_bytes.clear();
	_recovery_inspection.requires_manual_recovery = false;
	_has_recovery_inspection = false;
	_cleanup_device = 0;
	_cleanup_inode = 0;
	_recovery_inspection = RecoveryInspection();
	return Status::Ok;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::appendGuardedFrameLocked(
	RecordType type, const std::string& payload, uint64_t& sequence)
{
	sequence = 0;
	if (!_opened || !_healthy)
		return _opened ? Status::StorageFailed : Status::NotOpen;
	if (!validatePathsLocked())
		return storageFailureLocked("journal identity or private permissions changed; refusing mutation");
	if (payload.size() > kMaxReportBytes)
		return Status::InvalidArgument;
	if (_last_sequence >= std::numeric_limits<uint64_t>::max() - 2 ||
		_guard_sequence >= std::numeric_limits<uint64_t>::max() - 3)
	{
		_healthy = false;
		_last_error = "journal sequence space is exhausted; refusing further writes";
		return Status::SequenceExhausted;
	}
	if (type == RecordType::Bootstrap && (!_created_new || _last_sequence != 0 || _guard_sequence != 0))
		return Status::Corrupt;
	if (type == RecordType::Acknowledgement && _pending.empty())
		return Status::StaleHighWater;
	auto exposeWriteFailureRecovery = [this]() {
		const std::string detail = _last_error;
		const RecoveryInspection marker_evidence = _recovery_inspection;
		_healthy = false;
		const Status inspection_status = scanLocked(nullptr);
		if (inspection_status != Status::RecoveryRequired)
		{
			// A write/flush error after in-flight marker creation begins is
			// ambiguous. Retain that marker evidence even if a later read scan
			// happens to observe only a clean prefix.
			_recovery_inspection = marker_evidence;
			_recovery_inspection.requires_manual_recovery = true;
			_has_recovery_inspection = true;
			_healthy = false;
		}
		if (!detail.empty())
			_last_error = detail;
	};

	const uint64_t primary_sequence = _last_sequence + 1;
	const uint64_t prepare_sequence = _guard_sequence + 1;
	const uint64_t commit_sequence = _guard_sequence + 2;
	const std::string prepare_payload = makePreparePayload(
		static_cast<uint16_t>(type), primary_sequence, payload);
	Status status = writeInFlightMarkerLocked(type, primary_sequence, prepare_payload);
	if (status != Status::Ok)
	{
		if (status == Status::RecoveryRequired)
			_last_error = "an in-flight marker already exists; manual recovery is required";
		else if (_last_error.empty())
			_last_error = "unable to durably prepare journal mutation";
		_healthy = false;
		if (_has_recovery_inspection)
			exposeWriteFailureRecovery();
		return status;
	}
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (type == RecordType::Bootstrap) wtJournalBootstrapTestHook(5);
#endif

	std::string prepare_frame;
	uint64_t guard_offset = 0;
	status = appendRawFrameLocked(_guard_fd, kGuardMagic, kGuardVersion,
		static_cast<uint16_t>(GuardRecordType::Prepare), prepare_payload,
		prepare_sequence, TestFaultPoint::FailPrepareFlush, prepare_frame, guard_offset);
	if (status != Status::Ok)
	{
		exposeWriteFailureRecovery();
		return status;
	}
	_guard_sequence = prepare_sequence;
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (type == RecordType::Bootstrap) wtJournalBootstrapTestHook(6);
#endif

	std::string primary_frame;
	uint64_t primary_offset = 0;
	status = appendRawFrameLocked(_fd, kPrimaryMagic, kPrimaryVersion,
		static_cast<uint16_t>(type), payload, primary_sequence,
		TestFaultPoint::FailPrimaryFlush, primary_frame, primary_offset);
	if (status != Status::Ok)
	{
		exposeWriteFailureRecovery();
		return status;
	}
	const uint32_t primary_checksum = getU32(primary_frame.data() + 24);
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (type == RecordType::Bootstrap) wtJournalBootstrapTestHook(7);
#endif

	const std::string commit_payload = makeCommitPayload(
		prepare_sequence, primary_sequence, primary_checksum);
	std::string commit_frame;
	status = appendRawFrameLocked(_guard_fd, kGuardMagic, kGuardVersion,
		static_cast<uint16_t>(GuardRecordType::Commit), commit_payload,
		commit_sequence, TestFaultPoint::FailCommitFlush, commit_frame, guard_offset);
	if (status != Status::Ok)
	{
		exposeWriteFailureRecovery();
		return status;
	}
	_guard_sequence = commit_sequence;
	_last_sequence = primary_sequence;
#ifdef WT_JOURNAL_BOOTSTRAP_TEST_HOOKS
	if (type == RecordType::Bootstrap) wtJournalBootstrapTestHook(8);
#endif

	if (type == RecordType::RawReport)
		_pending.push_back(Report{ primary_sequence, payload, true, true });
	else if (type == RecordType::Acknowledgement)
	{
		Acknowledgement record;
		if (!appendAuditAckPayload(payload, record))
			return storageFailureLocked("internal acknowledgement payload is invalid");
		record.sequence = primary_sequence;
		record.committed = true;
		record.guard_commit_matches_primary = true;
		_pending.clear();
		_acknowledged_high_water = record.acknowledged_high_water;
		_acknowledgements.push_back(record);
	}
	auto exposeCleanupRecovery = [this]() {
		const std::string detail = _last_error;
		_healthy = false;
		scanLocked(nullptr);
		if (!detail.empty())
			_last_error = detail;
		return Status::RecoveryRequired;
	};
	status = writeCleanupMarkerLocked(type, primary_sequence, prepare_payload);
	if (status != Status::Ok)
		return exposeCleanupRecovery();
	status = clearInFlightMarkerLocked();
	if (status != Status::Ok)
		return exposeCleanupRecovery();
	status = clearCleanupMarkerLocked();
	if (status != Status::Ok)
		return exposeCleanupRecovery();
	sequence = primary_sequence;
	return Status::Ok;
}

UnresolvedReportJournal::Status UnresolvedReportJournal::scanLocked(std::string* error)
{
#ifdef _WIN32
	(void)error;
	return Status::PlatformUnsupported;
#else
	const bool marker_present = _recovery_inspection.has_in_flight_marker;
	const bool cleanup_present = _recovery_inspection.has_cleanup_marker;
	const std::string marker_bytes = _recovery_inspection.in_flight_marker_bytes;
	const std::string cleanup_bytes = _recovery_inspection.cleanup_marker_bytes;
	const std::string marker_issue = _recovery_inspection.guard_issue;
	_recovery_inspection = RecoveryInspection();
	_recovery_inspection.has_in_flight_marker = marker_present;
	_recovery_inspection.has_cleanup_marker = cleanup_present;
	_recovery_inspection.cleanup_uncertain = cleanup_present;
	_recovery_inspection.requires_manual_recovery = marker_present || cleanup_present;
	_recovery_inspection.in_flight_marker_bytes = marker_bytes;
	_recovery_inspection.cleanup_marker_bytes = cleanup_bytes;
	if (!marker_issue.empty())
		_recovery_inspection.guard_issue = marker_issue;
	FrameScan primary = scanFrames(_fd, kPrimaryMagic, kPrimaryVersion, 1, 3,
		kMaxReportBytes);
	FrameScan guard = scanFrames(_guard_fd, kGuardMagic, kGuardVersion, 1, 2,
		kMaxFramePayloadBytes);
	_recovery_inspection.primary_valid_prefix = primary.valid_prefix;
	_recovery_inspection.guard_valid_prefix = guard.valid_prefix;
	_recovery_inspection.primary_bad_offset = primary.bad_offset;
	_recovery_inspection.guard_bad_offset = guard.bad_offset;
	_recovery_inspection.primary_issue = primary.issue;
	if (!guard.issue.empty())
		_recovery_inspection.guard_issue = guard.issue;
	bool recovery = !primary.issue.empty() || !guard.issue.empty() || marker_present || cleanup_present;
	std::string recoveryReason;
	if (!primary.issue.empty()) recoveryReason = "primary WAL: " + primary.issue;
	if (!guard.issue.empty())
	{
		if (!recoveryReason.empty()) recoveryReason += "; ";
		recoveryReason += "guard WAL: " + guard.issue;
	}
	if (marker_present && recoveryReason.empty())
		recoveryReason = "in-flight marker requires manual recovery";
	if (cleanup_present && recoveryReason.empty())
		recoveryReason = "cleanup uncertainty marker requires manual recovery";
	if (marker_present && _recovery_inspection.guard_issue.empty())
		_recovery_inspection.guard_issue = "in-flight marker requires manual recovery";
	if (cleanup_present && _recovery_inspection.guard_issue.empty())
		_recovery_inspection.guard_issue = "cleanup uncertainty marker requires manual recovery";

	struct PrimaryRecord
	{
		uint16_t type = 0;
		uint64_t sequence = 0;
		uint32_t checksum = 0;
		std::string payload;
		bool committed = false;
	};
	std::vector<PrimaryRecord> primary_records;
	for (const ParsedFrame& frame : primary.frames)
	{
		PrimaryRecord record;
		record.type = frame.type;
		record.sequence = frame.sequence;
		record.checksum = frame.checksum;
		record.payload = frame.payload;
		primary_records.push_back(record);
		if (frame.type == static_cast<uint16_t>(RecordType::RawReport))
			_recovery_inspection.primary_reports.push_back(
				Report{ frame.sequence, frame.payload, false, false });
		else if (frame.type == static_cast<uint16_t>(RecordType::Acknowledgement))
		{
			Acknowledgement ack;
			if (!appendAuditAckPayload(frame.payload, ack))
			{
				recovery = true;
				if (recoveryReason.empty()) recoveryReason = "malformed primary acknowledgement payload";
				continue;
			}
			ack.sequence = frame.sequence;
			ack.committed = false;
			_recovery_inspection.primary_acknowledgements.push_back(ack);
		}
	}

	if (primary_records.empty() ||
		primary_records.front().type != static_cast<uint16_t>(RecordType::Bootstrap) ||
		primary_records.front().sequence != 1 ||
		!validAuditText(primary_records.front().payload))
	{
		recovery = true;
		if (recoveryReason.empty()) recoveryReason = "primary WAL lacks explicit bootstrap record";
	}

	_pending.clear();
	_acknowledgements.clear();
	_last_sequence = 0;
	_acknowledged_high_water = 0;
	_guard_sequence = guard.frames.empty() ? 0 : guard.frames.back().sequence;
	std::vector<bool> matched(primary_records.size(), false);
	bool have_bootstrap = false;
	for (size_t index = 0; index < guard.frames.size();)
	{
		const ParsedFrame& prepare_frame = guard.frames[index];
		if (prepare_frame.type != static_cast<uint16_t>(GuardRecordType::Prepare))
		{
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "guard WAL expected PREPARE";
			break;
		}
		uint16_t primary_type = 0;
		uint64_t primary_sequence = 0;
		std::string payload;
		if (!parsePreparePayload(prepare_frame.payload, primary_type, primary_sequence, payload) ||
			primary_type < static_cast<uint16_t>(RecordType::Bootstrap) || primary_type > 3 ||
			primary_sequence == 0)
		{
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "malformed guard PREPARE";
			break;
		}
		auto retainUnresolvedPrepare = [&]() {
			if (primary_type == static_cast<uint16_t>(RecordType::RawReport))
				_recovery_inspection.unresolved_prepares.push_back(
					Report{ primary_sequence, payload, false });
		};
			if (index + 1 >= guard.frames.size())
			{
			retainUnresolvedPrepare();
				recovery = true;
				if (recoveryReason.empty()) recoveryReason = "guard PREPARE has no COMMIT";
				break;
			}
		const ParsedFrame& commit_frame = guard.frames[index + 1];
		if (commit_frame.type != static_cast<uint16_t>(GuardRecordType::Commit))
		{
			retainUnresolvedPrepare();
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "guard PREPARE is not followed by COMMIT";
			break;
		}
		uint64_t committed_prepare = 0, committed_primary = 0;
		uint32_t committed_checksum = 0;
		if (!parseCommitPayload(commit_frame.payload, committed_prepare,
			committed_primary, committed_checksum) ||
			committed_prepare != prepare_frame.sequence ||
			committed_primary != primary_sequence)
		{
			retainUnresolvedPrepare();
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "guard COMMIT does not match PREPARE";
			index += 2;
			continue;
		}
		if (primary_sequence > primary_records.size())
		{
			retainUnresolvedPrepare();
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "guard COMMIT has no matching primary WAL record";
			index += 2;
			continue;
		}
		PrimaryRecord& primary_record = primary_records[static_cast<size_t>(primary_sequence - 1)];
		if (primary_record.sequence != primary_sequence || primary_record.type != primary_type ||
			primary_record.payload != payload || primary_record.checksum != committed_checksum)
		{
			retainUnresolvedPrepare();
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "primary WAL record does not match guard COMMIT";
			index += 2;
			continue;
		}
		primary_record.committed = true;
		matched[static_cast<size_t>(primary_sequence - 1)] = true;
		for (Report& candidate : _recovery_inspection.primary_reports)
		{
			if (candidate.sequence == primary_sequence)
				candidate.guard_commit_matches_primary = true;
		}
		for (Acknowledgement& candidate : _recovery_inspection.primary_acknowledgements)
		{
			if (candidate.sequence == primary_sequence)
				candidate.guard_commit_matches_primary = true;
		}
		if (primary_type == static_cast<uint16_t>(RecordType::Bootstrap))
		{
			if (have_bootstrap || primary_sequence != 1)
			{
				recovery = true;
				if (recoveryReason.empty()) recoveryReason = "duplicate or misplaced bootstrap transaction";
			}
			have_bootstrap = true;
		}
		else if (!have_bootstrap)
		{
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "journal mutation precedes bootstrap";
		}
		else if (primary_type == static_cast<uint16_t>(RecordType::RawReport))
		{
			if (payload.empty())
			{
				recovery = true;
				if (recoveryReason.empty()) recoveryReason = "empty committed report payload";
			}
			else
				_pending.push_back(Report{ primary_sequence, payload, true, true });
		}
		else
		{
			Acknowledgement ack;
			if (!appendAuditAckPayload(payload, ack) || _pending.empty() ||
				ack.acknowledged_high_water != _pending.back().sequence ||
				ack.acknowledged_high_water <= _acknowledged_high_water)
			{
				recovery = true;
				if (recoveryReason.empty()) recoveryReason = "committed ACK has invalid high-water";
			}
			else
			{
				ack.sequence = primary_sequence;
				ack.committed = true;
				ack.guard_commit_matches_primary = true;
				_pending.clear();
				_acknowledged_high_water = ack.acknowledged_high_water;
				_acknowledgements.push_back(ack);
			}
		}
		index += 2;
	}

	if (!have_bootstrap)
	{
		recovery = true;
		if (recoveryReason.empty()) recoveryReason = "guard WAL has no committed bootstrap";
	}
	if (!primary_records.empty() && primary_records.back().sequence >=
		std::numeric_limits<uint64_t>::max() - 2)
	{
		recovery = true;
		if (recoveryReason.empty()) recoveryReason = "primary journal sequence space is exhausted";
	}
	if (!guard.frames.empty() && guard.frames.back().sequence >=
		std::numeric_limits<uint64_t>::max() - 3)
	{
		recovery = true;
		if (recoveryReason.empty()) recoveryReason = "guard journal sequence space is exhausted";
	}
	for (size_t i = 0; i < primary_records.size(); ++i)
	{
		if (!matched[i])
		{
			recovery = true;
			if (recoveryReason.empty()) recoveryReason = "primary WAL contains an uncommitted record";
		}
	}
	if (marker_present || cleanup_present)
	{
		recovery = true;
		if (recoveryReason.empty()) recoveryReason = "journal cleanup state requires manual recovery";
	}
	if (recovery)
	{
		_recovery_inspection.requires_manual_recovery = true;
		_has_recovery_inspection = true;
		_healthy = false;
		_last_error = recoveryReason.empty() ? "journal requires manual recovery" : recoveryReason;
		if (error) *error = _last_error;
		return Status::RecoveryRequired;
	}
	for (Report& report : _recovery_inspection.primary_reports)
		report.committed = true;
	for (Acknowledgement& ack : _recovery_inspection.primary_acknowledgements)
		ack.committed = true;
	_last_sequence = primary_records.back().sequence;
	_healthy = true;
	_has_recovery_inspection = false;
	_recovery_inspection = RecoveryInspection();
	return Status::Ok;
#endif
}

bool UnresolvedReportJournal::validatePathsLocked(bool require_no_markers) const
{
#ifdef _WIN32
	return false;
#else
	if (_directory_fd < 0 || _fd < 0 || _guard_fd < 0 || _lock_fd < 0)
		return false;
	uint64_t directory_device = 0, directory_inode = 0;
	uint64_t file_device = 0, file_inode = 0;
	uint64_t guard_device = 0, guard_inode = 0;
	uint64_t lock_device = 0, lock_inode = 0;
	if (!validatePrivateDirectory(_directory_fd, directory_device, directory_inode) ||
		!validatePrivateFile(_fd, file_device, file_inode) ||
		!validatePrivateFile(_guard_fd, guard_device, guard_inode) ||
		!validatePrivateFile(_lock_fd, lock_device, lock_inode))
		return false;
	uint64_t named_device = 0, named_inode = 0;
	const bool base_paths_valid = directory_device == _directory_device && directory_inode == _directory_inode &&
		directoryIdentityAtPath(_directory_path, _directory_device, _directory_inode) &&
		file_device == _file_device && file_inode == _file_inode &&
		guard_device == _guard_device && guard_inode == _guard_inode &&
		lock_device == _lock_device && lock_inode == _lock_inode &&
		pathIdentityAt(_directory_fd, _wal_name, named_device, named_inode) &&
		named_device == _file_device && named_inode == _file_inode &&
		pathIdentityAt(_directory_fd, _guard_name, named_device, named_inode) &&
		named_device == _guard_device && named_inode == _guard_inode &&
		pathIdentityAt(_directory_fd, _lock_name, named_device, named_inode) &&
		named_device == _lock_device && named_inode == _lock_inode;
	if (!base_paths_valid || !require_no_markers)
		return base_paths_valid;
	return componentStateAt(_directory_fd, _inflight_name,
		_test_fault_point == TestFaultPoint::FailComponentProbe) == ComponentState::Absent &&
		componentStateAt(_directory_fd, _cleanup_name,
			_test_fault_point == TestFaultPoint::FailCleanupProbe) == ComponentState::Absent;
#endif
}

UnresolvedReportJournal::Status UnresolvedReportJournal::storageFailureLocked(
	const std::string& message)
{
	_healthy = false;
	_last_error = message;
	return Status::StorageFailed;
}

NS_WTP_END
