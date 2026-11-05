cmake_minimum_required(VERSION 3.16)
project(i03_dt_parser_host LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
# WT_BUILD: an existing coverage build of this repository providing
# libWtDtCore.a/libWTSTools.a/libWTSUtils.a (coverage-instrumented).
set(WT_BUILD "" CACHE PATH "Build providing static dependencies")
# WT_BEFORE: directory holding pre-change copies (src/<Module>/<file>) for the
# red-control variants. Optional.
set(WT_BEFORE "" CACHE PATH "Pre-change source copies")
set(WT_SANITIZE "" CACHE STRING "Optional -fsanitize= value for host targets")

set(BOOST_INC /opt/homebrew/opt/boost/include)
set(FIX "${CMAKE_CURRENT_SOURCE_DIR}")
set(PARSER_DIR "${CMAKE_BINARY_DIR}/parsers")

foreach(variant V2 Legacy)
  add_library(I03DtFake${variant} SHARED "${FIX}/fake_provider.cpp")
  target_include_directories(I03DtFake${variant} PRIVATE "${ROOT}/src" "${BOOST_INC}" /opt/homebrew/include)
  target_compile_definitions(I03DtFake${variant} PRIVATE BOOST_BIND_GLOBAL_PLACEHOLDERS)
  set_target_properties(I03DtFake${variant} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${PARSER_DIR}"
    PREFIX "lib" SUFFIX ".dylib")
  if(WT_SANITIZE)
    target_compile_options(I03DtFake${variant} PRIVATE -fsanitize=${WT_SANITIZE} -fno-omit-frame-pointer)
    target_link_options(I03DtFake${variant} PRIVATE -fsanitize=${WT_SANITIZE})
  endif()
endforeach()
target_compile_definitions(I03DtFakeLegacy PRIVATE FAKE_NO_V2)

set(WT_LIBS
  "${WT_BUILD}/build_x64/Debug/libs/libWtDtCore.a"
  "${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a"
  "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  boost_filesystem boost_thread pthread dl iconv)

function(dt_host_target name test_src product_dir is_new)
  add_executable(${name} "${test_src}" ${ARGN})
  if(is_new)
    target_compile_definitions(${name} PRIVATE WT_HOST_NEW)
    target_include_directories(${name} BEFORE PRIVATE "${ROOT}/src" "${FIX}")
  else()
    target_include_directories(${name} BEFORE PRIVATE "${product_dir}" "${FIX}")
    target_include_directories(${name} PRIVATE "${ROOT}/src" "${ROOT}/src/WtDtCore" "${ROOT}/src/WtDtServo")
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
  add_dependencies(${name} I03DtFakeV2 I03DtFakeLegacy)
endfunction()

dt_host_target(dtcore_host_cur "${FIX}/dtcore_host_test.cpp" "" ON
  "${ROOT}/src/WtDtCore/ParserAdapter.cpp")

if(WT_BEFORE)
  # Stage the pre-change WtDtCore adapter so its own quoted includes resolve
  # against the current module directory for every unchanged header.
  set(BASE_SRC "${CMAKE_BINARY_DIR}/base_src")
  file(MAKE_DIRECTORY "${BASE_SRC}/WtDtCore")
  configure_file("${WT_BEFORE}/src/WtDtCore/ParserAdapter.h" "${BASE_SRC}/WtDtCore/ParserAdapter.h" COPYONLY)
  configure_file("${WT_BEFORE}/src/WtDtCore/ParserAdapter.cpp" "${BASE_SRC}/WtDtCore/ParserAdapter.cpp" COPYONLY)
  dt_host_target(dtcore_host_base "${FIX}/dtcore_host_test.cpp" "${BASE_SRC}" OFF
    "${BASE_SRC}/WtDtCore/ParserAdapter.cpp")
endif()

# WtDtServo: compile its TUs directly (the product links only WTSTools/WTSUtils).
set(SERVO_LIBS
  "${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a"
  "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
  /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a
  boost_filesystem boost_thread pthread dl iconv)
set(SERVO_OTHER_TUS "${ROOT}/src/WtDtServo/WtDataManager.cpp" "${ROOT}/src/WtDtServo/WtHelper.cpp")

function(servo_host_target name product_dir is_new)
  add_executable(${name} "${FIX}/servo_host_test.cpp" ${ARGN} ${SERVO_OTHER_TUS})
  if(is_new)
    target_compile_definitions(${name} PRIVATE WT_HOST_NEW)
    target_include_directories(${name} BEFORE PRIVATE "${ROOT}/src" "${FIX}")
  else()
    target_include_directories(${name} BEFORE PRIVATE "${product_dir}" "${FIX}")
    target_include_directories(${name} PRIVATE "${ROOT}/src" "${ROOT}/src/WtDtServo")
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
  target_link_libraries(${name} PRIVATE ${SERVO_LIBS})
  add_dependencies(${name} I03DtFakeV2 I03DtFakeLegacy)
endfunction()

servo_host_target(servo_host_cur "" ON
  "${ROOT}/src/WtDtServo/ParserAdapter.cpp" "${ROOT}/src/WtDtServo/WtDtRunner.cpp")

if(WT_BEFORE)
  file(MAKE_DIRECTORY "${BASE_SRC}/WtDtServo")
  foreach(f ParserAdapter.h ParserAdapter.cpp WtDtRunner.h WtDtRunner.cpp)
    configure_file("${WT_BEFORE}/src/WtDtServo/${f}" "${BASE_SRC}/WtDtServo/${f}" COPYONLY)
  endforeach()
  servo_host_target(servo_host_base "${BASE_SRC}" OFF
    "${BASE_SRC}/WtDtServo/ParserAdapter.cpp" "${BASE_SRC}/WtDtServo/WtDtRunner.cpp")
endif()

# WtDtPorter runner (R5-007): current runner + the libWtDtCore archive adapter,
# and a red control with the pre-change runner and pre-change WtDtCore adapter.
set(PORTER_TUS "${ROOT}/src/WtDtPorter/ExpParser.cpp" "${ROOT}/src/WtDtPorter/ExpDumper.cpp")
dt_host_target(dtporter_host_cur "${FIX}/dtporter_host_test.cpp" "" ON
  "${ROOT}/src/WtDtPorter/WtDtRunner.cpp" "${ROOT}/src/WtDtPorter/WtDtPorter.cpp" ${PORTER_TUS})
target_include_directories(dtporter_host_cur PRIVATE "${ROOT}/src/WtDtPorter")
if(WT_BEFORE)
  file(MAKE_DIRECTORY "${BASE_SRC}/WtDtPorter")
  foreach(f WtDtRunner.h WtDtRunner.cpp WtDtPorter.cpp)
    configure_file("${WT_BEFORE}/src/WtDtPorter/${f}" "${BASE_SRC}/WtDtPorter/${f}" COPYONLY)
  endforeach()
  # Unchanged headers that the staged runner includes from its own directory.
  foreach(f PorterDefs.h ExpDumper.h ExpParser.h WtDtPorter.h)
    configure_file("${ROOT}/src/WtDtPorter/${f}" "${BASE_SRC}/WtDtPorter/${f}" COPYONLY)
  endforeach()
  dt_host_target(dtporter_host_base "${FIX}/dtporter_host_test.cpp" "${BASE_SRC}" OFF
    "${BASE_SRC}/WtDtPorter/WtDtRunner.cpp" "${BASE_SRC}/WtDtPorter/WtDtPorter.cpp"
    "${BASE_SRC}/WtDtCore/ParserAdapter.cpp" ${PORTER_TUS})
  target_include_directories(dtporter_host_base PRIVATE "${ROOT}/src/WtDtPorter")
endif()

# StateMonitor writer-callback modes (R22-002 / R5-007) with a local fake writer.
add_library(I03DtFakeWriter SHARED "${FIX}/fake_writer.cpp")
target_include_directories(I03DtFakeWriter PRIVATE "${ROOT}/src" "${BOOST_INC}" /opt/homebrew/include)
# Writers resolve as <module_dir>/lib<name>.dylib (not under parsers/).
set_target_properties(I03DtFakeWriter PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" PREFIX "lib" SUFFIX ".dylib")
if(WT_SANITIZE)
  target_compile_options(I03DtFakeWriter PRIVATE -fsanitize=${WT_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(I03DtFakeWriter PRIVATE -fsanitize=${WT_SANITIZE})
endif()
add_executable(monitor_writer_probe "${FIX}/monitor_writer_probe.cpp"
  "${ROOT}/src/WtDtCore/StateMonitor.cpp" "${ROOT}/src/WtDtCore/DataManager.cpp" "${ROOT}/src/WtDtCore/WtHelper.cpp")
target_include_directories(monitor_writer_probe BEFORE PRIVATE "${ROOT}/src")
target_include_directories(monitor_writer_probe PRIVATE /opt/homebrew/include "${BOOST_INC}")
target_compile_definitions(monitor_writer_probe PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL BOOST_BIND_GLOBAL_PLACEHOLDERS)
target_compile_options(monitor_writer_probe PRIVATE -include "${ROOT}/src/Share/fmt_enum_compat.h")
if(WT_SANITIZE)
  target_compile_options(monitor_writer_probe PRIVATE -fsanitize=${WT_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(monitor_writer_probe PRIVATE -fsanitize=${WT_SANITIZE})
endif()
target_link_options(monitor_writer_probe PRIVATE -fprofile-instr-generate)
target_link_directories(monitor_writer_probe PRIVATE /opt/homebrew/lib /usr/local/lib)
target_link_libraries(monitor_writer_probe PRIVATE
  "${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a" "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
  boost_filesystem boost_thread pthread dl iconv)
add_dependencies(monitor_writer_probe I03DtFakeWriter)
