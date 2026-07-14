if(NOT DEFINED KATANA_RECOMP OR NOT DEFINED FIXTURE_WRITER
    OR NOT DEFINED OUTPUT_DIR OR NOT DEFINED OVERRIDE_MANIFEST
    OR NOT DEFINED OVERRIDE_FILE)
    message(FATAL_ERROR "CLI-Eingangspfad-Testparameter fehlen.")
endif()

execute_process(
    COMMAND "${FIXTURE_WRITER}" "${OUTPUT_DIR}"
    RESULT_VARIABLE writer_result
)
if(NOT writer_result EQUAL 0)
    message(FATAL_ERROR "CLI-Fixtures konnten nicht erzeugt werden.")
endif()

function(run_ir input entry base output_variable)
    execute_process(
        COMMAND "${KATANA_RECOMP}" ir "${input}" "${entry}" "${base}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "IR-Eingang ${input} fehlgeschlagen: ${error}")
    endif()
    if(NOT output MATCHES "^katana-ir-v2")
        message(FATAL_ERROR "IR-Eingang ${input} liefert kein stabiles Schema.")
    endif()
    set(${output_variable} "${output}" PARENT_SCOPE)
endfunction()

run_ir("${OUTPUT_DIR}/program.bin" 8C010000 8C010000 raw_ir)
run_ir("${OUTPUT_DIR}/program.elf" 8C010000 0 elf_ir)
run_ir("${OUTPUT_DIR}/program.katana" 8C010000 0 manifest_ir)

if(NOT raw_ir STREQUAL elf_ir OR NOT raw_ir STREQUAL manifest_ir)
    message(FATAL_ERROR "Raw, ELF32-SH und Manifest erreichen nicht dieselbe IR.")
endif()

execute_process(
    COMMAND "${KATANA_RECOMP}" ir "${OVERRIDE_MANIFEST}" 0 0
        --overrides "${OVERRIDE_FILE}"
    RESULT_VARIABLE override_result
    OUTPUT_VARIABLE override_ir
    ERROR_VARIABLE override_error
)
if(NOT override_result EQUAL 0)
    message(FATAL_ERROR "Manifest-/Override-IR fehlgeschlagen: ${override_error}")
endif()
if(NOT override_ir MATCHES "resolved_targets=\\[0x00000000\\]")
    message(FATAL_ERROR "Override-Ziel wurde nicht bis in die IR transportiert.")
endif()
