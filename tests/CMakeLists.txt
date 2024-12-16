cmake_minimum_required(VERSION 3.14)

# 项目名称
project(wtTest)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(THREADS_PREFER_PTHREAD_FLAG ON)

# 依赖库
find_package(OpenSSL REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

# 启用测试
enable_testing()

# 包含目录
include_directories(
        ${CMAKE_SOURCE_DIR}/../src  # 主项目 src
        ${CMAKE_SOURCE_DIR}/src     # tests/src
        ${Boost_INCLUDE_DIRS}
)

# 自动查找 tests/src/ 和 tests/src/ 目录下的所有测试文件
file(GLOB_RECURSE TEST_SOURCES "src/*.cpp" "src/*/*.cpp")

# 如果没有找到任何测试源文件，抛出错误
if(TEST_SOURCES STREQUAL "")
    message(FATAL_ERROR "No source files found in the tests/ directory")
endif()

# 定义可执行文件目标并传入源文件
add_executable(${PROJECT_NAME} ${TEST_SOURCES})

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        gtest
        gtest_main
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
        ${RapidJSON_LIBRARIES}
)
# 自动发现和添加测试
add_test(AllTestsInMain ${PROJECT_NAME})



