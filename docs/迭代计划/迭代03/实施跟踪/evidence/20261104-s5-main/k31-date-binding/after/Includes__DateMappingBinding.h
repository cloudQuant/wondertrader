#pragma once
// Decision K31=A (2026-11-04): optional, versioned plugin exports through which a
// host that owns a concrete WTSBaseDataMgr injects its DateMappingV1 table into a
// plugin object it has just created (BUG-R12-13, card 1 path A for plugins).
//
// Contract
// - Both sides are optional. A plugin without the export, or an old host that
//   never calls it, keeps path C: DateMappingClient over the guarded legacy
//   IBaseDataMgr virtuals. Path C cannot detect a missing holiday calendar.
// - Each export is specific to one object type and must only receive an object
//   returned by the same module's factory for that type.
// - The host calls the export once, after the factory returned the object and
//   before init or any other call. The plugin validates size, version, context
//   and every function pointer, copies the table and returns 1; otherwise it
//   returns 0 and stays on path C. A second bind is rejected (0) and the first
//   table is kept.
// - The table context is the host's manager. It must outlive the plugin object,
//   exactly as the IBaseDataMgr* passed at init already must.
// - Return values: 1 bound, 0 rejected. Exports never throw.
#include <atomic>

#include "DateMappingClient.hpp"

#define WT_BIND_DATA_READER_DATE_MAPPING_V1	"bindDataReaderDateMappingV1"
#define WT_BIND_RDM_DT_READER_DATE_MAPPING_V1	"bindRdmDtReaderDateMappingV1"
#define WT_BIND_PARSER_DATE_MAPPING_V1		"bindParserDateMappingV1"

NS_WTP_BEGIN
class IDataReader;
class IRdmDtReader;
class IParserApi;
NS_WTP_END

typedef int (*FuncBindDataReaderDateMappingV1)(wtp::IDataReader*, const wtp::DateMappingV1*);
typedef int (*FuncBindRdmDtReaderDateMappingV1)(wtp::IRdmDtReader*, const wtp::DateMappingV1*);
typedef int (*FuncBindParserDateMappingV1)(wtp::IParserApi*, const wtp::DateMappingV1*);

NS_WTP_BEGIN

// A table is usable for binding only when every V1 entry is present.
inline bool isCompleteDateMappingV1(const DateMappingV1* table) noexcept
{
	return table != nullptr && table->size == sizeof(DateMappingV1) && table->version == 1 &&
		table->context != nullptr && table->calcTradingDate != nullptr && table->getBoundaryTime != nullptr &&
		table->getTradingDate != nullptr && table->nextTradingDate != nullptr && table->prevTradingDate != nullptr;
}

// Plugin-side holder. bind() may race only with itself; client() may be called
// from any thread after bind() returned.
class DateMappingBinding
{
public:
	DateMappingBinding() = default;
	DateMappingBinding(const DateMappingBinding&) = delete;
	DateMappingBinding& operator=(const DateMappingBinding&) = delete;

	bool bind(const DateMappingV1* table) noexcept
	{
		if (!isCompleteDateMappingV1(table))
			return false;
		bool expected = false;
		if (!_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
			return false;
		_table = *table;
		_bound.store(true, std::memory_order_release);
		return true;
	}

	bool bound() const noexcept { return _bound.load(std::memory_order_acquire); }

	// Path A when a table was bound, otherwise path C over the legacy manager.
	DateMappingClient client(IBaseDataMgr* legacy) const noexcept
	{
		if (_bound.load(std::memory_order_acquire))
			return DateMappingClient(&_table);
		return DateMappingClient(legacy);
	}

private:
	DateMappingV1		_table{};
	std::atomic<bool>	_claimed{ false };
	std::atomic<bool>	_bound{ false };
};

// Host-side call through an already resolved symbol (nullptr when the plugin
// does not export it). Returns 1 bound, 0 rejected, -1 export absent.
template <typename Object>
inline int bindDateMappingV1(void* symbol, Object* object, const DateMappingV1& table) noexcept
{
	if (symbol == nullptr)
		return -1;
	if (object == nullptr || !isCompleteDateMappingV1(&table))
		return 0;
	typedef int (*Bind)(Object*, const DateMappingV1*);
	try
	{
		return reinterpret_cast<Bind>(symbol)(object, &table) == 1 ? 1 : 0;
	}
	catch (...)
	{
		return 0;
	}
}

NS_WTP_END
