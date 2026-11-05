/*!
 * \file WTSCmpBudgetConfig.hpp
 * \brief 宿主可配置的 CMP_V2 解压预算（BUG-R132-001，决策 K37=B）
 *
 * 预算本身在 WTSCmpHelper 中按模块生效（头文件内联的静态量，每个动态库各一份），因此
 * 只有在“实际执行解压的模块”内调用才有效：读端在自身 init 中、回放器在 HisDataReplayer::init
 * 中、写端在 WtDataWriter::init 中分别读取本模块收到的配置。
 *
 * 配置键（与 path/adjfactor 等同级，放在读端 store 配置或回放器配置中）：
 *   - uncompress_max_bytes : 单帧声明解压大小的绝对上限（字节），0 表示不启用该项；
 *   - uncompress_max_ratio : 声明解压大小 / 压缩大小 的上限，0 表示不启用该项。
 * 两键都缺省时不改变当前预算（默认 1 GiB、1000:1）；只给一个键时另一项保持当前值。
 * 键存在但不是非负整数（JSON 整数、YAML/JSON 十进制数字串、整数值的有限实数）即为非法：
 * 本模块进入 fail-closed（任何解压都被拒绝），调用方记 ERROR，能返回失败的入口返回失败。
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "../Includes/WTSVariant.hpp"
#include "WTSCmpHelper.hpp"

namespace wt_cmp_budget
{
	static constexpr const char* KEY_MAX_BYTES = "uncompress_max_bytes";
	static constexpr const char* KEY_MAX_RATIO = "uncompress_max_ratio";

	/// 解析一个预算值；item 必须存在。返回 false 表示非法。
	inline bool parse_value(wtp::WTSVariant* item, uint64_t& out)
	{
		using wtp::WTSVariant;
		if (item == NULL)
			return false;
		switch (item->type())
		{
		case WTSVariant::VT_Int32:
		case WTSVariant::VT_Int64:
		{
			const int64_t v = item->asInt64();
			if (v < 0)
				return false;
			out = (uint64_t)v;
			return true;
		}
		case WTSVariant::VT_Uint32:
		case WTSVariant::VT_Uint64:
			out = item->asUInt64();
			return true;
		case WTSVariant::VT_Real:
		{
			const double v = item->asDouble();
			// 只接受整数值、可精确表示的有限非负实数（JSON 1e9 之类）
			if (!std::isfinite(v) || v < 0 || v != std::floor(v) || v > 9007199254740992.0)
				return false;
			out = (uint64_t)v;
			return true;
		}
		case WTSVariant::VT_String:
		{
			// YAML 标量统一读成字符串：只接受无符号十进制数字串，不接受空串、符号、空白或后缀
			const std::string s = item->asString();
			if (s.empty() || s.size() > 20)
				return false;
			uint64_t v = 0;
			for (char c : s)
			{
				if (c < '0' || c > '9')
					return false;
				const uint64_t digit = (uint64_t)(c - '0');
				if (v > (std::numeric_limits<uint64_t>::max() - digit) / 10)
					return false;
				v = v * 10 + digit;
			}
			out = v;
			return true;
		}
		default:
			return false;	// 布尔、对象、数组、空值都不是合法预算
		}
	}

	enum class ApplyResult
	{
		Unchanged,	///< 两键都缺省，预算保持不变
		Applied,	///< 合法配置，已生效
		Invalid		///< 非法配置，本模块已 fail-closed（拒绝所有解压）
	};

	/**
	 * @brief 读取 cfg 中的预算键并应用到当前模块
	 * @param error 非法时写入原因（含键名）
	 */
	inline ApplyResult apply(wtp::WTSVariant* cfg, std::string& error)
	{
		if (cfg == NULL || !cfg->isObject())
			return ApplyResult::Unchanged;
		const bool hasBytes = cfg->has(KEY_MAX_BYTES);
		const bool hasRatio = cfg->has(KEY_MAX_RATIO);
		if (!hasBytes && !hasRatio)
			return ApplyResult::Unchanged;

		uint64_t maxBytes = WTSCmpHelper::uncompress_max_bytes();
		uint64_t maxRatio = WTSCmpHelper::uncompress_max_ratio();
		if (hasBytes && !parse_value(cfg->get(KEY_MAX_BYTES), maxBytes))
		{
			error = std::string("invalid ") + KEY_MAX_BYTES + ": expected a non-negative integer";
			WTSCmpHelper::refuse_uncompress(true);
			return ApplyResult::Invalid;
		}
		if (hasRatio && !parse_value(cfg->get(KEY_MAX_RATIO), maxRatio))
		{
			error = std::string("invalid ") + KEY_MAX_RATIO + ": expected a non-negative integer";
			WTSCmpHelper::refuse_uncompress(true);
			return ApplyResult::Invalid;
		}
		WTSCmpHelper::set_uncompress_budget(maxBytes, maxRatio);
		return ApplyResult::Applied;
	}
}
