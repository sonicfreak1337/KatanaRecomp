if(NOT DEFINED KATANA_RECOMP OR NOT DEFINED FIXTURE OR NOT DEFINED OUTPUT_DIR
    OR NOT DEFINED CXX_COMPILER OR NOT DEFINED CXX_COMPILER_ID
    OR NOT DEFINED KATANA_SOURCE_DIR)
    message(FATAL_ERROR
        "KatanaRecomp, Fixture, Compiler, Quellpfad oder Ausgabeverzeichnis fehlt."
    )
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(optimized_cpp "${OUTPUT_DIR}/optimized.cpp")
set(unoptimized_cpp "${OUTPUT_DIR}/unoptimized.cpp")
set(dump_prefix "${OUTPUT_DIR}/pipeline")

execute_process(
    COMMAND "${KATANA_RECOMP}" emit-cpp "${FIXTURE}" 8C010000
        "${optimized_cpp}" 8C010000 --dump-ir "${dump_prefix}"
    RESULT_VARIABLE optimized_result
    OUTPUT_VARIABLE optimized_output
    ERROR_VARIABLE optimized_error
)
if(NOT optimized_result EQUAL 0)
    message(FATAL_ERROR "Optimierter CLI-Lauf fehlgeschlagen: ${optimized_error}")
endif()

execute_process(
    COMMAND "${KATANA_RECOMP}" emit-cpp "${FIXTURE}" 8C010000
        "${unoptimized_cpp}" 8C010000 --no-opt
    RESULT_VARIABLE unoptimized_result
    OUTPUT_VARIABLE unoptimized_output
    ERROR_VARIABLE unoptimized_error
)
if(NOT unoptimized_result EQUAL 0)
    message(FATAL_ERROR "Unoptimierter CLI-Lauf fehlgeschlagen: ${unoptimized_error}")
endif()

foreach(required_file IN ITEMS
    "${optimized_cpp}"
    "${unoptimized_cpp}"
    "${dump_prefix}.before.ir"
    "${dump_prefix}.after.ir"
)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Erwartete CLI-Ausgabe fehlt: ${required_file}")
    endif()
endforeach()

file(READ "${dump_prefix}.before.ir" before_dump)
file(READ "${dump_prefix}.after.ir" after_dump)
if(NOT before_dump MATCHES "^katana-ir-v2" OR NOT after_dump MATCHES "^katana-ir-v2")
    message(FATAL_ERROR "Vorher-/Nachher-Dump besitzt kein stabiles IR-Schema.")
endif()
if(NOT unoptimized_output MATCHES "Optimierungen:    0")
    message(FATAL_ERROR "--no-opt meldet keine vollstaendige Deaktivierung.")
endif()

foreach(variant IN ITEMS optimized unoptimized)
    set(harness "${OUTPUT_DIR}/${variant}_harness.cpp")
    set(executable "${OUTPUT_DIR}/${variant}_harness${EXECUTABLE_SUFFIX}")
    file(WRITE "${harness}"
        "#include \"${variant}.cpp\"\n"
        "#include <iostream>\n"
        "int main() { katana_generated::CpuState cpu; katana_generated::run(cpu); "
        "std::cout << cpu.r[1] << ' ' << cpu.r[2] << ' ' << cpu.pc << ' ' << cpu.pr << '\\n'; }\n"
    )
    if(CXX_COMPILER_ID STREQUAL "MSVC")
        execute_process(
            COMMAND
                "${CXX_COMPILER}"
                /nologo
                /std:c++20
                /EHsc
                /utf-8
                "/I${KATANA_SOURCE_DIR}/include"
                "${harness}"
                "${KATANA_SOURCE_DIR}/src/runtime/memory.cpp"
                "${KATANA_SOURCE_DIR}/src/runtime/runtime.cpp"
                "/Fe${executable}"
            RESULT_VARIABLE compile_result
            OUTPUT_VARIABLE compile_output
            ERROR_VARIABLE compile_error
        )
    else()
        execute_process(
            COMMAND
                "${CXX_COMPILER}"
                -std=c++20
                "-I${KATANA_SOURCE_DIR}/include"
                "${harness}"
                "${KATANA_SOURCE_DIR}/src/runtime/memory.cpp"
                "${KATANA_SOURCE_DIR}/src/runtime/runtime.cpp"
                -o
                "${executable}"
            RESULT_VARIABLE compile_result
            OUTPUT_VARIABLE compile_output
            ERROR_VARIABLE compile_error
        )
    endif()
    if(NOT compile_result EQUAL 0)
        message(FATAL_ERROR "${variant}-Harness kompiliert nicht: ${compile_output}${compile_error}")
    endif()
    execute_process(
        COMMAND "${executable}"
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )
    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR "${variant}-Harness laeuft nicht: ${run_error}")
    endif()
    set(${variant}_semantics "${run_output}")
endforeach()

if(NOT optimized_semantics STREQUAL unoptimized_semantics)
    message(FATAL_ERROR
        "Optimierter und --no-opt-Lauf unterscheiden sich semantisch: "
        "${optimized_semantics} != ${unoptimized_semantics}"
    )
endif()
