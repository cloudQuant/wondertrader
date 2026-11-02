# Windows-specific BUG-L00-010 fixture: two DLLs in distinct directories,
# including two independent ModuleHelper.hpp translation units in the first.
add_library(ModuleHelperWindowsA SHARED module_a.cpp module_a_second.cpp)
add_library(ModuleHelperWindowsB SHARED module_b.cpp)
add_executable(ModuleHelperWindowsTest module_helper_windows_test.cpp)
foreach(target ModuleHelperWindowsA ModuleHelperWindowsB ModuleHelperWindowsTest)
    target_include_directories(${target} PRIVATE ${WT_COV_INCS})
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
endforeach()
# Generator expressions prevent multi-config generators appending the same
# configuration directory to both fixture paths.
set_target_properties(ModuleHelperWindowsA PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/helper_a/$<0:>")
set_target_properties(ModuleHelperWindowsB PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/helper_b/$<0:>")
add_dependencies(ModuleHelperWindowsTest ModuleHelperWindowsA ModuleHelperWindowsB)
# The enclosing project does not enable CTest; keep execution explicit.
add_custom_target(run_ModuleHelperWindowsTest
    COMMAND "$<TARGET_FILE:ModuleHelperWindowsTest>"
        "$<TARGET_FILE:ModuleHelperWindowsA>" "$<TARGET_FILE:ModuleHelperWindowsB>"
    DEPENDS ModuleHelperWindowsTest
    VERBATIM)
