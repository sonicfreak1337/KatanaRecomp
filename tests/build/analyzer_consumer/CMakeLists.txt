cmake_minimum_required(VERSION 3.25)
project(KatanaAnalyzerConsumer LANGUAGES CXX)

find_package(KatanaRecomp ${KATANA_EXPECTED_VERSION} EXACT CONFIG REQUIRED)
if(NOT KatanaRecomp_ANALYZER_ABI_VERSION STREQUAL KATANA_EXPECTED_ANALYZER_ABI)
    message(FATAL_ERROR
        "unexpected analyzer ABI ${KatanaRecomp_ANALYZER_ABI_VERSION}")
endif()

add_executable(katana-analyzer-consumer main.cpp)
target_compile_features(katana-analyzer-consumer PRIVATE cxx_std_20)
target_link_libraries(katana-analyzer-consumer PRIVATE KatanaRecomp::analyzer)
target_compile_definitions(
    katana-analyzer-consumer
    PRIVATE KATANA_EXPECTED_ANALYZER_ABI=${KATANA_EXPECTED_ANALYZER_ABI}
)

add_executable(
    katana-stale-analyzer-consumer
    EXCLUDE_FROM_ALL
    stale_main.cpp
)
target_compile_features(katana-stale-analyzer-consumer PRIVATE cxx_std_20)
target_link_libraries(
    katana-stale-analyzer-consumer
    PRIVATE KatanaRecomp::analyzer
)
