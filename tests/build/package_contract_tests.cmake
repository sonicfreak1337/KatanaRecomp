if(NOT DEFINED KATANA_BUILD_DIR OR NOT DEFINED KATANA_SOURCE_DIR OR
   NOT DEFINED KATANA_EXPECTED_VERSION OR
   NOT DEFINED KATANA_EXPECTED_RUNTIME_ABI OR
   NOT DEFINED KATANA_EXPECTED_ANALYZER_ABI OR
   NOT DEFINED KATANA_EXPECTED_PORT_PROJECT_CONTRACT)
    message(FATAL_ERROR "build, source and expected contract values are required")
endif()

set(KATANA_INSTALL_DIR "${KATANA_BUILD_DIR}/package-contract/install")
set(KATANA_CONSUMER_BUILD_DIR "${KATANA_BUILD_DIR}/package-contract/consumer")
set(KATANA_ANALYZER_CONSUMER_BUILD_DIR
    "${KATANA_BUILD_DIR}/package-contract/analyzer-consumer")
set(KATANA_STALE_ANALYZER_CONSUMER_BUILD_DIR
    "${KATANA_BUILD_DIR}/package-contract/stale-analyzer-consumer")
file(REMOVE_RECURSE "${KATANA_BUILD_DIR}/package-contract")

set(KATANA_INSTALL_COMMAND
    "${CMAKE_COMMAND}" --install "${KATANA_BUILD_DIR}"
    --prefix "${KATANA_INSTALL_DIR}" --component runtime-sdk
)
if(KATANA_CONFIG)
    list(APPEND KATANA_INSTALL_COMMAND --config "${KATANA_CONFIG}")
endif()
execute_process(COMMAND ${KATANA_INSTALL_COMMAND} RESULT_VARIABLE KATANA_RESULT)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "runtime-sdk installation failed: ${KATANA_RESULT}")
endif()

if(NOT EXISTS "${KATANA_INSTALL_DIR}/include/katana/runtime/abi.hpp" OR
   NOT EXISTS "${KATANA_INSTALL_DIR}/include/katana/build_contract.hpp")
    message(FATAL_ERROR "runtime-sdk is missing public ABI headers")
endif()
if(EXISTS "${KATANA_INSTALL_DIR}/include/katana/analysis")
    message(FATAL_ERROR "runtime-sdk unexpectedly contains analyzer headers")
endif()

set(KATANA_ANALYZER_INSTALL_COMMAND
    "${CMAKE_COMMAND}" --install "${KATANA_BUILD_DIR}"
    --prefix "${KATANA_INSTALL_DIR}" --component analyzer-sdk
)
if(KATANA_CONFIG)
    list(APPEND KATANA_ANALYZER_INSTALL_COMMAND --config "${KATANA_CONFIG}")
endif()
execute_process(COMMAND ${KATANA_ANALYZER_INSTALL_COMMAND} RESULT_VARIABLE KATANA_RESULT)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "analyzer-sdk installation failed: ${KATANA_RESULT}")
endif()

if(NOT EXISTS "${KATANA_INSTALL_DIR}/include/katana/analysis/abi.hpp" OR
   NOT EXISTS
       "${KATANA_INSTALL_DIR}/include/katana/analysis/function_value_analysis.hpp")
    message(FATAL_ERROR "analyzer-sdk is missing public ABI headers")
endif()

set(KATANA_CONSUMER_GENERATOR_ARGS)
if(NOT WIN32)
    list(APPEND KATANA_CONSUMER_GENERATOR_ARGS
        -G "${KATANA_GENERATOR}"
        "-DCMAKE_MAKE_PROGRAM=${KATANA_MAKE_PROGRAM}"
        "-DCMAKE_CXX_COMPILER=${KATANA_CXX_COMPILER}"
    )
endif()
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${KATANA_SOURCE_DIR}/tests/build/runtime_consumer"
        -B "${KATANA_CONSUMER_BUILD_DIR}"
        ${KATANA_CONSUMER_GENERATOR_ARGS}
        "-DCMAKE_PREFIX_PATH=${KATANA_INSTALL_DIR}"
        "-DKATANA_EXPECTED_VERSION=${KATANA_EXPECTED_VERSION}"
        "-DKATANA_EXPECTED_RUNTIME_ABI=${KATANA_EXPECTED_RUNTIME_ABI}"
        "-DKATANA_EXPECTED_PORT_PROJECT_CONTRACT=${KATANA_EXPECTED_PORT_PROJECT_CONTRACT}"
    RESULT_VARIABLE KATANA_RESULT
)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "out-of-tree runtime consumer configure failed: ${KATANA_RESULT}")
endif()

set(KATANA_BUILD_COMMAND "${CMAKE_COMMAND}" --build "${KATANA_CONSUMER_BUILD_DIR}")
if(KATANA_CONFIG)
    list(APPEND KATANA_BUILD_COMMAND --config "${KATANA_CONFIG}")
endif()
execute_process(COMMAND ${KATANA_BUILD_COMMAND} RESULT_VARIABLE KATANA_RESULT)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "out-of-tree runtime consumer build failed: ${KATANA_RESULT}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${KATANA_SOURCE_DIR}/tests/build/analyzer_consumer"
        -B "${KATANA_ANALYZER_CONSUMER_BUILD_DIR}"
        ${KATANA_CONSUMER_GENERATOR_ARGS}
        "-DCMAKE_PREFIX_PATH=${KATANA_INSTALL_DIR}"
        "-DKATANA_EXPECTED_VERSION=${KATANA_EXPECTED_VERSION}"
        "-DKATANA_EXPECTED_ANALYZER_ABI=${KATANA_EXPECTED_ANALYZER_ABI}"
    RESULT_VARIABLE KATANA_RESULT
)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "out-of-tree analyzer consumer configure failed: ${KATANA_RESULT}")
endif()

set(
    KATANA_ANALYZER_BUILD_COMMAND
    "${CMAKE_COMMAND}" --build "${KATANA_ANALYZER_CONSUMER_BUILD_DIR}"
)
if(KATANA_CONFIG)
    list(APPEND KATANA_ANALYZER_BUILD_COMMAND --config "${KATANA_CONFIG}")
endif()
execute_process(COMMAND ${KATANA_ANALYZER_BUILD_COMMAND} RESULT_VARIABLE KATANA_RESULT)
if(NOT KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "out-of-tree analyzer consumer build failed: ${KATANA_RESULT}")
endif()

set(
    KATANA_STALE_ANALYZER_LINK_COMMAND
    "${CMAKE_COMMAND}" --build "${KATANA_ANALYZER_CONSUMER_BUILD_DIR}"
    --target katana-stale-analyzer-consumer
)
if(KATANA_CONFIG)
    list(APPEND KATANA_STALE_ANALYZER_LINK_COMMAND --config "${KATANA_CONFIG}")
endif()
execute_process(
    COMMAND ${KATANA_STALE_ANALYZER_LINK_COMMAND}
    RESULT_VARIABLE KATANA_RESULT
    OUTPUT_VARIABLE KATANA_STALE_LINK_OUTPUT
    ERROR_VARIABLE KATANA_STALE_LINK_ERROR
)
if(KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "stale analyzer ABI unexpectedly linked successfully")
endif()
string(
    CONCAT
    KATANA_STALE_LINK_DIAGNOSTIC
    "${KATANA_STALE_LINK_OUTPUT}"
    "${KATANA_STALE_LINK_ERROR}"
)
if(NOT KATANA_STALE_LINK_DIAGNOSTIC MATCHES "require_analyzer_abi")
    message(FATAL_ERROR
        "stale analyzer link failed for an unrelated reason: "
        "${KATANA_STALE_LINK_DIAGNOSTIC}")
endif()

math(EXPR KATANA_STALE_ANALYZER_ABI "${KATANA_EXPECTED_ANALYZER_ABI} + 1")
execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${KATANA_SOURCE_DIR}/tests/build/analyzer_consumer"
        -B "${KATANA_STALE_ANALYZER_CONSUMER_BUILD_DIR}"
        ${KATANA_CONSUMER_GENERATOR_ARGS}
        "-DCMAKE_PREFIX_PATH=${KATANA_INSTALL_DIR}"
        "-DKATANA_EXPECTED_VERSION=${KATANA_EXPECTED_VERSION}"
        "-DKATANA_EXPECTED_ANALYZER_ABI=${KATANA_STALE_ANALYZER_ABI}"
    RESULT_VARIABLE KATANA_RESULT
    OUTPUT_VARIABLE KATANA_STALE_CONFIGURE_OUTPUT
    ERROR_VARIABLE KATANA_STALE_CONFIGURE_ERROR
)
if(KATANA_RESULT EQUAL 0)
    message(FATAL_ERROR "stale analyzer ABI unexpectedly configured successfully")
endif()
string(
    CONCAT
    KATANA_STALE_CONFIGURE_DIAGNOSTIC
    "${KATANA_STALE_CONFIGURE_OUTPUT}"
    "${KATANA_STALE_CONFIGURE_ERROR}"
)
if(NOT KATANA_STALE_CONFIGURE_DIAGNOSTIC MATCHES "unexpected analyzer ABI")
    message(FATAL_ERROR
        "stale analyzer configure failed for an unrelated reason: "
        "${KATANA_STALE_CONFIGURE_DIAGNOSTIC}")
endif()
