cmake_minimum_required(VERSION 3.16)
project(i03_uft_ledger_ctx_concurrency LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
# WT_BUILD: an existing coverage build providing libWTSTools.a/libWTSUtils.a.
set(WT_BUILD "" CACHE PATH "Build providing static dependencies")
# WT_BEFORE: directory holding src/WtUftCore/<file> pre-change copies (red control).
set(WT_BEFORE "" CACHE PATH "Pre-change source copies")
set(WT_SANITIZE "" CACHE STRING "-fsanitize= value, e.g. thread or address,undefined")
set(BOOST_INC /opt/homebrew/opt/boost/include)
set(FIX "${CMAKE_CURRENT_SOURCE_DIR}")
set(UFT_TUS ActionPolicyMgr EventNotifier ParserAdapter ShareManager TraderAdapter UftStraContext
  UftStrategyMgr WtHelper WtUftDtMgr WtUftEngine WtUftTicker)
set(WT_LIBS
  "${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a"
  "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  boost_filesystem boost_thread pthread dl iconv)

function(uft_cc_target name core_dir)
  set(srcs "${FIX}/concurrency_test.cpp")
  foreach(tu ${UFT_TUS})
    list(APPEND srcs "${core_dir}/WtUftCore/${tu}.cpp")
  endforeach()
  add_executable(${name} ${srcs})
  # The test includes "WtUftCore/<x>.h"; resolve it to the chosen core first.
  target_include_directories(${name} BEFORE PRIVATE "${core_dir}")
  target_include_directories(${name} PRIVATE "${ROOT}/src" "${ROOT}/src/WtUftCore" /opt/homebrew/include "${BOOST_INC}")
  target_include_directories(${name} SYSTEM PRIVATE /usr/local/include)
  target_compile_definitions(${name} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL BOOST_BIND_GLOBAL_PLACEHOLDERS)
  target_compile_options(${name} PRIVATE -include "${ROOT}/src/Share/fmt_enum_compat.h")
  if(WT_SANITIZE)
    target_compile_options(${name} PRIVATE -fsanitize=${WT_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
    target_link_options(${name} PRIVATE -fsanitize=${WT_SANITIZE})
  endif()
  target_link_options(${name} PRIVATE -fprofile-instr-generate)
  target_link_directories(${name} PRIVATE /opt/homebrew/lib /usr/local/lib)
  target_link_libraries(${name} PRIVATE ${WT_LIBS})
endfunction()

uft_cc_target(uft_cc_cur "${ROOT}/src")

if(WT_BEFORE)
  # Entity copy of WtUftCore with the pre-change files overlaid (no links).
  set(BASE "${CMAKE_BINARY_DIR}/base_src")
  file(GLOB CUR_FILES "${ROOT}/src/WtUftCore/*.h" "${ROOT}/src/WtUftCore/*.cpp")
  file(MAKE_DIRECTORY "${BASE}/WtUftCore")
  foreach(f ${CUR_FILES})
    get_filename_component(n "${f}" NAME)
    if(EXISTS "${WT_BEFORE}/src/WtUftCore/${n}")
      configure_file("${WT_BEFORE}/src/WtUftCore/${n}" "${BASE}/WtUftCore/${n}" COPYONLY)
    else()
      configure_file("${f}" "${BASE}/WtUftCore/${n}" COPYONLY)
    endif()
  endforeach()
  uft_cc_target(uft_cc_base "${BASE}")
endif()
