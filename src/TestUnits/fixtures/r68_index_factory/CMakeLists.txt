# BUG-R68-003: ordinary-file dynamic fixture is loaded only by the r68 test.
add_library(R68IndexCountingFixture MODULE counting_factory.cpp)
target_include_directories(R68IndexCountingFixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set_target_properties(R68IndexCountingFixture PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set_target_properties(R68IndexCountingFixture PROPERTIES SUFFIX ".dylib")
else()
    set_target_properties(R68IndexCountingFixture PROPERTIES SUFFIX ".so")
endif()
add_dependencies(CoverageTests_dtcore R68IndexCountingFixture)
target_compile_definitions(CoverageTests_dtcore PRIVATE
    "WT_R68_COUNTING_PLUGIN_PATH=\"$<TARGET_FILE:R68IndexCountingFixture>\"")
