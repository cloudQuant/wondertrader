cmake_minimum_required(VERSION 3.16)
project(i03_uft_parser_lifecycle LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
# WT_BUILD: an existing coverage build providing libWTSTools.a/libWTSUtils.a.
set(WT_BUILD "" CACHE PATH "Build providing static dependencies")
# WT_BEFORE: directory holding src/WtUftCore/ParserAdapter.{h,cpp} pre-change copies.
set(WT_BEFORE "" CACHE PATH "Pre-change source copies")
set(WT_SANITIZE "" CACHE STRING "Optional -fsanitize= value")
set(BOOST_INC /opt/homebrew/opt/boost/include)
set(FIX "${CMAKE_CURRENT_SOURCE_DIR}")

foreach(mode RANGE 0 12)
  add_library(Pvt2${mode} SHARED "${FIX}/provider.cpp")
  target_compile_definitions(Pvt2${mode} PRIVATE P_MODE=${mode})
  target_include_directories(Pvt2${mode} PRIVATE "${ROOT}/src" /opt/homebrew/include "${BOOST_INC}")
  set_target_properties(Pvt2${mode} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/parsers" PREFIX "lib" SUFFIX ".dylib")
endforeach()

set(WT_LIBS
  "${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a"
  "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  boost_filesystem boost_thread pthread dl iconv)

function(uft_parser_target name adapter_cpp is_new)
  add_executable(${name} "${FIX}/parser_test.cpp" "${adapter_cpp}" "${ROOT}/src/WtUftCore/WtHelper.cpp")
  if(is_new)
    target_compile_definitions(${name} PRIVATE WT_HOST_NEW)
    target_include_directories(${name} BEFORE PRIVATE "${ROOT}/src")
  else()
    # Pre-change header first; its own relative includes resolve via src/WtUftCore.
    target_include_directories(${name} BEFORE PRIVATE "${CMAKE_BINARY_DIR}/base_src")
    target_include_directories(${name} PRIVATE "${ROOT}/src" "${ROOT}/src/WtUftCore")
  endif()
  target_include_directories(${name} PRIVATE /opt/homebrew/include "${BOOST_INC}")
  target_compile_definitions(${name} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL BOOST_BIND_GLOBAL_PLACEHOLDERS)
  target_compile_options(${name} PRIVATE -include "${ROOT}/src/Share/fmt_enum_compat.h")
  if(WT_SANITIZE)
    target_compile_options(${name} PRIVATE -fsanitize=${WT_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(${name} PRIVATE -fsanitize=${WT_SANITIZE})
  endif()
  target_link_options(${name} PRIVATE -fprofile-instr-generate)
  target_link_directories(${name} PRIVATE /opt/homebrew/lib /usr/local/lib)
  target_link_libraries(${name} PRIVATE ${WT_LIBS})
  foreach(mode RANGE 0 12)
    add_dependencies(${name} Pvt2${mode})
  endforeach()
endfunction()

uft_parser_target(uft_parser_cur "${ROOT}/src/WtUftCore/ParserAdapter.cpp" ON)

if(WT_BEFORE)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/base_src/WtUftCore")
  configure_file("${WT_BEFORE}/src/WtUftCore/ParserAdapter.h" "${CMAKE_BINARY_DIR}/base_src/WtUftCore/ParserAdapter.h" COPYONLY)
  configure_file("${WT_BEFORE}/src/WtUftCore/ParserAdapter.cpp" "${CMAKE_BINARY_DIR}/base_src/WtUftCore/ParserAdapter.cpp" COPYONLY)
  # WtUftEngine.h includes "ParserAdapter.h" relative to itself; stage the
  # (unchanged) current copy next to the old header so both resolve to it.
  configure_file("${ROOT}/src/WtUftCore/WtUftEngine.h" "${CMAKE_BINARY_DIR}/base_src/WtUftCore/WtUftEngine.h" COPYONLY)
  uft_parser_target(uft_parser_base "${CMAKE_BINARY_DIR}/base_src/WtUftCore/ParserAdapter.cpp" OFF)
endif()
