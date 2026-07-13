cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.3.0
    DESCRIPTION "Dreamcast SH-4 static recompilation framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(katana_enable_warnings target)
    if(MSVC)
        target_compile_options(
            ${target}
            PRIVATE
                /W4
                /permissive-
                /EHsc
        )
    else()
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
        )
    endif()
endfunction()

add_library(katana_core STATIC
    src/decoder/decoder.cpp
    src/io/binary_reader.cpp
    src/analysis/disassembler.cpp
)

target_include_directories(
    katana_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

katana_enable_warnings(katana_core)

add_executable(katana-recomp
    src/cli/main.cpp
)

target_link_libraries(
    katana-recomp
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-recomp)

enable_testing()

add_executable(katana-decoder-tests
    tests/decoder/decoder_tests.cpp
)

target_link_libraries(
    katana-decoder-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-decoder-tests)

add_test(
    NAME katana-decoder-tests
    COMMAND katana-decoder-tests
)

add_executable(katana-disassembler-tests
    tests/disassembler/disassembler_tests.cpp
)

target_link_libraries(
    katana-disassembler-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-disassembler-tests)

add_test(
    NAME katana-disassembler-tests
    COMMAND katana-disassembler-tests
)

add_executable(katana-control-flow-tests
    tests/control_flow/control_flow_tests.cpp
)

target_link_libraries(
    katana-control-flow-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-control-flow-tests)

add_test(
    NAME katana-control-flow-tests
    COMMAND katana-control-flow-tests
)
