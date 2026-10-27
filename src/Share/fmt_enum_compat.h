#pragma once

#include <fmt/format.h>
#include <atomic>
#include <type_traits>

template <typename T, typename Char>
struct fmt::formatter<T, Char, std::enable_if_t<std::is_enum<T>::value>>
	: fmt::formatter<std::underlying_type_t<T>, Char>
{
	template <typename FormatContext>
	auto format(T val, FormatContext& ctx) const
	{
		return fmt::formatter<std::underlying_type_t<T>, Char>::format(
			static_cast<std::underlying_type_t<T>>(val), ctx);
	}
};

template <typename T, typename Char>
struct fmt::formatter<std::atomic<T>, Char> : fmt::formatter<T, Char>
{
	template <typename FormatContext>
	auto format(const std::atomic<T>& val, FormatContext& ctx) const
	{
		return fmt::formatter<T, Char>::format(val.load(), ctx);
	}
};
