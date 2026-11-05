cmake_minimum_required(VERSION 3.16)
project(i03_dualthrust_config LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(fmt REQUIRED)
get_filename_component(WT_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_executable(i03_dualthrust_config config_test.cpp "${WT_SRC}/WtCtaStraFact/WtStraDualThrust.cpp")
target_include_directories(i03_dualthrust_config PRIVATE "${WT_SRC}")
target_link_libraries(i03_dualthrust_config PRIVATE fmt::fmt-header-only)
enable_testing()
add_test(NAME i03_dualthrust_config COMMAND i03_dualthrust_config)
find_package(GTest REQUIRED)
add_executable(i03_dualthrust_existing
    "${WT_SRC}/TestUnits/coverage/n06_strategies_mq_share/n06_cta_ctx_mock_test.cpp"
    "${WT_SRC}/TestUnits/coverage/n06_strategies_mq_share/n06_cta_stra_test.cpp"
    "${WT_SRC}/WtCtaStraFact/WtStraDualThrust.cpp"
    "${WT_SRC}/WtCtaTwoMaFact/WtCtaTwoMaFact.cpp"
    "${WT_SRC}/WtCtaTwoMaFact/WtStraTwoMa.cpp")
target_include_directories(i03_dualthrust_existing PRIVATE "${WT_SRC}")
target_link_libraries(i03_dualthrust_existing PRIVATE fmt::fmt-header-only GTest::gtest_main)
