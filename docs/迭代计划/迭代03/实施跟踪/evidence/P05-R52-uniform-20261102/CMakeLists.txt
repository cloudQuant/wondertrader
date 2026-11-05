cmake_minimum_required(VERSION 3.20)
project(storage_bounds LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address,undefined)
set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
include_directories(BEFORE
 /usr/local/include
 /opt/homebrew/Cellar/googletest/1.17.0/include
 /opt/homebrew/Cellar/boost/1.88.0/include
 /opt/homebrew/Cellar/fmt/11.2.0/include
 /opt/homebrew/Cellar/spdlog/1.15.3/include
 /opt/homebrew/Cellar/googletest/1.17.0/include/googletest/googletest/include
 /opt/homebrew/Cellar/googletest/1.17.0/include/googletest/googletest
 ${SRC} ${SRC}/Includes ${SRC}/Share ${SRC}/WTSUtils ${SRC}/WTSUtils/yamlcpp ${SRC}/WTSTools)
add_compile_definitions(FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL WT_RDM_DT_READER_TEST_HOOKS)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include stdlib.h -include ${SRC}/Share/fmt_enum_compat.h>")
file(GLOB UTILS ${SRC}/WTSUtils/*.cpp ${SRC}/WTSUtils/zstdlib/*.c ${SRC}/WTSUtils/yamlcpp/*.cpp ${SRC}/WTSUtils/lmdb/*.c ${SRC}/WTSUtils/StackTracer/StackTracer.cpp)
file(GLOB TOOLS ${SRC}/WTSTools/*.cpp)
add_executable(bounds
 ${SRC}/WtDataStorage/WtRdmDtReader.cpp ${SRC}/WtDataStorage/WtDataReader.cpp
 ${SRC}/TestUnits/coverage/q03_storage3/q03_rdm_kline_file_test.cpp
 ${SRC}/TestUnits/coverage/r23_storage4/r52_rdm_hot_adj_test.cpp
 ${UTILS} ${TOOLS}
 /opt/homebrew/Cellar/googletest/1.17.0/include/googletest/googletest/src/gtest-all.cc
 /opt/homebrew/Cellar/googletest/1.17.0/include/googletest/googletest/src/gtest_main.cc)
target_link_libraries(bounds /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_filesystem.dylib /opt/homebrew/Cellar/boost/1.88.0/lib/libboost_thread.dylib /usr/local/lib/libiconv.dylib pthread)
