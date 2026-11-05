cmake_minimum_required(VERSION 3.16)
project(i03_slice_shared_owner LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WT_SRC_ROOT "${REPO_SRC}" CACHE PATH "src root whose product sources are compiled (working tree or baseline snapshot)")
set(WT_LIB_DIR "" CACHE PATH "directory with libWTSUtils/libWTSTools/libWtBtCore/libWtDtCore/libWtCore built from WT_SRC_ROOT")
set(WT_BASELINE_ROOT "" CACHE PATH "baseline (pre-change) src root, used by the ABI old-header plugin")
set(WT_SAN "none" CACHE STRING "none|address|thread")
option(WT_GREEN "working-tree build: enable owner-API tests" ON)

if(WT_LIB_DIR STREQUAL "")
	message(FATAL_ERROR "WT_LIB_DIR is required")
endif()

set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(BOOST_LIB /opt/homebrew/Cellar/boost/1.88.0/lib)
set(Q03_DIR "${WT_SRC_ROOT}/TestUnits/coverage/q03_storage3")

set(SAN_FLAGS "")
if(WT_SAN STREQUAL "address")
	# alignment is excluded: the 12-byte packed history block header makes record
	# arrays misaligned (pre-existing, tracked by BUG-R132-001; not part of card 11).
	set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(WT_SAN STREQUAL "thread")
	set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

function(i03_setup tgt)
	target_include_directories(${tgt} PRIVATE "${WT_SRC_ROOT}" "${Q03_DIR}")
	target_include_directories(${tgt} SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
	target_compile_definitions(${tgt} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
	if(WT_GREEN)
		target_compile_definitions(${tgt} PRIVATE WT_I03_GREEN=1)
	endif()
	target_compile_options(${tgt} PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
	target_link_options(${tgt} PRIVATE ${SAN_FLAGS})
	target_link_directories(${tgt} PRIVATE /opt/homebrew/lib /usr/local/lib)
endfunction()

set(GTEST_LIBS /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a)
set(SYS_LIBS ${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)

# WtRdmDtReader: held slices across clearCache/reload, concurrent read + clear
add_executable(i03_slice_rdm
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	"${Q03_DIR}/q03_rdm_clear_lifetime_test.cpp"
	rdm_shared_owner_test.cpp)
i03_setup(i03_slice_rdm)
target_link_libraries(i03_slice_rdm PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a" ${GTEST_LIBS} ${SYS_LIBS})

# WtDtServo/WtDataManager: clear_cache + destructor order (R102/R123)
add_executable(i03_slice_servo
	"${WT_SRC_ROOT}/WtDtServo/WtDataManager.cpp"
	"${WT_SRC_ROOT}/WtDtServo/WtDtRunner.cpp"
	"${WT_SRC_ROOT}/WtDtServo/ParserAdapter.cpp"
	"${WT_SRC_ROOT}/WtDtServo/WtHelper.cpp"
	servo_owner_test.cpp)
i03_setup(i03_slice_servo)
target_link_libraries(i03_slice_servo PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a" ${GTEST_LIBS} ${SYS_LIBS})

# HisDataReplayer query caches (NEW-095 linkage)
add_executable(i03_slice_replay
	"${WT_SRC_ROOT}/WtBtCore/HisDataReplayer.cpp"
	replay_owner_test.cpp)
i03_setup(i03_slice_replay)
target_link_libraries(i03_slice_replay PRIVATE "${WT_LIB_DIR}/libWtBtCore.a" "${WT_LIB_DIR}/libWtDtCore.a" "${WT_LIB_DIR}/libWtCore.a"
	"${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a" ${GTEST_LIBS} /usr/local/lib/libnanomsg.dylib ${SYS_LIBS})

# reader-level micro-benchmark (meaningful only without sanitizers, Release)
add_executable(i03_slice_reader_bench
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	bench_reader.cpp)
i03_setup(i03_slice_reader_bench)
target_link_libraries(i03_slice_reader_bench PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a" ${GTEST_LIBS} ${SYS_LIBS})

# header-level micro-benchmark
add_executable(i03_slice_bench bench_slice.cpp)
i03_setup(i03_slice_bench)

if(WT_GREEN)
	add_executable(i03_slice_unit unit_owner_test.cpp)
	i03_setup(i03_slice_unit)
	target_link_libraries(i03_slice_unit PRIVATE ${GTEST_LIBS} pthread)

	if(NOT WT_BASELINE_ROOT STREQUAL "")
		# Old-header "plugin": compiled ONLY against the baseline WTSDataDef.hpp
		add_library(I03OldSlicePlugin MODULE abi_old_plugin.cpp)
		target_include_directories(I03OldSlicePlugin PRIVATE "${WT_BASELINE_ROOT}")
		target_include_directories(I03OldSlicePlugin SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
		target_compile_options(I03OldSlicePlugin PRIVATE "SHELL:-include cstdlib" -g -fvisibility=hidden -fvisibility-inlines-hidden ${SAN_FLAGS})
		target_link_options(I03OldSlicePlugin PRIVATE ${SAN_FLAGS})

		add_executable(i03_slice_abi abi_host_test.cpp)
		i03_setup(i03_slice_abi)
		target_compile_definitions(i03_slice_abi PRIVATE I03_OLD_PLUGIN_PATH="$<TARGET_FILE:I03OldSlicePlugin>")
		target_link_libraries(i03_slice_abi PRIVATE ${GTEST_LIBS} dl pthread)
		add_dependencies(i03_slice_abi I03OldSlicePlugin)
	endif()
endif()

# Directly related pre-existing WtRdmDtReader tests (not the full suite), run against
# the same product sources to detect behavior changes caused by the owner refactor.
set(COV "${WT_SRC_ROOT}/TestUnits/coverage")
file(GLOB I03_RDM_REGRESS
	"${COV}/q03_storage3/*rdm*_test.cpp"
	"${COV}/q03_storage3/r162_reader_edge_test.cpp"
	"${COV}/q03_storage3/r132_reader_his_test.cpp"
	"${COV}/p09_storage_readers/p09_rdm_*_test.cpp"
	"${COV}/r23_storage4/r23_rdm_count_slice_test.cpp"
	"${COV}/r23_storage4/r52_rdm_hot_adj_test.cpp"
	"${COV}/r1_storage/r1_rdm_hft_slice_test.cpp")
list(FILTER I03_RDM_REGRESS EXCLUDE REGEX "q03_rdm_clear_lifetime_test")
add_executable(i03_slice_rdm_regress
	"${WT_SRC_ROOT}/WtDataStorage/WtRdmDtReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataReader.cpp"
	"${WT_SRC_ROOT}/WtDataStorage/WtDataWriter.cpp"
	${I03_RDM_REGRESS})
i03_setup(i03_slice_rdm_regress)
target_include_directories(i03_slice_rdm_regress PRIVATE "${COV}" "${COV}/q03_storage3" "${COV}/p09_storage_readers" "${COV}/r23_storage4" "${COV}/r1_storage")
target_compile_definitions(i03_slice_rdm_regress PRIVATE WT_RDM_DT_READER_TEST_HOOKS WT_DATA_WRITER_TEST_HOOKS)
target_link_libraries(i03_slice_rdm_regress PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a" ${GTEST_LIBS} ${SYS_LIBS})
