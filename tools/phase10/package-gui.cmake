cmake_minimum_required(VERSION 3.25)

foreach(required SOURCE_DIR BUILD_DIR OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "package-gui.cmake requires ${required}")
    endif()
endforeach()

cmake_path(ABSOLUTE_PATH SOURCE_DIR NORMALIZE OUTPUT_VARIABLE source_root)
cmake_path(ABSOLUTE_PATH BUILD_DIR NORMALIZE OUTPUT_VARIABLE build_root)
cmake_path(ABSOLUTE_PATH OUTPUT_DIR NORMALIZE OUTPUT_VARIABLE output_root)
cmake_path(IS_PREFIX source_root output_root NORMALIZE output_in_source)
if(output_in_source AND NOT output_root MATCHES "/build-current/")
    message(FATAL_ERROR "Internal GUI package must stay in build-current")
endif()

if(WIN32)
    set(executable_suffix ".exe")
else()
    set(executable_suffix "")
endif()
set(cli "${build_root}/katana-recomp${executable_suffix}")
set(gui "${build_root}/katana-recomp-gui${executable_suffix}")
set(fixture_writer "${build_root}/katana-port-export-tests${executable_suffix}")
if(WIN32)
    set(dialog "${build_root}/katana-file-dialog.exe")
endif()
set(logo "${source_root}/assets/gui/KatanaLogo.png")
set(icon "${source_root}/assets/gui/KatanaLogo.ico")
set(asset_manifest "${source_root}/assets/gui/asset-manifest.json")
set(required_files "${cli}" "${gui}" "${fixture_writer}" "${logo}" "${icon}" "${asset_manifest}")
if(WIN32)
    list(APPEND required_files "${dialog}")
endif()
foreach(required_file IN LISTS required_files)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Internal GUI package input missing: ${required_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${output_root}")
set(diagnostic_sdk_root "${output_root}/diagnostic-runtime-sdk")
file(MAKE_DIRECTORY
    "${output_root}/assets"
    "${output_root}/docs"
    "${diagnostic_sdk_root}/cmake"
    "${diagnostic_sdk_root}/include/katana/io"
    "${diagnostic_sdk_root}/include/katana/sh4"
    "${diagnostic_sdk_root}/include/katana"
    "${diagnostic_sdk_root}/src/decoder"
    "${diagnostic_sdk_root}/src/io"
    "${diagnostic_sdk_root}/third_party"
    "${diagnostic_sdk_root}/src"
)
file(COPY "${cli}" "${gui}" DESTINATION "${output_root}")
if(WIN32)
    file(COPY "${dialog}" DESTINATION "${output_root}")
    if(DEFINED ASAN_RUNTIME AND NOT "${ASAN_RUNTIME}" STREQUAL "")
        if(NOT EXISTS "${ASAN_RUNTIME}")
            message(FATAL_ERROR "Configured ASAN runtime does not exist")
        endif()
        file(COPY "${ASAN_RUNTIME}" DESTINATION "${output_root}")
    endif()
endif()
file(COPY "${logo}" "${icon}" "${asset_manifest}" DESTINATION "${output_root}/assets")
file(COPY "${source_root}/include/katana/runtime" DESTINATION
    "${diagnostic_sdk_root}/include/katana")
file(COPY "${source_root}/include/katana/sh4" DESTINATION
    "${diagnostic_sdk_root}/include/katana")
file(COPY "${source_root}/src/runtime" DESTINATION "${diagnostic_sdk_root}/src")
file(COPY "${source_root}/third_party/skyemu" DESTINATION
    "${diagnostic_sdk_root}/third_party")
file(COPY "${source_root}/THIRD_PARTY_NOTICES.md" DESTINATION
    "${diagnostic_sdk_root}")
file(COPY
    "${source_root}/include/katana/abi_contract.hpp.in"
    "${source_root}/include/katana/build_contract.hpp.in"
    DESTINATION "${diagnostic_sdk_root}/include/katana")
file(COPY "${source_root}/include/katana/progress.hpp" DESTINATION
    "${diagnostic_sdk_root}/include/katana")
file(COPY "${source_root}/src/progress.cpp" DESTINATION
    "${diagnostic_sdk_root}/src")
file(COPY "${source_root}/cmake/KatanaVersions.cmake" DESTINATION
    "${diagnostic_sdk_root}/cmake")
file(COPY "${source_root}/VERSION" DESTINATION "${diagnostic_sdk_root}")
file(COPY
    "${source_root}/include/katana/io/input_output_error.hpp"
    "${source_root}/include/katana/io/input_provenance.hpp"
    "${source_root}/include/katana/io/json_report.hpp"
    DESTINATION "${diagnostic_sdk_root}/include/katana/io"
)
file(COPY
    "${source_root}/src/decoder/decoder.cpp"
    "${source_root}/src/decoder/instruction_metadata.cpp"
    DESTINATION "${diagnostic_sdk_root}/src/decoder"
)
file(COPY
    "${source_root}/src/io/input_provenance.cpp"
    "${source_root}/src/io/json_report.cpp"
    DESTINATION "${diagnostic_sdk_root}/src/io"
)
file(WRITE "${diagnostic_sdk_root}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.25)\n"
"include(\"\${CMAKE_CURRENT_SOURCE_DIR}/cmake/KatanaVersions.cmake\")\n"
"project(KatanaDiagnosticRuntimeSdk VERSION \${KATANA_PROJECT_VERSION} LANGUAGES CXX)\n"
"find_package(Threads REQUIRED)\n"
"set(CMAKE_CXX_STANDARD 20)\n"
"set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
"set(CMAKE_CXX_EXTENSIONS OFF)\n"
"if(MSVC)\n"
"  add_compile_options(/FS)\n"
"endif()\n"
"file(MAKE_DIRECTORY \"\${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana\")\n"
"configure_file(\"\${CMAKE_CURRENT_SOURCE_DIR}/include/katana/abi_contract.hpp.in\" \"\${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/abi_contract.hpp\" @ONLY)\n"
"configure_file(\"\${CMAKE_CURRENT_SOURCE_DIR}/include/katana/build_contract.hpp.in\" \"\${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/build_contract.hpp\" @ONLY)\n"
"file(GLOB runtime_core_sources CONFIGURE_DEPENDS \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp\")\n"
"set(runtime_diagnostic_sources\n"
"  \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/controlled_fallback.cpp\"\n"
"  \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/interpreter_boundary.cpp\"\n"
"  \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/dynamic_interpreter.cpp\")\n"
"list(REMOVE_ITEM runtime_core_sources \${runtime_diagnostic_sources})\n"
"if(NOT WIN32)\n"
"  list(REMOVE_ITEM runtime_core_sources \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/host_video_d3d11.cpp\")\n"
"endif()\n"
"set(decoder_sources\n"
"  \"\${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/decoder.cpp\"\n"
"  \"\${CMAKE_CURRENT_SOURCE_DIR}/src/decoder/instruction_metadata.cpp\")\n"
"file(GLOB io_sources CONFIGURE_DEPENDS \"\${CMAKE_CURRENT_SOURCE_DIR}/src/io/*.cpp\")\n"
"add_library(katana_runtime_core_objects OBJECT \${runtime_core_sources} \${io_sources} \"\${CMAKE_CURRENT_SOURCE_DIR}/src/progress.cpp\")\n"
"add_library(katana_runtime_diagnostic_objects OBJECT \${runtime_diagnostic_sources} \${decoder_sources})\n"
"foreach(runtime_object_target IN ITEMS katana_runtime_core_objects katana_runtime_diagnostic_objects)\n"
"  target_compile_features(\${runtime_object_target} PUBLIC cxx_std_20)\n"
"  target_include_directories(\${runtime_object_target} PRIVATE \"\${CMAKE_CURRENT_SOURCE_DIR}/include\" \"\${CMAKE_CURRENT_BINARY_DIR}/generated/include\" \"\${CMAKE_CURRENT_SOURCE_DIR}/third_party\")\n"
"  if(MSVC)\n"
"    target_compile_options(\${runtime_object_target} PRIVATE /W4 /permissive- /EHsc /utf-8 /fp:strict)\n"
"  else()\n"
"    target_compile_options(\${runtime_object_target} PRIVATE -Wall -Wextra -Wpedantic -frounding-math)\n"
"  endif()\n"
"endforeach()\n"
"target_link_libraries(katana_runtime_core_objects PRIVATE Threads::Threads)\n"
"add_library(katana_runtime_core STATIC $<TARGET_OBJECTS:katana_runtime_core_objects>)\n"
"add_library(katana_runtime STATIC $<TARGET_OBJECTS:katana_runtime_core_objects> $<TARGET_OBJECTS:katana_runtime_diagnostic_objects>)\n"
"add_library(KatanaRecomp::runtime_core ALIAS katana_runtime_core)\n"
"add_library(KatanaRecomp::runtime ALIAS katana_runtime)\n"
"set_target_properties(katana_runtime_core PROPERTIES EXPORT_NAME runtime_core)\n"
"set_target_properties(katana_runtime PROPERTIES EXPORT_NAME runtime)\n"
"function(katana_configure_runtime_archive target)\n"
"  target_compile_features(\${target} PUBLIC cxx_std_20)\n"
"  target_include_directories(\${target} PUBLIC \"\${CMAKE_CURRENT_SOURCE_DIR}/include\" \"\${CMAKE_CURRENT_BINARY_DIR}/generated/include\")\n"
"  target_link_libraries(\${target} PUBLIC Threads::Threads)\n"
"  if(WIN32)\n"
"    target_link_libraries(\${target} PUBLIC bcrypt d3d11 dxgi gdi32 user32 winmm)\n"
"  endif()\n"
"  set_target_properties(\${target} PROPERTIES\n"
"    KATANA_PROJECT_VERSION \"\${PROJECT_VERSION}\"\n"
"    KATANA_RUNTIME_ABI_VERSION \"\${KATANA_PRODUCT_RUNTIME_ABI_VERSION}\"\n"
"    KATANA_AOT_RUNTIME_ABI_VERSION \"\${KATANA_AOT_RUNTIME_ABI_VERSION}\"\n"
"    KATANA_BLOCK_ABI_VERSION \"\${KATANA_BLOCK_ABI_VERSION}\"\n"
"    KATANA_PLATFORM_SERVICES_ABI_VERSION \"\${KATANA_PLATFORM_SERVICES_ABI_VERSION}\"\n"
"    KATANA_PORT_PROJECT_CONTRACT_VERSION \"\${KATANA_PORT_PROJECT_CONTRACT_VERSION}\"\n"
"    EXPORT_PROPERTIES \"KATANA_PROJECT_VERSION;KATANA_RUNTIME_ABI_VERSION;KATANA_AOT_RUNTIME_ABI_VERSION;KATANA_BLOCK_ABI_VERSION;KATANA_PLATFORM_SERVICES_ABI_VERSION;KATANA_PORT_PROJECT_CONTRACT_VERSION\")\n"
"endfunction()\n"
"katana_configure_runtime_archive(katana_runtime_core)\n"
"katana_configure_runtime_archive(katana_runtime)\n"
)
file(COPY
    "${source_root}/docs/PHASE10_GUI_ARCHITECTURE.md"
    "${source_root}/docs/PHASE10_GUI_WORKFLOW.md"
    DESTINATION "${output_root}/docs"
)

execute_process(
    COMMAND "${output_root}/katana-recomp-gui${executable_suffix}" --smoke
    RESULT_VARIABLE smoke_result
    OUTPUT_VARIABLE smoke_output
    ERROR_VARIABLE smoke_error
)
if(NOT smoke_result EQUAL 0 OR NOT smoke_output MATCHES "KR_PHASE10_GUI_MINIMAL_START")
    message(FATAL_ERROR "Packaged GUI smoke failed: ${smoke_error}")
endif()

string(SHA256 relocated_key "${output_root}")
string(SUBSTRING "${relocated_key}" 0 12 relocated_key)
set(relocated_root "${build_root}/.katana-package-${relocated_key}")
file(REMOVE_RECURSE "${relocated_root}")
file(MAKE_DIRECTORY "${relocated_root}")
file(COPY "${output_root}/" DESTINATION "${relocated_root}")
file(MAKE_DIRECTORY "${relocated_root}/fixture")
execute_process(
    COMMAND "${fixture_writer}" --write-fixture "${relocated_root}/fixture"
    RESULT_VARIABLE fixture_result
    ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
    message(FATAL_ERROR "Relocated package fixture failed: ${fixture_error}")
endif()
file(WRITE "${relocated_root}/fixture/project.katana"
"schema = katana-project\n"
"version = 2\n"
"project.name = packaged-relocation\n"
"input.format = gdi\n"
"input.path = disc.gdi\n"
"image.entry_point = 0x8C010000\n"
"image.expected_entry_points = 0x8C01000A\n"
"execution.firmware = direct\n"
"execution.fallback = abort\n"
"execution.scheduler = deterministic\n"
"execution.mmu = disabled\n"
"execution.fastpath = conservative\n"
)
execute_process(
    COMMAND "${relocated_root}/katana-recomp${executable_suffix}"
            workflow build "${relocated_root}/fixture/project.katana"
            --output "${relocated_root}/workflow-output"
    RESULT_VARIABLE relocated_build_result
    OUTPUT_VARIABLE relocated_build_output
    ERROR_VARIABLE relocated_build_error
)
if(NOT relocated_build_result EQUAL 0 OR
   NOT EXISTS "${relocated_root}/workflow-output/game${executable_suffix}")
    message(FATAL_ERROR
        "Relocated package full GDI build failed: ${relocated_build_output} ${relocated_build_error}")
endif()
set(detached_port "${relocated_root}/detached-port")
file(MAKE_DIRECTORY "${detached_port}")
file(COPY "${relocated_root}/workflow-output/" DESTINATION "${detached_port}")
if(EXISTS "${detached_port}/content/game.katana-disc" OR
   NOT EXISTS "${detached_port}/content/game.katana-install")
    message(FATAL_ERROR "Relocated distribution contains retail data or no install recipe")
endif()
execute_process(
    COMMAND "${detached_port}/game${executable_suffix}" --install-disc
            "${relocated_root}/fixture/disc.gdi"
    RESULT_VARIABLE relocated_install_result
    OUTPUT_VARIABLE relocated_install_output
    ERROR_VARIABLE relocated_install_error
)
if(NOT relocated_install_result EQUAL 0 OR
   NOT relocated_install_output MATCHES "KATANA_DISC_INSTALL_OK" OR
   NOT EXISTS "${detached_port}/user-data/content/game.katana-disc")
    message(FATAL_ERROR
        "Relocated original-disc install failed: ${relocated_install_output} ${relocated_install_error}")
endif()
execute_process(
    COMMAND "${detached_port}/game${executable_suffix}"
    RESULT_VARIABLE relocated_game_result
    OUTPUT_VARIABLE relocated_game_output
    ERROR_VARIABLE relocated_game_error
)
if(NOT relocated_game_result EQUAL 0 OR
   NOT relocated_game_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT relocated_game_output MATCHES "indirect_dispatches=1" OR
   NOT relocated_game_output MATCHES "frames=0" OR
   NOT relocated_game_output MATCHES "audio_buffers=1")
    message(FATAL_ERROR
        "Relocated installed-content runtime failed: ${relocated_game_output} ${relocated_game_error}")
endif()
file(REMOVE_RECURSE "${relocated_root}")

set(entries
    "katana-recomp${executable_suffix}"
    "katana-recomp-gui${executable_suffix}"
    "assets/KatanaLogo.png"
    "assets/KatanaLogo.ico"
    "assets/asset-manifest.json"
    "docs/PHASE10_GUI_ARCHITECTURE.md"
    "docs/PHASE10_GUI_WORKFLOW.md"
    "diagnostic-runtime-sdk/CMakeLists.txt"
    "diagnostic-runtime-sdk/VERSION"
    "diagnostic-runtime-sdk/THIRD_PARTY_NOTICES.md"
    "diagnostic-runtime-sdk/cmake/KatanaVersions.cmake"
)
file(GLOB_RECURSE runtime_entries
    RELATIVE "${output_root}"
    "${diagnostic_sdk_root}/include/*"
    "${diagnostic_sdk_root}/src/*"
    "${diagnostic_sdk_root}/third_party/*"
)
list(APPEND entries ${runtime_entries})
if(WIN32)
    list(APPEND entries "katana-file-dialog.exe")
    if(DEFINED ASAN_RUNTIME AND NOT "${ASAN_RUNTIME}" STREQUAL "")
        list(APPEND entries "clang_rt.asan_dynamic-x86_64.dll")
    endif()
endif()
set(manifest "{\n  \"schema\": \"katana-phase10-internal-package\",\n  \"version\": 1,\n  \"release\": false,\n  \"files\": [\n")
list(LENGTH entries entry_count)
math(EXPR last_index "${entry_count} - 1")
foreach(index RANGE 0 ${last_index})
    list(GET entries ${index} relative)
    file(SHA256 "${output_root}/${relative}" sha256)
    file(SIZE "${output_root}/${relative}" size)
    string(APPEND manifest "    {\"path\":\"${relative}\",\"size\":${size},\"sha256\":\"${sha256}\"}")
    if(NOT index EQUAL last_index)
        string(APPEND manifest ",")
    endif()
    string(APPEND manifest "\n")
endforeach()
string(APPEND manifest "  ]\n}\n")
file(WRITE "${output_root}/package-manifest.json" "${manifest}")
message(STATUS "KR_PHASE10_GUI_PACKAGE_SUCCESS")
