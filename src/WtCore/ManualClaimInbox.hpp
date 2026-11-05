#pragma once
// K50 (2026-11-06, I03-NEW-089): explicit, file-based release command for the account's
// manual owner claim on one contract, for runners that have no API surface (WtRunner).
// It mirrors AccountResumeInbox (card 6b=A).
//
// An operator drops `<any name>.json` into the inbox folder:
//   {"trader":"<trader id>","code":"<claimed contract>","operator":"<who>","reason":"<why>",
//    "reference":"<ticket / reconciliation evidence>"}
// Every request is taken exactly once: it is first renamed to `<file>.<stamp>.processing`
// (a request that cannot be claimed is left alone and reported), parsed from the claimed
// file, handed to the account gate through the adapter, and then renamed to `.done`,
// `.rejected` (gate refused, e.g. live manual orders) or `.invalid` (malformed). The gate
// keeps its own durable audit record; the inbox only adds the outcome log. The operator
// identity is recorded, not authenticated: write access to the folder is the trust boundary.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

#include "AccountTradingGate.h"
#include "../Includes/WTSVariant.hpp"
#include "../Share/TimeUtils.hpp"
#include "../WTSUtils/WTSCfgLoader.h"

NS_WTP_BEGIN

struct ManualClaimOutcome
{
	std::string	request;		// original file name
	std::string	trader;
	std::string	code;
	std::string	archived;		// final file name; empty when the request could not be claimed
	bool		valid = false;
	AccountTradingGate::ClaimReleaseStatus status = AccountTradingGate::ClaimReleaseStatus::InvalidRequest;
	std::string	diagnostic;
};

class ManualClaimInbox
{
public:
	typedef std::function<AccountTradingGate::ClaimReleaseStatus(const std::string& trader,
		const AccountTradingGate::ClaimReleaseRequest& request, std::string& diagnostic)> Releaser;

	static const char* statusName(AccountTradingGate::ClaimReleaseStatus status)
	{
		switch (status)
		{
		case AccountTradingGate::ClaimReleaseStatus::Ok: return "Ok";
		case AccountTradingGate::ClaimReleaseStatus::NotClaimed: return "NotClaimed";
		case AccountTradingGate::ClaimReleaseStatus::InvalidRequest: return "InvalidRequest";
		case AccountTradingGate::ClaimReleaseStatus::LiveOrders: return "LiveOrders";
		case AccountTradingGate::ClaimReleaseStatus::AuditUnavailable: return "AuditUnavailable";
		case AccountTradingGate::ClaimReleaseStatus::AuditWriteFailed: return "AuditWriteFailed";
		}
		return "Unknown";
	}

	// Processes every `*.json` request in the folder, in name order. A missing folder is
	// not an error (no requests).
	static std::vector<ManualClaimOutcome> process(const std::string& folder, const Releaser& releaser)
	{
		namespace fs = std::filesystem;
		std::vector<ManualClaimOutcome> outcomes;
		std::error_code ec;
		if (folder.empty() || !fs::is_directory(folder, ec))
			return outcomes;

		std::vector<fs::path> requests;
		for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
		{
			const fs::path& p = it->path();
			std::error_code typeEc;
			if (p.extension() == ".json" && fs::is_regular_file(it->symlink_status(typeEc)) && !typeEc)
				requests.push_back(p);
		}
		std::sort(requests.begin(), requests.end());

		for (const fs::path& path : requests)
		{
			ManualClaimOutcome outcome;
			outcome.request = path.filename().string();
			const std::string stamp = std::to_string(TimeUtils::getYYYYMMDDhhmmss());
			const fs::path claimed = uniquePath(path.string() + "." + stamp + ".processing");
			std::error_code mv;
			fs::rename(path, claimed, mv);
			if (mv)
			{
				outcome.diagnostic = "request could not be claimed: " + mv.message();
				outcomes.push_back(outcome);
				continue;
			}

			AccountTradingGate::ClaimReleaseRequest request;
			if (!parse(claimed.string(), outcome.trader, request, outcome.diagnostic))
			{
				outcome.archived = finish(claimed, path.string() + "." + stamp + ".invalid");
				outcomes.push_back(outcome);
				continue;
			}
			outcome.code = request.code;
			outcome.valid = true;
			try
			{
				outcome.status = releaser(outcome.trader, request, outcome.diagnostic);
			}
			catch (...)
			{
				outcome.status = AccountTradingGate::ClaimReleaseStatus::InvalidRequest;
				outcome.diagnostic = "claim release handler threw";
			}
			const bool ok = outcome.status == AccountTradingGate::ClaimReleaseStatus::Ok;
			outcome.archived = finish(claimed, path.string() + "." + stamp + (ok ? ".done" : ".rejected"));
			outcomes.push_back(outcome);
		}
		return outcomes;
	}

private:
	static std::filesystem::path uniquePath(const std::string& base)
	{
		std::error_code ec;
		std::filesystem::path candidate(base);
		for (uint32_t seq = 1; std::filesystem::exists(candidate, ec); seq++)
			candidate = std::filesystem::path(base + "." + std::to_string(seq));
		return candidate;
	}

	static std::string finish(const std::filesystem::path& claimed, const std::string& target)
	{
		const std::filesystem::path finalPath = uniquePath(target);
		std::error_code ec;
		std::filesystem::rename(claimed, finalPath, ec);
		// A request that cannot be renamed stays `.processing` and is never taken again.
		return ec ? claimed.filename().string() : finalPath.filename().string();
	}

	static bool parse(const std::string& file, std::string& trader,
		AccountTradingGate::ClaimReleaseRequest& request, std::string& diagnostic)
	{
		// The claimed file no longer ends in .json, so read it here and parse the bytes as JSON.
		std::string content;
		{
			FILE* fp = fopen(file.c_str(), "rb");
			if (fp == NULL)
			{
				diagnostic = "request cannot be read";
				return false;
			}
			char buf[4096];
			size_t n = 0;
			while ((n = fread(buf, 1, sizeof(buf), fp)) > 0 && content.size() < 65536)
				content.append(buf, n);
			fclose(fp);
		}
		if (content.empty() || content.size() >= 65536)
		{
			diagnostic = "request is empty or too large";
			return false;
		}
		WTSVariant* doc = WTSCfgLoader::load_from_content(content, false);
		if (doc == NULL || doc->type() != WTSVariant::VT_Object)
		{
			if (doc != NULL)
				doc->release();
			diagnostic = "request is not a JSON object";
			return false;
		}
		auto text = [doc](const char* key) {
			const char* value = doc->getCString(key);
			return std::string(value == NULL ? "" : value);
		};
		trader = text("trader");
		request.code = text("code");
		request.operator_id = text("operator");
		request.reason = text("reason");
		request.reference = text("reference");
		doc->release();
		if (trader.empty() || request.code.empty() || request.operator_id.empty() || request.reason.empty())
		{
			diagnostic = "trader, code, operator and reason are required";
			return false;
		}
		return true;
	}
};

NS_WTP_END
