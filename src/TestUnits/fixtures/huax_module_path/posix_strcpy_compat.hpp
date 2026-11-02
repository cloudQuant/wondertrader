#pragma once
#include <cstring>
#include <cstddef>
// Test-only approximation of the MSVC array overload used by unrelated
// TraderHuaX login code, so this POSIX path fixture can compile the TU.
template <std::size_t N>
inline int strcpy_s(char (&destination)[N], const char* source)
{
    const std::size_t length = std::strlen(source);
    if (length >= N) { destination[0] = '\0'; return 1; }
    std::memcpy(destination, source, length + 1);
    return 0;
}
