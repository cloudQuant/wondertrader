#include "Share/ModuleHelper.hpp"

extern "C" __declspec(dllexport) const char* module_a_second_tu()
{
    return getBinDir();
}
