if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI OR
   NOT DEFINED KATANA_RUNTIME_ROOT)
  message(FATAL_ERROR
    "Counted-Loop-Differentialtest braucht Fixture-Writer, CLI und Runtime-SDK")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR
    "Counted-Loop-Differentialtest braucht ein temporaeres Verzeichnis")
endif()

set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")
file(TO_CMAKE_PATH "$ENV{TEMP}/katana-counted-loop-differential" fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")
set(ENV{KATANA_USER_DATA_ROOT} "${fixture}/user-data")

execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-counted-loop-fixture "${fixture}/disc"
  RESULT_VARIABLE writer_result
  OUTPUT_VARIABLE writer_output
  ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
  message(FATAL_ERROR
    "Synthetische Counted-Loop-Fixture fehlgeschlagen: ${writer_output} ${writer_error}")
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name counted_loop_game
  RESULT_VARIABLE port_result
  OUTPUT_VARIABLE port_output
  ERROR_VARIABLE port_error
)
if(NOT port_result EQUAL 0)
  message(FATAL_ERROR
    "Counted-Loop-Portbuild fehlgeschlagen (${port_result}):\n${port_output}\n${port_error}")
endif()

string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       port_build_match "${port_output}")
if(NOT port_build_match)
  message(FATAL_ERROR
    "Counted-Loop-Port meldet keinen stabilen Hostbuild-Cache: ${port_output}")
endif()
set(port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${port_build}" port_build)
get_filename_component(port_workspace "${port_build}" DIRECTORY)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${port_workspace}" -B "${port_build}"
          "-DKATANA_RUNTIME_ROOT=${KATANA_RUNTIME_ROOT}"
          -DKATANA_INTERNAL_COUNTED_LOOP_DIFFERENTIAL_TEST=ON
          -DCMAKE_BUILD_TYPE=RelWithDebInfo
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "Counted-Loop-Testhooks konnten nicht konfiguriert werden: "
    "${configure_output} ${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${port_build}" --target counted_loop_game
          --config RelWithDebInfo --parallel 2
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "Counted-Loop-Testport konnte nicht gebaut werden: ${build_output} ${build_error}")
endif()

if(WIN32)
  set(built_game "${port_build}/counted_loop_game.exe")
  set(game "${fixture}/port/counted_loop_game.exe")
  if(NOT EXISTS "${built_game}")
    set(built_game "${port_build}/RelWithDebInfo/counted_loop_game.exe")
  endif()
else()
  set(built_game "${port_build}/counted_loop_game")
  set(game "${fixture}/port/counted_loop_game")
endif()
if(NOT EXISTS "${built_game}" OR NOT EXISTS "${game}")
  message(FATAL_ERROR "Counted-Loop-Testport besitzt kein ausfuehrbares Hosttarget")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${built_game}" "${game}"
  RESULT_VARIABLE copy_result
  ERROR_VARIABLE copy_error
)
if(NOT copy_result EQUAL 0)
  message(FATAL_ERROR "Counted-Loop-Testbinary konnte nicht publiziert werden: ${copy_error}")
endif()

execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0 OR NOT install_output MATCHES "KATANA_DISC_INSTALL_OK")
  message(FATAL_ERROR
    "Counted-Loop-Testdisc konnte nicht installiert werden: "
    "${install_output} ${install_error}")
endif()

function(run_counted_loop_case label active_mmu scalar state_output trace_output)
  set(ENV{KATANA_PORT_COUNTED_LOOP_DIFFERENTIAL_TEST} "1")
  set(ENV{KATANA_PORT_COUNTED_LOOP_TRACE} "1")
  unset(ENV{KATANA_PORT_BLOCK_LIMIT})
  if(active_mmu)
    set(ENV{KATANA_PORT_TEST_ACTIVE_MMU} "1")
  else()
    unset(ENV{KATANA_PORT_TEST_ACTIVE_MMU})
  endif()
  if(scalar)
    set(ENV{KATANA_PORT_TEST_DISABLE_COUNTED_LOOP} "1")
  else()
    unset(ENV{KATANA_PORT_TEST_DISABLE_COUNTED_LOOP})
  endif()
  execute_process(
    COMMAND "${game}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
  )
  unset(ENV{KATANA_PORT_COUNTED_LOOP_DIFFERENTIAL_TEST})
  unset(ENV{KATANA_PORT_COUNTED_LOOP_TRACE})
  unset(ENV{KATANA_PORT_TEST_ACTIVE_MMU})
  unset(ENV{KATANA_PORT_TEST_DISABLE_COUNTED_LOOP})
  set(combined "${run_output}\n${run_error}")
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
      "Counted-Loop-Fall ${label} ist fehlgeschlagen (${run_result}): ${combined}")
  endif()
  string(REGEX MATCH "KATANA_COUNTED_LOOP_STATE [^\r\n]+" state_line "${combined}")
  if(NOT state_line)
    message(FATAL_ERROR
      "Counted-Loop-Fall ${label} lieferte keinen Architekturzustand: ${combined}")
  endif()
  set(${state_output} "${state_line}" PARENT_SCOPE)
  set(${trace_output} "${combined}" PARENT_SCOPE)
endfunction()

run_counted_loop_case("no-mmu-batch" FALSE FALSE no_mmu_batch no_mmu_batch_trace)
run_counted_loop_case("no-mmu-scalar" FALSE TRUE no_mmu_scalar no_mmu_scalar_trace)
if(NOT no_mmu_batch STREQUAL no_mmu_scalar OR
   NOT no_mmu_batch MATCHES " mmucr=0 mode=0 ")
  message(FATAL_ERROR
    "No-MMU-Batch und nativer Skalarpfad divergieren:\n"
    "batch:  ${no_mmu_batch}\nscalar: ${no_mmu_scalar}")
endif()
if(NOT no_mmu_batch_trace MATCHES "KATANA_COUNTED_LOOP_ADMIT iterations=")
  message(FATAL_ERROR
    "No-MMU-Differentialfall hat den Counted-Loop-Batch nicht ausgefuehrt: "
    "${no_mmu_batch_trace}")
endif()

run_counted_loop_case("active-mmu-batch-gate" TRUE FALSE active_mmu_gate active_mmu_trace)
run_counted_loop_case("active-mmu-scalar" TRUE TRUE active_mmu_scalar active_mmu_scalar_trace)
if(NOT active_mmu_gate STREQUAL active_mmu_scalar OR
   NOT active_mmu_gate MATCHES " mmucr=1 mode=1 ")
  message(FATAL_ERROR
    "Aktiver-MMU-Ablehnungspfad und nativer Skalarpfad divergieren:\n"
    "gate:   ${active_mmu_gate}\nscalar: ${active_mmu_scalar}")
endif()
if(NOT active_mmu_trace MATCHES "KATANA_COUNTED_LOOP_REJECT stage=initial-proof" OR
   active_mmu_trace MATCHES "KATANA_COUNTED_LOOP_ADMIT iterations=")
  message(FATAL_ERROR
    "Aktive MMU wurde vom Produktbatch nicht vor Ausfuehrung abgelehnt: "
    "${active_mmu_trace}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS
  "Counted-Loop No-MMU-Differential und aktive-MMU-Ablehnung erfolgreich")
