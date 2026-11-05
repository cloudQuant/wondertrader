#pragma once
// Card 6b=A (2026-11-04, BUG-L01-014): explicit, file-based resume command for a
// paused trading account, for runners that have no API surface (WtRunner).
//
// An operator drops `<traderId>.json` into the inbox folder:
//   {"operator":"<who>","reason":"<why>","generation":<reviewed pause generation>,
//    "reference":"<reconciliation evidence>"}
// Every request is taken exactly once: it is first renamed to
// `<file>.<stamp>.processing` (a request that cannot be claimed is left alone and
// reported), parsed from the claimed file, handed to the account gate, and then
// renamed to `.done`, `.rejected` (gate refused) or `.invalid` (malformed). The
// gate keeps its own durable audit record; the inbox only adds the outcome log.
// The operator identity is recorded, not authenticated: write access to the
// folder is the trust boundary.
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

struct AccountResumeOutcome
{
	std::string	request;		// original file name
	std::string	trader;
	std::string	archived;		// final file name; empty when the request could not be claimed
	bool		valid = false;
	AccountTradingGate::ResumeStatus status = AccountTradingGate::ResumeStatus::InvalidRequest;
	std::string	diagnostic;
};

class AccountResumeInbox
{
public:
	typedef std::function<AccountTradingGate::ResumeStatus(const std::string& trader,
		const AccountTradingGate::ResumeRequest& request, std::string& diagnostic)> Resumer;

	static const char* statusName(AccountTradingGate::ResumeStatus status)
	{
		switch (status)
		{
		case AccountTradingGate::ResumeStatus::Ok: return "Ok";
		case AccountTradingGate::ResumeStatus::NotPaused: return "NotPaused";
		case AccountTradingGate::ResumeStatus::InvalidRequest: return "InvalidRequest";
		case AccountTradingGate::ResumeStatus::StaleGeneration: return "StaleGeneration";
		case AccountTradingGate::ResumeStatus::AuditUnavailable: return "AuditUnavailable";
		case AccountTradingGate::ResumeStatus::AuditWriteFailed: return "AuditWriteFailed";
		case AccountTradingGate::ResumeStatus::ParticipantRejected: return "ParticipantRejected";
		}
		return "Unknown";
	}

	// Processes every `*.json` request in the folder, in name order. A missing
	// folder is not an error (no requests).
	static std::vector<AccountResumeOutcome> process(const std::string& folder, const Resumer& resumer)
	{
		namespace fs = std::filesystem;
		std::vector<AccountResumeOutcome> outcomes;
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
			AccountResumeOutcome outcome;
			outcome.request = path.filename().string();
			outcome.trader = path.stem().string();
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

			AccountTradingGate::ResumeRequest request;
			if (!parse(claimed.string(), request, outcome.diagnostic))
			{
				outcome.archived = finish(claimed, path.string() + "." + stamp + ".invalid");
				outcomes.push_back(outcome);
				continue;
			}
			outcome.valid = true;
			try
			{
				outcome.status = resumer(outcome.trader, request, outcome.diagnostic);
			}
			catch (...)
			{
				outcome.status = AccountTradingGate::ResumeStatus::InvalidRequest;
				outcome.diagnostic = "resume handler threw";
			}
			const bool ok = outcome.status == AccountTradingGate::ResumeStatus::Ok;
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

	static bool parse(const std::string& file, AccountTradingGate::ResumeRequest& request, std::string& diagnostic)
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
		const char* op = doc->getCString("operator");
		const char* reason = doc->getCString("reason");
		const bool hasGeneration = doc->has("generation");
		request.operator_id = op == NULL ? "" : op;
		request.reason = reason == NULL ? "" : reason;
		request.expected_generation = hasGeneration ? doc->getUInt64("generation") : 0;
		const char* reference = doc->getCString("reference");
		request.reconciliation_reference = reference == NULL ? "" : reference;
		doc->release();
		if (request.operator_id.empty() || request.reason.empty() || !hasGeneration || request.expected_generation == 0)
		{
			diagnostic = "operator, reason and a non-zero reviewed generation are required";
			return false;
		}
		return true;
	}
};

NS_WTP_END
