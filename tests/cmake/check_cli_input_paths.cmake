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

if(NOT raw_ir STREQUAL elf_ir)
    message(FATAL_ERROR "Raw und ELF32-SH erreichen nicht dieselbe IR.")
endif()
string(FIND "${manifest_ir}" "${raw_ir}" manifest_ir_prefix)
if(NOT manifest_ir_prefix EQUAL 0 OR NOT manifest_ir MATCHES
    "execution-profile-v1.*fallback=abort.*mmu=disabled.*fastpath=conservative")
    message(FATAL_ERROR
        "Manifest erreicht nicht dieselbe IR mit erhaltenem Standardprofil.")
endif()

run_ir("${OUTPUT_DIR}/program-profile.katana" 8C010000 0 profiled_ir)
if(NOT profiled_ir MATCHES
    "execution-profile-v1.*fallback=diagnostic.*mmu=disabled.*fastpath=guarded")
    message(FATAL_ERROR "Manifest-Ausfuehrungsprofil erreicht den IR-Bericht nicht.")
endif()
if(profiled_ir STREQUAL manifest_ir)
    message(FATAL_ERROR "Unterschiedliche Ausfuehrungsprofile bleiben oeffentlich unsichtbar.")
endif()

foreach(report_command IN ITEMS analyze-json cfg-json callgraph-json)
    execute_process(
        COMMAND "${KATANA_RECOMP}" "${report_command}"
            "${OUTPUT_DIR}/program-profile.katana"
        RESULT_VARIABLE report_result
        OUTPUT_VARIABLE report_output
        ERROR_VARIABLE report_error
    )
    if(NOT report_result EQUAL 0 OR NOT report_output MATCHES
        "\"execution_profile\".*\"fallback\":\"diagnostic\".*\"fastpath\":\"guarded\"")
        message(FATAL_ERROR
            "${report_command} verwirft das Manifest-Ausfuehrungsprofil: "
            "${report_result}: ${report_error}")
    endif()
endforeach()

execute_process(
    COMMAND "${KATANA_RECOMP}" ir-json "${OUTPUT_DIR}/program-profile.katana"
        8C010000 0
    RESULT_VARIABLE ir_json_result
    OUTPUT_VARIABLE ir_json_output
    ERROR_VARIABLE ir_json_error
)
if(NOT ir_json_result EQUAL 0 OR NOT ir_json_output MATCHES
    "\"execution_profile\".*\"fallback\":\"diagnostic\".*\"fastpath\":\"guarded\"")
    message(FATAL_ERROR
        "ir-json verwirft das Manifest-Ausfuehrungsprofil: "
        "${ir_json_result}: ${ir_json_error}")
endif()

execute_process(
    COMMAND "${KATANA_RECOMP}" emit-cpp "${OUTPUT_DIR}/program-profile.katana"
        8C010000 "${OUTPUT_DIR}/profile.cpp" 0
    RESULT_VARIABLE profile_result
    ERROR_VARIABLE profile_error
)
if(NOT profile_result EQUAL 6 OR NOT profile_error MATCHES "fallback-profile")
    message(FATAL_ERROR
        "Nicht angewendetes Fallbackprofil erzeugt keinen Capability-Fehler: "
        "${profile_result}: ${profile_error}")
endif()

execute_process(
    COMMAND "${KATANA_RECOMP}" emit-cpp "${OUTPUT_DIR}/program-mmu.katana"
        8C010000 "${OUTPUT_DIR}/mmu.cpp" 0
    RESULT_VARIABLE mmu_result
    ERROR_VARIABLE mmu_error
)
if(NOT mmu_result EQUAL 6 OR NOT mmu_error MATCHES "mmu")
    message(FATAL_ERROR
        "Nicht unterstuetztes MMU-Profil erzeugt keinen Capability-Fehler: "
        "${mmu_result}: ${mmu_error}")
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
