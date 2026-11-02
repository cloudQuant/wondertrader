#include "Share/ModuleHelper.hpp"
extern "C" const char* test_module_path() { return getInstPath().c_str(); }
extern "C" const char* test_module_dir() { return getBinDir(); }
