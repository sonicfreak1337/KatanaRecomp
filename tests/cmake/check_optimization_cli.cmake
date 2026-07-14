if(NOT DEFINED KATANA_RECOMP OR NOT DEFINED FIXTURE OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "KatanaRecomp, Fixture oder Ausgabeverzeichnis fehlt.")
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
