if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI OR
   NOT DEFINED KATANA_RUNTIME_ROOT)
  message(FATAL_ERROR
    "Composite-Callback-Fill-Differentialtest braucht Fixture-Writer, CLI und Runtime-SDK")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR
    "Composite-Callback-Fill-Differentialtest braucht ein temporaeres Verzeichnis")
endif()

set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")
file(TO_CMAKE_PATH "$ENV{TEMP}/katana-composite-callback-fill-differential" fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")
set(ENV{KATANA_USER_DATA_ROOT} "${fixture}/user-data")

execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-composite-callback-fixture
          "${fixture}/disc"
  RESULT_VARIABLE writer_result
  OUTPUT_VARIABLE writer_output
  ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
  message(FATAL_ERROR
    "Synthetische Composite-Callback-Fixture fehlgeschlagen: "
    "${writer_output} ${writer_error}")
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name composite_callback_game
  RESULT_VARIABLE port_result
  OUTPUT_VARIABLE port_output
  ERROR_VARIABLE port_error
)
if(NOT port_result EQUAL 0)
  message(FATAL_ERROR
    "Composite-Callback-Portbuild fehlgeschlagen (${port_result}):\n"
    "${port_output}\n${port_error}")
endif()

string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       port_build_match "${port_output}")
if(NOT port_build_match)
  message(FATAL_ERROR
    "Composite-Callback-Port meldet keinen stabilen Hostbuild-Cache: ${port_output}")
endif()
set(port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${port_build}" port_build)
get_filename_component(port_workspace "${port_build}" DIRECTORY)

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${port_workspace}" -B "${port_build}"
          "-DKATANA_RUNTIME_ROOT=${KATANA_RUNTIME_ROOT}"
          -DKATANA_INTERNAL_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST=ON
          -DKATANA_INTERNAL_BATCH_COMMIT_LIFECYCLE_TEST=ON
          -DCMAKE_BUILD_TYPE=RelWithDebInfo
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "Composite-Callback-Testhooks konnten nicht konfiguriert werden: "
    "${configure_output} ${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${port_build}" --target composite_callback_game
          --config RelWithDebInfo --parallel 2
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "Composite-Callback-Testport konnte nicht gebaut werden: "
    "${build_output} ${build_error}")
endif()

if(WIN32)
  set(built_game "${port_build}/composite_callback_game.exe")
  set(game "${fixture}/port/composite_callback_game.exe")
  if(NOT EXISTS "${built_game}")
    set(built_game "${port_build}/RelWithDebInfo/composite_callback_game.exe")
  endif()
else()
  set(built_game "${port_build}/composite_callback_game")
  set(game "${fixture}/port/composite_callback_game")
endif()
if(NOT EXISTS "${built_game}" OR NOT EXISTS "${game}")
  message(FATAL_ERROR
    "Composite-Callback-Testport besitzt kein ausfuehrbares Hosttarget")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${built_game}" "${game}"
  RESULT_VARIABLE copy_result
  ERROR_VARIABLE copy_error
)
if(NOT copy_result EQUAL 0)
  message(FATAL_ERROR
    "Composite-Callback-Testbinary konnte nicht publiziert werden: ${copy_error}")
endif()

execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0 OR NOT install_output MATCHES "KATANA_DISC_INSTALL_OK")
  message(FATAL_ERROR
    "Composite-Callback-Testdisc konnte nicht installiert werden: "
    "${install_output} ${install_error}")
endif()

function(run_composite_callback_case
         label active_mmu scalar state_output trace_output)
  set(ENV{KATANA_PORT_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST} "1")
  set(ENV{KATANA_PORT_COMPOSITE_CALLBACK_TRACE} "1")
  unset(ENV{KATANA_PORT_BLOCK_LIMIT})
  if(active_mmu)
    set(ENV{KATANA_PORT_TEST_ACTIVE_MMU} "1")
  else()
    unset(ENV{KATANA_PORT_TEST_ACTIVE_MMU})
  endif()
  if(scalar)
    set(ENV{KATANA_PORT_TEST_DISABLE_COMPOSITE_CALLBACK} "1")
  else()
    unset(ENV{KATANA_PORT_TEST_DISABLE_COMPOSITE_CALLBACK})
  endif()
  execute_process(
    COMMAND "${game}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
  )
  unset(ENV{KATANA_PORT_COMPOSITE_CALLBACK_DIFFERENTIAL_TEST})
  unset(ENV{KATANA_PORT_COMPOSITE_CALLBACK_TRACE})
  unset(ENV{KATANA_PORT_TEST_ACTIVE_MMU})
  unset(ENV{KATANA_PORT_TEST_DISABLE_COMPOSITE_CALLBACK})
  set(combined "${run_output}\n${run_error}")
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
      "Composite-Callback-Fall ${label} ist fehlgeschlagen (${run_result}): "
      "${combined}")
  endif()
  string(REGEX MATCH "KATANA_COMPOSITE_CALLBACK_STATE [^\r\n]+"
         state_line "${combined}")
  if(NOT state_line)
    message(FATAL_ERROR
      "Composite-Callback-Fall ${label} lieferte keinen Architekturzustand: "
      "${combined}")
  endif()
  set(${state_output} "${state_line}" PARENT_SCOPE)
  set(${trace_output} "${combined}" PARENT_SCOPE)
endfunction()

function(split_composite_callback_state
         state architecture_output indirect_output runtime_hits_output
         runtime_misses_output runtime_fallbacks_output runtime_only_hits_output
         runtime_only_misses_output runtime_only_fallbacks_output)
  if(NOT state MATCHES " code_provenance=[0-9]+" OR
     NOT state MATCHES " module_provenance=[0-9]+")
    message(FATAL_ERROR
      "Composite-State bindet CodeTracker/Eventhistorie oder ModuleCatalog nicht: ${state}")
  endif()
  string(REGEX MATCH " indirect_dispatches=([0-9]+)" indirect_match "${state}")
  if(NOT indirect_match)
    message(FATAL_ERROR "Composite-State besitzt keine Indirect-PERF-Metrik: ${state}")
  endif()
  set(indirect "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_dispatch_hits=([0-9]+)" runtime_hits_match "${state}")
  if(NOT runtime_hits_match)
    message(FATAL_ERROR "Composite-State besitzt keine Runtime-Hit-Metrik: ${state}")
  endif()
  set(runtime_hits "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_dispatch_misses=([0-9]+)" runtime_misses_match "${state}")
  if(NOT runtime_misses_match)
    message(FATAL_ERROR "Composite-State besitzt keine Runtime-Miss-Metrik: ${state}")
  endif()
  set(runtime_misses "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_dispatch_fallbacks=([0-9]+)"
       runtime_fallbacks_match "${state}")
  if(NOT runtime_fallbacks_match)
    message(FATAL_ERROR "Composite-State besitzt keine Runtime-Fallback-Metrik: ${state}")
  endif()
  set(runtime_fallbacks "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_only_dispatch_hits=([0-9]+)"
       runtime_only_hits_match "${state}")
  if(NOT runtime_only_hits_match)
    message(FATAL_ERROR "Composite-State besitzt keine RuntimeOnly-Hit-Metrik: ${state}")
  endif()
  set(runtime_only_hits "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_only_dispatch_misses=([0-9]+)"
       runtime_only_misses_match "${state}")
  if(NOT runtime_only_misses_match)
    message(FATAL_ERROR "Composite-State besitzt keine RuntimeOnly-Miss-Metrik: ${state}")
  endif()
  set(runtime_only_misses "${CMAKE_MATCH_1}")
  string(REGEX MATCH " runtime_only_dispatch_fallbacks=([0-9]+)"
       runtime_only_fallbacks_match "${state}")
  if(NOT runtime_only_fallbacks_match)
    message(FATAL_ERROR "Composite-State besitzt keine RuntimeOnly-Fallback-Metrik: ${state}")
  endif()
  set(runtime_only_fallbacks "${CMAKE_MATCH_1}")

  string(REGEX REPLACE
    " indirect_dispatches=[0-9]+ runtime_dispatch_hits=[0-9]+ runtime_dispatch_misses=[0-9]+ runtime_dispatch_fallbacks=[0-9]+ runtime_only_dispatch_hits=[0-9]+ runtime_only_dispatch_misses=[0-9]+ runtime_only_dispatch_fallbacks=[0-9]+"
    "" architecture "${state}")
  set(${architecture_output} "${architecture}" PARENT_SCOPE)
  set(${indirect_output} "${indirect}" PARENT_SCOPE)
  set(${runtime_hits_output} "${runtime_hits}" PARENT_SCOPE)
  set(${runtime_misses_output} "${runtime_misses}" PARENT_SCOPE)
  set(${runtime_fallbacks_output} "${runtime_fallbacks}" PARENT_SCOPE)
  set(${runtime_only_hits_output} "${runtime_only_hits}" PARENT_SCOPE)
  set(${runtime_only_misses_output} "${runtime_only_misses}" PARENT_SCOPE)
  set(${runtime_only_fallbacks_output} "${runtime_only_fallbacks}" PARENT_SCOPE)
endfunction()

run_composite_callback_case(
  "no-mmu-batch" FALSE FALSE no_mmu_batch no_mmu_batch_trace)
run_composite_callback_case(
  "no-mmu-scalar" FALSE TRUE no_mmu_scalar no_mmu_scalar_trace)
split_composite_callback_state(
  "${no_mmu_batch}" no_mmu_batch_arch no_mmu_batch_indirect
  no_mmu_batch_runtime_hits no_mmu_batch_runtime_misses
  no_mmu_batch_runtime_fallbacks no_mmu_batch_runtime_only_hits
  no_mmu_batch_runtime_only_misses no_mmu_batch_runtime_only_fallbacks)
split_composite_callback_state(
  "${no_mmu_scalar}" no_mmu_scalar_arch no_mmu_scalar_indirect
  no_mmu_scalar_runtime_hits no_mmu_scalar_runtime_misses
  no_mmu_scalar_runtime_fallbacks no_mmu_scalar_runtime_only_hits
  no_mmu_scalar_runtime_only_misses no_mmu_scalar_runtime_only_fallbacks)
if(NOT no_mmu_batch_arch STREQUAL no_mmu_scalar_arch OR
   NOT no_mmu_batch_arch MATCHES " mmucr=0 mode=0 ")
  message(FATAL_ERROR
    "No-MMU-Composite-Batch und nativer Skalarpfad divergieren architektonisch:\n"
    "batch:  ${no_mmu_batch_arch}\nscalar: ${no_mmu_scalar_arch}")
endif()
if(no_mmu_batch_indirect GREATER no_mmu_scalar_indirect OR
   NOT no_mmu_batch_runtime_hits LESS no_mmu_scalar_runtime_hits OR
   NOT no_mmu_batch_runtime_only_hits LESS no_mmu_scalar_runtime_only_hits OR
   NOT no_mmu_batch_runtime_misses STREQUAL no_mmu_scalar_runtime_misses OR
   NOT no_mmu_batch_runtime_fallbacks STREQUAL no_mmu_scalar_runtime_fallbacks OR
   NOT no_mmu_batch_runtime_only_misses STREQUAL no_mmu_scalar_runtime_only_misses OR
   NOT no_mmu_batch_runtime_only_fallbacks STREQUAL
       no_mmu_scalar_runtime_only_fallbacks)
  message(FATAL_ERROR
    "Composite-Batch reduziert Hostdispatches nicht ohne neue Misses/Fallbacks:\n"
    "batch:  ${no_mmu_batch}\nscalar: ${no_mmu_scalar}")
endif()
string(REGEX MATCH "KATANA_COMPOSITE_CALLBACK_ADMIT iterations=([0-9]+)"
       no_mmu_admit "${no_mmu_batch_trace}")
if(NOT no_mmu_admit)
  message(FATAL_ERROR
    "No-MMU-Differentialfall hat den Composite-Callback-Batch nicht ausgefuehrt: "
    "${no_mmu_batch_trace}")
endif()
set(no_mmu_iterations "${CMAKE_MATCH_1}")
if(no_mmu_iterations LESS_EQUAL 1)
  message(FATAL_ERROR
    "Composite-Callback-Trace belegt nicht mehr als eine Batchrunde: "
    "${no_mmu_batch_trace}")
endif()

run_composite_callback_case(
  "active-mmu-batch-gate" TRUE FALSE active_mmu_gate active_mmu_trace)
run_composite_callback_case(
  "active-mmu-scalar" TRUE TRUE active_mmu_scalar active_mmu_scalar_trace)
if(NOT active_mmu_gate STREQUAL active_mmu_scalar OR
   NOT active_mmu_gate MATCHES " mode=1 ")
  message(FATAL_ERROR
    "Aktiver-MMU-Composite-Ablehnungspfad und nativer Skalarpfad divergieren:\n"
    "gate:   ${active_mmu_gate}\nscalar: ${active_mmu_scalar}")
endif()
if(NOT active_mmu_trace MATCHES
       "KATANA_COMPOSITE_CALLBACK_REJECT stage=runtime-state" OR
   active_mmu_trace MATCHES "KATANA_COMPOSITE_CALLBACK_ADMIT iterations=")
  message(FATAL_ERROR
    "Aktive MMU wurde vom Composite-Callback-Batch nicht vor Ausfuehrung abgelehnt: "
      "${active_mmu_trace}")
endif()

set(ENV{KATANA_PORT_TEST_BATCH_COMMIT_ABORT} "composite-callback")
set(ENV{KATANA_PORT_COMPOSITE_CALLBACK_TRACE} "1")
unset(ENV{KATANA_PORT_BLOCK_LIMIT})
execute_process(
  COMMAND "${game}"
  RESULT_VARIABLE lifecycle_result
  OUTPUT_VARIABLE lifecycle_output
  ERROR_VARIABLE lifecycle_error
)
unset(ENV{KATANA_PORT_TEST_BATCH_COMMIT_ABORT})
unset(ENV{KATANA_PORT_COMPOSITE_CALLBACK_TRACE})
set(lifecycle_combined "${lifecycle_output}\n${lifecycle_error}")
if(NOT lifecycle_combined MATCHES
     "KATANA_BATCH_COMMIT_ABORT_CLEAN kind=composite-callback" OR
   lifecycle_combined MATCHES "KATANA_COMPOSITE_CALLBACK_ADMIT iterations=")
  message(FATAL_ERROR
    "Composite-Callback commitet bei Lifecycle-Abbruch vor voller Gastzeitannahme: "
    "${lifecycle_combined}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS
  "Composite-Callback-Fill Differential, MMU-Gate und atomarer Lifecycle-Abbruch erfolgreich")
