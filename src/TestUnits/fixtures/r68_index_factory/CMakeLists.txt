# BUG-R68-003: ordinary-file dynamic fixture is loaded only by the r68 test.
add_library(R68IndexCountingFixture MODULE counting_factory.cpp)
add_library(R68IndexCountingConflictFixture MODULE counting_factory.cpp)
target_include_directories(R68IndexCountingFixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_include_directories(R68IndexCountingConflictFixture PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
target_compile_definitions(R68IndexCountingConflictFixture PRIVATE R68_FACTORY_VARIANT="second")
set_target_properties(R68IndexCountingFixture PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
set_target_properties(R68IndexCountingConflictFixture PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set_target_properties(R68IndexCountingFixture PROPERTIES SUFFIX ".dylib")
    set_target_properties(R68IndexCountingConflictFixture PROPERTIES SUFFIX ".dylib")
else()
    set_target_properties(R68IndexCountingFixture PROPERTIES SUFFIX ".so")
    set_target_properties(R68IndexCountingConflictFixture PROPERTIES SUFFIX ".so")
endif()
add_dependencies(CoverageTests_dtcore R68IndexCountingFixture R68IndexCountingConflictFixture)
target_compile_definitions(CoverageTests_dtcore PRIVATE
    "WT_R68_COUNTING_PLUGIN_PATH=\"$<TARGET_FILE:R68IndexCountingFixture>\""
    "WT_R68_CONFLICT_PLUGIN_PATH=\"$<TARGET_FILE:R68IndexCountingConflictFixture>\"")
