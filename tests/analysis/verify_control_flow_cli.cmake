if(NOT DEFINED WRITER OR NOT DEFINED CLI OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "WRITER, CLI und OUTPUT_DIR sind erforderlich.")
endif()

execute_process(
    COMMAND "${WRITER}" "${OUTPUT_DIR}"
    RESULT_VARIABLE writer_result
    OUTPUT_VARIABLE writer_output
    ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "Fixture-Writer fehlgeschlagen: ${writer_output}${writer_error}")
endif()

execute_process(
    COMMAND "${CLI}" analyze
        "${OUTPUT_DIR}/control-flow.katana"
        "${OUTPUT_DIR}/control-flow.overrides"
    RESULT_VARIABLE cli_result
    OUTPUT_VARIABLE cli_output
    ERROR_VARIABLE cli_error
)
if(NOT cli_result EQUAL 0)
    message(FATAL_ERROR "Analyse-CLI fehlgeschlagen: ${cli_output}${cli_error}")
endif()

set(expected_fragments
    "Funktion 0x0000000C Konfidenz=high Herkunft=indirect-call"
    "jump 0x0000000C -> 0x00000012 [user-override]"
    "jump-table-jump 0x00000012 -> 0x00000018 [bounded-absolute-target]"
    "jump-table-jump 0x00000012 -> 0x0000001C [bounded-absolute-target]"
    "Bereich 0x00000018"
)
foreach(fragment IN LISTS expected_fragments)
    string(FIND "${cli_output}" "${fragment}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "CLI-Ausgabe enthaelt nicht '${fragment}':\n${cli_output}")
    endif()
endforeach()
