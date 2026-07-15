cmake_minimum_required(VERSION 3.25)

project(
    KatanaRecomp
    VERSION 0.26.0
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

add_library(katana_runtime STATIC
    src/runtime/dreamcast_memory.cpp
    src/runtime/exception.cpp
    src/runtime/fpu.cpp
    src/runtime/interrupt.cpp
    src/runtime/maple.cpp
    src/runtime/memory.cpp
    src/runtime/runtime.cpp
)

add_library(KatanaRecomp::runtime ALIAS katana_runtime)

target_include_directories(
    katana_runtime
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

katana_enable_warnings(katana_runtime)
if(MSVC)
    target_compile_options(katana_runtime PRIVATE /fp:strict)
else()
    target_compile_options(katana_runtime PRIVATE -frounding-math)
endif()

add_library(katana_core STATIC
    src/decoder/decoder.cpp
    src/decoder/decoder_validation.cpp
    src/decoder/instruction_metadata.cpp
    src/decoder/isa_coverage.cpp
    src/io/binary_reader.cpp
    src/io/executable_image.cpp
    src/io/elf32_sh_loader.cpp
    src/io/project_manifest.cpp
    src/io/raw_binary_loader.cpp
    src/io/symbol_map.cpp
    src/platform/dreamcast.cpp
    src/analysis/disassembler.cpp
    src/analysis/code_address.cpp
    src/analysis/basic_blocks.cpp
    src/analysis/function_analysis.cpp
    src/analysis/recursive_analysis.cpp
    src/analysis/value_analysis.cpp
    src/analysis/jump_table_analysis.cpp
    src/analysis/analysis_overrides.cpp
    src/analysis/control_flow_report.cpp
    src/analysis/control_flow_analysis.cpp
    src/ir/lower.cpp
    src/ir/accumulator_effect.cpp
    src/ir/memory_effect.cpp
    src/ir/operand_width.cpp
    src/ir/optimize.cpp
    src/ir/status_effect.cpp
    src/ir/serialize.cpp
    src/ir/verifier.cpp
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

add_executable(katana-runtime-tests
    tests/runtime/runtime_tests.cpp
)

target_link_libraries(katana-runtime-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-runtime-tests)

add_test(
    NAME katana-runtime-tests
    COMMAND katana-runtime-tests
)

add_executable(katana-memory-bus-tests
    tests/runtime/memory_bus_tests.cpp
)

target_link_libraries(katana-memory-bus-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-memory-bus-tests)

add_test(
    NAME katana-memory-bus-tests
    COMMAND katana-memory-bus-tests
)

add_executable(katana-dreamcast-ram-tests
    tests/runtime/dreamcast_ram_tests.cpp
)

target_link_libraries(katana-dreamcast-ram-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-dreamcast-ram-tests)

add_test(
    NAME katana-dreamcast-ram-tests
    COMMAND katana-dreamcast-ram-tests
)

add_executable(katana-dreamcast-video-audio-memory-tests
    tests/runtime/dreamcast_video_audio_memory_tests.cpp
)

target_link_libraries(
    katana-dreamcast-video-audio-memory-tests
    PRIVATE katana_runtime
)
katana_enable_warnings(katana-dreamcast-video-audio-memory-tests)

add_test(
    NAME katana-dreamcast-video-audio-memory-tests
    COMMAND katana-dreamcast-video-audio-memory-tests
)

add_executable(katana-dreamcast-firmware-memory-tests
    tests/runtime/dreamcast_firmware_memory_tests.cpp
)

target_link_libraries(
    katana-dreamcast-firmware-memory-tests
    PRIVATE katana_runtime
)
katana_enable_warnings(katana-dreamcast-firmware-memory-tests)

add_test(
    NAME katana-dreamcast-firmware-memory-tests
    COMMAND katana-dreamcast-firmware-memory-tests
)

add_executable(katana-mmio-memory-tests
    tests/runtime/mmio_memory_tests.cpp
)

target_link_libraries(katana-mmio-memory-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-mmio-memory-tests)

add_test(
    NAME katana-mmio-memory-tests
    COMMAND katana-mmio-memory-tests
)

add_executable(katana-memory-diagnostics-tests
    tests/runtime/memory_diagnostics_tests.cpp
)

target_link_libraries(
    katana-memory-diagnostics-tests
    PRIVATE katana_runtime
)
katana_enable_warnings(katana-memory-diagnostics-tests)

add_test(
    NAME katana-memory-diagnostics-tests
    COMMAND katana-memory-diagnostics-tests
)

add_executable(katana-cpu-state-tests
    tests/runtime/cpu_state_tests.cpp
)

target_link_libraries(katana-cpu-state-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-cpu-state-tests)

add_test(
    NAME katana-cpu-state-tests
    COMMAND katana-cpu-state-tests
)

add_executable(katana-reset-tests
    tests/runtime/reset_tests.cpp
)

target_link_libraries(katana-reset-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-reset-tests)

add_test(
    NAME katana-reset-tests
    COMMAND katana-reset-tests
)

add_executable(katana-exception-tests
    tests/runtime/exception_tests.cpp
)

target_link_libraries(katana-exception-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-exception-tests)

add_test(
    NAME katana-exception-tests
    COMMAND katana-exception-tests
)

add_executable(katana-interrupt-tests
    tests/runtime/interrupt_tests.cpp
)

target_link_libraries(katana-interrupt-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-interrupt-tests)

add_test(
    NAME katana-interrupt-tests
    COMMAND katana-interrupt-tests
)

add_executable(katana-fpu-tests
    tests/runtime/fpu_tests.cpp
)

target_link_libraries(katana-fpu-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-fpu-tests)

add_test(
    NAME katana-fpu-tests
    COMMAND katana-fpu-tests
)

add_executable(katana-flash-device-tests tests/runtime/flash_device_tests.cpp)
target_link_libraries(katana-flash-device-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-flash-device-tests)
add_test(NAME katana-flash-device-tests COMMAND katana-flash-device-tests)

add_executable(katana-dreamcast-boot-tests tests/platform/dreamcast_boot_tests.cpp)
target_link_libraries(katana-dreamcast-boot-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-dreamcast-boot-tests)
add_test(NAME katana-dreamcast-boot-tests COMMAND katana-dreamcast-boot-tests)

add_executable(katana-maple-bus-tests tests/runtime/maple_bus_tests.cpp)
target_link_libraries(katana-maple-bus-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-maple-bus-tests)
add_test(NAME katana-maple-bus-tests COMMAND katana-maple-bus-tests)

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

add_executable(katana-executable-image-tests
    tests/io/executable_image_tests.cpp
)

target_link_libraries(katana-executable-image-tests PRIVATE katana_core)
katana_enable_warnings(katana-executable-image-tests)

add_test(
    NAME katana-executable-image-tests
    COMMAND katana-executable-image-tests
)

add_executable(katana-raw-binary-loader-tests
    tests/io/raw_binary_loader_tests.cpp
)

target_link_libraries(katana-raw-binary-loader-tests PRIVATE katana_core)
target_compile_definitions(katana-raw-binary-loader-tests PRIVATE
    KATANA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
katana_enable_warnings(katana-raw-binary-loader-tests)

add_test(
    NAME katana-raw-binary-loader-tests
    COMMAND katana-raw-binary-loader-tests
)

add_executable(katana-elf32-sh-loader-tests
    tests/io/elf32_sh_loader_tests.cpp
)

target_link_libraries(katana-elf32-sh-loader-tests PRIVATE katana_core)
katana_enable_warnings(katana-elf32-sh-loader-tests)

add_test(
    NAME katana-elf32-sh-loader-tests
    COMMAND katana-elf32-sh-loader-tests
)

add_executable(katana-symbol-map-tests
    tests/io/symbol_map_tests.cpp
)

target_link_libraries(katana-symbol-map-tests PRIVATE katana_core)
katana_enable_warnings(katana-symbol-map-tests)

add_test(
    NAME katana-symbol-map-tests
    COMMAND katana-symbol-map-tests
)

add_executable(katana-project-manifest-tests
    tests/io/project_manifest_tests.cpp
)

target_link_libraries(katana-project-manifest-tests PRIVATE katana_core)
katana_enable_warnings(katana-project-manifest-tests)

add_test(
    NAME katana-project-manifest-tests
    COMMAND katana-project-manifest-tests
)

add_executable(katana-instruction-metadata-tests
    tests/decoder/instruction_metadata_tests.cpp
)

target_link_libraries(katana-instruction-metadata-tests PRIVATE katana_core)
katana_enable_warnings(katana-instruction-metadata-tests)

add_test(
    NAME katana-instruction-metadata-tests
    COMMAND katana-instruction-metadata-tests
)

add_executable(katana-fpu-pipeline-tests
    tests/fpu/fpu_pipeline_tests.cpp
)

target_link_libraries(katana-fpu-pipeline-tests PRIVATE katana_core)
katana_enable_warnings(katana-fpu-pipeline-tests)

add_test(
    NAME katana-fpu-pipeline-tests
    COMMAND katana-fpu-pipeline-tests
)

add_executable(katana-decoder-collision-tests
    tests/decoder/decoder_collision_tests.cpp
)

target_link_libraries(katana-decoder-collision-tests PRIVATE katana_core)
katana_enable_warnings(katana-decoder-collision-tests)

add_test(
    NAME katana-decoder-collision-tests
    COMMAND katana-decoder-collision-tests
)

add_executable(katana-isa-coverage-tests
    tests/decoder/isa_coverage_tests.cpp
)

target_link_libraries(katana-isa-coverage-tests PRIVATE katana_core)
katana_enable_warnings(katana-isa-coverage-tests)

add_test(
    NAME katana-isa-coverage-tests
    COMMAND katana-isa-coverage-tests
)

add_executable(katana-sh4-specification-tests
    tests/decoder/sh4_specification_vectors.cpp
)

target_link_libraries(katana-sh4-specification-tests PRIVATE katana_core)
katana_enable_warnings(katana-sh4-specification-tests)

add_test(
    NAME katana-sh4-specification-tests
    COMMAND katana-sh4-specification-tests
)

add_executable(katana-decoder-fuzzer
    tools/decoder_fuzzer.cpp
)

target_link_libraries(katana-decoder-fuzzer PRIVATE katana_core)
katana_enable_warnings(katana-decoder-fuzzer)

add_test(
    NAME katana-decoder-fuzzer
    COMMAND katana-decoder-fuzzer 0x00C0FFEE 200000
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

add_executable(katana-recursive-analysis-tests
    tests/analysis/recursive_analysis_tests.cpp
)

target_link_libraries(katana-recursive-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-recursive-analysis-tests)

add_test(
    NAME katana-recursive-analysis-tests
    COMMAND katana-recursive-analysis-tests
)

add_test(
    NAME katana-recursive-analysis-cli-tests
    COMMAND katana-recomp analyze
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recursive_analysis.katana
)

set_tests_properties(
    katana-recursive-analysis-cli-tests
    PROPERTIES PASS_REGULAR_EXPRESSION "Katana rekursive Analyse;Indirekter Kontrollfluss;Aufgeloest:;Ungeloest:"
)

add_test(
    NAME katana-indirect-control-flow-cli-tests
    COMMAND katana-recomp analyze
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect_control_flow.katana
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect_control_flow.overrides
)

set_tests_properties(
    katana-indirect-control-flow-cli-tests
    PROPERTIES PASS_REGULAR_EXPRESSION "user-override"
)

add_executable(katana-value-analysis-tests
    tests/analysis/value_analysis_tests.cpp
)

target_link_libraries(katana-value-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-value-analysis-tests)

add_test(
    NAME katana-value-analysis-tests
    COMMAND katana-value-analysis-tests
)

add_executable(katana-jump-table-analysis-tests
    tests/analysis/jump_table_analysis_tests.cpp
)

target_link_libraries(katana-jump-table-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-jump-table-analysis-tests)

add_test(
    NAME katana-jump-table-analysis-tests
    COMMAND katana-jump-table-analysis-tests
)

add_executable(katana-analysis-overrides-tests
    tests/analysis/analysis_overrides_tests.cpp
)

target_link_libraries(katana-analysis-overrides-tests PRIVATE katana_core)
katana_enable_warnings(katana-analysis-overrides-tests)

add_test(
    NAME katana-analysis-overrides-tests
    COMMAND katana-analysis-overrides-tests
)

add_executable(katana-control-flow-analysis-tests
    tests/analysis/control_flow_analysis_tests.cpp
)

target_link_libraries(katana-control-flow-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-control-flow-analysis-tests)

add_test(
    NAME katana-control-flow-analysis-tests
    COMMAND katana-control-flow-analysis-tests
)

add_executable(katana-control-flow-cli-fixture-writer
    tests/analysis/control_flow_cli_fixture_writer.cpp
)

katana_enable_warnings(katana-control-flow-cli-fixture-writer)

add_test(
    NAME katana-control-flow-fixpoint-cli-tests
    COMMAND ${CMAKE_COMMAND}
        -DWRITER=$<TARGET_FILE:katana-control-flow-cli-fixture-writer>
        -DCLI=$<TARGET_FILE:katana-recomp>
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/control-flow-cli-fixture
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/analysis/verify_control_flow_cli.cmake
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

add_executable(katana-ir-operand-width-tests
    tests/ir/operand_width_tests.cpp
)

target_link_libraries(katana-ir-operand-width-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-operand-width-tests)

add_test(
    NAME katana-ir-operand-width-tests
    COMMAND katana-ir-operand-width-tests
)

add_executable(katana-ir-status-effect-tests
    tests/ir/status_effect_tests.cpp
)

target_link_libraries(katana-ir-status-effect-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-status-effect-tests)

add_test(
    NAME katana-ir-status-effect-tests
    COMMAND katana-ir-status-effect-tests
)

add_executable(katana-ir-memory-effect-tests
    tests/ir/memory_effect_tests.cpp
)

target_link_libraries(katana-ir-memory-effect-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-memory-effect-tests)

add_test(
    NAME katana-ir-memory-effect-tests
    COMMAND katana-ir-memory-effect-tests
)

add_executable(katana-ir-verifier-tests
    tests/ir/verifier_tests.cpp
)

target_link_libraries(katana-ir-verifier-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-verifier-tests)

add_test(
    NAME katana-ir-verifier-tests
    COMMAND katana-ir-verifier-tests
)

add_executable(katana-ir-serialization-tests
    tests/ir/serialization_tests.cpp
)

target_link_libraries(katana-ir-serialization-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-serialization-tests)

add_test(
    NAME katana-ir-serialization-tests
    COMMAND katana-ir-serialization-tests
)

add_test(
    NAME katana-ir-cli-tests
    COMMAND ${CMAKE_COMMAND}
        -DKATANA_RECOMP=$<TARGET_FILE:katana-recomp>
        -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codegen_execution.bin
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_ir_cli.cmake
)

add_executable(katana-cli-input-fixture-writer
    tests/io/cli_input_fixture_writer.cpp
)
katana_enable_warnings(katana-cli-input-fixture-writer)

add_test(
    NAME katana-cli-input-path-tests
    COMMAND ${CMAKE_COMMAND}
        -DKATANA_RECOMP=$<TARGET_FILE:katana-recomp>
        -DFIXTURE_WRITER=$<TARGET_FILE:katana-cli-input-fixture-writer>
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-input-path-tests
        -DOVERRIDE_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect_control_flow.katana
        -DOVERRIDE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/indirect_control_flow.overrides
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_cli_input_paths.cmake
)

add_executable(katana-ir-optimization-tests
    tests/ir/optimization_tests.cpp
)

target_link_libraries(katana-ir-optimization-tests PRIVATE katana_core)
katana_enable_warnings(katana-ir-optimization-tests)

add_test(
    NAME katana-ir-optimization-tests
    COMMAND katana-ir-optimization-tests
)

add_test(
    NAME katana-optimization-cli-tests
    COMMAND ${CMAKE_COMMAND}
        -DKATANA_RECOMP=$<TARGET_FILE:katana-recomp>
        -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/codegen_execution.bin
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/optimization-cli-tests
        -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCXX_COMPILER_ID=${CMAKE_CXX_COMPILER_ID}
        -DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
        -DKATANA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_optimization_cli.cmake
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
add_executable(katana-sub-neg-not-tests
    tests/alu/sub_neg_not_tests.cpp
)

target_link_libraries(
    katana-sub-neg-not-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-sub-neg-not-tests)

add_test(
    NAME katana-sub-neg-not-tests
    COMMAND katana-sub-neg-not-tests
)

set(
    KATANA_GENERATED_SUB_NEG_NOT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_sub_neg_not_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_SUB_NEG_NOT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sub_neg_not_execution.bin
        8C010000
        ${KATANA_GENERATED_SUB_NEG_NOT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sub_neg_not_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1101 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_SUB_NEG_NOT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_sub_neg_not_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_SUB_NEG_NOT_CPP}
)

add_executable(katana-generated-sub-neg-not-tests
    tests/alu/generated_sub_neg_not_tests.cpp
    ${KATANA_GENERATED_SUB_NEG_NOT_CPP}
)

target_include_directories(
    katana-generated-sub-neg-not-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-sub-neg-not-tests)

add_dependencies(
    katana-generated-sub-neg-not-tests
    katana-recomp
)

add_test(
    NAME katana-generated-sub-neg-not-tests
    COMMAND katana-generated-sub-neg-not-tests
)
add_executable(katana-and-or-xor-tests
    tests/alu/and_or_xor_tests.cpp
)

target_link_libraries(
    katana-and-or-xor-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-and-or-xor-tests)

add_test(
    NAME katana-and-or-xor-tests
    COMMAND katana-and-or-xor-tests
)

set(
    KATANA_GENERATED_AND_OR_XOR_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_and_or_xor_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_AND_OR_XOR_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/and_or_xor_execution.bin
        8C010000
        ${KATANA_GENERATED_AND_OR_XOR_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/and_or_xor_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1102 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_AND_OR_XOR_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_and_or_xor_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_AND_OR_XOR_CPP}
)

add_executable(katana-generated-and-or-xor-tests
    tests/alu/generated_and_or_xor_tests.cpp
    ${KATANA_GENERATED_AND_OR_XOR_CPP}
)

target_include_directories(
    katana-generated-and-or-xor-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-and-or-xor-tests)

add_dependencies(
    katana-generated-and-or-xor-tests
    katana-recomp
)

add_test(
    NAME katana-generated-and-or-xor-tests
    COMMAND katana-generated-and-or-xor-tests
)
add_executable(katana-cmp-variants-tests
    tests/alu/cmp_variants_tests.cpp
)

target_link_libraries(
    katana-cmp-variants-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-cmp-variants-tests)

add_test(
    NAME katana-cmp-variants-tests
    COMMAND katana-cmp-variants-tests
)

set(
    KATANA_GENERATED_CMP_VARIANTS_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_cmp_variants_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_CMP_VARIANTS_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cmp_variants_execution.bin
        8C010000
        ${KATANA_GENERATED_CMP_VARIANTS_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/cmp_variants_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1103 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_CMP_VARIANTS_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_cmp_variants_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_CMP_VARIANTS_CPP}
)

add_executable(katana-generated-cmp-variants-tests
    tests/alu/generated_cmp_variants_tests.cpp
    ${KATANA_GENERATED_CMP_VARIANTS_CPP}
)

target_include_directories(
    katana-generated-cmp-variants-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-cmp-variants-tests)

add_dependencies(
    katana-generated-cmp-variants-tests
    katana-recomp
)

add_test(
    NAME katana-generated-cmp-variants-tests
    COMMAND katana-generated-cmp-variants-tests
)
add_executable(katana-carry-overflow-tests
    tests/alu/carry_overflow_tests.cpp
)

target_link_libraries(
    katana-carry-overflow-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-carry-overflow-tests)

add_test(
    NAME katana-carry-overflow-tests
    COMMAND katana-carry-overflow-tests
)

set(
    KATANA_GENERATED_CARRY_OVERFLOW_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_carry_overflow_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_CARRY_OVERFLOW_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/carry_overflow_execution.bin
        8C010000
        ${KATANA_GENERATED_CARRY_OVERFLOW_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/carry_overflow_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1104 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_CARRY_OVERFLOW_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_carry_overflow_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_CARRY_OVERFLOW_CPP}
)

add_executable(katana-generated-carry-overflow-tests
    tests/alu/generated_carry_overflow_tests.cpp
    ${KATANA_GENERATED_CARRY_OVERFLOW_CPP}
)

target_include_directories(
    katana-generated-carry-overflow-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-carry-overflow-tests)

add_dependencies(
    katana-generated-carry-overflow-tests
    katana-recomp
)

add_test(
    NAME katana-generated-carry-overflow-tests
    COMMAND katana-generated-carry-overflow-tests
)
add_executable(katana-extend-swap-xtrct-tests
    tests/alu/extend_swap_xtrct_tests.cpp
)

target_link_libraries(
    katana-extend-swap-xtrct-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-extend-swap-xtrct-tests)

add_test(
    NAME katana-extend-swap-xtrct-tests
    COMMAND katana-extend-swap-xtrct-tests
)

set(
    KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_extend_swap_xtrct_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/extend_swap_xtrct_execution.bin
        8C010000
        ${KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/extend_swap_xtrct_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1105 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_extend_swap_xtrct_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP}
)

add_executable(katana-generated-extend-swap-xtrct-tests
    tests/alu/generated_extend_swap_xtrct_tests.cpp
    ${KATANA_GENERATED_EXTEND_SWAP_XTRCT_CPP}
)

target_include_directories(
    katana-generated-extend-swap-xtrct-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-extend-swap-xtrct-tests)

add_dependencies(
    katana-generated-extend-swap-xtrct-tests
    katana-recomp
)

add_test(
    NAME katana-generated-extend-swap-xtrct-tests
    COMMAND katana-generated-extend-swap-xtrct-tests
)
add_executable(katana-dt-movt-tests
    tests/alu/dt_movt_tests.cpp
)

target_link_libraries(
    katana-dt-movt-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-dt-movt-tests)

add_test(
    NAME katana-dt-movt-tests
    COMMAND katana-dt-movt-tests
)

set(
    KATANA_GENERATED_DT_MOVT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_dt_movt_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_DT_MOVT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dt_movt_execution.bin
        8C010000
        ${KATANA_GENERATED_DT_MOVT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dt_movt_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1106 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_DT_MOVT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_dt_movt_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_DT_MOVT_CPP}
)

add_executable(katana-generated-dt-movt-tests
    tests/alu/generated_dt_movt_tests.cpp
    ${KATANA_GENERATED_DT_MOVT_CPP}
)

target_include_directories(
    katana-generated-dt-movt-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-dt-movt-tests)

add_dependencies(
    katana-generated-dt-movt-tests
    katana-recomp
)

add_test(
    NAME katana-generated-dt-movt-tests
    COMMAND katana-generated-dt-movt-tests
)
add_executable(katana-one-bit-shift-tests
    tests/alu/one_bit_shift_tests.cpp
)

target_link_libraries(
    katana-one-bit-shift-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-one-bit-shift-tests)

add_test(
    NAME katana-one-bit-shift-tests
    COMMAND katana-one-bit-shift-tests
)

set(
    KATANA_GENERATED_ONE_BIT_SHIFT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_one_bit_shift_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_ONE_BIT_SHIFT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/one_bit_shift_execution.bin
        8C010000
        ${KATANA_GENERATED_ONE_BIT_SHIFT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/one_bit_shift_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1201 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_ONE_BIT_SHIFT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_one_bit_shift_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_ONE_BIT_SHIFT_CPP}
)

add_executable(katana-generated-one-bit-shift-tests
    tests/alu/generated_one_bit_shift_tests.cpp
    ${KATANA_GENERATED_ONE_BIT_SHIFT_CPP}
)

target_include_directories(
    katana-generated-one-bit-shift-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-one-bit-shift-tests)

add_dependencies(
    katana-generated-one-bit-shift-tests
    katana-recomp
)

add_test(
    NAME katana-generated-one-bit-shift-tests
    COMMAND katana-generated-one-bit-shift-tests
)
add_executable(katana-fixed-shift-tests
    tests/alu/fixed_shift_tests.cpp
)

target_link_libraries(
    katana-fixed-shift-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-fixed-shift-tests)

add_test(
    NAME katana-fixed-shift-tests
    COMMAND katana-fixed-shift-tests
)

set(
    KATANA_GENERATED_FIXED_SHIFT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_fixed_shift_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_FIXED_SHIFT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fixed_shift_execution.bin
        8C010000
        ${KATANA_GENERATED_FIXED_SHIFT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/fixed_shift_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1202 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_FIXED_SHIFT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_fixed_shift_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_FIXED_SHIFT_CPP}
)

add_executable(katana-generated-fixed-shift-tests
    tests/alu/generated_fixed_shift_tests.cpp
    ${KATANA_GENERATED_FIXED_SHIFT_CPP}
)

target_include_directories(
    katana-generated-fixed-shift-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-fixed-shift-tests)

add_dependencies(
    katana-generated-fixed-shift-tests
    katana-recomp
)

add_test(
    NAME katana-generated-fixed-shift-tests
    COMMAND katana-generated-fixed-shift-tests
)
add_executable(katana-rotation-tests
    tests/alu/rotation_tests.cpp
)

target_link_libraries(
    katana-rotation-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-rotation-tests)

add_test(
    NAME katana-rotation-tests
    COMMAND katana-rotation-tests
)

set(
    KATANA_GENERATED_ROTATION_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_rotation_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_ROTATION_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rotation_execution.bin
        8C010000
        ${KATANA_GENERATED_ROTATION_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/rotation_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1203 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_ROTATION_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_rotation_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_ROTATION_CPP}
)

add_executable(katana-generated-rotation-tests
    tests/alu/generated_rotation_tests.cpp
    ${KATANA_GENERATED_ROTATION_CPP}
)

target_include_directories(
    katana-generated-rotation-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-rotation-tests)

add_dependencies(
    katana-generated-rotation-tests
    katana-recomp
)

add_test(
    NAME katana-generated-rotation-tests
    COMMAND katana-generated-rotation-tests
)
add_executable(katana-dynamic-shift-tests
    tests/alu/dynamic_shift_tests.cpp
)

target_link_libraries(
    katana-dynamic-shift-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-dynamic-shift-tests)

add_test(
    NAME katana-dynamic-shift-tests
    COMMAND katana-dynamic-shift-tests
)

set(
    KATANA_GENERATED_DYNAMIC_SHIFT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_dynamic_shift_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_DYNAMIC_SHIFT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_shift_execution.bin
        8C010000
        ${KATANA_GENERATED_DYNAMIC_SHIFT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/dynamic_shift_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1204 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_DYNAMIC_SHIFT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_dynamic_shift_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_DYNAMIC_SHIFT_CPP}
)

add_executable(katana-generated-dynamic-shift-tests
    tests/alu/generated_dynamic_shift_tests.cpp
    ${KATANA_GENERATED_DYNAMIC_SHIFT_CPP}
)

target_include_directories(
    katana-generated-dynamic-shift-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-dynamic-shift-tests)

add_dependencies(
    katana-generated-dynamic-shift-tests
    katana-recomp
)

add_test(
    NAME katana-generated-dynamic-shift-tests
    COMMAND katana-generated-dynamic-shift-tests
)
add_executable(katana-simple-multiplication-tests
    tests/alu/simple_multiplication_tests.cpp
)

target_link_libraries(
    katana-simple-multiplication-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-simple-multiplication-tests)

add_test(
    NAME katana-simple-multiplication-tests
    COMMAND katana-simple-multiplication-tests
)

set(
    KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_simple_multiplication_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simple_multiplication_execution.bin
        8C010000
        ${KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/simple_multiplication_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1301 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_simple_multiplication_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP}
)

add_executable(katana-generated-simple-multiplication-tests
    tests/alu/generated_simple_multiplication_tests.cpp
    ${KATANA_GENERATED_SIMPLE_MULTIPLICATION_CPP}
)

target_include_directories(
    katana-generated-simple-multiplication-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-simple-multiplication-tests)

add_dependencies(
    katana-generated-simple-multiplication-tests
    katana-recomp
)

add_test(
    NAME katana-generated-simple-multiplication-tests
    COMMAND katana-generated-simple-multiplication-tests
)
add_executable(katana-double-multiplication-tests
    tests/alu/double_multiplication_tests.cpp
)

target_link_libraries(
    katana-double-multiplication-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-double-multiplication-tests)

add_test(
    NAME katana-double-multiplication-tests
    COMMAND katana-double-multiplication-tests
)

set(
    KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_double_multiplication_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/double_multiplication_execution.bin
        8C010000
        ${KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/double_multiplication_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1302 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_double_multiplication_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP}
)

add_executable(katana-generated-double-multiplication-tests
    tests/alu/generated_double_multiplication_tests.cpp
    ${KATANA_GENERATED_DOUBLE_MULTIPLICATION_CPP}
)

target_include_directories(
    katana-generated-double-multiplication-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-double-multiplication-tests)

add_dependencies(
    katana-generated-double-multiplication-tests
    katana-recomp
)

add_test(
    NAME katana-generated-double-multiplication-tests
    COMMAND katana-generated-double-multiplication-tests
)
add_executable(katana-mac-tests
    tests/alu/mac_tests.cpp
)

target_link_libraries(
    katana-mac-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-mac-tests)

add_test(
    NAME katana-mac-tests
    COMMAND katana-mac-tests
)

set(
    KATANA_GENERATED_MAC_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_mac_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_MAC_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mac_execution.bin
        8C010000
        ${KATANA_GENERATED_MAC_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/mac_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1303 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_MAC_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_mac_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_MAC_CPP}
)

add_executable(katana-generated-mac-tests
    tests/alu/generated_mac_tests.cpp
    ${KATANA_GENERATED_MAC_CPP}
)

target_include_directories(
    katana-generated-mac-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-mac-tests)

add_dependencies(
    katana-generated-mac-tests
    katana-recomp
)

add_test(
    NAME katana-generated-mac-tests
    COMMAND katana-generated-mac-tests
)
add_executable(katana-division-tests
    tests/alu/division_tests.cpp
)

target_link_libraries(
    katana-division-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-division-tests)

add_test(
    NAME katana-division-tests
    COMMAND katana-division-tests
)

set(
    KATANA_GENERATED_DIVISION_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_division_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_DIVISION_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/division_execution.bin
        8C010000
        ${KATANA_GENERATED_DIVISION_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/division_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1304 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_DIVISION_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/alu/generated_division_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_DIVISION_CPP}
)

add_executable(katana-generated-division-tests
    tests/alu/generated_division_tests.cpp
    ${KATANA_GENERATED_DIVISION_CPP}
)

target_include_directories(
    katana-generated-division-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-division-tests)

add_dependencies(
    katana-generated-division-tests
    katana-recomp
)

add_test(
    NAME katana-generated-division-tests
    COMMAND katana-generated-division-tests
)

add_executable(katana-predec-postinc-tests
    tests/memory/predec_postinc_tests.cpp
)

target_link_libraries(
    katana-predec-postinc-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-predec-postinc-tests)

add_test(
    NAME katana-predec-postinc-tests
    COMMAND katana-predec-postinc-tests
)

set(
    KATANA_GENERATED_PREDEC_POSTINC_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_predec_postinc_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_PREDEC_POSTINC_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/predec_postinc_execution.bin
        8C010000
        ${KATANA_GENERATED_PREDEC_POSTINC_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/predec_postinc_execution.bin
    COMMENT
        "Generiere C++-Programm fuer KR-1401 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_PREDEC_POSTINC_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_predec_postinc_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_PREDEC_POSTINC_CPP}
)

add_executable(katana-generated-predec-postinc-tests
    tests/memory/generated_predec_postinc_tests.cpp
    ${KATANA_GENERATED_PREDEC_POSTINC_CPP}
)

target_include_directories(
    katana-generated-predec-postinc-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-predec-postinc-tests)

add_dependencies(
    katana-generated-predec-postinc-tests
    katana-recomp
)

add_test(
    NAME katana-generated-predec-postinc-tests
    COMMAND katana-generated-predec-postinc-tests
)

add_executable(katana-register-displacement-tests
    tests/memory/register_displacement_tests.cpp
)

target_link_libraries(
    katana-register-displacement-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-register-displacement-tests)

add_test(
    NAME katana-register-displacement-tests
    COMMAND katana-register-displacement-tests
)

set(
    KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_register_displacement_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/register_displacement_execution.bin
        8C010000
        ${KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/register_displacement_execution.bin
    COMMENT
        "Generiere KR-1402 C++ ueber Binary-Reader und CLI"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_register_displacement_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP}
)

add_executable(katana-generated-register-displacement-tests
    tests/memory/generated_register_displacement_tests.cpp
    ${KATANA_GENERATED_REGISTER_DISPLACEMENT_CPP}
)

target_include_directories(
    katana-generated-register-displacement-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-register-displacement-tests)

add_dependencies(
    katana-generated-register-displacement-tests
    katana-recomp
)

add_test(
    NAME katana-generated-register-displacement-tests
    COMMAND katana-generated-register-displacement-tests
)

add_executable(katana-r0-indexed-tests
    tests/memory/r0_indexed_tests.cpp
)

target_link_libraries(
    katana-r0-indexed-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-r0-indexed-tests)

add_test(
    NAME katana-r0-indexed-tests
    COMMAND katana-r0-indexed-tests
)

set(
    KATANA_GENERATED_R0_INDEXED_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_r0_indexed_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_R0_INDEXED_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/r0_indexed_execution.bin
        8C010000
        ${KATANA_GENERATED_R0_INDEXED_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/r0_indexed_execution.bin
    COMMENT
        "Generiere KR-1403 C++ ueber Binary-Reader und CLI"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_R0_INDEXED_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_r0_indexed_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_R0_INDEXED_CPP}
)

add_executable(katana-generated-r0-indexed-tests
    tests/memory/generated_r0_indexed_tests.cpp
    ${KATANA_GENERATED_R0_INDEXED_CPP}
)

target_include_directories(
    katana-generated-r0-indexed-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-r0-indexed-tests)

add_dependencies(
    katana-generated-r0-indexed-tests
    katana-recomp
)

add_test(
    NAME katana-generated-r0-indexed-tests
    COMMAND katana-generated-r0-indexed-tests
)

add_executable(katana-gbr-displacement-tests
    tests/memory/gbr_displacement_tests.cpp
)

target_link_libraries(
    katana-gbr-displacement-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-gbr-displacement-tests)

add_test(
    NAME katana-gbr-displacement-tests
    COMMAND katana-gbr-displacement-tests
)

set(
    KATANA_GENERATED_GBR_DISPLACEMENT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_gbr_displacement_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_GBR_DISPLACEMENT_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gbr_displacement_execution.bin
        8C010000
        ${KATANA_GENERATED_GBR_DISPLACEMENT_CPP}
        8C010000
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gbr_displacement_execution.bin
    COMMENT
        "Generiere KR-1404 C++ ueber Binary-Reader und CLI"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_GBR_DISPLACEMENT_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_gbr_displacement_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_GBR_DISPLACEMENT_CPP}
)

add_executable(katana-generated-gbr-displacement-tests
    tests/memory/generated_gbr_displacement_tests.cpp
    ${KATANA_GENERATED_GBR_DISPLACEMENT_CPP}
)

target_include_directories(
    katana-generated-gbr-displacement-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-gbr-displacement-tests)

add_dependencies(
    katana-generated-gbr-displacement-tests
    katana-recomp
)

add_test(
    NAME katana-generated-gbr-displacement-tests
    COMMAND katana-generated-gbr-displacement-tests
)

add_executable(katana-pc-relative-tests
    tests/memory/pc_relative_tests.cpp
)

target_link_libraries(
    katana-pc-relative-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-pc-relative-tests)

add_test(
    NAME katana-pc-relative-tests
    COMMAND katana-pc-relative-tests
)

set(
    KATANA_GENERATED_PC_RELATIVE_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_pc_relative_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_PC_RELATIVE_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-recomp>
        emit-cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pc_relative_execution.bin
        00000100
        ${KATANA_GENERATED_PC_RELATIVE_CPP}
        00000100
    DEPENDS
        katana-recomp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pc_relative_execution.bin
    COMMENT
        "Generiere KR-1405 C++ ueber Binary-Reader und CLI"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_PC_RELATIVE_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_pc_relative_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_PC_RELATIVE_CPP}
)

add_executable(katana-generated-pc-relative-tests
    tests/memory/generated_pc_relative_tests.cpp
    ${KATANA_GENERATED_PC_RELATIVE_CPP}
)

target_include_directories(
    katana-generated-pc-relative-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-pc-relative-tests)

add_dependencies(
    katana-generated-pc-relative-tests
    katana-recomp
)

add_test(
    NAME katana-generated-pc-relative-tests
    COMMAND katana-generated-pc-relative-tests
)

set(
    KATANA_GENERATED_PC_RELATIVE_EDGE_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_pc_relative_edge_program.cpp
)

add_custom_command(
    OUTPUT ${KATANA_GENERATED_PC_RELATIVE_EDGE_CPP}
    COMMAND
        $<TARGET_FILE:katana-pc-relative-tests>
        --emit-cpp
        ${KATANA_GENERATED_PC_RELATIVE_EDGE_CPP}
    DEPENDS
        katana-pc-relative-tests
        tests/memory/pc_relative_tests.cpp
    COMMENT "Generiere synthetische KR-1405-Codegen-Grenzfaelle"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_PC_RELATIVE_EDGE_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/memory/generated_pc_relative_edge_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_PC_RELATIVE_EDGE_CPP}
)

add_executable(katana-generated-pc-relative-edge-tests
    tests/memory/generated_pc_relative_edge_tests.cpp
    ${KATANA_GENERATED_PC_RELATIVE_EDGE_CPP}
)

target_include_directories(
    katana-generated-pc-relative-edge-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-pc-relative-edge-tests)
add_dependencies(
    katana-generated-pc-relative-edge-tests
    katana-pc-relative-tests
)

add_test(
    NAME katana-generated-pc-relative-edge-tests
    COMMAND katana-generated-pc-relative-edge-tests
)

add_executable(katana-system-register-tests
    tests/system/system_register_tests.cpp
)

target_link_libraries(
    katana-system-register-tests
    PRIVATE
        katana_core
)

katana_enable_warnings(katana-system-register-tests)

add_test(
    NAME katana-system-register-tests
    COMMAND katana-system-register-tests
)

set(
    KATANA_GENERATED_SYSTEM_REGISTER_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_system_register_program.cpp
)

add_custom_command(
    OUTPUT
        ${KATANA_GENERATED_SYSTEM_REGISTER_CPP}
    COMMAND
        ${CMAKE_COMMAND}
        -E
        make_directory
        ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-system-register-tests>
        --emit-cpp
        ${KATANA_GENERATED_SYSTEM_REGISTER_CPP}
    DEPENDS
        katana-system-register-tests
        tests/system/system_register_tests.cpp
    COMMENT
        "Generiere C++-Programm fuer KR-1406 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_SYSTEM_REGISTER_CPP}
    PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/system/generated_system_register_tests.cpp
    PROPERTIES
        OBJECT_DEPENDS ${KATANA_GENERATED_SYSTEM_REGISTER_CPP}
)

add_executable(katana-generated-system-register-tests
    tests/system/generated_system_register_tests.cpp
    ${KATANA_GENERATED_SYSTEM_REGISTER_CPP}
)

target_include_directories(
    katana-generated-system-register-tests
    PRIVATE
        ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-system-register-tests)

add_dependencies(
    katana-generated-system-register-tests
    katana-system-register-tests
)

add_test(
    NAME katana-generated-system-register-tests
    COMMAND katana-generated-system-register-tests
)

add_executable(katana-privileged-control-tests
    tests/control_flow/privileged_control_tests.cpp
)

target_link_libraries(katana-privileged-control-tests PRIVATE katana_core)
katana_enable_warnings(katana-privileged-control-tests)

add_test(
    NAME katana-privileged-control-tests
    COMMAND katana-privileged-control-tests
)

set(
    KATANA_GENERATED_PRIVILEGED_CONTROL_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_privileged_control_program.cpp
)

add_custom_command(
    OUTPUT ${KATANA_GENERATED_PRIVILEGED_CONTROL_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-privileged-control-tests>
        --emit-cpp
        ${KATANA_GENERATED_PRIVILEGED_CONTROL_CPP}
    DEPENDS
        katana-privileged-control-tests
        tests/control_flow/privileged_control_tests.cpp
    COMMENT "Generiere C++-Programm fuer KR-1407 End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_PRIVILEGED_CONTROL_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/control_flow/generated_privileged_control_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_PRIVILEGED_CONTROL_CPP}
)

add_executable(katana-generated-privileged-control-tests
    tests/control_flow/generated_privileged_control_tests.cpp
    ${KATANA_GENERATED_PRIVILEGED_CONTROL_CPP}
)

target_include_directories(
    katana-generated-privileged-control-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-privileged-control-tests)
add_dependencies(
    katana-generated-privileged-control-tests
    katana-privileged-control-tests
)

add_test(
    NAME katana-generated-privileged-control-tests
    COMMAND katana-generated-privileged-control-tests
)

add_executable(katana-delay-slot-program-writer
    tests/codegen/delay_slot_program_writer.cpp
)

target_link_libraries(katana-delay-slot-program-writer PRIVATE katana_core)
katana_enable_warnings(katana-delay-slot-program-writer)

set(
    KATANA_GENERATED_DELAY_SLOT_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_delay_slot_program.cpp
)

add_custom_command(
    OUTPUT ${KATANA_GENERATED_DELAY_SLOT_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-delay-slot-program-writer>
        ${KATANA_GENERATED_DELAY_SLOT_CPP}
    DEPENDS
        katana-delay-slot-program-writer
        tests/codegen/delay_slot_program_writer.cpp
    COMMENT "Generiere C++-Programm fuer Delay-Slot-Exception-End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_DELAY_SLOT_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/codegen/generated_delay_slot_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_DELAY_SLOT_CPP}
)

add_executable(katana-generated-delay-slot-tests
    tests/codegen/generated_delay_slot_tests.cpp
    ${KATANA_GENERATED_DELAY_SLOT_CPP}
)

target_include_directories(
    katana-generated-delay-slot-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-delay-slot-tests)
add_dependencies(katana-generated-delay-slot-tests katana-delay-slot-program-writer)

add_test(
    NAME katana-generated-delay-slot-tests
    COMMAND katana-generated-delay-slot-tests
)

set(
    KATANA_GENERATED_FPU_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_fpu_program.cpp
)

add_custom_command(
    OUTPUT ${KATANA_GENERATED_FPU_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KATANA_GENERATED_TEST_DIR}
    COMMAND
        $<TARGET_FILE:katana-fpu-pipeline-tests>
        --emit-cpp
        ${KATANA_GENERATED_FPU_CPP}
    DEPENDS katana-fpu-pipeline-tests tests/fpu/fpu_pipeline_tests.cpp
    COMMENT "Generiere C++-Programm fuer Phase-5-FPU-End-to-End-Test"
    VERBATIM
)

set_source_files_properties(
    ${KATANA_GENERATED_FPU_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)

set_source_files_properties(
    tests/fpu/generated_fpu_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_FPU_CPP}
)

add_executable(katana-generated-fpu-tests
    tests/fpu/generated_fpu_tests.cpp
    ${KATANA_GENERATED_FPU_CPP}
)

target_include_directories(
    katana-generated-fpu-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)

katana_enable_warnings(katana-generated-fpu-tests)
add_dependencies(katana-generated-fpu-tests katana-fpu-pipeline-tests)

add_test(
    NAME katana-generated-fpu-tests
    COMMAND katana-generated-fpu-tests
)

get_property(
    KATANA_BUILD_TARGETS
    DIRECTORY
    PROPERTY BUILDSYSTEM_TARGETS
)

foreach(KATANA_BUILD_TARGET IN LISTS KATANA_BUILD_TARGETS)
    if(KATANA_BUILD_TARGET MATCHES "^katana-generated-")
        target_link_libraries(
            ${KATANA_BUILD_TARGET}
            PRIVATE
                katana_runtime
        )
    endif()
endforeach()
