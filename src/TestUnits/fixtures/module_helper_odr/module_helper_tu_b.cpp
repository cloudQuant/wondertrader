#include "Share/ModuleHelper.hpp"

extern "C" const char* module_helper_path_from_tu_b()
{
	return getInstPath().c_str();
}
