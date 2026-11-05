/*!
 * \file ExecPolicyConfig.h
 * \brief Pure (side-effect free) validation shared by the Local, Diff and Arbi
 *        executer configuration entries.
 *
 * Iteration 03 decision card 8b=A: an executer configuration is validated in
 * full before any side effect. These helpers only read the WTSVariant tree; they
 * never retain, allocate runtime state, load modules or create execution units,
 * so runners can call them on a whole executer list before constructing any
 * executer object.
 *
 * Policy contract enforced here (all entries, not only the one a code needs):
 *   - `policy` exists and is an object;
 *   - `policy.default` exists (commodity-specific entries are optional);
 *   - every entry (default and commodity-specific) is an object whose `name`
 *     is a string of the exact form "<factory>.<unit>" with both parts
 *     non-empty, matching WtExecuterFactory's name split;
 *   - entry keys are non-empty.
 * Whether the named factory/unit is actually loadable is still resolved lazily
 * by the factory when a unit is created; that path stays fail-closed.
 */
#pragma once

#include "../Includes/WTSVariant.hpp"

#include <cstring>
#include <string>

namespace exec_policy_config
{
using wtp::WTSVariant;

inline bool validUnitName(const char* name)
{
	if (name == NULL)
		return false;
	const char* dot = std::strchr(name, '.');
	if (dot == NULL || dot == name || dot[1] == '\0')
		return false;
	// Exactly one separator: "<factory>.<unit>".
	return std::strchr(dot + 1, '.') == NULL;
}

inline bool validatePolicy(WTSVariant* params, std::string& error)
{
	WTSVariant* policy = params == NULL ? NULL : params->get("policy");
	if (policy == NULL)
	{
		error = "policy is missing";
		return false;
	}
	if (policy->type() != WTSVariant::VT_Object)
	{
		error = "policy must be an object";
		return false;
	}
	if (policy->get("default") == NULL)
	{
		error = "policy.default is missing";
		return false;
	}
	for (const std::string& key : policy->memberNames())
	{
		if (key.empty())
		{
			error = "policy contains an empty key";
			return false;
		}
		WTSVariant* entry = policy->get(key.c_str());
		if (entry == NULL || entry->type() != WTSVariant::VT_Object)
		{
			error = "policy." + key + " must be an object";
			return false;
		}
		WTSVariant* name = entry->get("name");
		if (name == NULL || name->type() != WTSVariant::VT_String)
		{
			error = "policy." + key + ".name must be a string";
			return false;
		}
		if (!validUnitName(name->asCString()))
		{
			error = "policy." + key + ".name must be <factory>.<unit>";
			return false;
		}
	}
	return true;
}

inline bool validateCodeList(WTSVariant* item, const char* label, std::string& error)
{
	if (item == NULL)
		return true;
	if (item->type() == WTSVariant::VT_String)
	{
		if (item->asCString()[0] != '\0')
			return true;
		error = std::string("clear.") + label + " must not be empty";
		return false;
	}
	if (item->type() != WTSVariant::VT_Array)
	{
		error = std::string("clear.") + label + " must be a string or an array of strings";
		return false;
	}
	for (uint32_t i = 0; i < item->size(); i++)
	{
		WTSVariant* code = item->get(i);
		if (code == NULL || code->type() != WTSVariant::VT_String || code->asCString()[0] == '\0')
		{
			error = std::string("clear.") + label + "[" + std::to_string(i) + "] must be a non-empty string";
			return false;
		}
	}
	return true;
}

/*! Shape of the optional auto-clear section used by Local and Arbi. */
inline bool validateClear(WTSVariant* params, std::string& error)
{
	WTSVariant* clear = params == NULL ? NULL : params->get("clear");
	if (clear == NULL)
		return true;
	if (clear->type() != WTSVariant::VT_Object)
	{
		error = "clear must be an object";
		return false;
	}
	return validateCodeList(clear->get("includes"), "includes", error) &&
		validateCodeList(clear->get("excludes"), "excludes", error);
}

} // namespace exec_policy_config
