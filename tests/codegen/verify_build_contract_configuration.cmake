if(NOT DEFINED KATANA_SOURCE_ROOT)
  message(FATAL_ERROR "Build-contract regression needs KATANA_SOURCE_ROOT")
endif()
if(NOT DEFINED KATANA_GIT_EXECUTABLE OR
   "${KATANA_GIT_EXECUTABLE}" STREQUAL "")
  message(FATAL_ERROR "Build-contract regression needs KATANA_GIT_EXECUTABLE")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR "Build-contract regression needs a temporary directory")
endif()

file(TO_CMAKE_PATH
     "$ENV{TEMP}/katana-build-contract-configuration-fixture"
     fixture)
file(REMOVE_RECURSE "${fixture}")

function(require_source_identity contract expected_commit expected_trusted context)
  if(NOT EXISTS "${contract}")
    message(FATAL_ERROR "${context}: build contract is missing: ${contract}")
  endif()
  file(READ "${contract}" contract_text)
  string(REPLACE "\r\n" "\n" contract_text "${contract_text}")
  string(REPLACE "\r" "\n" contract_text "${contract_text}")
  string(FIND "${contract_text}"
       "katana_git_commit = \"${expected_commit}\";"
       commit_position)
  string(FIND "${contract_text}"
       "source_identity_trusted =\n    ${expected_trusted} != 0;"
       trusted_position)
  if(commit_position EQUAL -1 OR trusted_position EQUAL -1)
    message(FATAL_ERROR
      "${context}: unexpected source identity in ${contract}: ${contract_text}")
  endif()
endfunction()

# Exercise the complete dirty -> committed transition in an isolated checkout.
# A plain source archive must remain untrusted, while a clean checkout must bind
# its exact HEAD. Committing the dirty fixture then has to make the existing
# build graph reconfigure before its source-identity target is evaluated.
set(identity_fixture "${fixture}-identity")
set(identity_source "${identity_fixture}/source")
set(identity_archive "${identity_fixture}/source.tar")
set(identity_non_git_build "${identity_fixture}/build-non-git")
set(identity_asserted_build "${identity_fixture}/build-asserted")
set(identity_clean_build "${identity_fixture}/build-clean")
set(identity_dirty_build "${identity_fixture}/build-dirty")
file(REMOVE_RECURSE "${identity_fixture}")
file(MAKE_DIRECTORY "${identity_source}")

execute_process(
  COMMAND
    "${KATANA_GIT_EXECUTABLE}" -C "${KATANA_SOURCE_ROOT}"
    archive --format=tar "--output=${identity_archive}" HEAD
  RESULT_VARIABLE archive_result
  OUTPUT_VARIABLE archive_output
  ERROR_VARIABLE archive_error
  TIMEOUT 60
)
if(NOT archive_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot create isolated build-contract source fixture: "
    "${archive_output}\n${archive_error}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E tar xf "${identity_archive}"
  WORKING_DIRECTORY "${identity_source}"
  RESULT_VARIABLE extract_result
  OUTPUT_VARIABLE extract_output
  ERROR_VARIABLE extract_error
  TIMEOUT 60
)
if(NOT extract_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot extract isolated build-contract source fixture: "
    "${extract_output}\n${extract_error}")
endif()
# The production CMake change under test is intentionally still uncommitted in
# the outer checkout, so overlay the exact configure-contract inputs on the
# archived source. This also lets an ABI bump prove its new contract before
# the commit which makes it part of the archive.
file(COPY_FILE
  "${KATANA_SOURCE_ROOT}/CMakeLists.txt"
  "${identity_source}/CMakeLists.txt"
  ONLY_IF_DIFFERENT
)
file(COPY_FILE
  "${KATANA_SOURCE_ROOT}/cmake/KatanaVersions.cmake"
  "${identity_source}/cmake/KatanaVersions.cmake"
  ONLY_IF_DIFFERENT
)

set(identity_generator_arguments)
if(DEFINED KATANA_TEST_GENERATOR AND
   NOT "${KATANA_TEST_GENERATOR}" STREQUAL "")
  list(APPEND identity_generator_arguments -G "${KATANA_TEST_GENERATOR}")
endif()
if(DEFINED KATANA_TEST_MAKE_PROGRAM AND
   NOT "${KATANA_TEST_MAKE_PROGRAM}" STREQUAL "")
  list(APPEND identity_generator_arguments
       "-DCMAKE_MAKE_PROGRAM=${KATANA_TEST_MAKE_PROGRAM}")
endif()
set(identity_configure_arguments
  -DKATANA_BUILD_DESKTOP_GUI=OFF
  -DKATANA_BUILD_FUZZERS=OFF
  -DKATANA_ENABLE_COVERAGE=OFF
  -DKATANA_ENABLE_SANITIZERS=OFF
  -DKATANA_ENABLE_STATIC_ANALYSIS=OFF
  -DKATANA_REPRODUCIBLE_ARTIFACTS=OFF
)
if(DEFINED KATANA_TEST_CXX_COMPILER AND
   NOT "${KATANA_TEST_CXX_COMPILER}" STREQUAL "")
  list(APPEND identity_configure_arguments
       "-DCMAKE_CXX_COMPILER=${KATANA_TEST_CXX_COMPILER}")
endif()
foreach(tool IN ITEMS LINKER RC_COMPILER MT)
  if(DEFINED KATANA_TEST_${tool} AND
     NOT "${KATANA_TEST_${tool}}" STREQUAL "")
    list(APPEND identity_configure_arguments
         "-DCMAKE_${tool}=${KATANA_TEST_${tool}}")
  endif()
endforeach()
if(DEFINED KATANA_TEST_FFMPEG_ROOT AND
   NOT "${KATANA_TEST_FFMPEG_ROOT}" STREQUAL "")
  list(APPEND identity_configure_arguments
       "-DKATANA_FFMPEG_ROOT=${KATANA_TEST_FFMPEG_ROOT}")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_non_git_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
  RESULT_VARIABLE non_git_result
  OUTPUT_VARIABLE non_git_output
  ERROR_VARIABLE non_git_error
  TIMEOUT 120
)
if(NOT non_git_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Unasserted source archive configure failed: "
    "${non_git_output}\n${non_git_error}")
endif()
require_source_identity(
  "${identity_non_git_build}/generated/include/katana/build_contract.hpp"
  "0000000000000000000000000000000000000000" 0
  "Unasserted source archive")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_non_git_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
    -DKATANA_GIT_COMMIT=0123456789abcdef0123456789abcdef01234567
  RESULT_VARIABLE non_git_asserted_result
  OUTPUT_VARIABLE non_git_asserted_output
  ERROR_VARIABLE non_git_asserted_error
  TIMEOUT 60
)
if(non_git_asserted_result EQUAL 0 OR
   NOT "${non_git_asserted_output}\n${non_git_asserted_error}" MATCHES
       "KATANA_GIT_COMMIT cannot be verified without an inspectable Git checkout")
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Explicit source assertion without verifiable Git did not fail closed: "
    "${non_git_asserted_output}\n${non_git_asserted_error}")
endif()

execute_process(
  COMMAND "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}" init
  RESULT_VARIABLE init_result
  OUTPUT_VARIABLE init_output
  ERROR_VARIABLE init_error
  TIMEOUT 30
)
if(NOT init_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot initialize source-identity fixture: ${init_output}\n${init_error}")
endif()
foreach(config_entry IN ITEMS
        "user.name=Katana Build Contract Test"
        "user.email=katana-build-contract@example.invalid")
  string(REPLACE "=" ";" config_parts "${config_entry}")
  list(GET config_parts 0 config_name)
  list(GET config_parts 1 config_value)
  execute_process(
    COMMAND
      "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}"
      config "${config_name}" "${config_value}"
    RESULT_VARIABLE config_result
    OUTPUT_VARIABLE config_output
    ERROR_VARIABLE config_error
    TIMEOUT 30
  )
  if(NOT config_result EQUAL 0)
    file(REMOVE_RECURSE "${identity_fixture}")
    message(FATAL_ERROR
      "Cannot configure source-identity fixture: "
      "${config_output}\n${config_error}")
  endif()
endforeach()
execute_process(
  COMMAND "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}" add --all
  RESULT_VARIABLE add_result
  OUTPUT_VARIABLE add_output
  ERROR_VARIABLE add_error
  TIMEOUT 60
)
if(NOT add_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot stage source-identity fixture: ${add_output}\n${add_error}")
endif()
execute_process(
  COMMAND
    "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}"
    commit --quiet -m initial
  RESULT_VARIABLE initial_commit_result
  OUTPUT_VARIABLE initial_commit_output
  ERROR_VARIABLE initial_commit_error
  TIMEOUT 60
)
if(NOT initial_commit_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot commit source-identity fixture: "
    "${initial_commit_output}\n${initial_commit_error}")
endif()
execute_process(
  COMMAND "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}" rev-parse HEAD
  RESULT_VARIABLE initial_head_result
  OUTPUT_VARIABLE initial_head
  ERROR_VARIABLE initial_head_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
  TIMEOUT 30
)
if(NOT initial_head_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot read initial source-identity HEAD: ${initial_head_error}")
endif()

set(launcher "${CMAKE_COMMAND};-E;env")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_asserted_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
    "-DKATANA_GIT_COMMIT=${initial_head}"
  RESULT_VARIABLE asserted_result
  OUTPUT_VARIABLE asserted_output
  ERROR_VARIABLE asserted_error
  TIMEOUT 120
)
if(NOT asserted_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Exact clean source assertion failed: ${asserted_output}\n${asserted_error}")
endif()
require_source_identity(
  "${identity_asserted_build}/generated/include/katana/build_contract.hpp"
  "${initial_head}" 1 "Exact clean source assertion")

set(mismatched_head "0123456789abcdef0123456789abcdef01234567")
if("${mismatched_head}" STREQUAL "${initial_head}")
  set(mismatched_head "fedcba9876543210fedcba9876543210fedcba98")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_asserted_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
    "-DKATANA_GIT_COMMIT=${mismatched_head}"
  RESULT_VARIABLE mismatch_result
  OUTPUT_VARIABLE mismatch_output
  ERROR_VARIABLE mismatch_error
  TIMEOUT 120
)
if(mismatch_result EQUAL 0 OR
   NOT "${mismatch_output}\n${mismatch_error}" MATCHES
       "KATANA_GIT_COMMIT must match the checked-out HEAD")
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Mismatched source assertion did not fail closed: "
    "${mismatch_output}\n${mismatch_error}")
endif()

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_clean_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${launcher}"
  RESULT_VARIABLE clean_result
  OUTPUT_VARIABLE clean_output
  ERROR_VARIABLE clean_error
  TIMEOUT 120
)
if(NOT clean_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Clean source-identity configure failed: ${clean_output}\n${clean_error}")
endif()
set(identity_contract
    "${identity_clean_build}/generated/include/katana/build_contract.hpp")
require_source_identity(
  "${identity_contract}" "${initial_head}" 1 "Clean checkout")

file(APPEND "${identity_source}/README.md" "\nsource identity dirty fixture\n")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_dirty_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
  RESULT_VARIABLE dirty_result
  OUTPUT_VARIABLE dirty_output
  ERROR_VARIABLE dirty_error
  TIMEOUT 120
)
if(NOT dirty_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Dirty source-identity configure failed: ${dirty_output}\n${dirty_error}")
endif()
require_source_identity(
  "${identity_dirty_build}/generated/include/katana/build_contract.hpp"
  "0000000000000000000000000000000000000000" 0
  "Dirty checkout")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${identity_source}"
    -B "${identity_dirty_build}"
    ${identity_generator_arguments}
    ${identity_configure_arguments}
    "-DKATANA_GIT_COMMIT=${initial_head}"
  RESULT_VARIABLE dirty_asserted_result
  OUTPUT_VARIABLE dirty_asserted_output
  ERROR_VARIABLE dirty_asserted_error
  TIMEOUT 120
)
if(dirty_asserted_result EQUAL 0 OR
   NOT "${dirty_asserted_output}\n${dirty_asserted_error}" MATCHES
       "KATANA_GIT_COMMIT requires a clean worktree")
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Dirty source assertion did not fail closed: "
    "${dirty_asserted_output}\n${dirty_asserted_error}")
endif()

execute_process(
  COMMAND "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}" add README.md
  RESULT_VARIABLE update_add_result
  OUTPUT_VARIABLE update_add_output
  ERROR_VARIABLE update_add_error
  TIMEOUT 30
)
if(NOT update_add_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot stage updated source-identity fixture: "
    "${update_add_output}\n${update_add_error}")
endif()
execute_process(
  COMMAND
    "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}"
    commit --quiet -m updated
  RESULT_VARIABLE updated_commit_result
  OUTPUT_VARIABLE updated_commit_output
  ERROR_VARIABLE updated_commit_error
  TIMEOUT 60
)
if(NOT updated_commit_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot commit updated source-identity fixture: "
    "${updated_commit_output}\n${updated_commit_error}")
endif()
execute_process(
  COMMAND "${KATANA_GIT_EXECUTABLE}" -C "${identity_source}" rev-parse HEAD
  RESULT_VARIABLE updated_head_result
  OUTPUT_VARIABLE updated_head
  ERROR_VARIABLE updated_head_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
  TIMEOUT 30
)
if(NOT updated_head_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Cannot read updated source-identity HEAD: ${updated_head_error}")
endif()
execute_process(
  COMMAND
    "${CMAKE_COMMAND}" --build "${identity_clean_build}"
    --target katana-source-identity --config Release
  RESULT_VARIABLE refresh_result
  OUTPUT_VARIABLE refresh_output
  ERROR_VARIABLE refresh_error
  TIMEOUT 120
)
if(NOT refresh_result EQUAL 0)
  file(REMOVE_RECURSE "${identity_fixture}")
  message(FATAL_ERROR
    "Committed source identity did not refresh before the build gate: "
    "${refresh_output}\n${refresh_error}")
endif()
require_source_identity(
  "${identity_contract}" "${updated_head}" 1
  "Automatically refreshed clean checkout")

set(generated_contract "${identity_contract}")
set(generated_abi_contract
    "${identity_clean_build}/generated/include/katana/abi_contract.hpp")
if(NOT EXISTS "${generated_contract}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Configure did not generate the build contract")
endif()
if(NOT EXISTS "${generated_abi_contract}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Configure did not generate the stable ABI contract")
endif()
file(READ "${generated_abi_contract}" generated_abi_contract_text)
include("${KATANA_SOURCE_ROOT}/cmake/KatanaVersions.cmake")
set(expected_aot_runtime_contract
    "runtime_abi_version = ${KATANA_AOT_RUNTIME_ABI_VERSION}u")
string(FIND "${generated_abi_contract_text}"
       "${expected_aot_runtime_contract}"
       aot_runtime_contract_position)
if(aot_runtime_contract_position EQUAL -1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Stable AOT ABI contract did not retain its dedicated ABI: "
    "${generated_abi_contract_text}")
endif()
if(generated_abi_contract_text MATCHES
   "katana_git_commit|source_identity_trusted|configured_compiler_launcher")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Stable ABI contract acquired build provenance and would invalidate AOT objects")
endif()
foreach(aot_contract_header IN ITEMS
        "include/katana/runtime/abi.hpp"
        "include/katana/runtime/block_abi.hpp"
        "include/katana/runtime/native_port.hpp")
  file(READ "${KATANA_SOURCE_ROOT}/${aot_contract_header}"
       aot_contract_header_text)
  if(aot_contract_header_text MATCHES "katana/build_contract.hpp")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "AOT contract header still imports commit-sensitive provenance: "
      "${aot_contract_header}")
  endif()
endforeach()
file(READ "${generated_contract}" generated_contract_text)
string(REPLACE "\r\n" "\n" normalized_contract_text
       "${generated_contract_text}")
string(REPLACE "\r" "\n" normalized_contract_text
       "${normalized_contract_text}")
set(expected_product_runtime_contract
    "runtime_abi_version =\n    ${KATANA_PRODUCT_RUNTIME_ABI_VERSION}u")
set(expected_aot_runtime_alias
    "aot_runtime_abi_version =\n    abi_contract::runtime_abi_version")
string(FIND "${normalized_contract_text}"
       "${expected_product_runtime_contract}"
       product_runtime_contract_position)
string(FIND "${normalized_contract_text}"
       "${expected_aot_runtime_alias}"
       aot_runtime_alias_position)
if(product_runtime_contract_position EQUAL -1 OR
   aot_runtime_alias_position EQUAL -1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Build contract did not keep product and generated-AOT ABI domains separate: "
    "${generated_contract_text}")
endif()
set(expected_launcher "${launcher}")
string(REPLACE "\\" "\\\\" expected_launcher "${expected_launcher}")
string(REPLACE "\"" "\\\"" expected_launcher "${expected_launcher}")
set(expected_contract_launcher
    "configured_compiler_launcher =\n    \"${expected_launcher}\";")
string(FIND "${normalized_contract_text}" "${expected_contract_launcher}"
       launcher_position)
if(launcher_position EQUAL -1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Build contract lost or split the semicolon compiler launcher: "
    "${generated_contract_text}")
endif()

file(REMOVE_RECURSE "${fixture}")
file(REMOVE_RECURSE "${identity_fixture}")
message(STATUS
  "Source identity refreshes after commit, fails closed elsewhere, and compiler launcher stays exact")
