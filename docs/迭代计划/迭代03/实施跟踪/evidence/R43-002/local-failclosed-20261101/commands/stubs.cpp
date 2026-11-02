#include <cstdint>
const char* FACT_NAME = "test";
uint32_t makeLocalOrderID() { static uint32_t id = 0; return ++id; }
