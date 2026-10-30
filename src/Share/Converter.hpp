/*!
 * \file StdUtils.hpp
 * \project	WonderTrader
 *
 * \author Wesley
 * \date 2020/03/30
 * 
 * \brief C++标准库一些定义的简单封装,方便调用
 */
#pragma once

#include <stdint.h>
#include <string>
#include <charconv>
#include <cstring>
#include <limits>
#include <system_error>


//////////////////////////////////////////////////////////////////////////
//
namespace convert
{
	template <typename T>
	static const char* to_str(T val) noexcept
	{
		thread_local static char buffer[64] = { 0 };
		const auto result = std::to_chars(buffer, buffer + sizeof(buffer) - 1, val);
		if (result.ec != std::errc{})
		{
			buffer[0] = '\0';
			return buffer;
		}
		*result.ptr = '\0';
		return buffer;
	}

	template <typename T>
	static const char* to_str(char* buffer, std::size_t len, T val) noexcept
	{
		if (buffer == nullptr || len == 0)
			return buffer;

		buffer[0] = '\0';
		if (len < 2)
			return buffer;

		const auto result = std::to_chars(buffer, buffer + len - 1, val);
		if (result.ec != std::errc{})
		{
			buffer[0] = '\0';
			return buffer;
		}
		*result.ptr = '\0';
		return buffer;
	}

	template <typename T>
	static T from_str(const char* str) noexcept
	{
		T ret{};
		if (str == nullptr)
			return ret;
		std::from_chars(str, str + std::strlen(str), ret);
		return ret;
	}

	template <typename T>
	static T from_str(const std::string& str) noexcept
	{
		T ret{};
		std::from_chars(str.data(), str.data() + str.size(), ret);
		return ret;
	}

	namespace detail
	{
		constexpr bool parse_unsigned_digits(const char* str, uint64_t limit, uint64_t& value) noexcept
		{
			if (str == nullptr || *str == '\0')
				return false;

			uint64_t parsed = 0;
			for (; *str != '\0'; ++str)
			{
				if (*str < '0' || *str > '9')
					return false;

				const uint64_t digit = static_cast<uint64_t>(*str - '0');
				if (parsed > (limit - digit) / 10)
					return false;
				parsed = parsed * 10 + digit;
			}

			value = parsed;
			return true;
		}

		template <typename T>
		constexpr bool parse_signed(const char* str, T& value) noexcept
		{
			value = 0;
			if (str == nullptr || *str == '\0')
				return false;

			bool negative = false;
			if (*str == '+' || *str == '-')
			{
				negative = *str == '-';
				++str;
			}
			if (*str == '\0')
				return false;

			const uint64_t positiveLimit = static_cast<uint64_t>((std::numeric_limits<T>::max)());
			const uint64_t negativeLimit = positiveLimit + 1;
			uint64_t magnitude = 0;
			if (!parse_unsigned_digits(str, negative ? negativeLimit : positiveLimit, magnitude))
				return false;

			if (!negative)
				value = static_cast<T>(magnitude);
			else if (magnitude == negativeLimit)
				value = (std::numeric_limits<T>::min)();
			else
				value = static_cast<T>(-static_cast<T>(magnitude));
			return true;
		}

		template <typename T>
		constexpr bool append_legacy_digit(T& value, uint64_t digit) noexcept
		{
			if constexpr (std::numeric_limits<T>::is_signed)
			{
				if (value < 0)
				{
					if (value < (std::numeric_limits<T>::min)() / 10)
						return false;
					value = static_cast<T>(value * 10 + static_cast<T>(digit));
				}
				else
				{
					const T maxValue = (std::numeric_limits<T>::max)();
					if (value > (maxValue - static_cast<T>(digit)) / 10)
						return false;
					value = static_cast<T>(value * 10 + static_cast<T>(digit));
				}
			}
			else
			{
				const T maxValue = (std::numeric_limits<T>::max)();
				if (value > (maxValue - static_cast<T>(digit)) / 10)
					return false;
				value = static_cast<T>(value * 10 + static_cast<T>(digit));
			}
			return true;
		}

		template <typename T>
		constexpr bool parse_legacy_accumulator(const char* str, T initial, T& value) noexcept
		{
			value = initial;
			if (str == nullptr)
				return false;
			for (; *str != '\0'; ++str)
			{
				if (*str < '0' || *str > '9' || !append_legacy_digit(value, static_cast<uint64_t>(*str - '0')))
				{
					value = 0;
					return false;
				}
			}
			return true;
		}
	}

	constexpr bool try_to_int32(const char* str, int32_t& value) noexcept
	{
		return detail::parse_signed(str, value);
	}

	constexpr bool try_to_uint32(const char* str, uint32_t& value) noexcept
	{
		value = 0;
		if (str == nullptr || *str == '\0')
			return false;
		if (*str == '+')
			++str;
		if (*str == '\0' || *str == '-')
			return false;

		uint64_t parsed = 0;
		if (!detail::parse_unsigned_digits(str, (std::numeric_limits<uint32_t>::max)(), parsed))
			return false;
		value = static_cast<uint32_t>(parsed);
		return true;
	}

	constexpr bool try_to_int64(const char* str, int64_t& value) noexcept
	{
		return detail::parse_signed(str, value);
	}

	constexpr bool try_to_uint64(const char* str, uint64_t& value) noexcept
	{
		value = 0;
		if (str == nullptr || *str == '\0')
			return false;
		if (*str == '+')
			++str;
		if (*str == '\0' || *str == '-')
			return false;
		return detail::parse_unsigned_digits(str, (std::numeric_limits<uint64_t>::max)(), value);
	}

	// Scalar wrappers return zero on parse failure. Use try_* when failure must
	// be distinguishable from the valid value zero.
	constexpr int32_t to_int32(const char* str) noexcept
	{
		int32_t value = 0;
		return try_to_int32(str, value) ? value : 0;
	}

	constexpr uint32_t to_uint32(const char* str) noexcept
	{
		uint32_t value = 0;
		return try_to_uint32(str, value) ? value : 0;
	}

	constexpr int64_t to_int64(const char* str) noexcept
	{
		int64_t value = 0;
		return try_to_int64(str, value) ? value : 0;
	}

	constexpr uint64_t to_uint64(const char* str) noexcept
	{
		uint64_t value = 0;
		return try_to_uint64(str, value) ? value : 0;
	}

	// Preserve the old accumulator overloads for source compatibility. They are
	// safe digit folds; callers needing signed parsing should use the overloads above.
	constexpr int32_t to_int32(const char* str, int32_t h) noexcept
	{
		if (h == 0)
			return to_int32(str);
		int32_t value = 0;
		return detail::parse_legacy_accumulator(str, h, value) ? value : 0;
	}

	constexpr uint32_t to_uint32(const char* str, uint32_t h) noexcept
	{
		if (h == 0)
			return to_uint32(str);
		uint32_t value = 0;
		return detail::parse_legacy_accumulator(str, h, value) ? value : 0;
	}

	constexpr int64_t to_int64(const char* str, int64_t h) noexcept
	{
		if (h == 0)
			return to_int64(str);
		int64_t value = 0;
		return detail::parse_legacy_accumulator(str, h, value) ? value : 0;
	}

	constexpr uint64_t to_uint64(const char* str, uint64_t h) noexcept
	{
		if (h == 0)
			return to_uint64(str);
		uint64_t value = 0;
		return detail::parse_legacy_accumulator(str, h, value) ? value : 0;
	}
};
