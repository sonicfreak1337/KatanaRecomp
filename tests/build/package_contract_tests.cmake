if(NOT DEFINED KATANA_BUILD_DIR OR NOT DEFINED KATANA_SOURCE_DIR)
    message(FATAL_ERROR "build and source directories are required")
endif()

set(KATANA_INSTALL_DIR "${KATANA_BUILD_DIR}/package-contract/install")
set(KATANA_CONSUMER_BUILD_DIR "${KATANA_BUILD_DIR}/package-contract/consumer")
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
