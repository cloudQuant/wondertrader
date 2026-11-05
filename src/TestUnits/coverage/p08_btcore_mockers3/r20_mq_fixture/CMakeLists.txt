cmake_minimum_required(VERSION 3.18)
project(r20_event_notifier_fake_mq LANGUAGES CXX)

# Standalone focused build: this does not modify or invoke the coverage suite.
if(WIN32)
    message(FATAL_ERROR "The standalone fake-MQ harness has only been prepared for Unix-like platforms")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(WT_R20_SRC "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(WT_R20_TOOLS_LIBRARY "" CACHE FILEPATH "Path to a compatible libWTSTools.a")
set(WT_R20_UTILS_LIBRARY "" CACHE FILEPATH "Path to a compatible libWTSUtils.a")
set(WT_R20_EXTRA_INCLUDES "" CACHE STRING "Semicolon-separated Boost, fmt, spdlog and rapidjson include roots")
set(WT_R20_EXTRA_LIBRARIES "" CACHE STRING "Semicolon-separated libraries required by the static dependencies")
foreach(lib IN ITEMS WT_R20_TOOLS_LIBRARY WT_R20_UTILS_LIBRARY)
    if(NOT EXISTS "${${lib}}")
        message(FATAL_ERROR "Set ${lib} to an existing compatible archive")
    endif()
endforeach()
find_package(Threads REQUIRED)

function(add_fake name)
    add_library(fake_${name} SHARED fake_mq.cpp)
    target_include_directories(fake_${name} PRIVATE "${WT_R20_SRC}")
    if(ARGN)
        target_compile_definitions(fake_${name} PRIVATE ${ARGN})
    endif()
    set_target_properties(fake_${name} PROPERTIES OUTPUT_NAME WtMsgQue
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/plugins/${name}")
endfunction()
add_fake(full)
add_fake(missing_create OMIT_CREATE)
add_fake(missing_destroy OMIT_DESTROY)
add_fake(missing_register OMIT_REGISTER)
add_fake(missing_publish OMIT_PUBLISH)
add_fake(zero FAIL_CREATE)

add_executable(notifier_check notifier_check.cpp
    "${WT_R20_SRC}/WtBtCore/EventNotifier.cpp"
    "${WT_R20_SRC}/WtBtCore/WtHelper.cpp")
target_include_directories(notifier_check PRIVATE "${WT_R20_SRC}" ${WT_R20_EXTRA_INCLUDES})
target_compile_definitions(notifier_check PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
target_compile_options(notifier_check PRIVATE -include "${WT_R20_SRC}/Share/fmt_enum_compat.h")
target_link_libraries(notifier_check PRIVATE "${WT_R20_TOOLS_LIBRARY}"
    "${WT_R20_UTILS_LIBRARY}" ${WT_R20_EXTRA_LIBRARIES} Threads::Threads ${CMAKE_DL_LIBS})
add_dependencies(notifier_check fake_full fake_missing_create fake_missing_destroy
    fake_missing_register fake_missing_publish fake_zero)

enable_testing()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run" "${CMAKE_CURRENT_BINARY_DIR}/logs")
function(add_check name mode plugin)
    add_test(NAME ${name} COMMAND notifier_check "${mode}" "$<TARGET_FILE_DIR:fake_${plugin}>" ${ARGN})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/run"
        ENVIRONMENT "WT_R20_MQ_LOG=${CMAKE_CURRENT_BINARY_DIR}/logs/${name}.log;LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/logs/${name}.profraw")
endfunction()
add_check(missing_create missing missing_create)
add_check(missing_destroy missing missing_destroy)
add_check(missing_register missing missing_register)
add_check(optional_publisher no-publish missing_publish)
add_check(zero_server_id zero zero)
add_check(full_plugin full full)
add_check(retry_after_missing_register retry missing_register "$<TARGET_FILE_DIR:fake_full>")
add_check(retry_after_zero_id retry-zero zero "$<TARGET_FILE_DIR:fake_full>")
add_check(full_plugin_single full-single full)
add_check(optional_publisher_single no-publish-single missing_publish)
# R20_wtbtcore#3 (card 9): explicit release, release before init, and release
# while publications are in flight (the fake sleeps inside publish).
add_check(release_destroys_once release full)
add_check(release_before_init release-before-init full)
add_check(concurrent_release concurrent-release full)
set_tests_properties(concurrent_release PROPERTIES ENVIRONMENT
    "WT_R20_MQ_LOG=${CMAKE_CURRENT_BINARY_DIR}/logs/concurrent_release.log;WT_R20_MQ_SLOW_PUBLISH_US=2000;LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/logs/concurrent_release.profraw")

