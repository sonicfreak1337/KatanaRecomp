cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/KatanaVersions.cmake)

project(
    KatanaRecomp
    VERSION ${KATANA_PROJECT_VERSION}
    DESCRIPTION "Dreamcast SH-4 static recompilation framework"
    LANGUAGES CXX
)

if(MSVC)
    # Multi-config generators compile many translation units against one target PDB.
    # Serialize those writes instead of relying on generator-specific scheduling.
    add_compile_options(/FS)
endif()

find_package(Threads REQUIRED)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/katana/build_contract.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/build_contract.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(
    KATANA_ENABLE_SANITIZERS
    "Enable the supported Address/UndefinedBehavior sanitizers for Debug gates"
    OFF
)

option(
    KATANA_BUILD_FUZZERS
    "Build the deterministic synthetic decoder, loader and IR fuzz runner"
    OFF
)

option(
    KATANA_ENABLE_COVERAGE
    "Enable source coverage instrumentation or the MSVC dynamic coverage contract"
    OFF
)

option(
    KATANA_ENABLE_STATIC_ANALYSIS
    "Enable the supported compiler-integrated static analysis profile"
    OFF
)

option(
    KATANA_REPRODUCIBLE_ARTIFACTS
    "Remove host-path and timestamp variance from pre-alpha Debug artifacts"
    OFF
)

option(
    KATANA_BUILD_DESKTOP_GUI
    "Build the optional native desktop shell"
    OFF
)

set(
    KATANA_COMPILER_CACHE
    ""
    CACHE FILEPATH
    "Optional compiler-cache executable (for example ccache or sccache)"
)
if(KATANA_COMPILER_CACHE)
    if(IS_ABSOLUTE "${KATANA_COMPILER_CACHE}")
        set(KATANA_COMPILER_CACHE_EXECUTABLE "${KATANA_COMPILER_CACHE}")
    else()
        find_program(
            KATANA_COMPILER_CACHE_EXECUTABLE
            NAMES "${KATANA_COMPILER_CACHE}"
            REQUIRED
        )
    endif()
    if(NOT EXISTS "${KATANA_COMPILER_CACHE_EXECUTABLE}")
        message(FATAL_ERROR "KATANA_COMPILER_CACHE does not exist: ${KATANA_COMPILER_CACHE}")
    endif()
    set(CMAKE_CXX_COMPILER_LAUNCHER "${KATANA_COMPILER_CACHE_EXECUTABLE}")
endif()

if(KATANA_ENABLE_SANITIZERS)
    if(MSVC)
        string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        add_compile_options(/fsanitize=address /Oy-)
        add_link_options(/INCREMENTAL:NO)
        get_filename_component(KATANA_MSVC_COMPILER_DIRECTORY "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(
            KATANA_MSVC_ASAN_RUNTIME
            "${KATANA_MSVC_COMPILER_DIRECTORY}/clang_rt.asan_dynamic-x86_64.dll"
        )
        if(NOT EXISTS "${KATANA_MSVC_ASAN_RUNTIME}")
            message(FATAL_ERROR "MSVC AddressSanitizer runtime was not found beside cl.exe")
        endif()
        configure_file(
            "${KATANA_MSVC_ASAN_RUNTIME}"
            "${CMAKE_BINARY_DIR}/clang_rt.asan_dynamic-x86_64.dll"
            COPYONLY
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        add_link_options(
            -fsanitize=address,undefined
            -fno-sanitize-recover=all
        )
    else()
        message(FATAL_ERROR "KATANA_ENABLE_SANITIZERS is unsupported by this compiler")
    endif()
endif()

if(KATANA_ENABLE_COVERAGE)
    if(MSVC)
        set(KATANA_COVERAGE_BACKEND "msvc-dynamic")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(--coverage -fprofile-abs-path)
        add_link_options(--coverage)
        set(KATANA_COVERAGE_BACKEND "gcov")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(--coverage)
        add_link_options(--coverage)
        set(KATANA_COVERAGE_BACKEND "gcov")
    else()
        message(FATAL_ERROR "KATANA_ENABLE_COVERAGE is unsupported by this compiler")
    endif()
    file(
        GENERATE
        OUTPUT "${CMAKE_BINARY_DIR}/katana-coverage-backend.txt"
        CONTENT "${KATANA_COVERAGE_BACKEND}\n"
    )
endif()

if(KATANA_ENABLE_STATIC_ANALYSIS)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    if(MSVC)
        add_compile_options(/analyze /analyze:external- /WX)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        find_program(KATANA_CLANG_TIDY_EXECUTABLE NAMES clang-tidy REQUIRED)
        set(
            CMAKE_CXX_CLANG_TIDY
            "${KATANA_CLANG_TIDY_EXECUTABLE};--warnings-as-errors=*"
        )
        add_compile_options(-Werror)
    else()
        message(FATAL_ERROR "KATANA_ENABLE_STATIC_ANALYSIS is unsupported by this compiler")
    endif()
endif()

if(KATANA_REPRODUCIBLE_ARTIFACTS)
    if(MSVC)
        add_compile_options(
            /Brepro
            /experimental:deterministic
            "/pathmap:${CMAKE_SOURCE_DIR}=."
        )
        add_link_options(/Brepro "/PDBALTPATH:%_PDB%")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(
            "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
            "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
        )
    else()
        message(FATAL_ERROR "KATANA_REPRODUCIBLE_ARTIFACTS is unsupported by this compiler")
    endif()
endif()

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
    src/decoder/decoder.cpp
    src/decoder/instruction_metadata.cpp
    src/io/input_provenance.cpp
    src/io/json_report.cpp
    src/runtime/aica.cpp
    src/runtime/block_abi.cpp
    src/runtime/block_table.cpp
    src/runtime/indirect_dispatch.cpp
    src/runtime/controlled_fallback.cpp
    src/runtime/crash_report.cpp
    src/runtime/dispatch_diagnostics.cpp
    src/runtime/code_invalidation.cpp
    src/runtime/firmware_handoff.cpp
    src/runtime/runtime_provenance.cpp
    src/runtime/block_dispatch.cpp
    src/runtime/scheduler_safepoint.cpp
    src/runtime/system_replay.cpp
    src/runtime/system_asic.cpp
    src/runtime/block_guards.cpp
    src/runtime/bios_abi.cpp
    src/runtime/interpreter_boundary.cpp
    src/runtime/store_queue.cpp
    src/runtime/cache_control.cpp
    src/runtime/io_port.cpp
    src/runtime/dma.cpp
    src/runtime/disc.cpp
    src/runtime/disc_install.cpp
    src/runtime/dreamcast_boot.cpp
    src/runtime/dreamcast_memory.cpp
    src/runtime/dynamic_interpreter.cpp
    src/runtime/executable_modules.cpp
    src/runtime/native_aot_template.cpp
    src/runtime/exception.cpp
    src/runtime/fpu.cpp
    src/runtime/gdi.cpp
    src/runtime/gdrom_controller.cpp
    src/runtime/holly_dma.cpp
    src/runtime/host_video.cpp
    src/runtime/host_runtime.cpp
    src/runtime/interrupt.cpp
    src/runtime/iso9660.cpp
    src/runtime/maple.cpp
    src/runtime/maple_mmio.cpp
    src/runtime/media_clock.cpp
    src/runtime/mmu_control.cpp
    src/runtime/packed_disc.cpp
    src/runtime/persistent_storage.cpp
    src/runtime/pvr.cpp
    src/runtime/platform_interrupt.cpp
    src/runtime/scheduler.cpp
    src/runtime/scif.cpp
    src/runtime/timers.cpp
    src/runtime/trace.cpp
    src/runtime/memory.cpp
    src/runtime/runtime.cpp
)

add_library(KatanaRecomp::runtime ALIAS katana_runtime)
set_target_properties(katana_runtime PROPERTIES EXPORT_NAME runtime)

target_include_directories(
    katana_runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# A static runtime built with sanitizers carries sanitizer-dependent ABI details
# into every object file. In particular, MSVC's ASan-enabled STL emits
# annotate_string/annotate_vector mismatch records. Consumers therefore must be
# compiled with the same sanitizer profile; keeping these flags PRIVATE makes
# an otherwise valid installed SDK fail at link time. Export the profile as
# target usage requirements so build-tree and installed consumers inherit it
# through KatanaRecomp::runtime.
if(KATANA_ENABLE_SANITIZERS)
    if(MSVC)
        target_compile_options(katana_runtime INTERFACE /fsanitize=address /Oy-)
        target_link_options(katana_runtime INTERFACE /INCREMENTAL:NO)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            katana_runtime
            INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
                -fno-sanitize-recover=all
        )
        target_link_options(
            katana_runtime
            INTERFACE
                -fsanitize=address,undefined
                -fno-sanitize-recover=all
        )
    endif()
endif()

katana_enable_warnings(katana_runtime)
if(MSVC)
    target_compile_options(katana_runtime PRIVATE /fp:strict)
else()
    target_compile_options(katana_runtime PRIVATE -frounding-math)
endif()

if(WIN32)
    target_link_libraries(katana_runtime PUBLIC bcrypt gdi32 user32 winmm)
endif()

add_library(katana_core STATIC
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
    src/platform/dreamcast_disc.cpp
    src/platform/firmware_diagnostics.cpp
    src/platform/firmware_profile.cpp
    src/platform/phase6_gate.cpp
    src/analysis/disassembler.cpp
    src/analysis/analysis_index.cpp
    src/analysis/code_address.cpp
    src/analysis/evidence.cpp
    src/analysis/basic_blocks.cpp
    src/analysis/function_analysis.cpp
    src/analysis/function_value_analysis.cpp
    src/analysis/hardware_audit.cpp
    src/analysis/executable_inventory.cpp
    src/analysis/recursive_analysis.cpp
    src/analysis/runtime_code_copy_analysis.cpp
    src/analysis/value_analysis.cpp
    src/analysis/jump_table_analysis.cpp
    src/analysis/analysis_overrides.cpp
    src/analysis/control_flow_report.cpp
    src/analysis/control_flow_analysis.cpp
    src/analysis/symbol_names.cpp
    src/analysis/graph_export.cpp
    src/ir/lower.cpp
    src/ir/accumulator_effect.cpp
    src/ir/memory_effect.cpp
    src/ir/operand_width.cpp
    src/ir/optimize.cpp
    src/ir/status_effect.cpp
    src/ir/serialize.cpp
    src/ir/verifier.cpp
    src/codegen/backend.cpp
    src/codegen/cache.cpp
    src/codegen/cpp_emitter.cpp
    src/codegen/metadata.cpp
    src/codegen/naming.cpp
    src/codegen/partition.cpp
    src/codegen/probe.cpp
    src/codegen/project.cpp
    src/codegen/port_export.cpp
    src/codegen/source_map.cpp
)

target_include_directories(
    katana_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(katana_core PUBLIC katana_runtime)
set_target_properties(katana_core PROPERTIES EXPORT_NAME analyzer)
katana_enable_warnings(katana_core)

add_library(katana_phase9 STATIC
    src/phase9/homebrew.cpp
    src/phase9/performance.cpp
)
add_library(KatanaRecomp::phase9 ALIAS katana_phase9)
target_include_directories(
    katana_phase9
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(katana_phase9 PUBLIC katana_core katana_runtime)
katana_enable_warnings(katana_phase9)

add_library(katana_app STATIC
    src/app/application.cpp
)
add_library(KatanaRecomp::app ALIAS katana_app)
target_include_directories(
    katana_app
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(katana_app PUBLIC katana_core katana_runtime)
katana_enable_warnings(katana_app)

add_library(katana_gui STATIC
    src/gui/model.cpp
)
add_library(KatanaRecomp::gui ALIAS katana_gui)
target_include_directories(
    katana_gui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(katana_gui PUBLIC katana_app)
katana_enable_warnings(katana_gui)

add_library(katana_test_support STATIC
    src/testing/differential_execution.cpp
)

add_library(KatanaRecomp::test_support ALIAS katana_test_support)

target_include_directories(
    katana_test_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(katana_test_support PUBLIC katana_runtime)
katana_enable_warnings(katana_test_support)

add_executable(
    katana-differential-program-writer
    tests/testing/differential_program_writer.cpp
)
target_link_libraries(
    katana-differential-program-writer
    PRIVATE
        katana_core
        katana_test_support
)
katana_enable_warnings(katana-differential-program-writer)

set(
    KATANA_DIFFERENTIAL_GENERATED_CPP
    ${CMAKE_CURRENT_BINARY_DIR}/generated-tests/differential_programs.cpp
)
add_custom_command(
    OUTPUT ${KATANA_DIFFERENTIAL_GENERATED_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated-tests
    COMMAND $<TARGET_FILE:katana-differential-program-writer>
            ${KATANA_DIFFERENTIAL_GENERATED_CPP}
    DEPENDS
        katana-differential-program-writer
        tests/testing/differential_program_writer.cpp
        src/testing/differential_execution.cpp
    COMMENT "Generiere echte C++-Runner fuer das Differentialkorpus"
    VERBATIM
)
set_source_files_properties(${KATANA_DIFFERENTIAL_GENERATED_CPP} PROPERTIES GENERATED TRUE)

add_executable(katana-recomp
    src/cli/main.cpp
)

target_link_libraries(
    katana-recomp
    PRIVATE
        katana_core
        katana_runtime
        katana_app
)

target_compile_definitions(
    katana-recomp
    PRIVATE
        KATANA_RECOMP_VERSION="${PROJECT_VERSION}"
)

katana_enable_warnings(katana-recomp)

set(KATANA_DESKTOP_GUI_AVAILABLE ${KATANA_BUILD_DESKTOP_GUI})
if(KATANA_DESKTOP_GUI_AVAILABLE AND UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(NOT X11_FOUND)
        set(KATANA_DESKTOP_GUI_AVAILABLE OFF)
        message(STATUS "X11 development files absent: building CLI/core without desktop GUI")
    endif()
endif()
if(KATANA_DESKTOP_GUI_AVAILABLE)
    if(WIN32)
        enable_language(RC)
        add_executable(katana-recomp-gui src/gui/main.cpp src/gui/katana-recomp-gui.rc)
    else()
        add_executable(katana-recomp-gui src/gui/main.cpp)
    endif()
    target_link_libraries(katana-recomp-gui PRIVATE katana_gui)
    if(WIN32)
        target_link_libraries(katana-recomp-gui PRIVATE comctl32 dwmapi user32 uxtheme)
        add_subdirectory(src/gui/dialog)
        add_dependencies(katana-recomp-gui katana-file-dialog)
    elseif(UNIX AND NOT APPLE)
        target_link_libraries(katana-recomp-gui PRIVATE X11::X11)
    else()
        message(FATAL_ERROR "The Phase-10 desktop shell supports Windows and Linux only")
    endif()
    target_compile_definitions(
        katana-recomp-gui
        PRIVATE
            KATANA_RECOMP_VERSION="${PROJECT_VERSION}"
    )
    katana_enable_warnings(katana-recomp-gui)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/gui/KatanaLogo.png
        ${CMAKE_CURRENT_BINARY_DIR}/assets/KatanaLogo.png
        COPYONLY
    )
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/gui/asset-manifest.json
        ${CMAKE_CURRENT_BINARY_DIR}/assets/asset-manifest.json
        COPYONLY
    )
endif()

enable_testing()

add_executable(
    katana-differential-execution-tests
    tests/testing/differential_execution_tests.cpp
    ${KATANA_DIFFERENTIAL_GENERATED_CPP}
)
target_link_libraries(
    katana-differential-execution-tests
    PRIVATE
        katana_test_support
)
katana_enable_warnings(katana-differential-execution-tests)
add_dependencies(katana-differential-execution-tests katana-differential-program-writer)
add_test(
    NAME katana-differential-execution-tests
    COMMAND katana-differential-execution-tests
)

add_executable(
    katana-phase9-homebrew-tests
    tests/phase9/homebrew_tests.cpp
)
target_link_libraries(katana-phase9-homebrew-tests PRIVATE katana_phase9)
katana_enable_warnings(katana-phase9-homebrew-tests)
add_test(NAME katana-phase9-homebrew-tests COMMAND katana-phase9-homebrew-tests)

add_executable(
    katana-phase9-performance-tests
    tests/phase9/performance_tests.cpp
)
target_link_libraries(katana-phase9-performance-tests PRIVATE katana_phase9)
katana_enable_warnings(katana-phase9-performance-tests)
add_test(NAME katana-phase9-performance-tests COMMAND katana-phase9-performance-tests)

add_executable(
    katana-phase10-application-tests
    tests/phase10/application_tests.cpp
)
target_link_libraries(katana-phase10-application-tests PRIVATE katana_gui)
target_compile_definitions(katana-phase10-application-tests PRIVATE
    KATANA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
katana_enable_warnings(katana-phase10-application-tests)
add_test(NAME katana-phase10-application-tests COMMAND katana-phase10-application-tests)
set_tests_properties(
    katana-phase10-application-tests
    PROPERTIES TIMEOUT 300 RESOURCE_LOCK katana-external-port-build
)

add_executable(
    katana-phase10-gui-model-tests
    tests/phase10/gui_model_tests.cpp
)
target_link_libraries(katana-phase10-gui-model-tests PRIVATE katana_gui)
katana_enable_warnings(katana-phase10-gui-model-tests)
add_test(NAME katana-phase10-gui-model-tests COMMAND katana-phase10-gui-model-tests)

if(TARGET katana-recomp-gui)
    add_test(NAME katana-phase10-gui-minimal-start COMMAND katana-recomp-gui --smoke)
    if(WIN32)
        add_test(NAME katana-phase10-native-shell-lifecycle COMMAND katana-recomp-gui --native-smoke)
        add_test(NAME katana-phase11-native-qol-tests COMMAND katana-recomp-gui --qol-smoke)
        add_test(
            NAME katana-phase11-private-retail-harness-tests
            COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass -File
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/phase11/run-private-retail-debug.ps1 -SelfTest
        )
    endif()
endif()
if(MSVC AND KATANA_ENABLE_SANITIZERS)
    add_test(
        NAME katana-phase10-msvc-asan-runtime
        COMMAND ${CMAKE_COMMAND} -E compare_files
            "${KATANA_MSVC_ASAN_RUNTIME}"
            "${CMAKE_BINARY_DIR}/clang_rt.asan_dynamic-x86_64.dll"
    )
endif()

add_executable(
    katana-phase9-benchmark
    tools/phase9/benchmark_runner.cpp
)
target_link_libraries(katana-phase9-benchmark PRIVATE katana_phase9)
katana_enable_warnings(katana-phase9-benchmark)

add_executable(
    katana-phase9-homebrew-report
    tools/phase9/homebrew_report.cpp
)
target_link_libraries(katana-phase9-homebrew-report PRIVATE katana_phase9)
katana_enable_warnings(katana-phase9-homebrew-report)

if(KATANA_BUILD_FUZZERS)
    add_executable(katana-fuzz tools/fuzz/fuzz_runner.cpp)
    target_link_libraries(katana-fuzz PRIVATE katana_core katana_runtime)
    katana_enable_warnings(katana-fuzz)

    function(katana_add_fuzz_smoke target seed)
        add_test(
            NAME katana-fuzz-${target}-smoke
            COMMAND katana-fuzz
                --target ${target}
                --seed ${seed}
                --iterations 256
                --max-input-size 4096
                --isolate
        )
        # Dynamic coverage adds substantial per-iteration overhead on MSVC.
        set_tests_properties(katana-fuzz-${target}-smoke PROPERTIES TIMEOUT 900)
    endfunction()

    # These are the exact per-target seeds previously derived by --target all
    # from the canonical seed 0x3703. Separate CTest entries preserve every
    # deterministic case while allowing the four independent targets to overlap.
    katana_add_fuzz_smoke(decoder 0xDEC0E902)
    katana_add_fuzz_smoke(loader 0x10ADD701)
    katana_add_fuzz_smoke(ir 0x1A003700)
    katana_add_fuzz_smoke(runtime 0x37083707)

endif()

add_executable(katana-runtime-tests
    tests/runtime/runtime_tests.cpp
)

target_link_libraries(katana-runtime-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-runtime-tests)

add_test(
    NAME katana-runtime-tests
    COMMAND katana-runtime-tests
)

add_executable(
    katana-phase8-diagnostics-tests
    tests/runtime/phase8_diagnostics_tests.cpp
)
target_link_libraries(katana-phase8-diagnostics-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-phase8-diagnostics-tests)
add_test(NAME katana-phase8-diagnostics-tests COMMAND katana-phase8-diagnostics-tests)

add_executable(katana-block-abi-tests tests/runtime/block_abi_tests.cpp)
target_link_libraries(katana-block-abi-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-block-abi-tests)
add_test(NAME katana-block-abi-tests COMMAND katana-block-abi-tests)

add_executable(katana-block-table-tests tests/runtime/block_table_tests.cpp)
target_link_libraries(katana-block-table-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-block-table-tests)
add_test(NAME katana-block-table-tests COMMAND katana-block-table-tests)

add_executable(katana-indirect-dispatch-tests tests/runtime/indirect_dispatch_tests.cpp)
target_link_libraries(katana-indirect-dispatch-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-indirect-dispatch-tests)
add_test(NAME katana-indirect-dispatch-tests COMMAND katana-indirect-dispatch-tests)

add_executable(katana-controlled-fallback-tests tests/runtime/controlled_fallback_tests.cpp)
target_link_libraries(katana-controlled-fallback-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-controlled-fallback-tests)
add_test(NAME katana-controlled-fallback-tests COMMAND katana-controlled-fallback-tests)

add_executable(katana-code-invalidation-tests tests/runtime/code_invalidation_tests.cpp)
target_link_libraries(katana-code-invalidation-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-code-invalidation-tests)
add_test(NAME katana-code-invalidation-tests COMMAND katana-code-invalidation-tests)

add_executable(katana-firmware-handoff-tests tests/runtime/firmware_handoff_tests.cpp)
target_link_libraries(katana-firmware-handoff-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-firmware-handoff-tests)
add_test(NAME katana-firmware-handoff-tests COMMAND katana-firmware-handoff-tests)

add_executable(katana-bios-abi-tests tests/runtime/bios_abi_tests.cpp)
target_link_libraries(katana-bios-abi-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-bios-abi-tests)
add_test(NAME katana-bios-abi-tests COMMAND katana-bios-abi-tests)

add_executable(katana-block-dispatch-tests tests/runtime/block_dispatch_tests.cpp)
target_link_libraries(katana-block-dispatch-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-block-dispatch-tests)
add_test(NAME katana-block-dispatch-tests COMMAND katana-block-dispatch-tests)

add_executable(katana-scheduler-safepoint-tests tests/runtime/scheduler_safepoint_tests.cpp)
target_link_libraries(katana-scheduler-safepoint-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-scheduler-safepoint-tests)
add_test(NAME katana-scheduler-safepoint-tests COMMAND katana-scheduler-safepoint-tests)

add_executable(katana-system-replay-tests tests/runtime/system_replay_tests.cpp)
target_link_libraries(katana-system-replay-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-system-replay-tests)
add_test(NAME katana-system-replay-tests COMMAND katana-system-replay-tests)

add_executable(katana-block-guard-tests tests/runtime/block_guard_tests.cpp)
target_link_libraries(katana-block-guard-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-block-guard-tests)
add_test(NAME katana-block-guard-tests COMMAND katana-block-guard-tests)

add_executable(katana-interpreter-boundary-tests tests/runtime/interpreter_boundary_tests.cpp)
target_link_libraries(katana-interpreter-boundary-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-interpreter-boundary-tests)
add_test(NAME katana-interpreter-boundary-tests COMMAND katana-interpreter-boundary-tests)

add_executable(katana-store-queue-tests tests/runtime/store_queue_tests.cpp)
target_link_libraries(katana-store-queue-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-store-queue-tests)
add_test(NAME katana-store-queue-tests COMMAND katana-store-queue-tests)

add_executable(katana-platform-services-tests tests/runtime/platform_services_tests.cpp)
target_link_libraries(katana-platform-services-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-platform-services-tests)
add_test(NAME katana-platform-services-tests COMMAND katana-platform-services-tests)

add_executable(katana-cache-control-tests tests/runtime/cache_control_tests.cpp)
target_link_libraries(katana-cache-control-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-cache-control-tests)
add_test(NAME katana-cache-control-tests COMMAND katana-cache-control-tests)

add_executable(katana-io-port-tests tests/runtime/io_port_tests.cpp)
target_link_libraries(katana-io-port-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-io-port-tests)
add_test(NAME katana-io-port-tests COMMAND katana-io-port-tests)

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

add_executable(katana-dreamcast-vertical-slice-tests
    tests/platform/dreamcast_vertical_slice_tests.cpp
)
target_link_libraries(katana-dreamcast-vertical-slice-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-dreamcast-vertical-slice-tests)
add_test(
    NAME katana-dreamcast-vertical-slice-tests
    COMMAND katana-dreamcast-vertical-slice-tests
)

add_executable(katana-phase6-gate-tests tests/platform/phase6_gate_tests.cpp)
target_link_libraries(katana-phase6-gate-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-phase6-gate-tests)
add_test(NAME katana-phase6-gate-tests COMMAND katana-phase6-gate-tests)
set_tests_properties(katana-phase6-gate-tests PROPERTIES TIMEOUT 60)

add_executable(
    katana-firmware-diagnostics-tests
    tests/platform/firmware_diagnostics_tests.cpp
)
target_link_libraries(katana-firmware-diagnostics-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-firmware-diagnostics-tests)
add_test(NAME katana-firmware-diagnostics-tests COMMAND katana-firmware-diagnostics-tests)

add_executable(katana-maple-bus-tests tests/runtime/maple_bus_tests.cpp)
target_link_libraries(katana-maple-bus-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-maple-bus-tests)
add_test(NAME katana-maple-bus-tests COMMAND katana-maple-bus-tests)

add_executable(katana-maple-controller-tests tests/runtime/maple_controller_tests.cpp)
target_link_libraries(katana-maple-controller-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-maple-controller-tests)
add_test(NAME katana-maple-controller-tests COMMAND katana-maple-controller-tests)

add_executable(katana-maple-vmu-tests tests/runtime/maple_vmu_tests.cpp)
target_link_libraries(katana-maple-vmu-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-maple-vmu-tests)
add_test(NAME katana-maple-vmu-tests COMMAND katana-maple-vmu-tests)

add_executable(katana-pvr-register-tests tests/runtime/pvr_register_tests.cpp)
target_link_libraries(katana-pvr-register-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-pvr-register-tests)
add_test(NAME katana-pvr-register-tests COMMAND katana-pvr-register-tests)

add_executable(katana-pvr-framebuffer-tests tests/runtime/pvr_framebuffer_tests.cpp)
target_link_libraries(katana-pvr-framebuffer-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-pvr-framebuffer-tests)
add_test(NAME katana-pvr-framebuffer-tests COMMAND katana-pvr-framebuffer-tests)

add_executable(katana-host-video-tests tests/runtime/host_video_tests.cpp)
target_link_libraries(katana-host-video-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-host-video-tests)
add_test(NAME katana-host-video-tests COMMAND katana-host-video-tests)

add_executable(katana-host-runtime-tests tests/runtime/host_runtime_tests.cpp)
target_link_libraries(katana-host-runtime-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-host-runtime-tests)
add_test(NAME katana-host-runtime-tests COMMAND katana-host-runtime-tests)

add_executable(katana-persistent-storage-tests tests/runtime/persistent_storage_tests.cpp)
target_link_libraries(katana-persistent-storage-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-persistent-storage-tests)
add_test(NAME katana-persistent-storage-tests COMMAND katana-persistent-storage-tests)

add_executable(katana-executable-modules-tests tests/runtime/executable_modules_tests.cpp)
target_link_libraries(katana-executable-modules-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-executable-modules-tests)
add_test(NAME katana-executable-modules-tests COMMAND katana-executable-modules-tests)

add_executable(katana-native-aot-template-tests tests/runtime/native_aot_template_tests.cpp)
target_link_libraries(katana-native-aot-template-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-native-aot-template-tests)
add_test(NAME katana-native-aot-template-tests COMMAND katana-native-aot-template-tests)

add_executable(katana-execution-coverage-tests tests/runtime/execution_coverage_tests.cpp)
target_link_libraries(katana-execution-coverage-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-execution-coverage-tests)
add_test(NAME katana-execution-coverage-tests COMMAND katana-execution-coverage-tests)

add_executable(katana-pvr-ta-tests tests/runtime/pvr_ta_tests.cpp)
target_link_libraries(katana-pvr-ta-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-pvr-ta-tests)
add_test(NAME katana-pvr-ta-tests COMMAND katana-pvr-ta-tests)

add_executable(katana-pvr-render-tests tests/runtime/pvr_render_tests.cpp)
target_link_libraries(katana-pvr-render-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-pvr-render-tests)
add_test(NAME katana-pvr-render-tests COMMAND katana-pvr-render-tests)

add_executable(katana-aica-register-tests tests/runtime/aica_register_tests.cpp)
target_link_libraries(katana-aica-register-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-aica-register-tests)
add_test(NAME katana-aica-register-tests COMMAND katana-aica-register-tests)

add_executable(katana-aica-sample-tests tests/runtime/aica_sample_tests.cpp)
target_link_libraries(katana-aica-sample-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-aica-sample-tests)
add_test(NAME katana-aica-sample-tests COMMAND katana-aica-sample-tests)

add_executable(katana-aica-mixer-tests tests/runtime/aica_mixer_tests.cpp)
target_link_libraries(katana-aica-mixer-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-aica-mixer-tests)
add_test(NAME katana-aica-mixer-tests COMMAND katana-aica-mixer-tests)

add_executable(katana-aica-execution-tests tests/runtime/aica_execution_tests.cpp)
target_link_libraries(katana-aica-execution-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-aica-execution-tests)
add_test(NAME katana-aica-execution-tests COMMAND katana-aica-execution-tests)

add_executable(katana-disc-source-tests tests/runtime/disc_source_tests.cpp)
target_link_libraries(katana-disc-source-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-disc-source-tests)
add_test(NAME katana-disc-source-tests COMMAND katana-disc-source-tests)

add_executable(katana-gdrom-command-tests tests/runtime/gdrom_command_tests.cpp)
target_link_libraries(katana-gdrom-command-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-gdrom-command-tests)
add_test(NAME katana-gdrom-command-tests COMMAND katana-gdrom-command-tests)

add_executable(katana-iso9660-tests tests/runtime/iso9660_tests.cpp)
target_link_libraries(katana-iso9660-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-iso9660-tests)
add_test(NAME katana-iso9660-tests COMMAND katana-iso9660-tests)

add_executable(katana-gdrom-async-tests tests/runtime/gdrom_async_tests.cpp)
target_link_libraries(katana-gdrom-async-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-gdrom-async-tests)
add_test(NAME katana-gdrom-async-tests COMMAND katana-gdrom-async-tests)

add_executable(katana-gdi-parser-tests tests/runtime/gdi_parser_tests.cpp)
target_link_libraries(katana-gdi-parser-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-gdi-parser-tests)
add_test(NAME katana-gdi-parser-tests COMMAND katana-gdi-parser-tests)

add_executable(katana-gdi-integration-tests tests/runtime/gdi_integration_tests.cpp)
target_link_libraries(katana-gdi-integration-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-gdi-integration-tests)
add_test(NAME katana-gdi-integration-tests COMMAND katana-gdi-integration-tests)

add_executable(katana-packed-disc-tests tests/runtime/packed_disc_tests.cpp)
target_link_libraries(katana-packed-disc-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-packed-disc-tests)
add_test(NAME katana-packed-disc-tests COMMAND katana-packed-disc-tests)

add_executable(katana-scheduler-tests tests/runtime/scheduler_tests.cpp)
target_link_libraries(katana-scheduler-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-scheduler-tests)
add_test(NAME katana-scheduler-tests COMMAND katana-scheduler-tests)

add_executable(katana-dynamic-interpreter-tests tests/runtime/dynamic_interpreter_tests.cpp)
target_link_libraries(katana-dynamic-interpreter-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-dynamic-interpreter-tests)
add_test(NAME katana-dynamic-interpreter-tests COMMAND katana-dynamic-interpreter-tests)

add_executable(katana-tmu-rtc-tests tests/runtime/tmu_rtc_tests.cpp)
target_link_libraries(katana-tmu-rtc-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-tmu-rtc-tests)
add_test(NAME katana-tmu-rtc-tests COMMAND katana-tmu-rtc-tests)

add_executable(katana-dma-tests tests/runtime/dma_tests.cpp)
target_link_libraries(katana-dma-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-dma-tests)
add_test(NAME katana-dma-tests COMMAND katana-dma-tests)

add_executable(katana-platform-interrupt-tests tests/runtime/platform_interrupt_tests.cpp)
target_link_libraries(katana-platform-interrupt-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-platform-interrupt-tests)
add_test(NAME katana-platform-interrupt-tests COMMAND katana-platform-interrupt-tests)

add_executable(katana-scif-tests tests/runtime/scif_tests.cpp)
target_link_libraries(katana-scif-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-scif-tests)
add_test(NAME katana-scif-tests COMMAND katana-scif-tests)

add_executable(katana-system-asic-tests tests/runtime/system_asic_tests.cpp)
target_link_libraries(katana-system-asic-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-system-asic-tests)
add_test(NAME katana-system-asic-tests COMMAND katana-system-asic-tests)

add_executable(katana-maple-mmio-tests tests/runtime/maple_mmio_tests.cpp)
target_link_libraries(katana-maple-mmio-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-maple-mmio-tests)
add_test(NAME katana-maple-mmio-tests COMMAND katana-maple-mmio-tests)

add_executable(katana-holly-dma-tests tests/runtime/holly_dma_tests.cpp)
target_link_libraries(katana-holly-dma-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-holly-dma-tests)
add_test(NAME katana-holly-dma-tests COMMAND katana-holly-dma-tests)

add_executable(katana-gdrom-controller-tests tests/runtime/gdrom_controller_tests.cpp)
target_link_libraries(katana-gdrom-controller-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-gdrom-controller-tests)
add_test(NAME katana-gdrom-controller-tests COMMAND katana-gdrom-controller-tests)

add_executable(katana-phase11-retail-boot-services-tests
    tests/phase11/retail_boot_services_tests.cpp
)
target_link_libraries(katana-phase11-retail-boot-services-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-phase11-retail-boot-services-tests)
add_test(
    NAME katana-phase11-retail-boot-services-tests
    COMMAND katana-phase11-retail-boot-services-tests
)

add_executable(katana-media-clock-tests tests/runtime/media_clock_tests.cpp)
target_link_libraries(katana-media-clock-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-media-clock-tests)
add_test(NAME katana-media-clock-tests COMMAND katana-media-clock-tests)

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

add_executable(katana-alpha-firmware-profile-tests
    tests/platform/alpha_firmware_profile_tests.cpp
)
target_link_libraries(katana-alpha-firmware-profile-tests PRIVATE katana_core)
katana_enable_warnings(katana-alpha-firmware-profile-tests)
add_test(
    NAME katana-alpha-firmware-profile-tests
    COMMAND katana-alpha-firmware-profile-tests
)

add_executable(katana-tooling-contract-tests tests/io/tooling_contract_tests.cpp)
target_link_libraries(katana-tooling-contract-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-tooling-contract-tests)
add_test(NAME katana-tooling-contract-tests COMMAND katana-tooling-contract-tests)

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

add_executable(katana-executable-inventory-tests
    tests/analysis/executable_inventory_tests.cpp
)
target_link_libraries(katana-executable-inventory-tests PRIVATE katana_core)
katana_enable_warnings(katana-executable-inventory-tests)
add_test(NAME katana-executable-inventory-tests COMMAND katana-executable-inventory-tests)

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

add_executable(katana-hardware-audit-tests
    tests/analysis/hardware_audit_tests.cpp
)

target_link_libraries(katana-hardware-audit-tests PRIVATE katana_core)
katana_enable_warnings(katana-hardware-audit-tests)

add_test(
    NAME katana-hardware-audit-tests
    COMMAND katana-hardware-audit-tests
)

add_executable(katana-hardware-audit-policy-tests
    tests/cli/hardware_audit_policy_tests.cpp
)

target_link_libraries(katana-hardware-audit-policy-tests PRIVATE katana_core)
katana_enable_warnings(katana-hardware-audit-policy-tests)

add_test(
    NAME katana-hardware-audit-policy-tests
    COMMAND katana-hardware-audit-policy-tests
)

add_executable(katana-function-value-analysis-tests
    tests/analysis/function_value_analysis_tests.cpp
)

target_link_libraries(katana-function-value-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-function-value-analysis-tests)

add_test(
    NAME katana-function-value-analysis-tests
    COMMAND katana-function-value-analysis-tests
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

add_executable(katana-runtime-code-copy-analysis-tests
    tests/analysis/runtime_code_copy_analysis_tests.cpp
)

target_link_libraries(katana-runtime-code-copy-analysis-tests PRIVATE katana_core)
katana_enable_warnings(katana-runtime-code-copy-analysis-tests)

add_test(
    NAME katana-runtime-code-copy-analysis-tests
    COMMAND katana-runtime-code-copy-analysis-tests
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

add_executable(katana-graph-export-tests tests/analysis/graph_export_tests.cpp)
target_link_libraries(katana-graph-export-tests PRIVATE katana_core)
katana_enable_warnings(katana-graph-export-tests)
add_test(NAME katana-graph-export-tests COMMAND katana-graph-export-tests)

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
        -DKATANA_RUNTIME=$<TARGET_FILE:katana_runtime>
        -DKATANA_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DKATANA_ENABLE_SANITIZERS=${KATANA_ENABLE_SANITIZERS}
        -DKATANA_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
        -DKATANA_GENERATED_INCLUDE=${CMAKE_CURRENT_BINARY_DIR}/generated/include
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_optimization_cli.cmake
)

add_executable(katana-cpp-emitter-tests
    tests/codegen/cpp_emitter_tests.cpp
)

add_executable(katana-backend-tests
    tests/codegen/backend_tests.cpp
)
target_link_libraries(katana-backend-tests PRIVATE katana_core)
katana_enable_warnings(katana-backend-tests)
add_test(NAME katana-backend-tests COMMAND katana-backend-tests)

add_executable(katana-cpp-backend-tests
    tests/codegen/cpp_backend_tests.cpp
)
target_link_libraries(katana-cpp-backend-tests PRIVATE katana_core)
katana_enable_warnings(katana-cpp-backend-tests)
add_test(NAME katana-cpp-backend-tests COMMAND katana-cpp-backend-tests)

add_executable(katana-backend-capability-tests
    tests/codegen/backend_capability_tests.cpp
)
target_link_libraries(katana-backend-capability-tests PRIVATE katana_core)
katana_enable_warnings(katana-backend-capability-tests)
add_test(NAME katana-backend-capability-tests COMMAND katana-backend-capability-tests)

add_executable(katana-codegen-partition-tests tests/codegen/partition_tests.cpp)
target_link_libraries(katana-codegen-partition-tests PRIVATE katana_core)
katana_enable_warnings(katana-codegen-partition-tests)
add_test(NAME katana-codegen-partition-tests COMMAND katana-codegen-partition-tests)

add_executable(katana-codegen-naming-tests tests/codegen/naming_tests.cpp)
target_link_libraries(katana-codegen-naming-tests PRIVATE katana_core)
katana_enable_warnings(katana-codegen-naming-tests)
add_test(NAME katana-codegen-naming-tests COMMAND katana-codegen-naming-tests)

add_executable(katana-codegen-cache-tests tests/codegen/cache_tests.cpp)
target_link_libraries(katana-codegen-cache-tests PRIVATE katana_core Threads::Threads)
katana_enable_warnings(katana-codegen-cache-tests)
add_test(NAME katana-codegen-cache-tests COMMAND katana-codegen-cache-tests)

add_executable(katana-codegen-project-tests tests/codegen/project_tests.cpp)
target_link_libraries(katana-codegen-project-tests PRIVATE katana_core)
target_compile_definitions(katana-codegen-project-tests PRIVATE
    KATANA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    KATANA_NINJA_EXECUTABLE="${CMAKE_MAKE_PROGRAM}"
)
katana_enable_warnings(katana-codegen-project-tests)
add_test(NAME katana-codegen-project-tests COMMAND katana-codegen-project-tests)

add_executable(katana-port-export-tests tests/codegen/port_export_tests.cpp)
target_link_libraries(katana-port-export-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-port-export-tests)
add_test(NAME katana-port-export-tests COMMAND katana-port-export-tests)
add_test(
    NAME katana-port-cli-tests
    COMMAND ${CMAKE_COMMAND}
        -DKATANA_FIXTURE_WRITER=$<TARGET_FILE:katana-port-export-tests>
        -DKATANA_CLI=$<TARGET_FILE:katana-recomp>
        -DKATANA_RUNTIME_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen/verify_port_cli.cmake
)
set_tests_properties(
    katana-port-cli-tests
    PROPERTIES TIMEOUT 300 RESOURCE_LOCK katana-external-port-build
)

add_executable(katana-codegen-metadata-tests tests/codegen/metadata_tests.cpp)
target_link_libraries(katana-codegen-metadata-tests PRIVATE katana_core katana_runtime)
katana_enable_warnings(katana-codegen-metadata-tests)
add_test(NAME katana-codegen-metadata-tests COMMAND katana-codegen-metadata-tests)

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

add_executable(katana-single-block-fallthrough-program-writer
    tests/codegen/single_block_fallthrough_program_writer.cpp
)
target_link_libraries(katana-single-block-fallthrough-program-writer PRIVATE katana_core)
katana_enable_warnings(katana-single-block-fallthrough-program-writer)

set(
    KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_single_block_fallthrough_program.cpp
)
add_custom_command(
    OUTPUT ${KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KATANA_GENERATED_TEST_DIR}
    COMMAND $<TARGET_FILE:katana-single-block-fallthrough-program-writer>
            ${KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP}
    DEPENDS
        katana-single-block-fallthrough-program-writer
        tests/codegen/single_block_fallthrough_program_writer.cpp
    COMMENT "Generiere externen Single-Block-Fallthrough-Test"
    VERBATIM
)
set_source_files_properties(
    ${KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)
set_source_files_properties(
    tests/codegen/generated_single_block_fallthrough_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP}
)
add_executable(katana-generated-single-block-fallthrough-tests
    tests/codegen/generated_single_block_fallthrough_tests.cpp
    ${KATANA_GENERATED_SINGLE_BLOCK_FALLTHROUGH_CPP}
)
target_include_directories(katana-generated-single-block-fallthrough-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)
target_link_libraries(katana-generated-single-block-fallthrough-tests PRIVATE katana_runtime)
katana_enable_warnings(katana-generated-single-block-fallthrough-tests)
add_dependencies(
    katana-generated-single-block-fallthrough-tests
    katana-single-block-fallthrough-program-writer
)
add_test(
    NAME katana-generated-single-block-fallthrough-tests
    COMMAND katana-generated-single-block-fallthrough-tests
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

add_executable(katana-alpha-integer-program-writer
    tests/alu/alpha_integer_program_writer.cpp
)
target_link_libraries(katana-alpha-integer-program-writer PRIVATE katana_core)
katana_enable_warnings(katana-alpha-integer-program-writer)

set(KATANA_GENERATED_ALPHA_INTEGER_CPP
    ${KATANA_GENERATED_TEST_DIR}/generated_alpha_integer_program.cpp
)
add_custom_command(
    OUTPUT ${KATANA_GENERATED_ALPHA_INTEGER_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${KATANA_GENERATED_TEST_DIR}
    COMMAND $<TARGET_FILE:katana-alpha-integer-program-writer>
            ${KATANA_GENERATED_ALPHA_INTEGER_CPP}
    DEPENDS katana-alpha-integer-program-writer tests/alu/alpha_integer_program_writer.cpp
    COMMENT "Generiere C++-Programm fuer KR-4502 Alpha-Integer-Test"
    VERBATIM
)
set_source_files_properties(
    ${KATANA_GENERATED_ALPHA_INTEGER_CPP}
    PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE
)
set_source_files_properties(
    tests/alu/generated_alpha_integer_tests.cpp
    PROPERTIES OBJECT_DEPENDS ${KATANA_GENERATED_ALPHA_INTEGER_CPP}
)
add_executable(katana-generated-alpha-integer-tests
    tests/alu/generated_alpha_integer_tests.cpp
    ${KATANA_GENERATED_ALPHA_INTEGER_CPP}
)
target_include_directories(katana-generated-alpha-integer-tests
    PRIVATE ${KATANA_GENERATED_TEST_DIR}
)
katana_enable_warnings(katana-generated-alpha-integer-tests)
add_dependencies(katana-generated-alpha-integer-tests katana-alpha-integer-program-writer)
add_test(NAME katana-generated-alpha-integer-tests COMMAND katana-generated-alpha-integer-tests)
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

# Stable subsystem labels keep local and CI shards independent of executable order.
get_property(KATANA_TESTS DIRECTORY PROPERTY TESTS)
foreach(KATANA_TEST IN LISTS KATANA_TESTS)
    set(KATANA_TEST_LABEL "core")
    if(KATANA_TEST MATCHES "(codegen|generated|backend|cpp-emitter|port-|source-map)")
        set(KATANA_TEST_LABEL "codegen")
    elseif(KATANA_TEST MATCHES "(analysis|control-flow|basic-block|function-|value-|jump-table|graph-export|disassembler|decoder|instruction|isa-|ir-|symbol-|executable-image|raw-binary|elf32|project-manifest)")
        set(KATANA_TEST_LABEL "analysis")
    elseif(KATANA_TEST MATCHES "(runtime|memory|cpu-|reset|exception|interrupt|fpu|flash|dreamcast|firmware|maple|pvr|host-|aica|disc|gdrom|gdi|iso9660|scheduler|tmu|dma|system-|media-clock|cache-control|store-queue|block-|bios-|platform-)")
        set(KATANA_TEST_LABEL "runtime")
    elseif(KATANA_TEST MATCHES "(phase9|performance|homebrew)")
        set(KATANA_TEST_LABEL "performance")
    endif()
    set_tests_properties(${KATANA_TEST} PROPERTIES LABELS "gate;${KATANA_TEST_LABEL}")
endforeach()

add_test(
    NAME katana-package-contract-tests
    COMMAND
        ${CMAKE_COMMAND}
        -DKATANA_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DKATANA_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DKATANA_GENERATOR=${CMAKE_GENERATOR}
        -DKATANA_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
        -DKATANA_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DKATANA_CONFIG=$<CONFIG>
        -DKATANA_EXPECTED_VERSION=${PROJECT_VERSION}
        -DKATANA_EXPECTED_RUNTIME_ABI=${KATANA_RUNTIME_ABI_VERSION}
        -P ${CMAKE_SOURCE_DIR}/tests/build/package_contract_tests.cmake
)
set_tests_properties(katana-package-contract-tests PROPERTIES LABELS "build;gate" TIMEOUT 180)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/KatanaRecompConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/KatanaRecompConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KatanaRecomp
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/KatanaRecompConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(
    TARGETS katana_runtime
    EXPORT KatanaRuntimeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime-sdk
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime-sdk
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime-sdk
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    TARGETS katana_core
    EXPORT KatanaAnalyzerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT analyzer-sdk
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT analyzer-sdk
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT analyzer-sdk
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    TARGETS katana-recomp
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT tools
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/katana/runtime
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/katana
    COMPONENT runtime-sdk
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/build_contract.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/katana
    COMPONENT runtime-sdk
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/katana/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/katana
    COMPONENT analyzer-sdk
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.in" EXCLUDE
)
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/include/katana/build_contract.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/katana
    COMPONENT analyzer-sdk
)
install(
    EXPORT KatanaRuntimeTargets
    FILE KatanaRuntimeTargets.cmake
    NAMESPACE KatanaRecomp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KatanaRecomp
    COMPONENT runtime-sdk
)
install(
    EXPORT KatanaAnalyzerTargets
    FILE KatanaAnalyzerTargets.cmake
    NAMESPACE KatanaRecomp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KatanaRecomp
    COMPONENT analyzer-sdk
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/KatanaRecompConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/KatanaRecompConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/KatanaRecomp
    COMPONENT runtime-sdk
)
