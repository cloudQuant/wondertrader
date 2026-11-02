#include "Share/ModuleHelper.hpp"

#include <string>

namespace {
const std::string pathDuringInitialization(getBinDir());
}

extern "C" __declspec(dllexport) const char* module_a_first_tu()
{
    return getBinDir();
}

extern "C" __declspec(dllexport) const char* module_a_static_initialization()
{
    return pathDuringInitialization.c_str();
}
