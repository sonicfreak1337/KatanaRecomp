if(NOT DEFINED KATANA_RECOMP OR NOT DEFINED FIXTURE)
    message(FATAL_ERROR "KatanaRecomp oder IR-Fixture fehlt.")
endif()

function(run_ir command output_variable)
    execute_process(
        COMMAND "${KATANA_RECOMP}" "${command}" "${FIXTURE}" 8C010000 8C010000
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${command} fehlgeschlagen: ${error}")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_ir(ir text_first)
run_ir(ir text_second)
if(NOT text_first STREQUAL text_second)
    message(FATAL_ERROR "IR-Textausgabe ist nicht deterministisch.")
endif()
if(NOT text_first MATCHES "^katana-ir-v2")
    message(FATAL_ERROR "IR-Textausgabe besitzt kein stabiles Schema.")
endif()

run_ir(ir-json json_first)
run_ir(ir-json json_second)
if(NOT json_first STREQUAL json_second)
    message(FATAL_ERROR "IR-JSON-Ausgabe ist nicht deterministisch.")
endif()

string(JSON schema ERROR_VARIABLE json_error GET "${json_first}" schema)
if(json_error OR NOT schema STREQUAL "katana-ir-v2")
    message(FATAL_ERROR "IR-JSON ist ungueltig oder besitzt das falsche Schema: ${json_error}")
endif()
