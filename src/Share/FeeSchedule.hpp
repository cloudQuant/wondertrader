#pragma once

// Contract/product association must be verified against the metadata of the
// actual fill by the caller before lookup or construction of a Context.
#include <rapidjson/document.h>
#include <cmath>
#include <cstdint>
#include <locale>
#include <sstream>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace wt_fee
{
struct Rates
{
	double open = 0;
	double close = 0;
	double close_today = 0;
	double min_fee = 0;
	double margin = 0;
	bool by_volume = false;
};

// Syntax checks do not establish that a physical contract belongs to a product.
inline bool validIdentityKey(const std::string& key) noexcept
{
	const size_t dot = key.find('.');
	if (dot == std::string::npos || dot == 0 || dot + 1 == key.size() || key.find('.', dot + 1) != std::string::npos)
		return false;
	for (char c : key)
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
	return true;
}

struct Context
{
	Rates rates;
	double volume_scale = 0;
	std::string physical_contract, product;
};

enum class CalculationStatus { ok, invalid_input, invalid_context, overflow };
struct CalculationResult
{
	CalculationStatus status;
	double value;
};

// This is a value snapshot: lookup and actual contract/product association are
// the caller's responsibility and no mutable schedule is read here.
inline CalculationResult calculate(const Context& context, double price, double qty, uint32_t offset) noexcept
{
	if (!std::isfinite(price) || price < 0 || !std::isfinite(qty) || qty < 0 || offset > 2)
		return {CalculationStatus::invalid_input, 0};
	const Rates& rates = context.rates;
	if (!validIdentityKey(context.physical_contract) || !validIdentityKey(context.product) ||
		context.physical_contract.compare(0, context.physical_contract.find('.'), context.product, 0, context.product.find('.')) != 0 ||
		!std::isfinite(context.volume_scale) || context.volume_scale <= 0 ||
		!std::isfinite(rates.open) || !std::isfinite(rates.close) || !std::isfinite(rates.close_today) ||
		!std::isfinite(rates.min_fee) || rates.min_fee < 0 || !std::isfinite(rates.margin))
		return {CalculationStatus::invalid_context, 0};
	if (qty == 0) return {CalculationStatus::ok, 0};
	const double rate = offset == 0 ? rates.open : offset == 1 ? rates.close : rates.close_today;
	double fee = rate;
	if (!rates.by_volume)
	{
		fee *= price;
		if (!std::isfinite(fee)) return {CalculationStatus::overflow, 0};
	}
	fee *= qty;
	if (!std::isfinite(fee)) return {CalculationStatus::overflow, 0};
	if (!rates.by_volume)
	{
		fee *= context.volume_scale;
		if (!std::isfinite(fee)) return {CalculationStatus::overflow, 0};
	}
	// Same per-fill minimum rule as HisDataReplayer::apply_fee_minimum.
	if (qty > 0 && rate > 0 && rates.min_fee > 0 && fee < rates.min_fee)
		fee = rates.min_fee;
	const double cents = fee * 100.0;
	if (!std::isfinite(cents)) return {CalculationStatus::overflow, 0};
	const double rounded = std::round(cents) / 100.0;
	if (!std::isfinite(rounded)) return {CalculationStatus::overflow, 0};
	return {CalculationStatus::ok, rounded};
}

enum class LookupStatus { contract, product, missing, invalid };
struct LookupResult
{
	LookupStatus status = LookupStatus::invalid;
	Rates rates;
};

class Schedule
{
public:
	// v2 JSON: {"version":2,"contracts":{key:rates},"products":{key:rates}}.
	// An unversioned object is interpreted solely as legacy product entries.
	// A failed reload invalidates the whole schedule, preventing stale use.
	bool loadJson(const std::string& input, std::string& error)
	{
		_valid = false;
		_contracts.clear();
		_products.clear();
		error.clear();
		if (input.find('\0') != std::string::npos)
			return fail(error, "embedded NUL");
		rapidjson::Document root;
		root.Parse<rapidjson::kParseValidateEncodingFlag>(input.data(), input.size());
		if (root.HasParseError() || !root.IsObject())
			return fail(error, "invalid JSON object");
		if (!uniqueKeys(root)) return fail(error, "duplicate root key");

		std::map<std::string, Rates> contracts, products;
		auto version = root.FindMember("version");
		if (version == root.MemberEnd())
		{
			if (!parseTable(root, products, true, error)) return false;
		}
		else
		{
			if (!version->value.IsInt() || version->value.GetInt() != 2)
				return fail(error, "unsupported fee version");
			for (auto it = root.MemberBegin(); it != root.MemberEnd(); ++it)
			{
				const std::string name(it->name.GetString(), it->name.GetStringLength());
				if (name != "version" && name != "contracts" && name != "products")
					return fail(error, "unknown envelope field");
			}
			auto c = root.FindMember("contracts"), p = root.FindMember("products");
			if (c == root.MemberEnd() || p == root.MemberEnd() ||
				!c->value.IsObject() || !p->value.IsObject())
				return fail(error, "contracts and products must be objects");
			if (!parseTable(c->value, contracts, false, error) ||
				!parseTable(p->value, products, false, error)) return false;
		}
		_contracts.swap(contracts);
		_products.swap(products);
		_valid = true;
		return true;
	}

	bool valid() const noexcept { return _valid; }
	const std::map<std::string, Rates>& contractRates() const noexcept { return _contracts; }
	const std::map<std::string, Rates>& productRates() const noexcept { return _products; }

	LookupResult lookup(const std::string& physicalContract, const std::string& product) const
	{
		LookupResult result;
		if (!_valid || !validKey(physicalContract) || !validKey(product) ||
			physicalContract.substr(0, physicalContract.find('.')) != product.substr(0, product.find('.')))
			return result;
		auto c = _contracts.find(physicalContract);
		if (c != _contracts.end()) { result.status = LookupStatus::contract; result.rates = c->second; return result; }
		auto p = _products.find(product);
		if (p != _products.end()) { result.status = LookupStatus::product; result.rates = p->second; return result; }
		result.status = LookupStatus::missing;
		return result;
	}

private:
	static bool fail(std::string& error, const char* message) { error = message; return false; }
	static bool validKey(const std::string& key) { return validIdentityKey(key); }
	static bool uniqueKeys(const rapidjson::Value& object)
	{
		std::set<std::string> keys;
		for (auto it = object.MemberBegin(); it != object.MemberEnd(); ++it)
			if (!keys.emplace(it->name.GetString(), it->name.GetStringLength()).second) return false;
		return true;
	}
	static bool number(const rapidjson::Value& value, bool legacy, double& out)
	{
		if (value.IsNumber()) out = value.GetDouble();
		else if (legacy && value.IsString())
		{
			std::string text(value.GetString(), value.GetStringLength());
			if (text.empty() || text.find('\0') != std::string::npos) return false;
			std::istringstream stream(text);
			stream.imbue(std::locale::classic());
			if (!(stream >> std::noskipws >> out) || stream.peek() != std::char_traits<char>::eof()) return false;
		}
		else return false;
		return std::isfinite(out);
	}
	static bool parseTable(const rapidjson::Value& table, std::map<std::string, Rates>& out,
		bool legacy, std::string& error)
	{
		if (!table.IsObject() || !uniqueKeys(table)) return fail(error, "invalid or duplicate fee key");
		for (auto it = table.MemberBegin(); it != table.MemberEnd(); ++it)
		{
			std::string key(it->name.GetString(), it->name.GetStringLength());
			if (!validKey(key) || !it->value.IsObject() || !uniqueKeys(it->value))
				return fail(error, "invalid fee entry");
			Rates rate;
			bool seenOpen = false, seenClose = false, seenToday = false, seenVolume = false;
			for (auto f = it->value.MemberBegin(); f != it->value.MemberEnd(); ++f)
			{
				const std::string name(f->name.GetString(), f->name.GetStringLength());
				if (name == "byvolume")
				{
					if (!f->value.IsBool()) return fail(error, "byvolume must be boolean");
					rate.by_volume = f->value.GetBool(); seenVolume = true;
				}
				else
				{
					double* dest = nullptr;
					if (name == "open") { dest = &rate.open; seenOpen = true; }
					else if (name == "close") { dest = &rate.close; seenClose = true; }
					else if (name == "closetoday") { dest = &rate.close_today; seenToday = true; }
					else if (name == "minfee") dest = &rate.min_fee;
					else if (name == "margin") dest = &rate.margin;
					else return fail(error, "unknown fee field");
					if (!number(f->value, legacy, *dest) || (name == "minfee" && *dest < 0))
						return fail(error, "invalid fee number");
				}
			}
			// Missing fields are not a zero fee: without all four values we cannot
			// distinguish an incomplete template from an explicitly free trade.
			if (!(seenOpen && seenClose && seenToday && seenVolume))
				return fail(error, "incomplete fee entry");
			out.emplace(std::move(key), rate);
		}
		return true;
	}
	bool _valid = false;
	std::map<std::string, Rates> _contracts;
	std::map<std::string, Rates> _products;
};
} // namespace wt_fee
