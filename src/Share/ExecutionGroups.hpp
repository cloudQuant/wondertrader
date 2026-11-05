#pragma once

#include "../Includes/WTSVariant.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace execution_groups
{
using wtp::WTSVariant;
struct Group
{
 std::string name;
 std::map<std::string, double> items;
 bool has_limit = false;
 double max_abs = 0.0;
};
using Groups = std::vector<Group>;
using Positions = std::map<std::string, double>;

inline bool number(WTSVariant* value, double& out)
{
 if (!value) return false;
 switch (value->type())
 {
 case WTSVariant::VT_Int32: case WTSVariant::VT_Uint32: case WTSVariant::VT_Int64:
 case WTSVariant::VT_Uint64: case WTSVariant::VT_Real:
  out = value->asDouble(); break;
 case WTSVariant::VT_String:
 {
  std::istringstream input(value->asString()); input.imbue(std::locale::classic()); input >> out;
  const bool subnormal = input.fail() && input.eof() && std::fpclassify(out) == FP_SUBNORMAL;
  if (input.bad() || (input.fail() && !subnormal)) return false;
  if (subnormal) input.clear(input.rdstate() & ~std::ios::failbit);
  input >> std::ws; if (!input.eof()) return false; break;
 }
 default: return false;
 }
 return std::isfinite(out);
}
inline bool validName(const std::string& name, size_t maxLen = std::numeric_limits<size_t>::max())
{
 return !name.empty() && name.size() <= maxLen && name.find('\0') == std::string::npos;
}
inline bool readItems(WTSVariant* value, Group& group, std::string& error)
{
 if (!value || value->type() != WTSVariant::VT_Object || value->size() == 0)
 { error = "group items must be a non-empty object"; return false; }
 for (const std::string& code : value->memberNames())
 {
  double weight = 0.0;
  if (!validName(code) || !number(value->get(code.c_str()), weight) || weight == 0.0)
  { error = "group leg code or weight is invalid"; return false; }
  group.items.emplace(code, weight);
 }
 return true;
}
inline bool parse(WTSVariant* params, Groups& result, std::string& error)
{
 Groups groups; WTSVariant* source = params->get("groups"); WTSVariant* order = params->get("group_order");
 if (source)
 {
  if (source->type() == WTSVariant::VT_Array)
  {
   if (order) { error = "group_order applies only to object groups"; return false; }
   for (uint32_t i = 0; i < source->size(); ++i)
   {
    WTSVariant* item = source->get(i);
    if (!item || item->type() != WTSVariant::VT_Object) { error = "group entry must be an object"; return false; }
    WTSVariant* name = item->get("name"); Group group;
    if (!name || name->type() != WTSVariant::VT_String || !validName(group.name = name->asString(), 31) ||
        !readItems(item->get("items"), group, error)) { if (error.empty()) error = "group name or items invalid"; return false; }
    groups.push_back(std::move(group));
   }
  }
  else if (source->type() == WTSVariant::VT_Object)
  {
   for (const std::string& name : source->memberNames())
   {
    Group group; group.name = name;
    if (!validName(name, 31) || !readItems(source->get(name.c_str()), group, error))
    { if (error.empty()) error = "group name or items invalid"; return false; }
    groups.push_back(std::move(group));
   }
   std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.name < b.name; });
   if (order)
   {
    if (order->type() != WTSVariant::VT_Array || order->size() != groups.size())
    { error = "group_order must list every group exactly once"; return false; }
    Groups sorted; std::set<std::string> seen;
    for (uint32_t i = 0; i < order->size(); ++i)
    {
     WTSVariant* item = order->get(i);
     if (!item || item->type() != WTSVariant::VT_String) { error = "group_order names must be strings"; return false; }
     const std::string name = item->asString();
     auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == name; });
     if (it == groups.end() || !seen.insert(name).second) { error = "group_order contains an unknown or duplicate name"; return false; }
     sorted.push_back(*it);
    }
    groups.swap(sorted);
   }
   else
   {
    std::set<std::string> legs;
    for (const Group& group : groups) for (const auto& leg : group.items)
     if (!legs.insert(leg.first).second) { error = "overlapping object groups require group_order"; return false; }
   }
  }
  else { error = "groups must be an object or ordered array"; return false; }
 }
 else if (order) { error = "group_order requires groups"; return false; }
 std::set<std::string> names;
 for (const Group& group : groups)
  if (!names.insert(group.name).second) { error = "duplicate group name"; return false; }
 for (const Group& group : groups) for (const auto& leg : group.items)
  if (names.count(leg.first)) { error = "group names cannot be group legs"; return false; }
 WTSVariant* limits = params->get("group_limits");
 if (limits)
 {
  if (limits->type() != WTSVariant::VT_Object) { error = "group_limits must be an object"; return false; }
  for (const std::string& name : limits->memberNames())
  {
   auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == name; }); double limit = 0.0;
   if (it == groups.end() || !number(limits->get(name.c_str()), limit) || limit < 0.0)
   { error = "group limit must name a group and be finite and non-negative"; return false; }
   it->has_limit = true; it->max_abs = limit;
  }
 }
 result.swap(groups); return true;
}

template<class Map>
inline bool calculate(const Groups& groups, const Map& input, double limitScale,
 Positions& output, Positions& physical, std::string& error, bool decompose = true)
{
 Positions targets;
 for (const auto& item : input)
 {
  if (!validName(item.first) || !std::isfinite(item.second)) { error = "invalid target code or value"; return false; }
  targets[item.first] = item.second;
 }
 Positions net;
 for (const auto& item : targets)
 {
  auto group = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.name == item.first; });
  if (group == groups.end())
  {
   const double sum = net[item.first] + item.second;
   if (!std::isfinite(sum)) { error = "physical target overflow"; return false; }
   net[item.first] = sum;
  }
  else
  {
   for (const auto& leg : group->items)
   {
    const double expanded = item.second * leg.second;
    const double sum = net[leg.first] + expanded;
    if (!std::isfinite(expanded) || !std::isfinite(sum)) { error = "group expansion overflow"; return false; }
    net[leg.first] = sum;
   }
  }
 }
 if (decompose) for (const Group& group : groups)
 {
  if (targets.find(group.name) != targets.end()) continue;
  int sign = 0; double quantity = std::numeric_limits<double>::max(); bool matched = true;
  for (const auto& leg : group.items)
  {
   auto it = targets.find(leg.first);
   const double residual = it == targets.end() ? 0.0 : it->second;
   const double ratio = residual / leg.second;
   if (!std::isfinite(ratio)) { error = "group ratio overflow"; return false; }
   if (ratio == 0.0) { matched = false; break; }
   const int current = ratio > 0.0 ? 1 : -1;
   if (sign && current != sign) { matched = false; break; }
   sign = current; quantity = std::min(quantity, std::fabs(ratio));
  }
  if (!matched || sign == 0) continue;
  const double q = sign * quantity;
  if (!std::isfinite(q)) { error = "group target overflow"; return false; }
  for (const auto& leg : group.items)
  {
   const double allocation = q * leg.second;
   const double residual = targets.find(leg.first)->second - allocation;
   if (!std::isfinite(allocation) || !std::isfinite(residual)) { error = "group residual overflow"; return false; }
   targets[leg.first] = residual;
  }
  targets[group.name] = q;
 }
 for (const Group& group : groups)
 {
  auto it = targets.find(group.name);
  if (it == targets.end()) continue;
  const double scaled = it->second * limitScale;
  if (!std::isfinite(scaled)) { error = "scaled group target overflow"; return false; }
  if (group.has_limit && std::fabs(scaled) > group.max_abs) { error = "group target exceeds limit: " + group.name; return false; }
 }
 for (const auto& item : targets)
  if (!std::isfinite(item.second * limitScale)) { error = "scaled target overflow"; return false; }
 for (const auto& item : net)
  if (!std::isfinite(item.second * limitScale)) { error = "scaled physical target overflow"; return false; }
 output.swap(targets); physical.swap(net); return true;
}
}
