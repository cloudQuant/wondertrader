cmake_minimum_required(VERSION 3.16)
project(i03_btcore_s5 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 迭代03 s5-btcore red/green fixture (2026-11-04).
# WT_SRC_ROOT selects the product sources (working tree = green; an rsync
# --no-links copy with the pre-change product files overlaid = red). Test
# sources always come from this repository's coverage lanes.
# WT_LIB_DIR provides libWTSTools.a / libWTSUtils.a; WT_PROFILE_LINK=ON when
# those archives carry -fprofile-instr-generate instrumentation.
get_filename_component(REPO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(WT_SRC_ROOT "${REPO_SRC}" CACHE PATH "src root whose product sources are compiled")
set(WT_LIB_DIR "" CACHE PATH "directory with libWTSUtils.a / libWTSTools.a")
option(WT_PROFILE_LINK "link with -fprofile-instr-generate" ON)
option(WT_S5_RED "red build: product predates new diagnostic APIs" OFF)
if(WT_LIB_DIR STREQUAL "")
	message(FATAL_ERROR "WT_LIB_DIR is required")
endif()
set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(BOOST_LIB /opt/homebrew/Cellar/boost/1.88.0/lib)
set(COV "${REPO_SRC}/TestUnits/coverage")

file(GLOB BT_SRCS "${WT_SRC_ROOT}/WtBtCore/*.cpp")
set(SYS_LIBS ${BOOST_LIB}/libboost_filesystem.dylib ${BOOST_LIB}/libboost_thread.dylib /usr/local/lib/libiconv.dylib dl pthread)

function(s5_target tgt)
	add_executable(${tgt} ${BT_SRCS} ${ARGN})
	# product headers first (red/green switch), then repo src for test-only headers
	target_include_directories(${tgt} PRIVATE "${WT_SRC_ROOT}" "${REPO_SRC}" "${COV}/q02_btcore3" "${COV}")
	target_include_directories(${tgt} SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include /usr/local/include)
	target_compile_definitions(${tgt} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
	target_compile_options(${tgt} PRIVATE "SHELL:-include cstdlib" "SHELL:-include ${WT_SRC_ROOT}/Share/fmt_enum_compat.h" -g -Wno-error)
	if(WT_PROFILE_LINK)
		target_link_options(${tgt} PRIVATE -fprofile-instr-generate)
	endif()
	target_link_options(${tgt} PRIVATE "-Wl,-undefined,dynamic_lookup")
	target_link_directories(${tgt} PRIVATE /opt/homebrew/lib /usr/local/lib)
	target_link_libraries(${tgt} PRIVATE "${WT_LIB_DIR}/libWTSTools.a" "${WT_LIB_DIR}/libWTSUtils.a"
		/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a ${SYS_LIBS})
endfunction()

# BUG-Q02-003 card 3d=C for Sel/Hft mockers
s5_target(s5_frozen_tdate "${COV}/q02_btcore3/s5_sel_hft_frozen_tdate_test.cpp")
# BUG-Q02-003 integration cases (fee/contract fixture under card 13d)
s5_target(s5_q02_integration "${COV}/q02_btcore3/q02_cta_replayer_integration_test.cpp")
# BUG-I02-NEW-029 per-stream out-of-session filter (counter API only in green)
s5_target(s5_new029 "${COV}/q02_btcore3/s5_new029_out_of_session_streams_test.cpp")
if(WT_S5_RED)
	target_compile_definitions(s5_new029 PRIVATE WT_S5_NO_OOS_COUNTER=1)
endif()
# bin-mode replay reads .dsb through the WtDataStorage module (dlopen by name);
# link a built copy so the module is already loaded in-process.
if(EXISTS "${WT_LIB_DIR}/libWtDataStorage.dylib")
	target_link_libraries(s5_new029 PRIVATE "${WT_LIB_DIR}/libWtDataStorage.dylib")
	set_target_properties(s5_new029 PROPERTIES BUILD_RPATH "${WT_LIB_DIR}")
endif()
