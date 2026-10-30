#include <atomic>
#include <cstdint>
uint32_t makeLocalOrderID() { static std::atomic<uint32_t> id{0}; return ++id; }
