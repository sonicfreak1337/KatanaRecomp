cmake_minimum_required(VERSION 3.25)
project(KatanaRuntimeConsumer LANGUAGES CXX)

find_package(KatanaRecomp ${KATANA_EXPECTED_VERSION} EXACT CONFIG REQUIRED)
if(NOT KatanaRecomp_RUNTIME_ABI_VERSION STREQUAL KATANA_EXPECTED_RUNTIME_ABI)
    message(FATAL_ERROR "unexpected runtime ABI ${KatanaRecomp_RUNTIME_ABI_VERSION}")
endif()

add_executable(katana-runtime-consumer main.cpp)
target_compile_features(katana-runtime-consumer PRIVATE cxx_std_20)
target_link_libraries(katana-runtime-consumer PRIVATE KatanaRecomp::runtime)
target_compile_definitions(
    katana-runtime-consumer
    PRIVATE KATANA_EXPECTED_RUNTIME_ABI=${KATANA_EXPECTED_RUNTIME_ABI}
)
