cmake_minimum_required(VERSION 3.25)
project(KatanaRuntimeConsumer LANGUAGES CXX)

find_package(KatanaRecomp ${KATANA_EXPECTED_VERSION} EXACT CONFIG REQUIRED)
if(NOT KatanaRecomp_RUNTIME_ABI_VERSION STREQUAL KATANA_EXPECTED_RUNTIME_ABI)
    message(FATAL_ERROR "unexpected runtime ABI ${KatanaRecomp_RUNTIME_ABI_VERSION}")
endif()
if(NOT KatanaRecomp_PORT_PROJECT_CONTRACT_VERSION STREQUAL
   KATANA_EXPECTED_PORT_PROJECT_CONTRACT)
    message(FATAL_ERROR
        "unexpected port project contract ${KatanaRecomp_PORT_PROJECT_CONTRACT_VERSION}")
endif()
if(NOT KatanaRecomp_NATIVE_PORT_PROFILE_CONTRACT_VERSION STREQUAL
   KATANA_EXPECTED_NATIVE_PORT_PROFILE_CONTRACT)
    message(FATAL_ERROR
        "unexpected native port profile contract ${KatanaRecomp_NATIVE_PORT_PROFILE_CONTRACT_VERSION}")
endif()
if(NOT TARGET KatanaRecomp::native_port_runtime)
    message(FATAL_ERROR "runtime SDK is missing KatanaRecomp::native_port_runtime")
endif()

add_executable(katana-runtime-consumer main.cpp)
target_compile_features(katana-runtime-consumer PRIVATE cxx_std_20)
target_link_libraries(
    katana-runtime-consumer
    PRIVATE KatanaRecomp::runtime KatanaRecomp::native_port_runtime
)
target_compile_definitions(
    katana-runtime-consumer
    PRIVATE
        KATANA_EXPECTED_RUNTIME_ABI=${KATANA_EXPECTED_RUNTIME_ABI}
        KATANA_EXPECTED_PORT_PROJECT_CONTRACT=${KATANA_EXPECTED_PORT_PROJECT_CONTRACT}
        KATANA_EXPECTED_NATIVE_PORT_PROFILE_CONTRACT=${KATANA_EXPECTED_NATIVE_PORT_PROFILE_CONTRACT}
)
