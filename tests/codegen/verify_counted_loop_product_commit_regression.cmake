if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI OR
   NOT DEFINED KATANA_RUNTIME_ROOT)
  message(FATAL_ERROR
    "Counted-Loop-Produktcommit-Test braucht Fixture-Writer, CLI und Runtime-SDK")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR
    "Counted-Loop-Produktcommit-Test braucht ein temporaeres Verzeichnis")
endif()

set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")
file(TO_CMAKE_PATH
     "$ENV{TEMP}/katana-counted-loop-product-commit-regression" fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")
set(ENV{KATANA_USER_DATA_ROOT} "${fixture}/user-data")

execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}"
          --write-counted-loop-on-chip-fixture "${fixture}/disc"
  RESULT_VARIABLE writer_result
  OUTPUT_VARIABLE writer_output
  ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
  message(FATAL_ERROR
    "On-Chip-RAM-Counted-Loop-Fixture fehlgeschlagen: "
    "${writer_output} ${writer_error}")
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name counted_loop_product_commit
  RESULT_VARIABLE port_result
  OUTPUT_VARIABLE port_output
  ERROR_VARIABLE port_error
)
if(NOT port_result EQUAL 0)
  message(FATAL_ERROR
    "On-Chip-RAM-Counted-Loop-Portbuild fehlgeschlagen (${port_result}):\n"
    "${port_output}\n${port_error}")
endif()

string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       port_build_match "${port_output}")
if(NOT port_build_match)
  message(FATAL_ERROR
    "On-Chip-RAM-Port meldet keinen stabilen Hostbuild-Cache: ${port_output}")
endif()
set(port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${port_build}" port_build)
get_filename_component(port_workspace "${port_build}" DIRECTORY)

file(READ "${fixture}/port/src/main.cpp" generated_main)
if(generated_main MATCHES
     "Counted-Loop-Commit scheiterte nach Gastzeitannahme")
  message(FATAL_ERROR
    "Produktport enthaelt noch den falliblen Counted-Loop-Commit nach Zeitannahme")
endif()
string(FIND "${generated_main}" "bool try_counted_loop_batch("
       counted_method_begin)
string(FIND "${generated_main}"
       "ExecutableCodeTracker* executable_code_tracker()"
       counted_method_end)
if(counted_method_begin EQUAL -1 OR counted_method_end EQUAL -1 OR
   NOT counted_method_begin LESS counted_method_end)
  message(FATAL_ERROR
    "Produktport enthaelt keine isolierbare Counted-Loop-Implementierung")
endif()
math(EXPR counted_method_size "${counted_method_end} - ${counted_method_begin}")
string(SUBSTRING "${generated_main}" "${counted_method_begin}"
       "${counted_method_size}" counted_method)
string(FIND "${counted_method}"
       "prepare_prevalidated_repeated_u32_sequence(" prepare_position)
string(FIND "${counted_method}"
       "accept_batch_guest_cycles_before_commit(" accept_position)
string(FIND "${counted_method}"
       "commit_prepared_repeated_u32_sequence(" commit_position)
if(prepare_position EQUAL -1 OR accept_position EQUAL -1 OR
   commit_position EQUAL -1 OR
   NOT prepare_position LESS accept_position OR
   NOT accept_position LESS commit_position OR
   NOT counted_method MATCHES
       "counted_loop_batch_rejected\\(\"memory-prepare\"\\)" OR
   counted_method MATCHES
       "if \\(!cpu_\\.memory\\.commit_prepared_repeated_u32_sequence")
  message(FATAL_ERROR
    "Counted-Loop-Produktcode besitzt keine unfehlbare "
    "Prepare-accept-Commit-Reihenfolge")
endif()

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
    "On-Chip-RAM-Testhooks konnten nicht konfiguriert werden: "
    "${configure_output} ${configure_error}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${port_build}"
          --target counted_loop_product_commit
          --config RelWithDebInfo --parallel 2
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "On-Chip-RAM-Testport konnte nicht gebaut werden: "
    "${build_output} ${build_error}")
endif()

if(WIN32)
  set(built_game "${port_build}/counted_loop_product_commit.exe")
  set(game "${fixture}/port/counted_loop_product_commit.exe")
  if(NOT EXISTS "${built_game}")
    set(built_game
        "${port_build}/RelWithDebInfo/counted_loop_product_commit.exe")
  endif()
else()
  set(built_game "${port_build}/counted_loop_product_commit")
  set(game "${fixture}/port/counted_loop_product_commit")
endif()
if(NOT EXISTS "${built_game}" OR NOT EXISTS "${game}")
  message(FATAL_ERROR
    "On-Chip-RAM-Testport besitzt kein ausfuehrbares Hosttarget")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${built_game}" "${game}"
  RESULT_VARIABLE copy_result
  ERROR_VARIABLE copy_error
)
if(NOT copy_result EQUAL 0)
  message(FATAL_ERROR
    "On-Chip-RAM-Testbinary konnte nicht publiziert werden: ${copy_error}")
endif()

execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0 OR
   NOT install_output MATCHES "KATANA_DISC_INSTALL_OK")
  message(FATAL_ERROR
    "On-Chip-RAM-Testdisc konnte nicht installiert werden: "
    "${install_output} ${install_error}")
endif()

function(run_product_counter_case label scalar state_output trace_output)
  set(ENV{KATANA_PORT_COUNTED_LOOP_DIFFERENTIAL_TEST} "1")
  set(ENV{KATANA_PORT_COUNTED_LOOP_TRACE} "1")
  unset(ENV{KATANA_PORT_BLOCK_LIMIT})
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
  unset(ENV{KATANA_PORT_TEST_DISABLE_COUNTED_LOOP})
  set(combined "${run_output}\n${run_error}")
  if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
      "On-Chip-RAM-Counted-Loop-Fall ${label} ist fehlgeschlagen "
      "(${run_result}): ${combined}")
  endif()
  if(combined MATCHES "Commit scheiterte nach Gastzeitannahme")
    message(FATAL_ERROR
      "On-Chip-RAM-Fall ${label} reproduziert den post-accept Produktabbruch: "
      "${combined}")
  endif()
  string(REGEX MATCH "KATANA_COUNTED_LOOP_STATE [^\r\n]+" state_line
         "${combined}")
  if(NOT state_line)
    message(FATAL_ERROR
      "On-Chip-RAM-Fall ${label} lieferte keinen Architekturzustand: "
      "${combined}")
  endif()
  if(NOT state_line MATCHES " mmucr=0 mode=0 " OR
     NOT state_line MATCHES " r15=2097156656($| )" OR
     NOT state_line MATCHES " write_observer=1 " OR
     NOT state_line MATCHES " stable_write_observer=1 ")
    message(FATAL_ERROR
      "On-Chip-RAM-Fall ${label} lief nicht mit der synthetischen "
      "OCRAM-Aliasbasis, "
      "No-MMU und stabilem Produktobserver: ${state_line}")
  endif()
  set(${state_output} "${state_line}" PARENT_SCOPE)
  set(${trace_output} "${combined}" PARENT_SCOPE)
endfunction()

function(strip_runtime_dispatch_metrics state output)
  string(REGEX REPLACE
    " indirect_dispatches=[0-9]+ runtime_dispatch_hits=[0-9]+ runtime_dispatch_misses=[0-9]+ runtime_dispatch_fallbacks=[0-9]+ runtime_only_dispatch_hits=[0-9]+ runtime_only_dispatch_misses=[0-9]+ runtime_only_dispatch_fallbacks=[0-9]+"
    "" architecture "${state}")
  set(${output} "${architecture}" PARENT_SCOPE)
endfunction()

run_product_counter_case("prepare-fallback" FALSE batch_state batch_trace)
run_product_counter_case("forced-scalar" TRUE scalar_state scalar_trace)
strip_runtime_dispatch_metrics("${batch_state}" batch_architecture)
strip_runtime_dispatch_metrics("${scalar_state}" scalar_architecture)

if(NOT batch_architecture STREQUAL scalar_architecture)
  message(FATAL_ERROR
    "On-Chip-RAM-Prepare-Fallback divergiert vom skalaren Produktpfad:\n"
    "batch:  ${batch_architecture}\nscalar: ${scalar_architecture}")
endif()
if(NOT batch_trace MATCHES
       "KATANA_COUNTED_LOOP_REJECT stage=memory-prepare" OR
   batch_trace MATCHES "KATANA_COUNTED_LOOP_ADMIT iterations=")
  message(FATAL_ERROR
    "Nicht linear commitbares On-Chip-RAM wurde nicht vor Zeitannahme am "
    "Prepare abgelehnt oder trotzdem als Batch zugelassen: ${batch_trace}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS
  "Counted Loop faellt fuer den synthetischen OCRAM-Alias vor Gastzeitannahme "
  "skalar zurueck")
