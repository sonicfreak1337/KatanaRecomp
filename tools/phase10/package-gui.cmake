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
set(asset_manifest "${source_root}/assets/gui/asset-manifest.json")
set(required_files "${cli}" "${gui}" "${fixture_writer}" "${logo}" "${asset_manifest}")
if(WIN32)
    list(APPEND required_files "${dialog}")
endif()
foreach(required_file IN LISTS required_files)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Internal GUI package input missing: ${required_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${output_root}")
file(MAKE_DIRECTORY
    "${output_root}/assets"
    "${output_root}/docs"
    "${output_root}/runtime-sdk/include/katana"
    "${output_root}/runtime-sdk/src"
)
file(COPY "${cli}" "${gui}" DESTINATION "${output_root}")
if(WIN32)
    file(COPY "${dialog}" DESTINATION "${output_root}")
    if(NOT DEFINED ASAN_RUNTIME OR NOT EXISTS "${ASAN_RUNTIME}")
        message(FATAL_ERROR "Windows internal Debug package requires ASAN_RUNTIME")
    endif()
    file(COPY "${ASAN_RUNTIME}" DESTINATION "${output_root}")
endif()
file(COPY "${logo}" "${asset_manifest}" DESTINATION "${output_root}/assets")
file(COPY "${source_root}/include/katana/runtime" DESTINATION
    "${output_root}/runtime-sdk/include/katana")
file(COPY "${source_root}/src/runtime" DESTINATION "${output_root}/runtime-sdk/src")
file(WRITE "${output_root}/runtime-sdk/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.25)\n"
"project(KatanaRuntimeSdk LANGUAGES CXX)\n"
"file(GLOB runtime_sources CONFIGURE_DEPENDS \"\${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/*.cpp\")\n"
"add_library(katana_runtime STATIC \${runtime_sources})\n"
"target_compile_features(katana_runtime PUBLIC cxx_std_20)\n"
"target_include_directories(katana_runtime PUBLIC \"\${CMAKE_CURRENT_SOURCE_DIR}/include\")\n"
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
execute_process(
    COMMAND "${relocated_root}/workflow-output/game${executable_suffix}"
            "${relocated_root}/fixture/disc.gdi"
    RESULT_VARIABLE relocated_game_result
    OUTPUT_VARIABLE relocated_game_output
    ERROR_VARIABLE relocated_game_error
)
if(NOT relocated_game_result EQUAL 0 OR
   NOT relocated_game_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT relocated_game_output MATCHES "indirect_dispatches=1")
    message(FATAL_ERROR
        "Relocated game GDI runtime failed: ${relocated_game_output} ${relocated_game_error}")
endif()
file(REMOVE_RECURSE "${relocated_root}")

set(entries
    "katana-recomp${executable_suffix}"
    "katana-recomp-gui${executable_suffix}"
    "assets/KatanaLogo.png"
    "assets/asset-manifest.json"
    "docs/PHASE10_GUI_ARCHITECTURE.md"
    "docs/PHASE10_GUI_WORKFLOW.md"
    "runtime-sdk/CMakeLists.txt"
)
file(GLOB_RECURSE runtime_entries
    RELATIVE "${output_root}"
    "${output_root}/runtime-sdk/include/*"
    "${output_root}/runtime-sdk/src/*"
)
list(APPEND entries ${runtime_entries})
if(WIN32)
    list(APPEND entries "katana-file-dialog.exe" "clang_rt.asan_dynamic-x86_64.dll")
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
