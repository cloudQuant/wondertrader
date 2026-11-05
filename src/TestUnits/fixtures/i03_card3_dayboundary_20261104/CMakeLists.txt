cmake_minimum_required(VERSION 3.16)
project(i03_card3_dayboundary LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Decision card 3 (3a=A, 3b=C default A, 3c=C, 3d=C) fixture, 2026-11-04.
# WT_SRC_ROOT selects the product sources (working tree = green, pre-change snapshot = red);
# test sources always come from this repository. WTSTools/WTSUtils come from WT_LIB_DIR.
# The ticker (3a) probe is driven separately by r202_run.py.
get_filename_component(REPO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WT_SRC_ROOT "${REPO_SRC}" CACHE PATH "src root whose product sources are compiled")
set(WT_LIB_DIR "" CACHE PATH "directory with libWTSUtils.a / libWTSTools.a")
set(WT_SAN "none" CACHE STRING "none|address|thread")
option(WT_GREEN "working-tree build: enable tests of new hooks/config" ON)
if(WT_LIB_DIR STREQUAL "")
	message(FATAL_ERROR "WT_LIB_DIR is required")
endif()

set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(BOOST_LIB /opt/homebrew/Cellar/boost/1.88.0/lib)
set(COV "${REPO_SRC}/TestUnits/coverage")

set(SAN_FLAGS "")
if(WT_SAN STREQUAL "address")
	set(SAN_FLAGS -fsanitize=address,undefined -fno-sanitize=alignment -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(WT_SAN STREQUAL "thread")
	set(SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
endif()

function(c3_setup tgt)
	# product headers first (red/green switch), then repo src for test-only headers
	target_include_directories(${tgt} PRIVATE "${WT_SRC_ROOT}" "${WT_SRC_ROOT}/API" "${REPO_SRC}")
	target_include_directories(${tgt} SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
	target_compile_definitions(${tgt} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
	if(WT_GREEN)
		target_compile_definitions(${tgt} PRIVATE WT_C3_GREEN=1)
	endif()
	target_compile_options(${tgt} PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g ${SAN_FLAGS})
	target_link_options(${tgt} PRIVATE ${SAN_FLAGS})
	target_link_directories(${tgt} PRIVATE /opt/homebrew/lib /usr/local/lib)
endfunction()

set(GTEST_LIB /opt/homebrew/lib/libgtest.a)
set(GTEST_MAIN /opt/homebrew/lib/libgtest_main.a)
set(SYS_LIBS ${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)
set(WT_LIBS "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a")

# ---- 3b: parsers (BUG-R105-001) --------------------------------------------------------
# card3 cases (fake clock through WT_ACTDAY_TEST_CLOCK) and, separately, the existing lane
# tests of each parser compiled against the same product sources (wall-clock based).
foreach(idx 1 2 3)
	if(idx EQUAL 1)
		set(mod ParserCTP)
		set(lane t_ParserCTP)
	elseif(idx EQUAL 2)
		set(mod ParserCTPMini)
		set(lane t_ParserCTPMini)
	else()
		set(mod ParserFemas)
		set(lane t_ParserFemas)
	endif()
	add_executable(c3_actday_${mod} "${WT_SRC_ROOT}/${mod}/${mod}.cpp" c3_actday_test.cpp)
	c3_setup(c3_actday_${mod})
	target_compile_definitions(c3_actday_${mod} PRIVATE WT_C3_PARSER=${idx} WT_ACTDAY_TEST_CLOCK)
	target_include_directories(c3_actday_${mod} PRIVATE "${COV}/${lane}")
	target_link_options(c3_actday_${mod} PRIVATE "-Wl,-undefined,dynamic_lookup")
	target_link_libraries(c3_actday_${mod} PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

	file(GLOB _lane_tests "${COV}/${lane}/*_test.cpp")
	add_executable(c3_regress_${mod} "${WT_SRC_ROOT}/${mod}/${mod}.cpp" ${_lane_tests})
	c3_setup(c3_regress_${mod})
	target_include_directories(c3_regress_${mod} PRIVATE "${COV}/${lane}" "${COV}" "${WT_SRC_ROOT}/Includes" "${WT_SRC_ROOT}/Share")
	target_compile_options(c3_regress_${mod} PRIVATE -Wno-error)
	target_link_options(c3_regress_${mod} PRIVATE "-Wl,-undefined,dynamic_lookup")
	target_link_libraries(c3_regress_${mod} PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})
endforeach()

# ---- 3c: AD day bar contract (R4-BUG-13) ----------------------------------------------
set(C3_AD_SRCS
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataWriterAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtDataReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtRdmDtReaderAD.cpp"
	"${WT_SRC_ROOT}/WtDataStorageAD/WtBtDtReaderAD.cpp")
add_executable(c3_ad_daybar ${C3_AD_SRCS} c3_ad_daybar_test.cpp)
c3_setup(c3_ad_daybar)
target_include_directories(c3_ad_daybar PRIVATE "${COV}/p05_datastorage2" "${COV}")
target_link_libraries(c3_ad_daybar PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

# directly related pre-existing AD writer/reader tests (not the full suite), run case-by-case
file(GLOB C3_REGRESS_AD
	"${COV}/p05_datastorage2/*writer_ad*_test.cpp"
	"${COV}/p05_datastorage2/r112_reader_ad_day_test.cpp"
	"${COV}/p05_datastorage2/r132_reader_ad_cache_test.cpp"
	"${COV}/p05_datastorage2/r182_ad_reader_cache_test.cpp"
	"${COV}/p05_datastorage2/r100_reader_ad_test.cpp"
	"${COV}/p05_datastorage2/r112_reader_ad_kline_test.cpp"
	"${COV}/p05_datastorage2/l03_ad_bar_alignment_test.cpp"
	"${COV}/p05_datastorage2/l03_ad_writer_date_test.cpp"
	"${COV}/p05_datastorage2/p05_btdtreader_ad_lmdb_test.cpp"
	"${COV}/p05_datastorage2/p05_rdmdtreader_ad_slices_test.cpp")
add_executable(c3_regress_ad ${C3_AD_SRCS} ${C3_REGRESS_AD})
c3_setup(c3_regress_ad)
target_include_directories(c3_regress_ad PRIVATE "${COV}/p05_datastorage2" "${COV}")
target_compile_options(c3_regress_ad PRIVATE -Wno-error)
target_link_libraries(c3_regress_ad PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

# ---- 3d: CtaMocker frozen (BUG-Q02-003) -----------------------------------------------
file(GLOB C3_BT_SRCS "${WT_SRC_ROOT}/WtBtCore/*.cpp")
add_executable(c3_cta_frozen ${C3_BT_SRCS} c3_cta_frozen_test.cpp)
c3_setup(c3_cta_frozen)
target_compile_options(c3_cta_frozen PRIVATE -Wno-error)
target_link_options(c3_cta_frozen PRIVATE "-Wl,-undefined,dynamic_lookup")
target_link_libraries(c3_cta_frozen PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})

file(GLOB C3_REGRESS_Q02
	"${COV}/q02_btcore3/q02_cta_*_test.cpp")
add_executable(c3_regress_q02 ${C3_BT_SRCS} ${C3_REGRESS_Q02})
c3_setup(c3_regress_q02)
target_include_directories(c3_regress_q02 PRIVATE "${COV}/q02_btcore3" "${COV}")
target_compile_options(c3_regress_q02 PRIVATE -Wno-error)
target_link_options(c3_regress_q02 PRIVATE "-Wl,-undefined,dynamic_lookup")
target_link_libraries(c3_regress_q02 PRIVATE ${WT_LIBS} ${GTEST_MAIN} ${GTEST_LIB} ${SYS_LIBS})
