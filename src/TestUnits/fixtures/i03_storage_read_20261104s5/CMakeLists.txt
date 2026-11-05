cmake_minimum_required(VERSION 3.16)
project(i03_storage_read_s5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Red/green fixture for iteration 03 package s5 storage-read (L04-004, R100-003/R32-003,
# R122-002, R21-BUG-07). WT_SRC_ROOT selects the working tree (green) or a snapshot whose
# reader files are the pre-change copies (red, WT_S5_BASELINE=ON excludes new-API tests).
get_filename_component(REPO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WT_SRC_ROOT "${REPO_SRC}" CACHE PATH "src root whose product and test sources are compiled")
set(WT_LIB_DIR "" CACHE PATH "directory with libWTSUtils.a/libWTSTools.a (plain, same compiler)")
set(WT_SAN "address" CACHE STRING "none|address|thread")
option(WT_S5_BASELINE "red build against pre-change product sources" OFF)
# Pre-existing packed cache-item fixtures in the p05 lane trip UBSan alignment before
# reaching their assertions (I03-NEW-007 territory); allow excluding only that check.
option(WT_SAN_ALIGNMENT "keep UBSan alignment checking in address builds" ON)
if(WT_LIB_DIR STREQUAL "")
	message(FATAL_ERROR "WT_LIB_DIR is required")
endif()

set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(BOOST_LIB /opt/homebrew/Cellar/boost/1.88.0/lib)
set(COV "${WT_SRC_ROOT}/TestUnits/coverage")
set(SAN_FLAGS "")
if(WT_SAN STREQUAL "address")
	set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
	if(NOT WT_SAN_ALIGNMENT)
		list(APPEND SAN_FLAGS -fno-sanitize=alignment)
	endif()
elseif(WT_SAN STREQUAL "thread")
	set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

add_executable(s5_ad_read
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtRdmDtReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtBtDtReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataWriterAD.cpp"
	"${COV}/p05_datastorage2/s5_reader_ad_bounds_test.cpp"
	"${COV}/p05_datastorage2/s5_rdm_ad_snapshot_test.cpp"
	"${COV}/p05_datastorage2/r21_rdmbars_kline_test.cpp")
target_include_directories(s5_ad_read PRIVATE "${WT_SRC_ROOT}" "${COV}" "${COV}/p05_datastorage2")
target_include_directories(s5_ad_read SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
target_compile_definitions(s5_ad_read PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
if(WT_S5_BASELINE)
	target_compile_definitions(s5_ad_read PRIVATE WT_S5_BASELINE=1)
endif()
target_compile_options(s5_ad_read PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
target_link_options(s5_ad_read PRIVATE ${SAN_FLAGS})
target_link_directories(s5_ad_read PRIVATE /opt/homebrew/lib /usr/local/lib)
target_link_libraries(s5_ad_read PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a"
	/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
	${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)

# Card-15 Z-item reruns on WtDataStorage readers (NEW-032/082 q03 day tail fragment,
# NEW-059 r162 historical HFT endpoints, NEW-060 r193 unknown loader period). These
# product files are not modified by this package; the target only re-runs the cases.
add_executable(s5_storage_z
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataWriter.cpp"
	"${COV}/q03_storage3/q03_rdm_kline_file_test.cpp"
	"${COV}/q03_storage3/r162_reader_edge_test.cpp"
	"${COV}/q03_storage3/r193_storage_reader_residual_test.cpp")
target_include_directories(s5_storage_z PRIVATE "${WT_SRC_ROOT}" "${COV}" "${COV}/q03_storage3" "${COV}/r23_storage4" "${COV}/p09_storage_readers")
target_include_directories(s5_storage_z SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
target_compile_definitions(s5_storage_z PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL WT_RDM_DT_READER_TEST_HOOKS WT_DATA_WRITER_TEST_HOOKS)
target_compile_options(s5_storage_z PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
target_link_options(s5_storage_z PRIVATE ${SAN_FLAGS})
target_link_directories(s5_storage_z PRIVATE /opt/homebrew/lib /usr/local/lib)
target_link_libraries(s5_storage_z PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a"
	/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
	${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)

# Whole p05_datastorage2 lane (AD readers/writer) under ASan/UBSan against WT_SRC_ROOT,
# used as the directly related regression for this package's AD reader changes.
file(GLOB S5_P05_LANE "${COV}/p05_datastorage2/*.cpp")
add_executable(s5_ad_lane
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtRdmDtReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtBtDtReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataWriterAD.cpp"
	${S5_P05_LANE})
target_include_directories(s5_ad_lane PRIVATE "${WT_SRC_ROOT}" "${COV}" "${COV}/p05_datastorage2")
target_include_directories(s5_ad_lane SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
target_compile_definitions(s5_ad_lane PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
if(WT_S5_BASELINE)
	target_compile_definitions(s5_ad_lane PRIVATE WT_S5_BASELINE=1)
endif()
target_compile_options(s5_ad_lane PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
target_link_options(s5_ad_lane PRIVATE ${SAN_FLAGS})
target_link_directories(s5_ad_lane PRIVATE /opt/homebrew/lib /usr/local/lib)
target_link_libraries(s5_ad_lane PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a"
	/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
	${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)

# Round 2: WtDataStorage readers with UBSan alignment kept ON (R132-001 packed history
# records, I03-NEW-052 unknown commodity, I03-NEW-039 long stdCode keys) plus the
# directly related existing history-file cases.
add_executable(s5_storage_align
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataWriter.cpp"
	"${COV}/q03_storage3/s5_storage_reader_guards_test.cpp"
	"${COV}/q03_storage3/r162_reader_edge_test.cpp"
	"${COV}/q03_storage3/r112_reader_crossday_missing_test.cpp"
	"${COV}/q03_storage3/q03_rdm_tick_file_test.cpp"
	"${COV}/q03_storage3/r142_reader_hft_his_test.cpp"
	"${COV}/q03_storage3/r132_reader_his_test.cpp"
	"${COV}/q03_storage3/r100_rdm_crossday_test.cpp")
target_include_directories(s5_storage_align PRIVATE "${WT_SRC_ROOT}" "${COV}" "${COV}/q03_storage3" "${COV}/r23_storage4" "${COV}/p09_storage_readers")
target_include_directories(s5_storage_align SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
target_compile_definitions(s5_storage_align PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL WT_RDM_DT_READER_TEST_HOOKS WT_DATA_WRITER_TEST_HOOKS)
target_compile_options(s5_storage_align PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
target_link_options(s5_storage_align PRIVATE ${SAN_FLAGS})
target_link_directories(s5_storage_align PRIVATE /opt/homebrew/lib /usr/local/lib)
target_link_libraries(s5_storage_align PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a"
	/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
	${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)
