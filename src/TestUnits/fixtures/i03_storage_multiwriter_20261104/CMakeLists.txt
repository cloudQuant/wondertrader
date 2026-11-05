cmake_minimum_required(VERSION 3.16)
project(i03_storage_multiwriter LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Decision card 13 (13a=A, 13b=B, 13c=A) fixture. WT_SRC_ROOT selects the product sources
# (working tree = green, pre-change snapshot = red). Product writer/reader TUs are compiled
# directly into each test with the selected sanitizer; WTSTools/WTSUtils come from WT_LIB_DIR.
get_filename_component(REPO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WT_SRC_ROOT "${REPO_SRC}" CACHE PATH "src root whose product sources are compiled")
set(WT_LIB_DIR "" CACHE PATH "directory with libWTSUtils.a / libWTSTools.a")
set(WT_SAN "none" CACHE STRING "none|address|thread")
option(WT_GREEN "working-tree build: enable tests of new protocol APIs/hooks" ON)
if(WT_LIB_DIR STREQUAL "")
	message(FATAL_ERROR "WT_LIB_DIR is required")
endif()

set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(BOOST_LIB /opt/homebrew/Cellar/boost/1.88.0/lib)
set(COV "${WT_SRC_ROOT}/TestUnits/coverage")

set(SAN_FLAGS "")
if(WT_SAN STREQUAL "address")
	# alignment excluded as in the card 11 fixture (packed history headers, BUG-R132-001).
	set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(WT_SAN STREQUAL "thread")
	set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

function(mw_setup tgt)
	target_include_directories(${tgt} PRIVATE "${WT_SRC_ROOT}")
	target_include_directories(${tgt} SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
	target_compile_definitions(${tgt} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
	if(WT_GREEN)
		target_compile_definitions(${tgt} PRIVATE WT_MW_GREEN=1)
	endif()
	target_compile_options(${tgt} PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
	target_link_options(${tgt} PRIVATE ${SAN_FLAGS})
	target_link_directories(${tgt} PRIVATE /opt/homebrew/lib /usr/local/lib)
endfunction()

set(GTEST_LIB /opt/homebrew/lib/libgtest.a)
set(GTEST_MAIN /opt/homebrew/lib/libgtest_main.a)
set(SYS_LIBS ${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)
set(WT_LIBS "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a")

# WtDataStorage writer: multi-process append/cache/crash tests (custom main + exec helpers)
add_executable(mw_storage "${WT_SRC_ROOT}/WtDataStorage/WtDataWriter.cpp" mw_storage_test.cpp)
mw_setup(mw_storage)
target_compile_definitions(mw_storage PRIVATE WT_DATA_WRITER_TEST_HOOKS)
target_include_directories(mw_storage PRIVATE "${COV}/r23_storage4" "${COV}/p10_storage_writer")
target_link_libraries(mw_storage PRIVATE ${WT_LIBS} ${GTEST_LIB} ${SYS_LIBS})

# WtDataStorage reader: visibility rule on the real WtDataReader
add_executable(mw_reader "${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp" mw_reader_test.cpp)
mw_setup(mw_reader)
target_include_directories(mw_reader PRIVATE "${COV}/q03_storage3")
target_link_libraries(mw_reader PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

# WtDataStorageAD writer: prepare-then-publish, quarantine, minute key bound, two-process cache
add_executable(mw_ad "${WT_SRC_ROOT}/WtDataStorageAD/WtDataWriterAD.cpp" mw_ad_test.cpp)
mw_setup(mw_ad)
target_include_directories(mw_ad PRIVATE "${COV}/p05_datastorage2")
target_link_libraries(mw_ad PRIVATE ${WT_LIBS} ${GTEST_LIB} ${SYS_LIBS})

# Directly related pre-existing tests (not the full suite), compiled against the same product
# sources to detect behaviour changes caused by the publish protocol. Run case-by-case.
file(GLOB MW_REGRESS_STORAGE
	"${COV}/r23_storage4/*writer*_test.cpp"
	"${COV}/p10_storage_writer/*_test.cpp"
	"${COV}/q03_storage3/q03_reader_file_test.cpp"
	"${COV}/q03_storage3/r122_reader_rt_branches_test.cpp"
	"${COV}/q03_storage3/r94_reader_loader_rt_test.cpp"
	"${COV}/q03_storage3/r94_reader_hft_size_test.cpp"
	"${COV}/q03_storage3/r162_reader_edge_test.cpp"
	"${COV}/q03_storage3/r182_reader_integrated_edges_test.cpp")
add_executable(mw_regress_storage
	"${WT_SRC_ROOT}/WtDataStorage/WtDataWriter.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	${MW_REGRESS_STORAGE})
mw_setup(mw_regress_storage)
target_compile_definitions(mw_regress_storage PRIVATE WT_DATA_WRITER_TEST_HOOKS WT_RDM_DT_READER_TEST_HOOKS)
target_include_directories(mw_regress_storage PRIVATE "${COV}/r23_storage4" "${COV}/p10_storage_writer" "${COV}/q03_storage3" "${COV}")
target_link_libraries(mw_regress_storage PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

file(GLOB MW_REGRESS_AD
	"${COV}/p05_datastorage2/*writer_ad*_test.cpp"
	"${COV}/p05_datastorage2/l03_ad_*_test.cpp"
	"${COV}/p05_datastorage2/p05_minbar_key_overflow_test.cpp"
	"${COV}/p05_datastorage2/r4_ad_init_retry_isolation_test.cpp")
add_executable(mw_regress_ad "${WT_SRC_ROOT}/WtDataStorageAD/WtDataWriterAD.cpp" ${MW_REGRESS_AD})
mw_setup(mw_regress_ad)
target_include_directories(mw_regress_ad PRIVATE "${COV}/p05_datastorage2" "${COV}")
target_link_libraries(mw_regress_ad PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})
