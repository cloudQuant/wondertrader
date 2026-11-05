cmake_minimum_required(VERSION 3.16)
project(i03_parser_vendor_v2 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Vendor parser lifecycle V2 fixture (iteration 03, card 9), 2026-11-04.
# Builds, per parser family, the current module (src/<Parser>/), the saved
# pre-change module (baseline/<Parser>/), a local fake vendor SDK placed next
# to each module, the legacy-API scenarios (current = green, baseline = red)
# and the V2 / real-ParserAdapter checks (current only).
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}")
set(WT_BUILD "" CACHE PATH "Build providing libWtCore.a / libWTSTools.a / libWTSUtils.a")
set(WT_FAMILIES "CTP;CTPMini;CTPOpt;Femas;XTP" CACHE STRING "Parser families to build")
if(WT_BUILD STREQUAL "")
	message(FATAL_ERROR "WT_BUILD is required")
endif()
set(BOOST_INC /opt/homebrew/Cellar/boost/1.88.0/include)
set(CUR_DIR "${CMAKE_BINARY_DIR}/parsers")
set(BASE_DIR "${CMAKE_BINARY_DIR}/baseline/parsers")

function(vfx_common tgt)
	target_include_directories(${tgt} PRIVATE "${ROOT}/src" "${FIXTURE}")
	target_include_directories(${tgt} SYSTEM PRIVATE ${BOOST_INC} /opt/homebrew/include)
	target_compile_definitions(${tgt} PRIVATE FMT_HEADER_ONLY SPDLOG_FMT_EXTERNAL)
	target_compile_options(${tgt} PRIVATE -include "${ROOT}/src/Share/fmt_enum_compat.h")
	target_link_directories(${tgt} PRIVATE /opt/homebrew/lib)
	target_link_libraries(${tgt} PRIVATE boost_filesystem boost_thread pthread dl /usr/local/lib/libiconv.dylib)
endfunction()

foreach(fam ${WT_FAMILIES})
	string(TOUPPER "${fam}" FAM)
	set(parser "Parser${fam}")
	string(TOLOWER "${fam}" low)
	if(fam STREQUAL "Femas")
		set(fake_src "${FIXTURE}/fake_femas.cpp")
	elseif(fam STREQUAL "XTP")
		set(fake_src "${FIXTURE}/fake_xtp.cpp")
	else()
		set(fake_src "${FIXTURE}/fake_ctp_family.cpp")
	endif()
	if(fam STREQUAL "CTPOpt")
		set(fake_prefix "")	# ParserCTPOpt wraps ctpmodule without a "lib" prefix
	else()
		set(fake_prefix "lib")
	endif()
	set(fake_name "fake_${low}_md")

	foreach(variant cur base)
		if(variant STREQUAL "cur")
			set(outdir "${CUR_DIR}")
			set(psrc "${ROOT}/src/${parser}/${parser}.cpp")
		else()
			set(outdir "${BASE_DIR}")
			set(psrc "${FIXTURE}/baseline/${parser}/${parser}.cpp")
		endif()
		set(fake_tgt "fake_${low}_${variant}")
		add_library(${fake_tgt} SHARED "${fake_src}")
		vfx_common(${fake_tgt})
		target_compile_definitions(${fake_tgt} PRIVATE FAKE_FAMILY_${FAM})
		set_target_properties(${fake_tgt} PROPERTIES OUTPUT_NAME "${fake_name}" PREFIX "${fake_prefix}"
			LIBRARY_OUTPUT_DIRECTORY "${outdir}" BUILD_WITH_INSTALL_RPATH ON INSTALL_NAME_DIR "${outdir}")

		set(mod_tgt "${parser}_${variant}")
		add_library(${mod_tgt} SHARED "${psrc}")
		vfx_common(${mod_tgt})
		# Baseline copies resolve "../Includes" and "../Share" through src/<Parser>.
		target_include_directories(${mod_tgt} PRIVATE "${ROOT}/src/${parser}")
		target_compile_options(${mod_tgt} PRIVATE -Wall -Wextra)
		set_target_properties(${mod_tgt} PROPERTIES OUTPUT_NAME "${parser}" LIBRARY_OUTPUT_DIRECTORY "${outdir}"
			CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
		if(fam STREQUAL "CTP")
			# macOS ParserCTP binds CThostFtdcMdApi::CreateFtdcMdApi at link time.
			target_link_libraries(${mod_tgt} PRIVATE ${fake_tgt})
		endif()
		set(module_path "${outdir}/lib${parser}.dylib")
		set(fake_path "${outdir}/${fake_prefix}${fake_name}.dylib")

		set(scn_tgt "scenarios_${low}_${variant}")
		add_executable(${scn_tgt} "${FIXTURE}/scenarios.cpp")
		vfx_common(${scn_tgt})
		target_compile_definitions(${scn_tgt} PRIVATE FAMILY_${FAM} "PARSER_MODULE=\"${module_path}\"" "FAKE_LIB=\"${fake_path}\"")
		target_link_libraries(${scn_tgt} PRIVATE /opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a)
		add_dependencies(${scn_tgt} ${mod_tgt} ${fake_tgt})

		# v2_<f> checks the current module; v2_<f>_base is the red control and
		# must run with cwd=baseline so the real ParserAdapter loads ./parsers there.
		if(variant STREQUAL "cur")
			set(v2_tgt "v2_${low}")
		else()
			set(v2_tgt "v2_${low}_base")
		endif()
		add_executable(${v2_tgt} "${FIXTURE}/v2_check.cpp" "${ROOT}/src/WtCore/ParserAdapter.cpp")
		vfx_common(${v2_tgt})
		target_compile_definitions(${v2_tgt} PRIVATE FAMILY_${FAM} "PARSER_MODULE=\"${module_path}\"" "FAKE_LIB=\"${fake_path}\"")
		target_link_libraries(${v2_tgt} PRIVATE "${WT_BUILD}/build_x64/Debug/libs/libWtCore.a"
			"${WT_BUILD}/build_x64/Debug/libs/libWTSTools.a" "${WT_BUILD}/build_x64/Debug/libs/libWTSUtils.a"
			/opt/homebrew/lib/libgtest_main.a /opt/homebrew/lib/libgtest.a)
		# The prebuilt WtCore archives are coverage-instrumented.
		target_link_options(${v2_tgt} PRIVATE -fprofile-instr-generate)
		add_dependencies(${v2_tgt} ${mod_tgt} ${fake_tgt})
	endforeach()
endforeach()
