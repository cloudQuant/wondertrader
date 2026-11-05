#include "DiffOrderOwnershipV1.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

NS_WTP_BEGIN

namespace
{
const uint8_t kMagic[4] = { 'D', 'O', 'W', '4' };
// Draft frame v4 records the evidence reference for the required durable
// local-order-id floor. Earlier draft formats are intentionally rejected.
const uint16_t kFormatVersion = 4;
const uint32_t kMaxPayloadBytes = 1024 * 1024;
const uint32_t kMaxStringBytes = 4096;
const size_t kFrameHeaderBytes = 24;

bool validText(const std::string& value, bool allowEmpty = false)
{
	if ((!allowEmpty && value.empty()) || value.size() > kMaxStringBytes)
		return false;
	return value.find('\0') == std::string::npos;
}

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

bool putDouble(std::string& out, double value)
{
	if (!std::numeric_limits<double>::is_iec559 || !std::isfinite(value))
		return false;
	uint64_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "IEEE-754 double width required");
	std::memcpy(&bits, &value, sizeof(bits));
	putU64(out, bits);
	return true;
}

bool putString(std::string& out, const std::string& value, bool allowEmpty = false)
{
	if (!validText(value, allowEmpty))
		return false;
	putU32(out, static_cast<uint32_t>(value.size()));
	out.append(value);
	return true;
}

class PayloadReader
{
public:
	PayloadReader(const std::string& bytes) : _bytes(bytes), _offset(0) {}

	bool getU32(uint32_t& value)
	{
		if (_bytes.size() - _offset < 4)
			return false;
		value = 0;
		for (unsigned i = 0; i < 4; ++i)
			value |= static_cast<uint32_t>(static_cast<uint8_t>(_bytes[_offset++])) << (i * 8);
		return true;
	}

	bool getU8(uint8_t& value)
	{
		if (_bytes.size() - _offset < 1)
			return false;
		value = static_cast<uint8_t>(_bytes[_offset++]);
		return true;
	}

	bool getU64(uint64_t& value)
	{
		if (_bytes.size() - _offset < 8)
			return false;
		value = 0;
		for (unsigned i = 0; i < 8; ++i)
			value |= static_cast<uint64_t>(static_cast<uint8_t>(_bytes[_offset++])) << (i * 8);
		return true;
	}

	bool getDouble(double& value)
	{
		uint64_t bits = 0;
		if (!getU64(bits))
			return false;
		std::memcpy(&value, &bits, sizeof(value));
		return std::numeric_limits<double>::is_iec559 && std::isfinite(value);
	}

	bool getString(std::string& value, bool allowEmpty = false)
	{
		uint32_t size = 0;
		if (!getU32(size) || size > kMaxStringBytes || _bytes.size() - _offset < size)
			return false;
		value.assign(_bytes.data() + _offset, size);
		_offset += size;
		return validText(value, allowEmpty);
	}

	bool atEnd() const { return _offset == _bytes.size(); }

private:
	const std::string& _bytes;
	size_t _offset;
};

uint32_t crc32(const uint8_t* data, size_t size, uint32_t crc = 0xffffffffu)
{
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return crc;
}

uint32_t frameChecksum(const std::string& header, const std::string& payload)
{
	uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(header.data()), 20);
	crc = crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), crc);
	return crc ^ 0xffffffffu;
}

uint32_t getU32(const char* data)
{
	return static_cast<uint32_t>(static_cast<uint8_t>(data[0])) |
		(static_cast<uint32_t>(static_cast<uint8_t>(data[1])) << 8) |
		(static_cast<uint32_t>(static_cast<uint8_t>(data[2])) << 16) |
		(static_cast<uint32_t>(static_cast<uint8_t>(data[3])) << 24);
}

uint16_t getU16(const char* data)
{
	return static_cast<uint16_t>(static_cast<uint8_t>(data[0])) |
		(static_cast<uint16_t>(static_cast<uint8_t>(data[1])) << 8);
}

uint64_t getU64(const char* data)
{
	uint64_t value = 0;
	for (unsigned i = 0; i < 8; ++i)
		value |= static_cast<uint64_t>(static_cast<uint8_t>(data[i])) << (i * 8);
	return value;
}

void putU32At(std::string& out, size_t offset, uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		out[offset + i] = static_cast<char>((value >> (i * 8)) & 0xff);
}

bool makeFrame(uint16_t type, uint64_t sequence, const std::string& payload,
	std::string& frame)
{
	if (payload.size() > kMaxPayloadBytes)
		return false;
	frame.assign(reinterpret_cast<const char*>(kMagic), sizeof(kMagic));
	putU16(frame, kFormatVersion);
	putU16(frame, type);
	putU64(frame, sequence);
	putU32(frame, static_cast<uint32_t>(payload.size()));
	putU32(frame, 0);
	frame.append(payload);
	putU32At(frame, 20, frameChecksum(frame, payload));
	return true;
}

#ifdef _WIN32
bool getFileSize(HANDLE handle, uint64_t& size)
{
	LARGE_INTEGER value;
	if (!GetFileSizeEx(handle, &value) || value.QuadPart < 0)
		return false;
	size = static_cast<uint64_t>(value.QuadPart);
	return true;
}

bool readExactAt(HANDLE handle, uint64_t offset, char* output, size_t size)
{
	LARGE_INTEGER position;
	position.QuadPart = static_cast<LONGLONG>(offset);
	if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN))
		return false;
	while (size != 0)
	{
		DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1024 * 1024));
		DWORD readBytes = 0;
		if (!ReadFile(handle, output, chunk, &readBytes, NULL) || readBytes == 0)
			return false;
		output += readBytes;
		size -= readBytes;
	}
	return true;
}

bool writeAll(HANDLE handle, const char* data, size_t size)
{
	LARGE_INTEGER end;
	end.QuadPart = 0;
	if (!SetFilePointerEx(handle, end, NULL, FILE_END))
		return false;
	while (size != 0)
	{
		DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1024 * 1024));
		DWORD written = 0;
		if (!WriteFile(handle, data, chunk, &written, NULL) || written == 0)
			return false;
		data += written;
		size -= written;
	}
	return true;
}
#else
bool getFileSize(int fd, uint64_t& size)
{
	struct stat info;
	if (fstat(fd, &info) != 0 || info.st_size < 0)
		return false;
	size = static_cast<uint64_t>(info.st_size);
	return true;
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

bool writeAll(int fd, const char* data, size_t size)
{
	if (lseek(fd, 0, SEEK_END) < 0)
		return false;
	while (size != 0)
	{
		const ssize_t written = write(fd, data, size);
		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return false;
		data += written;
		size -= static_cast<size_t>(written);
	}
	return true;
}

bool syncParentDirectory(const std::string& filePath)
{
	const size_t slash = filePath.find_last_of('/');
	const std::string directory = slash == std::string::npos ? "." :
		(slash == 0 ? "/" : filePath.substr(0, slash));
	const int fd = open(directory.c_str(), O_RDONLY
#ifdef O_DIRECTORY
		| O_DIRECTORY
#endif
	);
	if (fd < 0)
		return false;
	const bool result = fsync(fd) == 0;
	close(fd);
	return result;
}
#endif

bool encodeDoubleFields(std::string& payload, double a, double b)
{
	return putDouble(payload, a) && putDouble(payload, b);
}

bool encodeDoubleFields(std::string& payload, double a, double b, double c)
{
	return putDouble(payload, a) && putDouble(payload, b) && putDouble(payload, c);
}

bool encodeDoubleFields(std::string& payload, double a, double b, double c, double d, double e)
{
	return putDouble(payload, a) && putDouble(payload, b) && putDouble(payload, c) &&
		putDouble(payload, d) && putDouble(payload, e);
}

double quantityTolerance(double total)
{
	return std::max(1.0, std::fabs(total)) * 1e-9;
}
}

enum class DiffOrderOwnershipJournalV1::RecordType : uint16_t
{
	Header = 1,
	OrderIntent = 2,
	OrderBinding = 3,
	TradeApplied = 4,
	TargetChanged = 5,
	Baseline = 6,
	OrderProgress = 7,
	LocalIdHighWater = 8
};

DiffOrderOwnershipJournalV1::DiffOrderOwnershipJournalV1()
	: _last_sequence(0)
	, _local_order_id_high_water(0)
	, _local_order_id_floor_established(false)
	, _opened(false)
	, _healthy(false)
	, _has_header(false)
	, _fd(-1)
	, _win_handle(NULL)
	, _test_fault_point(TestFaultPoint::None)
{
}

DiffOrderOwnershipJournalV1::~DiffOrderOwnershipJournalV1()
{
	close();
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::open(
	const std::string& wal_path, const std::string& stable_account_id,
	std::string* error)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (_opened)
		return Status::Conflict;
	if (!validText(wal_path) || !validText(stable_account_id))
	{
		if (error)
			*error = "WAL path and stable account id must be non-empty text";
		return Status::InvalidArgument;
	}

	_path = wal_path;
	_account_id = stable_account_id;
	_last_sequence = 0;
	_local_order_id_high_water = 0;
	_local_order_id_floor_established = false;
	_local_order_id_floor_evidence.clear();
	_has_header = false;
	_healthy = true;
	_last_error.clear();
	_positions.clear();
	_intents.clear();
	_local_order_to_intent.clear();
	_trades.clear();

	bool createdNew = false;
#ifdef _WIN32
	const std::wstring widePath = std::filesystem::u8path(wal_path).wstring();
	HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
		NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
	if (handle == INVALID_HANDLE_VALUE)
	{
		DWORD osError = GetLastError();
		if (osError == ERROR_FILE_EXISTS || osError == ERROR_ALREADY_EXISTS)
			handle = CreateFileW(widePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
				NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
		if (handle == INVALID_HANDLE_VALUE)
		{
			osError = GetLastError();
			if (error)
				*error = "unable to open exclusive ownership WAL, Win32 error " + std::to_string(osError);
			return (osError == ERROR_SHARING_VIOLATION || osError == ERROR_LOCK_VIOLATION) ?
				Status::Busy : Status::StorageFailed;
		}
	}
	else
		createdNew = true;
	_win_handle = handle;
#else
	int openFlags = O_RDWR;
#ifdef O_CLOEXEC
	openFlags |= O_CLOEXEC;
#endif
	_fd = ::open(wal_path.c_str(), openFlags | O_CREAT | O_EXCL, 0600);
	if (_fd >= 0)
		createdNew = true;
	else if (errno == EEXIST)
		_fd = ::open(wal_path.c_str(), openFlags);
	if (_fd < 0)
	{
		if (error)
			*error = "unable to open ownership WAL: " + std::string(std::strerror(errno));
		return Status::StorageFailed;
	}
	if (flock(_fd, LOCK_EX | LOCK_NB) != 0)
	{
		if (error)
			*error = "ownership WAL is already open by another writer";
		::close(_fd);
		_fd = -1;
		return Status::Busy;
	}
#endif
	_opened = true;

	uint64_t size = 0;
#ifdef _WIN32
	const bool sized = getFileSize(static_cast<HANDLE>(_win_handle), size);
#else
	const bool sized = getFileSize(_fd, size);
#endif
	if (!sized)
	{
		const Status status = storageFailureLocked("unable to read ownership WAL size");
		if (error)
			*error = _last_error;
		closeLocked();
		return status;
	}
	if (size == 0 && !createdNew)
	{
		_last_error = "existing ownership WAL is empty; refusing to treat a truncated log as new";
		if (error)
			*error = _last_error;
		closeLocked();
		return Status::Corrupt;
	}

	Status status = replayLocked(error);
	if (status == Status::Ok && size == 0)
	{
		std::string payload;
		if (!putString(payload, _account_id))
			status = Status::InvalidArgument;
		else
		{
			uint64_t sequence = 0;
			status = appendLocked(RecordType::Header, payload, sequence);
#ifndef _WIN32
			if (status == Status::Ok && !syncParentDirectory(wal_path))
				status = storageFailureLocked("unable to sync parent directory after creating ownership WAL");
#endif
		}
		if (status != Status::Ok && error)
			*error = _last_error.empty() ? "unable to initialize ownership WAL header" : _last_error;
	}
	if (status != Status::Ok)
	{
		if (_last_error.empty())
			_last_error = (error && !error->empty()) ? *error :
				"ownership WAL replay or initialization failed";
		if (error && error->empty())
			*error = _last_error;
		closeLocked();
	}
	return status;
}

void DiffOrderOwnershipJournalV1::close()
{
	std::lock_guard<std::mutex> lock(_mutex);
	closeLocked();
}

void DiffOrderOwnershipJournalV1::closeLocked()
{
#ifdef _WIN32
	if (_win_handle != NULL)
	{
		CloseHandle(static_cast<HANDLE>(_win_handle));
		_win_handle = NULL;
	}
#else
	if (_fd >= 0)
	{
		flock(_fd, LOCK_UN);
		::close(_fd);
		_fd = -1;
	}
#endif
	_opened = false;
	_healthy = false;
	_has_header = false;
	_last_sequence = 0;
	_local_order_id_high_water = 0;
	_local_order_id_floor_established = false;
	_local_order_id_floor_evidence.clear();
	_path.clear();
	_account_id.clear();
	_positions.clear();
	_intents.clear();
	_local_order_to_intent.clear();
	_trades.clear();
}

bool DiffOrderOwnershipJournalV1::isOpen() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _opened;
}

bool DiffOrderOwnershipJournalV1::isHealthy() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _opened && _healthy;
}

std::string DiffOrderOwnershipJournalV1::accountId() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _account_id;
}

std::string DiffOrderOwnershipJournalV1::lastError() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _last_error;
}

DiffOrderOwnershipJournalV1::Snapshot DiffOrderOwnershipJournalV1::snapshot() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	Snapshot result;
	result.account_id = _account_id;
	result.last_sequence = _last_sequence;
	result.local_order_id_high_water = _local_order_id_high_water;
	result.local_order_id_floor_established = _local_order_id_floor_established;
	result.local_order_id_floor_evidence = _local_order_id_floor_evidence;
	result.positions = _positions;
	result.intents = _intents;
	return result;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::prepareOrderIntent(
	const std::string& owner_id, const std::string& intent_id,
	const std::string& std_code, double signed_quantity,
	uint32_t& reserved_local_order_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	reserved_local_order_id = 0;
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(owner_id) || !validText(intent_id) || !validText(std_code) ||
		!std::isfinite(signed_quantity) || signed_quantity == 0.0)
		return Status::InvalidArgument;
	if (!_local_order_id_floor_established)
		return Status::LocalOrderIdFloorRequired;

	auto found = _intents.find(intent_id);
	if (found != _intents.end())
	{
		const OrderIntentState& prior = found->second;
		if (prior.owner_id == owner_id && prior.std_code == std_code &&
			prior.signed_quantity == signed_quantity)
		{
			reserved_local_order_id = prior.local_order_id;
			return Status::Duplicate;
		}
		return conflictFailureLocked("order intent id was reused with different ownership or order contents");
	}
	if (_local_order_id_high_water >= std::numeric_limits<uint32_t>::max() - 1)
		return Status::SequenceExhausted;
	const uint32_t localOrderId = _local_order_id_high_water + 1;

	std::string payload;
	if (!putString(payload, owner_id) || !putString(payload, intent_id) ||
		!putString(payload, std_code) || !putDouble(payload, signed_quantity))
		return Status::InvalidArgument;
	putU32(payload, localOrderId);
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::OrderIntent, payload, sequence);
	if (result != Status::Ok)
		return result;

	OrderIntentState intent;
	intent.owner_id = owner_id;
	intent.intent_id = intent_id;
	intent.std_code = std_code;
	intent.signed_quantity = signed_quantity;
	intent.local_order_id = localOrderId;
	intent.remaining_quantity = std::fabs(signed_quantity);
	intent.reported_remaining_quantity = std::fabs(signed_quantity);
	intent.last_sequence = sequence;
	_intents.emplace(intent_id, intent);
	_local_order_to_intent.emplace(localOrderId, intent_id);
	_local_order_id_high_water = localOrderId;
	reserved_local_order_id = localOrderId;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::advanceLocalOrderIdHighWater(
	uint32_t observed_high_water, const std::string& complete_source_evidence)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (observed_high_water == std::numeric_limits<uint32_t>::max() ||
		!validText(complete_source_evidence))
		return Status::InvalidArgument;
	if (_local_order_id_floor_established && observed_high_water == _local_order_id_high_water)
		return complete_source_evidence == _local_order_id_floor_evidence ? Status::Duplicate :
			conflictFailureLocked("local order id floor evidence changed without increasing the floor");
	if (observed_high_water < _local_order_id_high_water)
		return Status::InvalidArgument;
	if (!_intents.empty())
		return conflictFailureLocked("local order id high-water cannot be advanced after intent allocation");
	std::string payload;
	putU32(payload, observed_high_water);
	if (!putString(payload, complete_source_evidence))
		return Status::InvalidArgument;
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::LocalIdHighWater, payload, sequence);
	if (result != Status::Ok)
		return result;
	_local_order_id_high_water = observed_high_water;
	_local_order_id_floor_established = true;
	_local_order_id_floor_evidence = complete_source_evidence;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::bindOrder(
	const std::string& intent_id, uint32_t local_order_id,
	const std::string& broker_order_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(intent_id) || !validText(broker_order_id, true) || local_order_id == 0)
		return Status::InvalidArgument;
	auto intent = _intents.find(intent_id);
	if (intent == _intents.end())
		return Status::UnknownIntent;
	if (intent->second.local_order_id != local_order_id)
		return conflictFailureLocked("bound local order id differs from the durable intent reservation");
	if (intent->second.order_bound)
	{
		if (intent->second.broker_order_id == broker_order_id)
			return Status::Duplicate;
		return conflictFailureLocked("order intent was rebound to a different broker identity");
	}
	auto local = _local_order_to_intent.find(local_order_id);
	if (local == _local_order_to_intent.end() || local->second != intent_id)
		return conflictFailureLocked("reserved local order id has no unique intent mapping");

	std::string payload;
	if (!putString(payload, intent_id))
		return Status::InvalidArgument;
	putU32(payload, local_order_id);
	if (!putString(payload, broker_order_id, true))
		return Status::InvalidArgument;
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::OrderBinding, payload, sequence);
	if (result != Status::Ok)
		return result;
	intent->second.local_order_id = local_order_id;
	intent->second.broker_order_id = broker_order_id;
	intent->second.order_bound = true;
	intent->second.last_sequence = sequence;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::recordOrderProgress(
	const std::string& intent_id, uint32_t status_code,
	double reported_filled_quantity, double reported_remaining_quantity,
	bool terminal)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(intent_id) || !std::isfinite(reported_filled_quantity) ||
		!std::isfinite(reported_remaining_quantity) || reported_filled_quantity < 0.0 ||
		reported_remaining_quantity < 0.0)
		return Status::InvalidArgument;
	auto intent = _intents.find(intent_id);
	if (intent == _intents.end())
		return Status::UnknownIntent;
	if (intent->second.terminal && (!terminal || intent->second.status_code != status_code))
		return conflictFailureLocked("order progress changed or regressed after a terminal state");

	const double total = std::fabs(intent->second.signed_quantity);
	const double tolerance = quantityTolerance(total);
	if (reported_filled_quantity + tolerance < intent->second.reported_filled_quantity ||
		reported_filled_quantity + reported_remaining_quantity > total + tolerance)
		return conflictFailureLocked("order progress regressed or exceeds the original order quantity");
	if (intent->second.terminal &&
		reported_remaining_quantity > intent->second.reported_remaining_quantity)
		return conflictFailureLocked("terminal order reported remaining quantity increased");
	const double newFilled = std::max(reported_filled_quantity,
		intent->second.trade_filled_quantity);
	const double boundedRemaining = std::max(0.0, total - newFilled);
	const double newRemaining = terminal ? 0.0 :
		std::min(intent->second.remaining_quantity,
			std::min(reported_remaining_quantity, boundedRemaining));
	if (intent->second.status_code == status_code &&
		intent->second.reported_filled_quantity == reported_filled_quantity &&
		intent->second.reported_remaining_quantity == reported_remaining_quantity &&
		intent->second.terminal == terminal)
		return Status::Duplicate;

	std::string payload;
	if (!putString(payload, intent_id))
		return Status::InvalidArgument;
	putU32(payload, status_code);
	putDouble(payload, reported_filled_quantity);
	putDouble(payload, reported_remaining_quantity);
	payload.push_back(terminal ? 1 : 0);
	putDouble(payload, newFilled);
	putDouble(payload, newRemaining);
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::OrderProgress, payload, sequence);
	if (result != Status::Ok)
		return result;
	intent->second.status_code = status_code;
	intent->second.reported_filled_quantity = reported_filled_quantity;
	intent->second.reported_remaining_quantity = reported_remaining_quantity;
	intent->second.filled_quantity = newFilled;
	intent->second.remaining_quantity = newRemaining;
	intent->second.terminal = terminal;
	intent->second.terminal_state = terminal ? status_code : 0;
	intent->second.last_sequence = sequence;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::applyTrade(
	const std::string& trade_key, const std::string& intent_id,
	const std::string& std_code, double signed_diff_delta)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(trade_key) || !validText(intent_id) || !validText(std_code) ||
		!std::isfinite(signed_diff_delta) || signed_diff_delta == 0.0)
		return Status::InvalidArgument;
	auto intent = _intents.find(intent_id);
	if (intent == _intents.end())
		return Status::UnknownIntent;
	if (intent->second.std_code != std_code)
		return conflictFailureLocked("trade instrument does not match its recorded order intent");
	const TradeState proposed{ intent_id, intent->second.owner_id, std_code, signed_diff_delta };
	auto priorTrade = _trades.find(trade_key);
	if (priorTrade != _trades.end())
	{
		const TradeState& old = priorTrade->second;
		if (old.intent_id == proposed.intent_id && old.owner_id == proposed.owner_id &&
			old.std_code == proposed.std_code && old.signed_delta == proposed.signed_delta)
			return Status::Duplicate;
		return conflictFailureLocked("trade key was reused with different owner or trade contents");
	}
	if ((signed_diff_delta < 0.0) != (intent->second.signed_quantity > 0.0))
		return conflictFailureLocked("trade direction does not match its recorded order intent");

	OwnerCodeKey key{ intent->second.owner_id, std_code };
	const auto position = _positions.find(key);
	const double before = position == _positions.end() ? 0.0 : position->second.diff;
	const double after = before + signed_diff_delta;
	const double tradeFilled = intent->second.trade_filled_quantity + std::fabs(signed_diff_delta);
	const double total = std::fabs(intent->second.signed_quantity);
	const double tolerance = quantityTolerance(total);
	if (!std::isfinite(after) || !std::isfinite(tradeFilled) || tradeFilled > total + tolerance)
		return conflictFailureLocked("trade fill exceeds the recorded order quantity");
	const double newFilled = std::max(intent->second.reported_filled_quantity, tradeFilled);
	const double newRemaining = intent->second.terminal ? 0.0 :
		std::min(intent->second.remaining_quantity, std::max(0.0, total - newFilled));
	std::string payload;
	if (!putString(payload, trade_key) || !putString(payload, intent_id) ||
		!putString(payload, intent->second.owner_id) || !putString(payload, std_code) ||
		!encodeDoubleFields(payload, signed_diff_delta, before, after,
			tradeFilled, newFilled) || !putDouble(payload, newRemaining))
		return Status::InvalidArgument;
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::TradeApplied, payload, sequence);
	if (result != Status::Ok)
		return result;
	PositionState& state = _positions[key];
	state.diff = after;
	state.last_sequence = sequence;
	intent->second.trade_filled_quantity = tradeFilled;
	intent->second.filled_quantity = newFilled;
	intent->second.remaining_quantity = newRemaining;
	intent->second.last_sequence = sequence;
	_trades.emplace(trade_key, proposed);
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::setTarget(
	const std::string& owner_id, const std::string& std_code, double target, double diff)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(owner_id) || !validText(std_code) || !std::isfinite(target) || !std::isfinite(diff))
		return Status::InvalidArgument;
	std::string payload;
	if (!putString(payload, owner_id) || !putString(payload, std_code) ||
		!encodeDoubleFields(payload, target, diff))
		return Status::InvalidArgument;
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::TargetChanged, payload, sequence);
	if (result != Status::Ok)
		return result;
	PositionState& state = _positions[OwnerCodeKey{ owner_id, std_code }];
	state.target = target;
	state.diff = diff;
	state.last_sequence = sequence;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::setBaseline(
	const std::string& owner_id, const std::string& std_code,
	double account_position, double target, double diff, uint32_t trading_day)
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(owner_id) || !validText(std_code) || !std::isfinite(account_position) ||
		!std::isfinite(target) || !std::isfinite(diff) || trading_day == 0)
		return Status::InvalidArgument;
	std::string payload;
	if (!putString(payload, owner_id) || !putString(payload, std_code) ||
		!encodeDoubleFields(payload, account_position, target, diff))
		return Status::InvalidArgument;
	putU32(payload, trading_day);
	uint64_t sequence = 0;
	const Status result = appendLocked(RecordType::Baseline, payload, sequence);
	if (result != Status::Ok)
		return result;
	PositionState& state = _positions[OwnerCodeKey{ owner_id, std_code }];
	state.baseline_account_position = account_position;
	state.target = target;
	state.diff = diff;
	state.baseline_trading_day = trading_day;
	state.last_sequence = sequence;
	state.has_baseline = true;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::ownerForIntent(
	const std::string& intent_id, std::string& owner_id) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	auto intent = _intents.find(intent_id);
	if (intent == _intents.end())
		return Status::UnknownIntent;
	owner_id = intent->second.owner_id;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::ownerForLocalOrder(
	uint32_t local_order_id, std::string& owner_id, std::string* intent_id) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (local_order_id == 0)
		return Status::InvalidArgument;
	auto mapping = _local_order_to_intent.find(local_order_id);
	if (mapping == _local_order_to_intent.end())
		return Status::UnknownIntent;
	auto intent = _intents.find(mapping->second);
	if (intent == _intents.end())
		return Status::Corrupt;
	owner_id = intent->second.owner_id;
	if (intent_id)
		*intent_id = intent->second.intent_id;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::checkCancelOwner(
	uint32_t local_order_id, const std::string& owner_id) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (local_order_id == 0 || !validText(owner_id))
		return Status::InvalidArgument;
	auto mapping = _local_order_to_intent.find(local_order_id);
	if (mapping == _local_order_to_intent.end())
		return Status::UnknownIntent;
	auto intent = _intents.find(mapping->second);
	if (intent == _intents.end())
		return Status::Corrupt;
	if (intent->second.owner_id != owner_id || !intent->second.order_bound || intent->second.terminal ||
		intent->second.remaining_quantity <= 0.0)
		return Status::Conflict;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::activeOrders(
	const std::string& owner_id, const std::string& std_code,
	std::vector<OrderIntentState>& orders) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(owner_id) || !validText(std_code))
		return Status::InvalidArgument;
	orders.clear();
	for (const auto& item : _intents)
	{
		const OrderIntentState& intent = item.second;
		if (intent.owner_id == owner_id && intent.std_code == std_code &&
			!intent.terminal && intent.remaining_quantity > 0.0)
			orders.push_back(intent);
	}
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::undoneQuantity(
	const std::string& owner_id, const std::string& std_code,
	double& signed_quantity) const
{
	std::lock_guard<std::mutex> lock(_mutex);
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	if (!validText(owner_id) || !validText(std_code))
		return Status::InvalidArgument;
	signed_quantity = 0.0;
	for (const auto& item : _intents)
	{
		const OrderIntentState& intent = item.second;
		if (intent.owner_id == owner_id && intent.std_code == std_code &&
			!intent.terminal && intent.remaining_quantity > 0.0)
			signed_quantity += std::copysign(intent.remaining_quantity, intent.signed_quantity);
	}
	return std::isfinite(signed_quantity) ? Status::Ok : Status::StorageFailed;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::appendLocked(
	RecordType type, const std::string& payload, uint64_t& committed_sequence)
{
	if (!_opened)
		return Status::NotOpen;
	if (!_healthy)
		return Status::StorageFailed;
	const bool header = type == RecordType::Header;
	if ((header && _has_header) || (!header && !_has_header))
		return Status::Conflict;
	if (!header && _last_sequence >= std::numeric_limits<uint64_t>::max() - 1)
		return Status::SequenceExhausted;
	const uint64_t sequence = header ? 0 : _last_sequence + 1;
	const std::string framePayload = payload;
	std::string frame;
	if (!makeFrame(static_cast<uint16_t>(type), sequence, framePayload, frame))
		return Status::InvalidArgument;
	if (_test_fault_point == TestFaultPoint::BeforeWrite)
		return storageFailureLocked("fault injection before WAL write");

#ifdef _WIN32
	const bool wrote = writeAll(static_cast<HANDLE>(_win_handle), frame.data(), frame.size());
#else
	const bool wrote = writeAll(_fd, frame.data(), frame.size());
#endif
	if (!wrote)
		return storageFailureLocked("failed to append a complete WAL frame");
	if (_test_fault_point == TestFaultPoint::BeforeFlush)
		return storageFailureLocked("fault injection before WAL durability flush");

#ifdef _WIN32
	const bool flushed = FlushFileBuffers(static_cast<HANDLE>(_win_handle)) != 0;
#else
	const bool flushed = fsync(_fd) == 0;
#endif
	if (!flushed)
		return storageFailureLocked("WAL durability flush failed");
	if (header)
		_has_header = true;
	else
		_last_sequence = sequence;
	committed_sequence = sequence;
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::replayLocked(std::string* error)
{
	uint64_t fileSize = 0;
#ifdef _WIN32
	const bool sized = getFileSize(static_cast<HANDLE>(_win_handle), fileSize);
#else
	const bool sized = getFileSize(_fd, fileSize);
#endif
	if (!sized)
		return storageFailureLocked("unable to inspect ownership WAL size");
	if (fileSize == 0)
		return Status::Ok;

	uint64_t offset = 0;
	uint64_t expectedSequence = 0;
	while (offset < fileSize)
	{
		if (fileSize - offset < kFrameHeaderBytes)
		{
			if (error)
				*error = "truncated WAL frame header";
			return Status::Corrupt;
		}
		std::string header(kFrameHeaderBytes, '\0');
#ifdef _WIN32
		const bool gotHeader = readExactAt(static_cast<HANDLE>(_win_handle), offset, &header[0], header.size());
#else
		const bool gotHeader = readExactAt(_fd, offset, &header[0], header.size());
#endif
		if (!gotHeader || std::memcmp(header.data(), kMagic, sizeof(kMagic)) != 0 ||
			getU16(header.data() + 4) != kFormatVersion)
		{
			if (error)
				*error = "invalid WAL magic or unsupported frame version";
			return Status::Corrupt;
		}
		const uint16_t typeValue = getU16(header.data() + 6);
		const uint64_t sequence = getU64(header.data() + 8);
		const uint32_t payloadSize = getU32(header.data() + 16);
		const uint32_t expectedChecksum = getU32(header.data() + 20);
		if (payloadSize > kMaxPayloadBytes || sequence != expectedSequence ||
			sequence == std::numeric_limits<uint64_t>::max() ||
			fileSize - offset - kFrameHeaderBytes < payloadSize)
		{
			if (error)
				*error = "invalid WAL frame sequence, size, or truncated payload";
			return Status::Corrupt;
		}
		std::string payload(payloadSize, '\0');
		if (payloadSize != 0)
		{
#ifdef _WIN32
			const bool gotPayload = readExactAt(static_cast<HANDLE>(_win_handle),
				offset + kFrameHeaderBytes, &payload[0], payload.size());
#else
			const bool gotPayload = readExactAt(_fd, offset + kFrameHeaderBytes,
				&payload[0], payload.size());
#endif
			if (!gotPayload)
			{
				if (error)
					*error = "unable to read complete WAL payload";
				return Status::Corrupt;
			}
		}
		if (frameChecksum(header, payload) != expectedChecksum)
		{
			if (error)
				*error = "WAL frame checksum mismatch";
			return Status::Corrupt;
		}
		if (typeValue < static_cast<uint16_t>(RecordType::Header) ||
			typeValue > static_cast<uint16_t>(RecordType::LocalIdHighWater))
		{
			if (error)
				*error = "unknown WAL record type";
			return Status::Corrupt;
		}
		const Status applied = applyRecordLocked(static_cast<RecordType>(typeValue),
			payload, sequence, error);
		if (applied != Status::Ok)
			return applied;
		_last_sequence = sequence;
		expectedSequence = sequence + 1;
		offset += kFrameHeaderBytes + payloadSize;
	}
	if (!_has_header)
	{
		if (error)
			*error = "WAL is missing its account header";
		return Status::Corrupt;
	}
	return Status::Ok;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::applyRecordLocked(
	RecordType type, const std::string& payload, uint64_t sequence, std::string* error)
{
	auto corrupt = [error](const char* message) {
		if (error)
			*error = message;
		return Status::Corrupt;
	};
	PayloadReader reader(payload);
	if (type == RecordType::Header)
	{
		std::string account;
		if (_has_header || sequence != 0 || !reader.getString(account) || !reader.atEnd())
			return corrupt("invalid or duplicate WAL account header");
		if (account != _account_id)
			return corrupt("WAL stable account id does not match the requested account");
		_has_header = true;
		return Status::Ok;
	}
	if (!_has_header || sequence == 0)
		return corrupt("owner event appears before the account header");

	if (type == RecordType::OrderIntent)
	{
		OrderIntentState intent;
		uint32_t localId = 0;
		if (!reader.getString(intent.owner_id) || !reader.getString(intent.intent_id) ||
			!reader.getString(intent.std_code) || !reader.getDouble(intent.signed_quantity) ||
			!reader.getU32(localId) || !reader.atEnd() || intent.signed_quantity == 0.0 ||
			!_local_order_id_floor_established ||
			localId == 0 || localId == std::numeric_limits<uint32_t>::max() ||
			_local_order_id_high_water >= std::numeric_limits<uint32_t>::max() - 1 ||
			localId != _local_order_id_high_water + 1 ||
			_intents.find(intent.intent_id) != _intents.end())
			return corrupt("invalid or duplicate order intent record");
		intent.local_order_id = localId;
		intent.remaining_quantity = std::fabs(intent.signed_quantity);
		intent.reported_remaining_quantity = intent.remaining_quantity;
		intent.last_sequence = sequence;
		_intents.emplace(intent.intent_id, intent);
		_local_order_to_intent.emplace(localId, intent.intent_id);
		_local_order_id_high_water = localId;
		return Status::Ok;
	}
	if (type == RecordType::OrderBinding)
	{
		std::string intentId;
		uint32_t localId = 0;
		std::string brokerId;
		if (!reader.getString(intentId) || !reader.getU32(localId) || localId == 0 ||
			!reader.getString(brokerId, true) || !reader.atEnd())
			return corrupt("invalid order binding record");
		auto intent = _intents.find(intentId);
		auto mapping = _local_order_to_intent.find(localId);
		if (intent == _intents.end() || intent->second.local_order_id != localId ||
			intent->second.order_bound || mapping == _local_order_to_intent.end() ||
			mapping->second != intentId)
			return corrupt("order binding has no unique prior intent");
		intent->second.broker_order_id = brokerId;
		intent->second.order_bound = true;
		intent->second.last_sequence = sequence;
		return Status::Ok;
	}
	if (type == RecordType::OrderProgress)
	{
		std::string intentId;
		uint32_t statusCode = 0;
		double reportedFilled = 0.0, reportedRemaining = 0.0;
		double newFilled = 0.0, newRemaining = 0.0;
		uint8_t terminalByte = 0;
		if (!reader.getString(intentId) || !reader.getU32(statusCode) ||
			!reader.getDouble(reportedFilled) || !reader.getDouble(reportedRemaining) ||
			!reader.getU8(terminalByte) || terminalByte > 1 ||
			!reader.getDouble(newFilled) || !reader.getDouble(newRemaining) ||
			!reader.atEnd() || reportedFilled < 0.0 || reportedRemaining < 0.0 ||
			newFilled < 0.0 || newRemaining < 0.0)
			return corrupt("invalid order progress record");
		auto intent = _intents.find(intentId);
		if (intent == _intents.end())
			return corrupt("order progress has no prior intent");
		const bool terminal = terminalByte != 0;
		if (intent->second.terminal &&
			(!terminal || statusCode != intent->second.status_code))
			return corrupt("order progress changed or regressed after a terminal state");
		const double total = std::fabs(intent->second.signed_quantity);
		const double tolerance = quantityTolerance(total);
		if (reportedFilled + tolerance < intent->second.reported_filled_quantity ||
			reportedFilled + reportedRemaining > total + tolerance)
			return corrupt("order progress regressed or exceeds the original order quantity");
		if (intent->second.terminal &&
			reportedRemaining > intent->second.reported_remaining_quantity)
			return corrupt("terminal order reported remaining quantity increased");
		const double expectedFilled = std::max(reportedFilled,
			intent->second.trade_filled_quantity);
		const double boundedRemaining = std::max(0.0, total - expectedFilled);
		const double expectedRemaining = terminal ? 0.0 :
			std::min(intent->second.remaining_quantity,
				std::min(reportedRemaining, boundedRemaining));
		if (newFilled != expectedFilled || newRemaining != expectedRemaining)
			return corrupt("order progress result does not match replay state");
		intent->second.status_code = statusCode;
		intent->second.reported_filled_quantity = reportedFilled;
		intent->second.reported_remaining_quantity = reportedRemaining;
		intent->second.filled_quantity = newFilled;
		intent->second.remaining_quantity = newRemaining;
		intent->second.terminal = terminal;
		intent->second.terminal_state = terminal ? statusCode : 0;
		intent->second.last_sequence = sequence;
		return Status::Ok;
	}
	if (type == RecordType::TradeApplied)
	{
		std::string tradeKey, intentId, ownerId, stdCode;
		double delta = 0.0, before = 0.0, after = 0.0;
		double tradeFilled = 0.0, newFilled = 0.0, newRemaining = 0.0;
		if (!reader.getString(tradeKey) || !reader.getString(intentId) ||
			!reader.getString(ownerId) || !reader.getString(stdCode) ||
			!reader.getDouble(delta) || !reader.getDouble(before) ||
			!reader.getDouble(after) || !reader.getDouble(tradeFilled) ||
			!reader.getDouble(newFilled) || !reader.getDouble(newRemaining) ||
			!reader.atEnd() || delta == 0.0)
			return corrupt("invalid trade application record");
		auto intent = _intents.find(intentId);
		if (intent == _intents.end() || intent->second.owner_id != ownerId ||
			intent->second.std_code != stdCode || _trades.find(tradeKey) != _trades.end())
			return corrupt("trade record has unknown owner or duplicate trade key");
		if ((delta < 0.0) != (intent->second.signed_quantity > 0.0))
			return corrupt("trade direction does not match its recorded order intent");
		OwnerCodeKey key{ ownerId, stdCode };
		auto position = _positions.find(key);
		const double current = position == _positions.end() ? 0.0 : position->second.diff;
		const double expectedTradeFilled = intent->second.trade_filled_quantity + std::fabs(delta);
		const double total = std::fabs(intent->second.signed_quantity);
		const double tolerance = quantityTolerance(total);
		const double expectedFilled = std::max(intent->second.reported_filled_quantity,
			expectedTradeFilled);
		const double expectedRemaining = intent->second.terminal ? 0.0 :
			std::min(intent->second.remaining_quantity, std::max(0.0, total - expectedFilled));
		if (before != current || after != before + delta || !std::isfinite(after) ||
			!std::isfinite(expectedTradeFilled) || expectedTradeFilled > total + tolerance ||
			tradeFilled != expectedTradeFilled || newFilled != expectedFilled ||
			newRemaining != expectedRemaining)
			return corrupt("trade record Diff transition does not match replay state");
		PositionState& state = _positions[key];
		state.diff = after;
		state.last_sequence = sequence;
		intent->second.trade_filled_quantity = tradeFilled;
		intent->second.filled_quantity = newFilled;
		intent->second.remaining_quantity = newRemaining;
		intent->second.last_sequence = sequence;
		_trades.emplace(tradeKey, TradeState{ intentId, ownerId, stdCode, delta });
		return Status::Ok;
	}
	if (type == RecordType::TargetChanged)
	{
		std::string ownerId, stdCode;
		double target = 0.0, diff = 0.0;
		if (!reader.getString(ownerId) || !reader.getString(stdCode) ||
			!reader.getDouble(target) || !reader.getDouble(diff) || !reader.atEnd())
			return corrupt("invalid target change record");
		PositionState& state = _positions[OwnerCodeKey{ ownerId, stdCode }];
		state.target = target;
		state.diff = diff;
		state.last_sequence = sequence;
		return Status::Ok;
	}
	if (type == RecordType::Baseline)
	{
		std::string ownerId, stdCode;
		double baseline = 0.0, target = 0.0, diff = 0.0;
		uint32_t tradingDay = 0;
		if (!reader.getString(ownerId) || !reader.getString(stdCode) ||
			!reader.getDouble(baseline) || !reader.getDouble(target) ||
			!reader.getDouble(diff) || !reader.getU32(tradingDay) ||
			tradingDay == 0 || !reader.atEnd())
			return corrupt("invalid baseline record");
		PositionState& state = _positions[OwnerCodeKey{ ownerId, stdCode }];
		state.baseline_account_position = baseline;
		state.target = target;
		state.diff = diff;
		state.baseline_trading_day = tradingDay;
		state.last_sequence = sequence;
		state.has_baseline = true;
		return Status::Ok;
	}
	if (type == RecordType::LocalIdHighWater)
	{
		uint32_t highWater = 0;
		std::string sourceEvidence;
		if (!reader.getU32(highWater) || !reader.getString(sourceEvidence) || !reader.atEnd() ||
			highWater == std::numeric_limits<uint32_t>::max() || !_intents.empty() ||
			(_local_order_id_floor_established && highWater <= _local_order_id_high_water) ||
			(!_local_order_id_floor_established && highWater < _local_order_id_high_water))
			return corrupt("invalid or late local order id high-water record");
		_local_order_id_high_water = highWater;
		_local_order_id_floor_established = true;
		_local_order_id_floor_evidence = sourceEvidence;
		return Status::Ok;
	}
	return corrupt("unknown WAL event");
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::storageFailureLocked(
	const std::string& error)
{
	_healthy = false;
	_last_error = error;
	return Status::StorageFailed;
}

DiffOrderOwnershipJournalV1::Status DiffOrderOwnershipJournalV1::conflictFailureLocked(
	const std::string& error)
{
	_healthy = false;
	_last_error = error;
	return Status::Conflict;
}

NS_WTP_END
