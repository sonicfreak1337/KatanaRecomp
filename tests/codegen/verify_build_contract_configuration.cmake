if(NOT DEFINED KATANA_SOURCE_ROOT)
  message(FATAL_ERROR "Build-contract regression needs KATANA_SOURCE_ROOT")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR "Build-contract regression needs a temporary directory")
endif()

file(TO_CMAKE_PATH
     "$ENV{TEMP}/katana-build-contract-configuration-fixture"
     fixture)
file(REMOVE_RECURSE "${fixture}")

execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${KATANA_SOURCE_ROOT}"
    -B "${fixture}"
    -DKATANA_GIT_COMMIT=0123456789abcdef0123456789abcdef01234567
    -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error
  TIMEOUT 60
)

if(configure_result EQUAL 0 OR
   NOT "${configure_output}\n${configure_error}" MATCHES
       "KATANA_GIT_COMMIT cannot be verified without an inspectable Git checkout")
  message(FATAL_ERROR
    "Explicit source assertion without verifiable Git did not fail closed: "
    "${configure_output}\n${configure_error}")
endif()

file(REMOVE_RECURSE "${fixture}")

set(launcher "${CMAKE_COMMAND};-E;env")
execute_process(
  COMMAND
    "${CMAKE_COMMAND}"
    -S "${KATANA_SOURCE_ROOT}"
    -B "${fixture}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${launcher}"
    -DKATANA_BUILD_DESKTOP_GUI=OFF
    -DKATANA_BUILD_FUZZERS=OFF
  RESULT_VARIABLE launcher_result
  OUTPUT_VARIABLE launcher_output
  ERROR_VARIABLE launcher_error
  TIMEOUT 120
)
if(NOT launcher_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Semicolon compiler-launcher configure failed: "
    "${launcher_output}\n${launcher_error}")
endif()

set(generated_contract
    "${fixture}/generated/include/katana/build_contract.hpp")
set(generated_abi_contract
    "${fixture}/generated/include/katana/abi_contract.hpp")
if(NOT EXISTS "${generated_contract}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Configure did not generate the build contract")
endif()
if(NOT EXISTS "${generated_abi_contract}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Configure did not generate the stable ABI contract")
endif()
file(READ "${generated_abi_contract}" generated_abi_contract_text)
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
message(STATUS
  "Unverifiable source identity fails closed and compiler launcher stays exact")
