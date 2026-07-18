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
    "Funktion 0x0000000C Konfidenz=high Evidenz=proven-complete Herkunft=indirect-call"
    "jump 0x0000000C [user-override; evidence=forced-override; status=guarded_partial; class=runtime-pointer; candidate=0x00000012]"
    "jump-table-jump 0x00000012 [bounded-table; evidence=forced-override]"
    "Bereich 0x00000018"
)
foreach(fragment IN LISTS expected_fragments)
    string(FIND "${cli_output}" "${fragment}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "CLI-Ausgabe enthaelt nicht '${fragment}':\n${cli_output}")
    endif()
endforeach()
