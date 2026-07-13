cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.11.0
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
                /utf-8
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
    src/analysis/basic_blocks.cpp
    src/analysis/function_analysis.cpp
    src/ir/lower.cpp
    src/codegen/cpp_emitter.cpp
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

add_executable(katana-basic-block-tests
    tests/basic_blocks/basic_block_tests.cpp
)

target_link_libraries(
    katana-basic-block-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-basic-block-tests)

add_test(
    NAME katana-basic-block-tests
    COMMAND katana-basic-block-tests
)

add_executable(katana-function-analysis-tests
    tests/functions/function_analysis_tests.cpp
)

target_link_libraries(
    katana-function-analysis-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-function-analysis-tests)

add_test(
    NAME katana-function-analysis-tests
    COMMAND katana-function-analysis-tests
)

add_executable(katana-ir-lowering-tests
    tests/ir/ir_lowering_tests.cpp
)

target_link_libraries(
    katana-ir-lowering-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-ir-lowering-tests)

add_test(
    NAME katana-ir-lowering-tests
    COMMAND katana-ir-lowering-tests
)

add_executable(katana-cpp-emitter-tests
    tests/codegen/cpp_emitter_tests.cpp
)

target_link_libraries(
    katana-cpp-emitter-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-cpp-emitter-tests)

add_test(
    NAME katana-cpp-emitter-tests
    COMMAND katana-cpp-emitter-tests
)

set(
    KATANA_GENERATED_TEST_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/generated-tests
)

set(
    KATANA_GENERATED_TEST_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_execution_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_TEST_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codegen_execution.bin
        8C010000
        ${KATANA_GENERATED_TEST_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codegen_execution.bin
    COMMENT
        "Generiere C++-Programm fuer End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_TEST_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/codegen/generated_execution_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_TEST_CPP}
)

add_executable(katana-generated-execution-tests
    tests/codegen/generated_execution_tests.cpp
    ${KATANA_GENERATED_TEST_CPP}
)

target_include_directories(
    katana-generated-execution-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-execution-tests)

add_dependencies(
    katana-generated-execution-tests
    katana-recomp
)

add_test(
    NAME katana-generated-execution-tests
    COMMAND katana-generated-execution-tests
)

set(
    KATANA_GENERATED_MEMORY_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_memory_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_MEMORY_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memory_execution.bin
        8C010000
        ${KATANA_GENERATED_MEMORY_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memory_execution.bin
    COMMENT
        "Generiere C++-Programm fuer Speicher-End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_MEMORY_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/codegen/generated_memory_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_MEMORY_CPP}
)

add_executable(katana-generated-memory-tests
    tests/codegen/generated_memory_tests.cpp
    ${KATANA_GENERATED_MEMORY_CPP}
)

target_include_directories(
    katana-generated-memory-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-memory-tests)

add_dependencies(
    katana-generated-memory-tests
    katana-recomp
)

add_test(
    NAME katana-generated-memory-tests
    COMMAND katana-generated-memory-tests
)

set(
    KATANA_GENERATED_STATUS_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_status_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_STATUS_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/status_execution.bin
        8C010000
        ${KATANA_GENERATED_STATUS_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/status_execution.bin
    COMMENT
        "Generiere C++-Programm fuer Statusregister-End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_STATUS_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/status/generated_status_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_STATUS_CPP}
)

add_executable(katana-generated-status-tests
    tests/status/generated_status_tests.cpp
    ${KATANA_GENERATED_STATUS_CPP}
)

target_include_directories(
    katana-generated-status-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-status-tests)

add_dependencies(
    katana-generated-status-tests
    katana-recomp
)

add_test(
    NAME katana-generated-status-tests
    COMMAND katana-generated-status-tests
)