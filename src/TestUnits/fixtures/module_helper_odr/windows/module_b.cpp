#include "Share/ModuleHelper.hpp"

extern "C" __declspec(dllexport) const char* module_b_path()
{
    return getBinDir();
}
