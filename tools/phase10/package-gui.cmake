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
if(WIN32)
    set(dialog "${build_root}/katana-file-dialog.exe")
endif()
set(logo "${source_root}/assets/gui/KatanaLogo.png")
set(asset_manifest "${source_root}/assets/gui/asset-manifest.json")
set(required_files "${cli}" "${gui}" "${logo}" "${asset_manifest}")
if(WIN32)
    list(APPEND required_files "${dialog}")
endif()
foreach(required_file IN LISTS required_files)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Internal GUI package input missing: ${required_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${output_root}")
file(MAKE_DIRECTORY "${output_root}/assets" "${output_root}/docs")
file(COPY "${cli}" "${gui}" DESTINATION "${output_root}")
if(WIN32)
    file(COPY "${dialog}" DESTINATION "${output_root}")
    if(NOT DEFINED ASAN_RUNTIME OR NOT EXISTS "${ASAN_RUNTIME}")
        message(FATAL_ERROR "Windows internal Debug package requires ASAN_RUNTIME")
    endif()
    file(COPY "${ASAN_RUNTIME}" DESTINATION "${output_root}")
endif()
file(COPY "${logo}" "${asset_manifest}" DESTINATION "${output_root}/assets")
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

set(entries
    "katana-recomp${executable_suffix}"
    "katana-recomp-gui${executable_suffix}"
    "assets/KatanaLogo.png"
    "assets/asset-manifest.json"
    "docs/PHASE10_GUI_ARCHITECTURE.md"
    "docs/PHASE10_GUI_WORKFLOW.md"
)
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
