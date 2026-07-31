if(NOT DEFINED KATANA_PORT_CLI_CASE OR
   NOT KATANA_PORT_CLI_CASE MATCHES "^(core|cache|publish|runtime-edge)$")
  message(FATAL_ERROR
    "Port-CLI-Test braucht einen isolierten Fall: core, cache, publish oder runtime-edge")
endif()

if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI OR
   NOT DEFINED KATANA_RUNTIME_ROOT OR
   NOT DEFINED KATANA_RUNTIME_BUILD_TARGETS_FILE)
  message(FATAL_ERROR "Port-CLI-Test braucht Fixture-Writer, CLI und Runtime-SDK")
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
  unset(ENV{KATANA_RUNTIME_BUILD_TARGETS})
  set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")
else()
  file(TO_CMAKE_PATH
       "${KATANA_RUNTIME_BUILD_TARGETS_FILE}"
       isolated_runtime_build_targets_file)
  if(NOT EXISTS "${isolated_runtime_build_targets_file}")
    message(FATAL_ERROR
      "Isolierter Port-CLI-Fall besitzt keinen Runtime-Targetexport: "
      "${isolated_runtime_build_targets_file}")
  endif()
  unset(ENV{KATANA_RUNTIME_ROOT})
  set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
      "${isolated_runtime_build_targets_file}")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR "Port-CLI-Test braucht ein temporaeres Verzeichnis ausserhalb des Quellbaums")
endif()
file(TO_CMAKE_PATH
     "$ENV{TEMP}/katana-port-cli-${KATANA_PORT_CLI_CASE}"
     fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")
set(ENV{KATANA_USER_DATA_ROOT} "${fixture}/user-data")
set(ENV{KATANA_PORT_WORKSPACE_ROOT} "${fixture}")
# The CLI owns every Configure/Build process tree and must get the first
# timeout. CMake retains two minutes for CLI cleanup/error propagation before
# any execute_process guard can fire.
set(ENV{KATANA_PORT_HOST_COMMAND_TIMEOUT_MS} "600000")

execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-fixture "${fixture}/disc"
  RESULT_VARIABLE writer_result
  OUTPUT_VARIABLE writer_output
  ERROR_VARIABLE writer_error
  TIMEOUT 30
)
if(NOT writer_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Synthetische Portfixture fehlgeschlagen: ${writer_error}")
endif()
set(runtime_image_payload_binding
    "product-gate-runtime-image=${fixture}/disc/product-gate-runtime-image.bin")
file(READ "${fixture}/disc/latent-aot-entry.txt" latent_aot_entry_binding)
string(STRIP "${latent_aot_entry_binding}" latent_aot_entry_binding)
if(latent_aot_entry_binding STREQUAL "")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Portfixture besitzt keinen exakten Latent-AOT-Hint")
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/invalid-target-port"
          --target-name "invalid&target"
  RESULT_VARIABLE invalid_target_result
  OUTPUT_VARIABLE invalid_target_output
  ERROR_VARIABLE invalid_target_error
  TIMEOUT 30
)
if(invalid_target_result EQUAL 0 OR
   NOT invalid_target_error MATCHES
       "--target-name ist kein sicherer CMake-Targetname" OR
   EXISTS "${fixture}/invalid-target-port")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Ungueltiger Targetname erreichte Cache, Host-Shell oder Publish: "
    "${invalid_target_output} ${invalid_target_error}")
endif()

file(MAKE_DIRECTORY "${fixture}/linked-output-target")
file(CREATE_LINK
     "${fixture}/linked-output-target"
     "${fixture}/linked-output-parent"
     SYMBOLIC
     RESULT linked_output_result)
if(linked_output_result STREQUAL "0")
  execute_process(
    COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
            --output "${fixture}/linked-output-parent/port"
            --target-name linked_parent_game
    RESULT_VARIABLE linked_parent_result
    OUTPUT_VARIABLE linked_parent_output
    ERROR_VARIABLE linked_parent_error
    TIMEOUT 30
  )
  if(linked_parent_result EQUAL 0 OR
     NOT linked_parent_error MATCHES
         "enthaelt eine unsichere Elternkomponente" OR
     EXISTS "${fixture}/linked-output-target/port")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Portexport akzeptierte einen verlinkten Ausgabe-Elternpfad: "
      "${linked_parent_output} ${linked_parent_error}")
  endif()
endif()
endif()

function(find_unexpected_publish_stage_paths output_root output_variable)
  # Der aktuelle Publisher bindet Journal, Journal-Staging und Transaktionsbaum
  # direkt an den kanonischen Ausgabeort. Nach einem erfolgreichen Commit darf
  # davon kein <output>.katana-publish-transaction* Artefakt uebrig bleiben.
  file(GLOB publish_candidates
       "${output_root}.katana-publish-transaction*")
  set("${output_variable}" "${publish_candidates}" PARENT_SCOPE)
endfunction()

function(find_active_port_publish_roots output_root output_variable)
  file(GLOB transaction_candidates
       "${output_root}.katana-publish-transaction.*")
  set(transaction_roots)
  foreach(candidate IN LISTS transaction_candidates)
    if(IS_DIRECTORY "${candidate}" OR IS_SYMLINK "${candidate}")
      list(APPEND transaction_roots "${candidate}")
    endif()
  endforeach()
  set("${output_variable}" "${transaction_roots}" PARENT_SCOPE)
endfunction()

function(require_port_phase_timing_contract output context require_parallel_sample)
  string(REGEX MATCHALL
         "KATANA_PORT_PHASE_TIMINGS [^\r\n]+"
         timing_lines
         "${output}")
  list(LENGTH timing_lines timing_line_count)
  if(NOT timing_line_count EQUAL 1)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: erwartete genau ein strukturiertes Phasenzeitdokument, "
      "erhielt ${timing_line_count}: ${output}")
  endif()
  list(GET timing_lines 0 timing_line)
  string(REGEX REPLACE
         "^KATANA_PORT_PHASE_TIMINGS "
         ""
         timing_json
         "${timing_line}")

  string(JSON timing_schema ERROR_VARIABLE timing_error
         GET "${timing_json}" schema)
  if(NOT timing_error STREQUAL "NOTFOUND" OR
     NOT timing_schema STREQUAL "katana-port-phase-timings-v1")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: ungueltiges Phasenzeit-Schema: ${timing_error} ${timing_json}")
  endif()
  string(JSON timing_total ERROR_VARIABLE timing_error
         GET "${timing_json}" total_ms)
  if(NOT timing_error STREQUAL "NOTFOUND" OR timing_total LESS 0)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: ungueltige Gesamtzeit: ${timing_error} ${timing_json}")
  endif()
  string(JSON timing_phase_count ERROR_VARIABLE timing_error
         LENGTH "${timing_json}" phases)
  if(NOT timing_error STREQUAL "NOTFOUND" OR timing_phase_count LESS 1)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: Phasenliste fehlt: ${timing_error} ${timing_json}")
  endif()

  set(sequential_total 0)
  set(previous_sequential_phase "")
  set(disc_load_seen FALSE)
  set(post_disc_analysis_seen FALSE)
  set(host_build_seen FALSE)
  set(package_seen FALSE)
  set(parallel_module_seen FALSE)
  set(parallel_before_parent FALSE)
  set(latent_discovery_seen FALSE)
  math(EXPR timing_last_phase "${timing_phase_count} - 1")
  foreach(timing_index RANGE 0 ${timing_last_phase})
    string(JSON timing_phase ERROR_VARIABLE timing_error
           GET "${timing_json}" phases ${timing_index} phase)
    if(NOT timing_error STREQUAL "NOTFOUND")
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "${context}: Phasenname ist ungueltig: ${timing_error} ${timing_json}")
    endif()
    string(JSON timing_duration ERROR_VARIABLE timing_error
           GET "${timing_json}" phases ${timing_index} duration_ms)
    if(NOT timing_error STREQUAL "NOTFOUND" OR timing_duration LESS 0)
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "${context}: Phasendauer ist ungueltig: ${timing_error} ${timing_json}")
    endif()
    string(JSON timing_parallel ERROR_VARIABLE timing_error
           GET "${timing_json}" phases ${timing_index} parallel)
    if(NOT timing_error STREQUAL "NOTFOUND")
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "${context}: Parallelkennzeichen ist ungueltig: "
        "${timing_error} ${timing_json}")
    endif()

    if(timing_parallel)
      if(timing_phase MATCHES "^export:latent-aot-module-analysis-[0-9]+$")
        set(parallel_module_seen TRUE)
        if(NOT latent_discovery_seen)
          set(parallel_before_parent TRUE)
        endif()
      endif()
      continue()
    endif()

    math(EXPR sequential_total "${sequential_total} + ${timing_duration}")
    if(previous_sequential_phase STREQUAL "disc-load")
      if(NOT timing_phase STREQUAL "analysis-codegen")
        file(REMOVE_RECURSE "${fixture}")
        message(FATAL_ERROR
          "${context}: disc-load geht nicht direkt in analysis-codegen ueber: "
          "${timing_json}")
      endif()
      set(post_disc_analysis_seen TRUE)
    endif()
    if(timing_phase STREQUAL "disc-load")
      set(disc_load_seen TRUE)
    elseif(timing_phase STREQUAL "export:latent-aot-discovery")
      set(latent_discovery_seen TRUE)
    elseif(timing_phase STREQUAL "host-build")
      set(host_build_seen TRUE)
    elseif(timing_phase STREQUAL "package")
      set(package_seen TRUE)
    endif()
    set(previous_sequential_phase "${timing_phase}")
  endforeach()

  math(EXPR uncovered_time "${timing_total} - ${sequential_total}")
  if(uncovered_time LESS 0)
    math(EXPR uncovered_time "0 - ${uncovered_time}")
  endif()
  math(EXPR timing_rounding_tolerance "${timing_phase_count} + 50")
  if(NOT disc_load_seen OR NOT post_disc_analysis_seen OR
     NOT host_build_seen OR NOT package_seen OR
     parallel_before_parent OR
     uncovered_time GREATER timing_rounding_tolerance OR
     (require_parallel_sample AND NOT parallel_module_seen))
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: Phasenreihenfolge, Parallelzuordnung oder Zeitabdeckung "
      "ist unvollstaendig (uncovered=${uncovered_time}, "
      "tolerance=${timing_rounding_tolerance}): ${timing_json}")
  endif()
endfunction()

function(require_cache_tree_progress output label context terminal_marker)
  string(REGEX MATCH
         "KATANA_PROGRESS operation=program-validation state=started[^\r\n]*label=\"${label}\""
         tree_started_line
         "${output}")
  string(REGEX MATCH
         "KATANA_PROGRESS operation=program-validation state=completed[^\r\n]*label=\"${label}\""
         tree_completed_line
         "${output}")
  if(NOT tree_started_line OR NOT tree_completed_line)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: Whole-Export-Baumvalidierung besitzt keinen typisierten "
      "Start/Abschluss: ${output}")
  endif()
  string(FIND "${output}" "${tree_started_line}" tree_started_position)
  string(FIND "${output}" "${tree_completed_line}" tree_completed_position)
  if(tree_started_position GREATER tree_completed_position)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "${context}: Whole-Export-Baumvalidierung endet vor ihrem Start")
  endif()
  if(NOT terminal_marker STREQUAL "")
    string(FIND "${output}" "${terminal_marker}" terminal_marker_position)
    if(terminal_marker_position EQUAL -1 OR
       tree_completed_position GREATER terminal_marker_position)
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "${context}: Cache-Hit wurde vor Abschluss der Baumvalidierung "
        "veroeffentlicht: ${output}")
    endif()
  endif()
endfunction()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
execute_process(
  COMMAND "${KATANA_CLI}" disc-audit "${fixture}/disc/disc.gdi" --json
  RESULT_VARIABLE audit_result
  OUTPUT_VARIABLE audit_output
  ERROR_VARIABLE audit_error
  TIMEOUT 30
)
if(NOT audit_result EQUAL 0 OR
   NOT audit_output MATCHES "\"scope\":\"native_disc_aot_boot_graph\"")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Disc-Audit verliert den nativen AOT-Bootgraph-Scope: ${audit_output} ${audit_error}")
endif()
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE port_result
  OUTPUT_VARIABLE port_output
  ERROR_VARIABLE port_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(NOT port_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Port-CLI/Hostbuild fehlgeschlagen (${port_result}):\n${port_output}\n${port_error}")
endif()
require_cache_tree_progress(
  "${port_output}"
  "whole-export-cache-tree-store"
  "Kalter Portexport"
  "")
require_port_phase_timing_contract(
  "${port_output}" "Kalter Portexport" TRUE)

if(WIN32)
  set(game "${fixture}/port/cli_game.exe")
else()
  set(game "${fixture}/port/cli_game")
endif()
if(NOT EXISTS "${game}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Port-CLI hat kein ausfuehrbares Hosttarget erzeugt")
endif()

if(WIN32)
  find_program(KATANA_TEST_POWERSHELL NAMES pwsh.exe powershell.exe REQUIRED)
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
if(WIN32)
  set(gate_runner "${fixture}/port/run-product-gate.ps1")
  set(gate_child_directory "${fixture}/gate child with spaces")
  set(gate_child "${gate_child_directory}/gate child.exe")
  set(gate_budget_file "${gate_child_directory}/observed budget.txt")
  file(MAKE_DIRECTORY "${gate_child_directory}")
  configure_file("${KATANA_FIXTURE_WRITER}" "${gate_child}" COPYONLY)
  if(NOT EXISTS "${gate_runner}" OR NOT EXISTS "${gate_child}")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR "Produktgate-Fixture konnte nicht vorbereitet werden")
  endif()

  set(gate_parent_budget_was_defined FALSE)
  if(DEFINED ENV{KATANA_GUEST_CYCLE_BUDGET})
    set(gate_parent_budget_was_defined TRUE)
    set(gate_parent_budget "$ENV{KATANA_GUEST_CYCLE_BUDGET}")
  endif()
  set(ENV{KATANA_GUEST_CYCLE_BUDGET} "parent-review-budget")
  set(ENV{KATANA_PRODUCT_GATE_CHILD_BUDGET_FILE} "${gate_budget_file}")

  function(require_product_gate_child_exit expected_exit)
    file(REMOVE "${gate_budget_file}")
    set(ENV{KATANA_PRODUCT_GATE_CHILD_EXIT} "${expected_exit}")
    execute_process(
      COMMAND "${KATANA_TEST_POWERSHELL}" -NoProfile -NonInteractive
              -ExecutionPolicy Bypass -File "${gate_runner}"
              -Executable "${gate_child}" -WatchdogSeconds 10
      RESULT_VARIABLE gate_child_result
      OUTPUT_VARIABLE gate_child_output
      ERROR_VARIABLE gate_child_error
      TIMEOUT 30
    )
    if(NOT gate_child_result EQUAL expected_exit OR
       NOT EXISTS "${gate_budget_file}")
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "Produktgate propagiert Child-Exit ${expected_exit} nicht: "
        "${gate_child_output} ${gate_child_error}")
    endif()
    file(STRINGS "${gate_budget_file}" observed_gate_budget LIMIT_COUNT 1)
    if(NOT observed_gate_budget STREQUAL "600000000" OR
       NOT "$ENV{KATANA_GUEST_CYCLE_BUDGET}" STREQUAL "parent-review-budget")
      file(REMOVE_RECURSE "${fixture}")
      message(FATAL_ERROR
        "Produktgate bindet das Gastbudget oder die Elternumgebung falsch")
    endif()
  endfunction()

  foreach(gate_child_exit IN ITEMS 0 1 3)
    require_product_gate_child_exit("${gate_child_exit}")
  endforeach()

  file(REMOVE "${gate_budget_file}")
  set(ENV{KATANA_PRODUCT_GATE_CHILD_EXIT} "0")
  set(ENV{KATANA_PRODUCT_GATE_CHILD_DELAY_SECONDS} "5")
  execute_process(
    COMMAND "${KATANA_TEST_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass -File "${gate_runner}"
            -Executable "${gate_child}" -WatchdogSeconds 1
    RESULT_VARIABLE gate_watchdog_result
    OUTPUT_VARIABLE gate_watchdog_output
    ERROR_VARIABLE gate_watchdog_error
    TIMEOUT 30
  )
  if(NOT gate_watchdog_result EQUAL 124 OR
     NOT gate_watchdog_error MATCHES
         "KATANA_PRODUCT_GATE status=host-watchdog-hang" OR
     NOT "$ENV{KATANA_GUEST_CYCLE_BUDGET}" STREQUAL "parent-review-budget")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Produktgate-Watchdog liefert keinen stabilen Exit 124: "
      "${gate_watchdog_output} ${gate_watchdog_error}")
  endif()

  unset(ENV{KATANA_PRODUCT_GATE_CHILD_EXIT})
  unset(ENV{KATANA_PRODUCT_GATE_CHILD_DELAY_SECONDS})
  unset(ENV{KATANA_PRODUCT_GATE_CHILD_BUDGET_FILE})
  if(gate_parent_budget_was_defined)
    set(ENV{KATANA_GUEST_CYCLE_BUDGET} "${gate_parent_budget}")
  else()
    unset(ENV{KATANA_GUEST_CYCLE_BUDGET})
  endif()
endif()

set(product_game_command "${game}")
if(WIN32)
  set(product_game_command
      "${KATANA_TEST_POWERSHELL}" -NoProfile -NonInteractive
      -ExecutionPolicy Bypass -File "${gate_runner}"
      -Executable "${game}" -WatchdogSeconds 10)
endif()

execute_process(
  COMMAND "${game}"
  RESULT_VARIABLE missing_cache_result
  OUTPUT_VARIABLE missing_cache_output
  ERROR_VARIABLE missing_cache_error
  TIMEOUT 60
)
if(missing_cache_result EQUAL 0 OR NOT EXISTS "${fixture}/port/content/game.katana-install" OR
   EXISTS "${fixture}/port/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Distributionsport startet ohne Originaldisc-Installation oder enthaelt Retailsektoren")
endif()
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core" OR
   KATANA_PORT_CLI_CASE STREQUAL "publish" OR
   KATANA_PORT_CLI_CASE STREQUAL "runtime-edge")
execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
  TIMEOUT 60
)
if(NOT install_result EQUAL 0 OR NOT install_output MATCHES "KATANA_DISC_INSTALL_OK" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Originaldisc-Installation fehlgeschlagen: ${install_output} ${install_error}")
endif()
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
execute_process(
  COMMAND ${product_game_command}
  RESULT_VARIABLE game_result
  OUTPUT_VARIABLE game_output
  ERROR_VARIABLE game_error
  TIMEOUT 60
)
if(NOT game_result EQUAL 1 OR
   NOT game_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT game_output MATCHES "KR_GUEST_PROGRAM_ENTERED" OR
   NOT game_output MATCHES
       "status=early-exit-before-requested-budget" OR
   NOT game_output MATCHES
       "required_milestone=GameCodeProgressed required_milestone_reached=1" OR
   NOT game_output MATCHES "highest_milestone=GameCodeProgressed" OR
   NOT game_output MATCHES "requested_post_entry_cycles=600000000" OR
   NOT game_output MATCHES "first_problem=none")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Porttarget startet nicht aus dem lokal installierten Cache: ${game_output} ${game_error}")
endif()

set(runtime_jobs_was_defined FALSE)
if(DEFINED ENV{KATANA_RUNTIME_JOBS})
  set(runtime_jobs_was_defined TRUE)
  set(runtime_jobs_before_smoke "$ENV{KATANA_RUNTIME_JOBS}")
endif()
set(ENV{KATANA_RUNTIME_JOBS} "2")
execute_process(
  COMMAND "${game}" --content "${fixture}/port/user-data/content/game.katana-disc"
  RESULT_VARIABLE generated_result
  OUTPUT_VARIABLE generated_output
  ERROR_VARIABLE generated_error
  TIMEOUT 60
)
if(runtime_jobs_was_defined)
  set(ENV{KATANA_RUNTIME_JOBS} "${runtime_jobs_before_smoke}")
else()
  unset(ENV{KATANA_RUNTIME_JOBS})
endif()
# Bootstrap und drei Post-Entry-Bloecke werden zentral abgeschlossen; der
# bewiesene interne Folgeblock bleibt im lokalen statischen AOT-Chaining.
if(NOT generated_result EQUAL 0 OR
   NOT generated_error MATCHES "KATANA_RUNTIME_PARALLEL jobs=2" OR
   NOT generated_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT generated_output MATCHES "KR_GUEST_PROGRAM_DISPATCHED" OR
   NOT generated_output MATCHES "KR_GUEST_PROGRAM_PROGRESSED" OR
   NOT generated_output MATCHES "KR_GUEST_PROGRAM_ENTERED" OR
   NOT generated_output MATCHES "silent_failures=0" OR
   NOT generated_output MATCHES "indirect_dispatches=0" OR
   NOT generated_output MATCHES "runtime_dispatch_hits=4 runtime_dispatch_misses=0" OR
   NOT generated_output MATCHES "executed_blocks=3 guest_cycle_contract=2" OR
   NOT generated_output MATCHES "post_entry_host_presented_frames=0" OR
   NOT generated_output MATCHES "post_entry_host_audio_submitted_buffers=0" OR
   NOT generated_output MATCHES
       "required_milestone=GameCodeProgressed required_milestone_reached=1" OR
   NOT generated_output MATCHES "first_problem=none")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Eigenstaendiger PackedDiscSource-Runtimepfad ist nicht lauffaehig (${generated_result}): "
    "${generated_output} ${generated_error}")
endif()

set(ENV{KATANA_PORT_CONTROLLER_TEST} "1")
set(ENV{KATANA_PORT_IGNORE_FOCUS} "1")
execute_process(
  COMMAND "${game}" --content "${fixture}/port/user-data/content/game.katana-disc"
  RESULT_VARIABLE controller_result
  OUTPUT_VARIABLE controller_output
  ERROR_VARIABLE controller_error
  TIMEOUT 60
)
unset(ENV{KATANA_PORT_CONTROLLER_TEST})
unset(ENV{KATANA_PORT_IGNORE_FOCUS})
if(NOT controller_result EQUAL 0 OR
   NOT controller_output MATCHES "KR_GUEST_PROGRAM_ENTERED" OR
   NOT controller_output MATCHES "silent_failures=0" OR
   NOT controller_output MATCHES "controller_changes=[1-9][0-9]*" OR
   NOT controller_output MATCHES "controller_samples=[1-9][0-9]*" OR
   NOT controller_output MATCHES "controller_contract=31" OR
   NOT controller_output MATCHES "first_problem=none")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Produkt-Controllervertrag erreicht Gamepad-Timeline und Maple nicht: "
    "${controller_output} ${controller_error}")
endif()
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core" OR
   KATANA_PORT_CLI_CASE STREQUAL "cache")
string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       port_build_cache_match "${port_output}")
if(NOT port_build_cache_match)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Port-CLI meldet keinen stabilen Hostbuild-Cache: ${port_output}")
endif()
set(port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${port_build}" port_build)
get_filename_component(port_workspace "${port_build}" DIRECTORY)
get_filename_component(port_workspace_name "${port_workspace}" NAME)
if(NOT port_workspace_name MATCHES "^\\.katana-port-work-[0-9a-f]+$" OR
   NOT EXISTS "${port_workspace}/CMakeLists.txt" OR
   NOT EXISTS "${port_build}/CMakeCache.txt" OR
   EXISTS "${fixture}/port/build" OR EXISTS "${fixture}/port/build-ninja")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Portexport trennt den stabilen Buildcache nicht sauber vom vorgebauten Paket: ${port_build}")
endif()

file(READ "${port_build}/CMakeCache.txt" port_cmake_cache)
string(REPLACE "\\" "/" port_cmake_cache "${port_cmake_cache}")
set(expected_cache_workspace "${port_workspace}")
set(expected_cache_build "${port_build}")
set(cache_home_key "CMAKE_HOME_DIRECTORY:INTERNAL=")
set(cache_build_key "CMAKE_CACHEFILE_DIR:INTERNAL=")
if(WIN32)
  string(TOLOWER "${port_cmake_cache}" port_cmake_cache)
  string(TOLOWER "${expected_cache_workspace}" expected_cache_workspace)
  string(TOLOWER "${expected_cache_build}" expected_cache_build)
  string(TOLOWER "${cache_home_key}" cache_home_key)
  string(TOLOWER "${cache_build_key}" cache_build_key)
endif()
string(FIND "${port_cmake_cache}"
       "${cache_home_key}${expected_cache_workspace}" cache_source_position)
string(FIND "${port_cmake_cache}"
       "${cache_build_key}${expected_cache_build}" cache_build_position)
if(cache_source_position EQUAL -1 OR cache_build_position EQUAL -1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Hostbuild-Cache verweist nicht auf das stabile Arbeits-/Buildverzeichnis")
endif()
endif()

if(KATANA_PORT_CLI_CASE STREQUAL "core")
# Ein installiertes oder verpacktes Runtime-SDK muss vor dem Compile exakt zum
# exportierten Portvertrag passen. Eine bloss vorhandene Config/Library reicht
# nicht, weil sonst alte Runtime- und Block-ABIs erst spaet oder gar nicht
# typisiert scheitern.
file(READ "${port_workspace}/CMakeLists.txt" generated_root_cmake)
foreach(expected_contract
        PROJECT_VERSION
        RUNTIME_ABI_VERSION
        BLOCK_ABI_VERSION
        PLATFORM_SERVICES_ABI_VERSION
        PROJECT_CONTRACT_VERSION)
  string(REGEX MATCH
         "set\\(KATANA_PORT_EXPECTED_${expected_contract} \"([^\"]+)\"\\)"
         expected_contract_match
         "${generated_root_cmake}")
  if(NOT expected_contract_match)
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Generiertes Portprojekt verliert erwarteten Runtimevertrag "
      "${expected_contract}")
  endif()
  set("expected_${expected_contract}" "${CMAKE_MATCH_1}")
endforeach()
math(EXPR stale_runtime_abi "${expected_RUNTIME_ABI_VERSION} - 1")
set(fake_runtime_prefix "${fixture}/fake-runtime-sdk")
set(fake_runtime_config_directory
    "${fake_runtime_prefix}/lib/cmake/KatanaRecomp")
file(MAKE_DIRECTORY "${fake_runtime_config_directory}")
set(fake_runtime_config
    "${fake_runtime_config_directory}/KatanaRecompConfig.cmake")
function(write_fake_runtime_config runtime_abi)
  file(WRITE "${fake_runtime_config}"
    "add_library(KatanaRecomp::runtime_core INTERFACE IMPORTED)\n"
    "set_target_properties(KatanaRecomp::runtime_core PROPERTIES\n"
    "  IMPORTED_CONFIGURATIONS RELWITHDEBINFO\n"
    "  KATANA_PROJECT_VERSION \"${expected_PROJECT_VERSION}\"\n"
    "  KATANA_RUNTIME_ABI_VERSION \"${runtime_abi}\"\n"
    "  KATANA_BLOCK_ABI_VERSION \"${expected_BLOCK_ABI_VERSION}\"\n"
    "  KATANA_PLATFORM_SERVICES_ABI_VERSION "
      "\"${expected_PLATFORM_SERVICES_ABI_VERSION}\"\n"
    "  KATANA_PORT_PROJECT_CONTRACT_VERSION "
      "\"${expected_PROJECT_CONTRACT_VERSION}\")\n")
endfunction()

write_fake_runtime_config("${stale_runtime_abi}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${port_workspace}"
          -B "${fixture}/stale-runtime-configure"
          "-DKATANA_RUNTIME_PREFIX=${fake_runtime_prefix}"
  RESULT_VARIABLE stale_runtime_result
  OUTPUT_VARIABLE stale_runtime_output
  ERROR_VARIABLE stale_runtime_error
  TIMEOUT 30
)
if(stale_runtime_result EQUAL 0 OR
   NOT "${stale_runtime_output}\n${stale_runtime_error}" MATCHES
       "KatanaRecomp runtime contract mismatch")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "ABI-altes installiertes Runtime-SDK scheiterte nicht vor dem Compile: "
    "${stale_runtime_output} ${stale_runtime_error}")
endif()

write_fake_runtime_config("${expected_RUNTIME_ABI_VERSION}")
execute_process(
  COMMAND "${CMAKE_COMMAND}"
          -S "${port_workspace}"
          -B "${fixture}/matching-runtime-configure"
          "-DKATANA_RUNTIME_PREFIX=${fake_runtime_prefix}"
  RESULT_VARIABLE matching_runtime_result
  OUTPUT_VARIABLE matching_runtime_output
  ERROR_VARIABLE matching_runtime_error
  TIMEOUT 30
)
if(NOT matching_runtime_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Exakt passendes installiertes Runtime-SDK wurde abgelehnt: "
    "${matching_runtime_output} ${matching_runtime_error}")
endif()

find_unexpected_publish_stage_paths("${fixture}/port" publish_stages)
if(publish_stages)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Atomarer Portexport hinterlaesst Publishing-Staging: ${publish_stages}")
endif()

# Der Cache muss auch nach dem atomaren Publish direkt fuer einen inkrementellen
# Runtime-Neubau verwendbar bleiben; dabei darf kein Disc-Installationsschritt noetig sein.
set(cached_build_native_arguments)
if(WIN32)
  list(APPEND cached_build_native_arguments -- /nodeReuse:false)
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${port_build}" --target cli_game
          --config RelWithDebInfo --parallel 2
          ${cached_build_native_arguments}
  RESULT_VARIABLE cached_build_result
  OUTPUT_VARIABLE cached_build_output
  ERROR_VARIABLE cached_build_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 600
)
if(NOT cached_build_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Stabiler Hostbuild-Cache ist nach dem Publish unbrauchbar: "
    "${cached_build_output} ${cached_build_error}")
endif()

file(WRITE "${port_build}/katana-incremental-marker" "keep-build-cache\n")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE incremental_port_result
  OUTPUT_VARIABLE incremental_port_output
  ERROR_VARIABLE incremental_port_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       incremental_build_cache_match "${incremental_port_output}")
set(incremental_port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${incremental_port_build}" incremental_port_build)
if(NOT incremental_port_result EQUAL 0 OR NOT incremental_build_cache_match OR
   NOT "${incremental_port_build}" STREQUAL "${port_build}" OR
   NOT EXISTS "${port_build}/katana-incremental-marker" OR
   NOT incremental_port_output MATCHES "Analyse-/IR-Cache-Hit: ja" OR
   NOT incremental_port_output MATCHES
       "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
   incremental_port_output MATCHES
       "KATANA_PORT_SUBPHASE control-flow-analysis" OR
   EXISTS "${fixture}/port/build" OR EXISTS "${fixture}/port/build-ninja" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Inkrementeller Portbuild verliert Buildcache oder lokalen Nutzercache: "
    "${incremental_port_output} ${incremental_port_error}")
endif()
require_cache_tree_progress(
  "${incremental_port_output}"
  "whole-export-cache-tree-load"
  "Inkrementeller Whole-Export-Cache-Hit"
  "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit")

# Ein lokaler Katana-Buildtree darf seine bereits gebaute Runtime direkt
# wiederverwenden. Vor dem Portlink muss der CLI-Pfad das echte Runtime-Target
# inkrementell aktualisieren; der generierte Port darf die Runtime danach
# weder nochmals aus dem Quellbaum kompilieren noch eine stale Bibliothek
# verwenden.
file(TO_CMAKE_PATH
     "${KATANA_RUNTIME_BUILD_TARGETS_FILE}"
     runtime_build_targets_file)
if(NOT EXISTS "${runtime_build_targets_file}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Katana-Buildtree besitzt keinen Runtime-Targetexport: "
    "${runtime_build_targets_file}")
endif()

# Mehrere Runtimebindungen duerfen nicht still nach Prioritaet ausgewaehlt
# werden. Sonst kann ein altes Prefix die explizit angeforderte lokale
# Buildtree-Runtime verdraengen.
set(ENV{KATANA_RUNTIME_BUILD_TARGETS} "${runtime_build_targets_file}")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/conflicting-runtime-bindings"
          --target-name conflicting_runtime_game
  RESULT_VARIABLE conflicting_runtime_result
  OUTPUT_VARIABLE conflicting_runtime_output
  ERROR_VARIABLE conflicting_runtime_error
  TIMEOUT 30
)
unset(ENV{KATANA_RUNTIME_BUILD_TARGETS})
if(conflicting_runtime_result EQUAL 0 OR
   NOT conflicting_runtime_error MATCHES
       "genau eine Runtime-Umgebungsbindung" OR
   EXISTS "${fixture}/conflicting-runtime-bindings")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Widerspruechliche Runtime-Umgebungsbindungen wurden nicht fail-closed "
    "abgelehnt: ${conflicting_runtime_output} ${conflicting_runtime_error}")
endif()

unset(ENV{KATANA_RUNTIME_ROOT})

if(WIN32)
  # Visual-Studio-Builds legen das CLI unter <build>/<Config>/ ab, der
  # Targetexport liegt dagegen direkt unter <build>/. Eine testexklusive,
  # eindeutig benannte Kopie simuliert diesen Pfad, ohne einen echten
  # Konfigurationsordner zu beruehren.
  get_filename_component(runtime_build_tree_root
                         "${runtime_build_targets_file}" DIRECTORY)
  set(runtime_discovery_directory
      "${runtime_build_tree_root}/katana-runtime-discovery-probe-port-cli-test")
  if(EXISTS "${runtime_discovery_directory}")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Exklusiver Runtime-Discovery-Probeordner existiert bereits; "
      "fremde Daten werden nicht geloescht: ${runtime_discovery_directory}")
  endif()
  set(runtime_discovery_cli
      "${runtime_discovery_directory}/katana-runtime-discovery-probe.exe")
  file(MAKE_DIRECTORY "${runtime_discovery_directory}")
  file(COPY_FILE "${KATANA_CLI}" "${runtime_discovery_cli}" ONLY_IF_DIFFERENT)
else()
  set(runtime_discovery_cli "${KATANA_CLI}")
  set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
      "${runtime_build_targets_file}")
endif()
execute_process(
  COMMAND "${runtime_discovery_cli}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE buildtree_runtime_result
  OUTPUT_VARIABLE buildtree_runtime_output
  ERROR_VARIABLE buildtree_runtime_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(WIN32)
  file(REMOVE "${runtime_discovery_cli}")
  file(REMOVE_RECURSE "${runtime_discovery_directory}")
else()
  unset(ENV{KATANA_RUNTIME_BUILD_TARGETS})
endif()
set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")
file(READ "${port_build}/CMakeCache.txt"
     buildtree_runtime_cmake_cache)
string(REPLACE "\\" "/"
       buildtree_runtime_cmake_cache
       "${buildtree_runtime_cmake_cache}")
set(buildtree_runtime_config_missing FALSE)
if(NOT buildtree_runtime_output MATCHES
       "KATANA_PORT_PHASE runtime-sdk-build config=(RelWithDebInfo|Release|MinSizeRel) generator=(single|multi)")
  set(buildtree_runtime_config_missing TRUE)
endif()
if(NOT buildtree_runtime_result EQUAL 0 OR
   NOT buildtree_runtime_output MATCHES
       "KATANA_PORT_PHASE runtime-sdk-build" OR
   buildtree_runtime_config_missing OR
   NOT buildtree_runtime_output MATCHES
       "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
   NOT buildtree_runtime_cmake_cache MATCHES
       "KATANA_RUNTIME_BUILD_TARGETS:FILEPATH=${runtime_build_targets_file}" OR
   buildtree_runtime_cmake_cache MATCHES
       "KATANA_RUNTIME_ROOT:PATH=${KATANA_RUNTIME_ROOT}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Direkte Buildtree-Runtime ist nicht inkrementell, nicht verdrahtet "
    "oder laesst den Quellruntime-Pfad aktiv: "
    "${buildtree_runtime_output} ${buildtree_runtime_error}")
endif()

endif()

if(KATANA_PORT_CLI_CASE STREQUAL "publish")
# Ein neuer Targetname erzeugt absichtlich eine andere Workspace-ID. Der erste
# Lauf muss daher die bestehende, valide A-Distribution als anders gebunden
# erkennen und leer exportieren, statt darin ein B-Executable zu verlangen.
# Derselbe Pfad prueft mit einem 1-ms-Testvertrag, dass der echte Configure-
# Prozessbaum beaufsichtigt und sein unvollstaendiger CMake-Zustand entfernt
# wird, ohne das bereits publizierte A-Paket anzutasten.
set(target_switch_name cli_game_target_b)
if(WIN32)
  set(target_switch_game
      "${fixture}/port/${target_switch_name}.exe")
else()
  set(target_switch_game
      "${fixture}/port/${target_switch_name}")
endif()
set(target_switch_user_marker
    "${fixture}/port/user-data/target-switch-preserved.txt")
file(WRITE "${target_switch_user_marker}"
     "preserve across sequential target switch\n")
file(GLOB_RECURSE pre_timeout_cmake_caches
     LIST_DIRECTORIES FALSE
     "${fixture}/.katana-port-work-*/build-*/CMakeCache.txt")
list(SORT pre_timeout_cmake_caches)
set(ENV{KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_STAGE} "configure")
set(ENV{KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_MS} "1")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port"
          --target-name "${target_switch_name}"
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE supervised_timeout_result
  OUTPUT_VARIABLE supervised_timeout_output
  ERROR_VARIABLE supervised_timeout_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
unset(ENV{KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_STAGE})
unset(ENV{KATANA_PORT_HOST_COMMAND_TEST_TIMEOUT_MS})
file(GLOB_RECURSE post_timeout_cmake_caches
     LIST_DIRECTORIES FALSE
     "${fixture}/.katana-port-work-*/build-*/CMakeCache.txt")
list(SORT post_timeout_cmake_caches)
if(supervised_timeout_result EQUAL 0 OR
   NOT supervised_timeout_error MATCHES
       "KATANA_PORT_HOST_COMMAND_TIMEOUT stage=configure limit_ms=1 process_tree=terminated" OR
   NOT "${pre_timeout_cmake_caches}" STREQUAL
       "${post_timeout_cmake_caches}" OR
   NOT EXISTS "${game}" OR
   EXISTS "${target_switch_game}" OR
   NOT EXISTS "${target_switch_user_marker}" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Beaufsichtigter Configure-Timeout ist nicht stabil, bereinigt seinen "
    "CMake-Zustand nicht oder veraendert das publizierte A-Paket: "
    "${supervised_timeout_output} ${supervised_timeout_error}")
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port"
          --target-name "${target_switch_name}"
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE target_switch_result
  OUTPUT_VARIABLE target_switch_output
  ERROR_VARIABLE target_switch_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
file(READ
     "${fixture}/port/generated/metadata/port-project.json"
     target_switch_metadata)
find_unexpected_publish_stage_paths(
  "${fixture}/port" target_switch_publish_stages)
if(NOT target_switch_result EQUAL 0 OR
   NOT EXISTS "${target_switch_game}" OR
   EXISTS "${game}" OR
   NOT target_switch_metadata MATCHES
       "\"target_name\":\"${target_switch_name}\"" OR
   NOT EXISTS "${target_switch_user_marker}" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc" OR
   target_switch_publish_stages)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Sequentieller Targetwechsel A nach B exportiert/publiziert nicht "
    "targetrein oder verliert lokale Nutzerdaten: "
    "${target_switch_output} ${target_switch_error}")
endif()

# Die weitere Matrix arbeitet bewusst wieder mit ihrem kanonischen Target A.
# Der Rueckwechsel nutzt dessen bereits vorhandenen targetgebundenen Workspace
# und muss dieselben lokalen Daten erneut erhalten.
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE target_restore_result
  OUTPUT_VARIABLE target_restore_output
  ERROR_VARIABLE target_restore_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
find_unexpected_publish_stage_paths(
  "${fixture}/port" target_restore_publish_stages)
if(NOT target_restore_result EQUAL 0 OR
   NOT EXISTS "${game}" OR
   EXISTS "${target_switch_game}" OR
   NOT EXISTS "${target_switch_user_marker}" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc" OR
   target_restore_publish_stages)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Target-Rueckwechsel B nach A verliert Workspace- oder Nutzerdaten: "
    "${target_restore_output} ${target_restore_error}")
endif()

# Der Publish-Lock ist ausschliesslich an den physischen Ausgabeort gebunden.
# Ein zweiter Targetname darf daher nicht parallel denselben Output betreten.
# Der erste Prozess beendet sich nach der Lock-/Recovery-Pruefung, damit diese
# Regression keinen zusaetzlichen Hostbuild erzeugt.
if(WIN32)
  execute_process(
    COMMAND "${KATANA_TEST_POWERSHELL}" -NoProfile -NonInteractive
            -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_LIST_DIR}/verify_port_publish_race.ps1"
            -Cli "${KATANA_CLI}"
            -Disc "${fixture}/disc/disc.gdi"
            -Output "${fixture}/port"
            -GameProject
              "${fixture}/disc/product-gate.katana-game-project"
            -RuntimeImagePayload "${runtime_image_payload_binding}"
            -LatentAotEntry "${latent_aot_entry_binding}"
            -WorkingDirectory "${fixture}"
    RESULT_VARIABLE publish_race_result
    OUTPUT_VARIABLE publish_race_output
    ERROR_VARIABLE publish_race_error
    ECHO_OUTPUT_VARIABLE
    ECHO_ERROR_VARIABLE
    TIMEOUT 210
  )
  if(NOT publish_race_result EQUAL 0 OR
     NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Targetunabhaengiger Output-Lock verhindert den Cross-Target-Race "
      "nicht: ${publish_race_output} ${publish_race_error}")
  endif()
endif()

# Ein altes, nicht markiertes Backup aus frueheren Versionen ist fremder
# Nutzerzustand. Der neue Publisher darf diesen deterministischen Baum weder
# uebernehmen noch loeschen.
set(foreign_legacy_stale
    "${fixture}/port.katana-stale-port")
file(MAKE_DIRECTORY "${foreign_legacy_stale}")
file(WRITE "${foreign_legacy_stale}/do-not-delete.txt"
     "foreign legacy tree\n")

# Crash nach dem Wegbewegen des Altports: Der naechste Lauf muss den Altport
# samt lokalen Nutzerdaten zurueckstellen, ohne das fertige Staging oder einen
# fremden Ausgabeordner zu erraten.
file(WRITE "${fixture}/port/user-data/crash-old-move.txt"
     "keep old move user data\n")
set(ENV{KATANA_PORT_PUBLISH_TEST_CRASH_POINT} "after-old-move")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE old_move_crash_result
  OUTPUT_VARIABLE old_move_crash_output
  ERROR_VARIABLE old_move_crash_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
unset(ENV{KATANA_PORT_PUBLISH_TEST_CRASH_POINT})
find_active_port_publish_roots(
  "${fixture}/port" old_move_transaction_roots)
list(LENGTH old_move_transaction_roots old_move_transaction_root_count)
if(old_move_crash_result EQUAL 0 OR
   NOT old_move_crash_error MATCHES
       "KATANA_PORT_PUBLISH_TEST_CRASH point=after-old-move" OR
   EXISTS "${fixture}/port" OR
   NOT EXISTS "${fixture}/port.katana-publish-transaction" OR
   NOT old_move_transaction_root_count EQUAL 1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Crash nach old-move hinterlaesst keinen eindeutig recoverbaren "
    "Publishzustand: ${old_move_crash_output} ${old_move_crash_error} "
    "${old_move_transaction_roots}")
endif()
list(GET old_move_transaction_roots 0 old_move_transaction_root)
if(NOT EXISTS
       "${old_move_transaction_root}/backup/user-data/crash-old-move.txt")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Crash nach old-move verliert lokale Nutzerdaten im Backup")
endif()

set(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY} "1")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name recovery_probe
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE old_move_recovery_result
  OUTPUT_VARIABLE old_move_recovery_output
  ERROR_VARIABLE old_move_recovery_error
  TIMEOUT 60
)
unset(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY})
find_active_port_publish_roots(
  "${fixture}/port" old_move_recovery_roots)
if(NOT old_move_recovery_result EQUAL 0 OR
   NOT old_move_recovery_output MATCHES
       "KATANA_PORT_PUBLISH_TEST_RECOVERY_COMPLETE" OR
   NOT EXISTS "${fixture}/port/user-data/crash-old-move.txt" OR
   EXISTS "${fixture}/port.katana-publish-transaction" OR
   NOT "${old_move_recovery_roots}" STREQUAL "" OR
   NOT EXISTS "${foreign_legacy_stale}/do-not-delete.txt")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Recovery nach old-move stellt Altport/Nutzerdaten nicht sicher "
    "wieder her oder loescht einen Fremdbaum: "
    "${old_move_recovery_output} ${old_move_recovery_error}")
endif()

# Crash nach der neuen Output-Rename: Nur der mitgewanderte Owner-Marker darf
# den neuen Output autorisieren. Die Recovery rettet danach die lokalen Daten
# idempotent aus dem Backup in genau diesen Port.
file(WRITE "${fixture}/port/user-data/crash-new-publish.txt"
     "keep new publish user data\n")
set(ENV{KATANA_PORT_PUBLISH_TEST_CRASH_POINT} "after-new-publish")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE new_publish_crash_result
  OUTPUT_VARIABLE new_publish_crash_output
  ERROR_VARIABLE new_publish_crash_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
unset(ENV{KATANA_PORT_PUBLISH_TEST_CRASH_POINT})
find_active_port_publish_roots(
  "${fixture}/port" new_publish_transaction_roots)
list(LENGTH new_publish_transaction_roots
     new_publish_transaction_root_count)
if(new_publish_crash_result EQUAL 0 OR
   NOT new_publish_crash_error MATCHES
       "KATANA_PORT_PUBLISH_TEST_CRASH point=after-new-publish" OR
   NOT EXISTS "${fixture}/port/.katana-publish-owner" OR
   NOT EXISTS "${fixture}/port.katana-publish-transaction" OR
   NOT new_publish_transaction_root_count EQUAL 1)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Crash nach new-publish hinterlaesst keinen eindeutig eigenen "
    "recoverbaren Output: ${new_publish_crash_output} "
    "${new_publish_crash_error} ${new_publish_transaction_roots}")
endif()
list(GET new_publish_transaction_roots 0 new_publish_transaction_root)
if(NOT EXISTS
       "${new_publish_transaction_root}/backup/user-data/crash-new-publish.txt")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Crash nach new-publish verliert das noch nicht migrierte user-data")
endif()

set(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY} "1")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name recovery_probe
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE new_publish_recovery_result
  OUTPUT_VARIABLE new_publish_recovery_output
  ERROR_VARIABLE new_publish_recovery_error
  TIMEOUT 60
)
unset(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY})
find_active_port_publish_roots(
  "${fixture}/port" new_publish_recovery_roots)
if(NOT new_publish_recovery_result EQUAL 0 OR
   NOT new_publish_recovery_output MATCHES
       "KATANA_PORT_PUBLISH_TEST_RECOVERY_COMPLETE" OR
   EXISTS "${fixture}/port/.katana-publish-owner" OR
   NOT EXISTS "${fixture}/port/user-data/crash-old-move.txt" OR
   NOT EXISTS "${fixture}/port/user-data/crash-new-publish.txt" OR
   EXISTS "${fixture}/port.katana-publish-transaction" OR
   NOT "${new_publish_recovery_roots}" STREQUAL "" OR
   NOT EXISTS "${foreign_legacy_stale}/do-not-delete.txt")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Recovery nach new-publish rettet user-data nicht idempotent oder "
    "raeumt die eigene Transaktion nicht auf: "
    "${new_publish_recovery_output} ${new_publish_recovery_error}")
endif()

# Ein fremder Baum auf dem Journalpfad blockiert fail-closed und bleibt
# bytegenau erhalten. Ein journal-loser Baum, der nur wie eine Transaktion
# aussieht, wird ebenfalls niemals als eigener Cleanup-Kandidat geraten.
set(foreign_publish_output "${fixture}/foreign-publish-port")
set(foreign_publish_journal
    "${foreign_publish_output}.katana-publish-transaction")
file(MAKE_DIRECTORY "${foreign_publish_journal}")
file(WRITE "${foreign_publish_journal}/do-not-delete.txt"
     "foreign journal tree\n")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${foreign_publish_output}"
          --target-name foreign_publish_probe
  RESULT_VARIABLE foreign_publish_result
  OUTPUT_VARIABLE foreign_publish_output_text
  ERROR_VARIABLE foreign_publish_error
  TIMEOUT 60
)
if(foreign_publish_result EQUAL 0 OR
   NOT foreign_publish_error MATCHES
       "Port-Publish-Transaktionsmarker" OR
   NOT EXISTS "${foreign_publish_journal}/do-not-delete.txt" OR
   EXISTS "${foreign_publish_output}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Fremder Publish-Journalbaum wurde uebernommen oder veraendert: "
    "${foreign_publish_output_text} ${foreign_publish_error}")
endif()

set(orphan_publish_output "${fixture}/orphan-publish-port")
set(orphan_publish_tree
    "${orphan_publish_output}.katana-publish-transaction.0123456789abcdef0123456789abcdef")
file(MAKE_DIRECTORY "${orphan_publish_tree}")
file(WRITE "${orphan_publish_tree}/do-not-delete.txt"
     "foreign orphan tree\n")
set(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY} "1")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${orphan_publish_output}"
          --target-name orphan_publish_probe
  RESULT_VARIABLE orphan_publish_result
  OUTPUT_VARIABLE orphan_publish_output_text
  ERROR_VARIABLE orphan_publish_error
  TIMEOUT 60
)
unset(ENV{KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY})
if(NOT orphan_publish_result EQUAL 0 OR
   NOT EXISTS "${orphan_publish_tree}/do-not-delete.txt" OR
   EXISTS "${orphan_publish_output}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Journal-loser fremder Publishbaum wurde als eigene Transaktion "
    "behandelt: ${orphan_publish_output_text} ${orphan_publish_error}")
endif()

endif()

if(KATANA_PORT_CLI_CASE STREQUAL "cache")
file(WRITE "${port_build}/katana-incremental-marker" "keep-build-cache\n")

# Derselbe validierte Inhalt und dasselbe Ziel muessen auch fuer einen anderen,
# noch nicht vorhandenen Ausgabeordner exakt denselben Arbeits-/Buildcache
# verwenden. Das Publish und lokale Nutzerdaten bleiben dagegen ausgabebezogen.
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port-fresh" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE fresh_port_result
  OUTPUT_VARIABLE fresh_port_output
  ERROR_VARIABLE fresh_port_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       fresh_build_cache_match "${fresh_port_output}")
set(fresh_port_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${fresh_port_build}" fresh_port_build)
if(WIN32)
  set(fresh_game "${fixture}/port-fresh/cli_game.exe")
else()
  set(fresh_game "${fixture}/port-fresh/cli_game")
endif()
find_unexpected_publish_stage_paths(
  "${fixture}/port-fresh" fresh_publish_stages)
if(NOT fresh_port_result EQUAL 0 OR NOT fresh_build_cache_match OR
   NOT "${fresh_port_build}" STREQUAL "${port_build}" OR
   NOT EXISTS "${port_build}/katana-incremental-marker" OR
   NOT fresh_port_output MATCHES "Analyse-/IR-Cache-Hit: ja" OR
   NOT fresh_port_output MATCHES
       "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
   fresh_port_output MATCHES "KATANA_PORT_SUBPHASE control-flow-analysis" OR
   NOT EXISTS "${fresh_game}" OR
   NOT EXISTS "${fixture}/port-fresh/content/game.katana-install" OR
   EXISTS "${fixture}/port-fresh/build" OR
   EXISTS "${fixture}/port-fresh/build-ninja" OR
   EXISTS "${fixture}/port-fresh/user-data/content/game.katana-disc" OR
   fresh_publish_stages)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Ausgabeunabhaengiger Port-Arbeitscache wird fuer frischen Zielordner "
    "nicht stabil wiederverwendet oder Publish/Nutzerdaten sind gekoppelt: "
    "${fresh_port_output} ${fresh_port_error}")
endif()
require_port_phase_timing_contract(
  "${fresh_port_output}" "Whole-Export-Cache-Hit" FALSE)

# Der per-user Workspace-Stamm darf nicht wieder implizit an den Elternordner
# der Distribution gekoppelt werden. Auch ein Ausgabeziel in einem anderen
# Baum muss denselben content-addressed Analyse-, Codegen- und Hostbuild-Cache
# treffen.
file(MAKE_DIRECTORY "${fixture}/other-output-parent")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/other-output-parent/port"
          --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE cross_parent_result
  OUTPUT_VARIABLE cross_parent_output
  ERROR_VARIABLE cross_parent_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
string(REGEX MATCH "Inkrementeller Hostbuild-Cache: ([^\r\n]+)"
       cross_parent_build_match "${cross_parent_output}")
set(cross_parent_build "${CMAKE_MATCH_1}")
file(TO_CMAKE_PATH "${cross_parent_build}" cross_parent_build)
if(WIN32)
  set(cross_parent_game
      "${fixture}/other-output-parent/port/cli_game.exe")
else()
  set(cross_parent_game
      "${fixture}/other-output-parent/port/cli_game")
endif()
file(GLOB cross_parent_local_workspaces
     "${fixture}/other-output-parent/.katana-port-work-*")
if(NOT cross_parent_result EQUAL 0 OR
   NOT cross_parent_build_match OR
   NOT "${cross_parent_build}" STREQUAL "${port_build}" OR
   NOT cross_parent_output MATCHES "Analyse-/IR-Cache-Hit: ja" OR
   NOT cross_parent_output MATCHES
       "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
   cross_parent_output MATCHES
       "KATANA_PORT_SUBPHASE control-flow-analysis" OR
   NOT EXISTS "${cross_parent_game}" OR
   cross_parent_local_workspaces)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Globaler Port-Arbeitscache ist noch an den Ausgabe-Elternpfad gekoppelt: "
    "${cross_parent_output} ${cross_parent_error}")
endif()

# Die Cacheidentitaet folgt den buildgebundenen Analyse-/IR-/Codegen-
# Komponenten, nicht jedem irrelevanten Byte des monolithischen CLI. Ein
# angehaengter, nicht ausfuehrbarer Host-/UI-Marker darf deshalb die bewiesenen
# Analyseartefakte nicht wegwerfen.
set(mutated_cli_directory "${fixture}/mutated-exporter")
file(MAKE_DIRECTORY "${mutated_cli_directory}")
file(COPY "${KATANA_CLI}" DESTINATION "${mutated_cli_directory}")
get_filename_component(mutated_cli_name "${KATANA_CLI}" NAME)
set(mutated_cli
    "${mutated_cli_directory}/${mutated_cli_name}")
file(APPEND "${mutated_cli}"
     "\nKATANA_TEST_DIFFERENT_EXPORTER_IMPLEMENTATION\n")
execute_process(
  COMMAND "${mutated_cli}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/implementation-port"
          --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE implementation_miss_result
  OUTPUT_VARIABLE implementation_miss_output
  ERROR_VARIABLE implementation_miss_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(NOT implementation_miss_result EQUAL 0 OR
   NOT implementation_miss_output MATCHES
       "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
   implementation_miss_output MATCHES
       "KATANA_PORT_SUBPHASE control-flow-analysis")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Whole-Export-Cache wurde weiterhin durch ein nicht outputrelevantes "
    "Byte des monolithischen Exporters invalidiert: "
    "${implementation_miss_output} ${implementation_miss_error}")
endif()

function(require_whole_export_cache_miss case_name)
  execute_process(
    COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
            --output "${fixture}/port" --target-name cli_game
            --game-project "${fixture}/disc/product-gate.katana-game-project"
            --runtime-image-payload "${runtime_image_payload_binding}"
            --latent-aot-mode exact-only
            --latent-aot-entry "${latent_aot_entry_binding}"
    RESULT_VARIABLE cache_miss_result
    OUTPUT_VARIABLE cache_miss_output
    ERROR_VARIABLE cache_miss_error
    ECHO_OUTPUT_VARIABLE
    ECHO_ERROR_VARIABLE
    TIMEOUT 720
  )
  if(NOT cache_miss_result EQUAL 0 OR
     cache_miss_output MATCHES
         "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit" OR
     NOT cache_miss_output MATCHES
         "KATANA_PORT_SUBPHASE control-flow-analysis" OR
     cache_miss_output MATCHES
         "state=cached[^\r\n]*label=\"boot-analysis-cache\"")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Whole-Export-Cache akzeptiert ${case_name}, konsumiert einen unsicheren "
      "positiven Bootcache oder kann den Miss nicht reparieren: "
      "${cache_miss_output} ${cache_miss_error}")
  endif()
endfunction()

# Der Whole-Export-Hit ist nur gueltig, wenn das Codegen-Manifest, jede darin
# aufgefuehrte Datei und das tatsaechliche Dateiset exakt erhalten sind.
set(generated_manifest
    "${port_workspace}/generated/.katana-generated-artifacts")
set(generated_dispatch_header
    "${port_workspace}/generated/include/runtime-dispatch-internal.hpp")
set(generated_port_header
    "${port_workspace}/generated/include/katana_port.hpp")
set(injected_generated_header
    "${port_workspace}/generated/include/cache-injected.hpp")
set(injected_generated_source
    "${port_workspace}/src/cache-injected.cpp")
if(NOT EXISTS "${generated_manifest}" OR
   NOT EXISTS "${generated_dispatch_header}" OR
   NOT EXISTS "${generated_port_header}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Whole-Export-Cache besitzt kein vollstaendiges generiertes Artefaktinventar")
endif()
file(SHA256 "${generated_dispatch_header}" generated_dispatch_header_identity)
file(READ "${generated_manifest}" cold_generated_manifest_content)

file(APPEND "${generated_dispatch_header}" "\ncache-modified\n")
require_whole_export_cache_miss("einen veraenderten Dispatchheader")
file(SHA256 "${generated_dispatch_header}" restored_dispatch_header_identity)
if(NOT restored_dispatch_header_identity STREQUAL
       generated_dispatch_header_identity)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Neu-Export stellt einen veraenderten Dispatchheader nicht exakt wieder her")
endif()
file(READ "${generated_manifest}" bootcache_generated_manifest_content)
if(NOT bootcache_generated_manifest_content STREQUAL
       cold_generated_manifest_content)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Vollanalyse driftet zwischen Cold- und Warm-Whole-Miss")
endif()

file(REMOVE "${generated_port_header}")
require_whole_export_cache_miss("einen fehlenden Portheader")
if(NOT EXISTS "${generated_port_header}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Neu-Export stellt einen fehlenden Portheader nicht wieder her")
endif()
file(READ "${generated_manifest}" second_bootcache_generated_manifest_content)
if(NOT second_bootcache_generated_manifest_content STREQUAL
       bootcache_generated_manifest_content)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Zwei unmittelbar aufeinanderfolgende Bootcache-Warmexports driften")
endif()

file(WRITE "${injected_generated_header}" "injected cache artifact\n")
file(WRITE "${injected_generated_source}" "injected cache source\n")
require_whole_export_cache_miss("ein injiziertes include/- und src/-Artefakt")
if(EXISTS "${injected_generated_header}" OR EXISTS "${injected_generated_source}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Neu-Export entfernt ein nicht manifestiertes include/- oder src/-Artefakt nicht")
endif()
file(READ "${generated_manifest}" third_bootcache_generated_manifest_content)
if(NOT third_bootcache_generated_manifest_content STREQUAL
       bootcache_generated_manifest_content)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Bootcache-Warmexport bleibt nach Artefaktbereinigung nicht bytegleich")
endif()

# Jeder Whole-Export-Miss muss die vollstaendige Analyse wiederholen und exakt
# dieselben kanonischen Artefakte wie der Cold-Lauf erzeugen.
string(REPLACE "katana-codegen-artifacts-v1"
               "katana-codegen-artifacts-v0"
               invalid_generated_manifest
               "${bootcache_generated_manifest_content}")
file(WRITE "${generated_manifest}" "${invalid_generated_manifest}")
require_whole_export_cache_miss("ein veraendertes Artefaktmanifest")
file(READ "${generated_manifest}" restored_generated_manifest_content)
if(NOT restored_generated_manifest_content STREQUAL
       bootcache_generated_manifest_content)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Neu-Export stellt das Artefaktmanifest nicht exakt wieder her.\n"
    "Erwartet:\n${bootcache_generated_manifest_content}\n"
    "Erhalten:\n${restored_generated_manifest_content}")
endif()

# Die finale Distribution ist eine positive Dateiliste. Gemeinsam injizierte
# Rootdateien, Bootsektoren und Trackabbilder duerfen weder durch einen
# Whole-Export-Hit noch durch die inkrementelle Publish-Kopie gelangen.
set(injected_root_files
    "${port_workspace}/cache-injected-root.txt"
    "${port_workspace}/IP.BIN"
    "${port_workspace}/content/track03.bin")
foreach(injected IN LISTS injected_root_files)
  file(WRITE "${injected}" "synthetic distribution injection\n")
endforeach()
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
          --game-project "${fixture}/disc/product-gate.katana-game-project"
          --runtime-image-payload "${runtime_image_payload_binding}"
          --latent-aot-mode exact-only
          --latent-aot-entry "${latent_aot_entry_binding}"
  RESULT_VARIABLE injected_distribution_result
  OUTPUT_VARIABLE injected_distribution_output
  ERROR_VARIABLE injected_distribution_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(injected_distribution_result EQUAL 0 OR
   NOT "${injected_distribution_error}" MATCHES
       "nicht distributionsgebundene Datei" OR
   EXISTS "${fixture}/port/cache-injected-root.txt" OR
   EXISTS "${fixture}/port/IP.BIN" OR
   EXISTS "${fixture}/port/content/track03.bin")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Distributions-Allowlist akzeptiert injizierte Root-/IP.BIN-/Trackdaten: "
    "${injected_distribution_output} ${injected_distribution_error}")
endif()
file(REMOVE ${injected_root_files})

execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE reinstall_result
  OUTPUT_VARIABLE reinstall_output
  ERROR_VARIABLE reinstall_error
  TIMEOUT 60
)
if(NOT reinstall_result EQUAL 0 OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Originaldisc wurde nach inkrementellem Publish nicht lokal reinstalliert: "
    "${reinstall_output} ${reinstall_error}")
endif()

endif()

if(KATANA_PORT_CLI_CASE STREQUAL "runtime-edge")
foreach(lifecycle_case IN ITEMS
        close-before-running running-close focus-resume-close paused-close)
  set(ENV{KATANA_PORT_LIFECYCLE_TEST} "${lifecycle_case}")
  execute_process(
    COMMAND "${game}"
    RESULT_VARIABLE lifecycle_result
    OUTPUT_VARIABLE lifecycle_output
    ERROR_VARIABLE lifecycle_error
    TIMEOUT 60
  )
  if(NOT lifecycle_result EQUAL 1 OR
     NOT lifecycle_output MATCHES "KR_HOST_SHUTDOWN guest_dispatch_stopped=1" OR
     NOT lifecycle_output MATCHES
         "KATANA_BRINGUP_RUN status=(error|early-exit-before-required-milestone)" OR
     NOT lifecycle_output MATCHES
         "first_problem=(runtime-contract|required-milestone-not-reached)" OR
     lifecycle_error MATCHES
         "Runtime-Einstieg besitzt keinen Dispatchnachweis|first_problem=runtime-exception")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Lifecycle ${lifecycle_case} beendet nativen Gastdispatch nicht: "
      "${lifecycle_output} ${lifecycle_error}")
  endif()
  if(lifecycle_case STREQUAL "close-before-running" AND
     (NOT lifecycle_output MATCHES
          "KR_HOST_SHUTDOWN guest_dispatch_stopped=1 host_events=2 input_events=0" OR
      NOT lifecycle_output MATCHES "silent_failures=1" OR
      NOT lifecycle_output MATCHES "post_entry_central_dispatches=0" OR
      NOT lifecycle_output MATCHES "milestone_bits=0" OR
      NOT lifecycle_output MATCHES "first_problem=runtime-contract"))
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Close vor erstem Dispatch wird nicht geordnet und fail-closed beendet: "
      "${lifecycle_output} ${lifecycle_error}")
  endif()
endforeach()
unset(ENV{KATANA_PORT_LIFECYCLE_TEST})

file(APPEND "${fixture}/disc/high.bin" "identity-change")
execute_process(
  COMMAND "${game}" --gdi-debug "./disc.gdi"
  WORKING_DIRECTORY "${fixture}/disc"
  RESULT_VARIABLE mismatch_result
  OUTPUT_VARIABLE mismatch_output
  ERROR_VARIABLE mismatch_error
  TIMEOUT 60
)
if(mismatch_result EQUAL 0 OR
   NOT mismatch_error MATCHES "source-identity-mismatch" OR
   mismatch_output MATCHES "KR_GUEST_PROGRAM_ENTERED|silent_failures=0")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Geaenderte Laufzeit-GDI wurde nicht vor Gastcode abgelehnt: ${mismatch_output} ${mismatch_error}")
endif()

file(MAKE_DIRECTORY "${fixture}/trap-disc")
execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-trap-fixture "${fixture}/trap-disc"
  RESULT_VARIABLE trap_writer_result
  ERROR_VARIABLE trap_writer_error
  TIMEOUT 30
)
if(NOT trap_writer_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Trap-Portfixture fehlgeschlagen: ${trap_writer_error}")
endif()
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/trap-disc/disc.gdi"
          --output "${fixture}/trap-port" --target-name trap_game
          --latent-aot-mode exact-only
  RESULT_VARIABLE trap_port_result
  OUTPUT_VARIABLE trap_port_output
  ERROR_VARIABLE trap_port_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(NOT trap_port_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Trap-Port konnte nicht gebaut werden: ${trap_port_output} ${trap_port_error}")
endif()
if(WIN32)
  set(trap_game "${fixture}/trap-port/trap_game.exe")
else()
  set(trap_game "${fixture}/trap-port/trap_game")
endif()
execute_process(
  COMMAND "${trap_game}" --install-disc "${fixture}/trap-disc/disc.gdi"
  RESULT_VARIABLE trap_install_result
  OUTPUT_VARIABLE trap_install_output
  ERROR_VARIABLE trap_install_error
  TIMEOUT 60
)
if(NOT trap_install_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Trap-Originaldisc-Installation fehlgeschlagen: ${trap_install_output} ${trap_install_error}")
endif()
execute_process(
  COMMAND "${trap_game}"
  RESULT_VARIABLE trap_result
  OUTPUT_VARIABLE trap_output
  ERROR_VARIABLE trap_error
  TIMEOUT 60
)
if(trap_result EQUAL 0 OR trap_output MATCHES "KR_GUEST_PROGRAM_ENTERED|silent_failures=0")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Trap-Einstieg erzeugte einen falschen Hauptprogrammnachweis: ${trap_output} ${trap_error}")
endif()

file(MAKE_DIRECTORY "${fixture}/unknown-target-disc")
execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-unknown-target-fixture
          "${fixture}/unknown-target-disc"
  RESULT_VARIABLE unknown_writer_result
  ERROR_VARIABLE unknown_writer_error
  TIMEOUT 30
)
if(NOT unknown_writer_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Unknown-Target-Portfixture fehlgeschlagen: ${unknown_writer_error}")
endif()
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/unknown-target-disc/disc.gdi"
          --output "${fixture}/unknown-target-port" --target-name unknown_target_game
          --latent-aot-mode exact-only
  RESULT_VARIABLE unknown_port_result
  OUTPUT_VARIABLE unknown_port_output
  ERROR_VARIABLE unknown_port_error
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE
  TIMEOUT 720
)
if(NOT unknown_port_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Normaler Unknown-Target-Port konnte nicht gebaut werden: "
    "${unknown_port_output} ${unknown_port_error}")
endif()
if(WIN32)
  set(unknown_target_game "${fixture}/unknown-target-port/unknown_target_game.exe")
else()
  set(unknown_target_game "${fixture}/unknown-target-port/unknown_target_game")
endif()
if(NOT EXISTS "${fixture}/unknown-target-port/generated/metadata/port-project.json" OR
   NOT EXISTS "${fixture}/unknown-target-port/generated/code/runtime-dispatch.cpp")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Unknown-Target-Port besitzt keinen pruefbaren Produktvertrag")
endif()
file(READ "${fixture}/unknown-target-port/generated/metadata/port-project.json"
     unknown_target_project_contract)
file(READ "${fixture}/unknown-target-port/generated/code/runtime-dispatch.cpp"
     unknown_target_runtime_source)
if(NOT unknown_target_project_contract MATCHES "\"diagnostic_partial\":false" OR
   unknown_target_runtime_source MATCHES "runtime-sh4-interpreter")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Unknown-Target-Beweis wurde nicht als normaler interpreterfreier Produktport gebaut")
endif()
execute_process(
  COMMAND "${unknown_target_game}" --install-disc "${fixture}/unknown-target-disc/disc.gdi"
  RESULT_VARIABLE unknown_install_result
  OUTPUT_VARIABLE unknown_install_output
  ERROR_VARIABLE unknown_install_error
  TIMEOUT 60
)
if(NOT unknown_install_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Unknown-Target-Originaldisc-Installation fehlgeschlagen: "
    "${unknown_install_output} ${unknown_install_error}")
endif()
set(unknown_target_game_command "${unknown_target_game}")
if(WIN32)
  set(unknown_target_game_command
      "${KATANA_TEST_POWERSHELL}" -NoProfile -NonInteractive
      -ExecutionPolicy Bypass -File
      "${fixture}/unknown-target-port/run-product-gate.ps1"
      -Executable "${unknown_target_game}" -WatchdogSeconds 10)
endif()
execute_process(
  COMMAND ${unknown_target_game_command}
  RESULT_VARIABLE unknown_target_result
  OUTPUT_VARIABLE unknown_target_output
  ERROR_VARIABLE unknown_target_error
  TIMEOUT 60
)
if(NOT unknown_target_result EQUAL 1 OR
   NOT unknown_target_error MATCHES "KATANA_RUNTIME_DISPATCH_ERROR" OR
   NOT unknown_target_error MATCHES "\"error\":\"unknown-target\"" OR
   NOT unknown_target_error MATCHES "\"class\":\"runtime-only\"" OR
   NOT unknown_target_error MATCHES "\"target\":\"0x8C100000\"" OR
   unknown_target_output MATCHES "KR_GUEST_PROGRAM_ENTERED|silent_failures=0" OR
   unknown_target_error MATCHES "KR_GUEST_PROGRAM_ENTERED|silent_failures=0")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Unbewiesenes RAM-Ziel brach im normalen Produktpfad nicht typisiert ab: "
    "${unknown_target_output} ${unknown_target_error}")
endif()

execute_process(
  COMMAND "${game}" --run-generated
  RESULT_VARIABLE missing_generated_result
  OUTPUT_VARIABLE missing_generated_output
  ERROR_VARIABLE missing_generated_error
  TIMEOUT 30
)
if(missing_generated_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "--run-generated akzeptiert einen Lauf ohne Bootimage")
endif()

execute_process(
  COMMAND "${game}" --gdi-debug "${fixture}/missing/disc.gdi"
  RESULT_VARIABLE missing_source_result
  OUTPUT_VARIABLE missing_source_output
  ERROR_VARIABLE missing_source_error
  TIMEOUT 30
)
if(missing_source_result EQUAL 0 OR missing_source_error MATCHES "${fixture}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Fehlende GDI liefert Erfolg oder einen unredigierten Hostpfad: ${missing_source_error}")
endif()

file(RENAME "${fixture}/port" "${fixture}/moved-port")
if(WIN32)
  set(moved_game "${fixture}/moved-port/cli_game.exe")
else()
  set(moved_game "${fixture}/moved-port/cli_game")
endif()
execute_process(
  COMMAND "${moved_game}"
  RESULT_VARIABLE moved_result
  OUTPUT_VARIABLE moved_output
  ERROR_VARIABLE moved_error
  TIMEOUT 60
)
if(NOT moved_result EQUAL 0 OR
   NOT moved_output MATCHES "KR_GUEST_PROGRAM_ENTERED" OR
   NOT moved_output MATCHES "first_problem=none" OR
   NOT EXISTS "${fixture}/disc/disc.gdi")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Verschobener lokaler Port startet nicht oder Original-GDI ging verloren: "
    "${moved_output} ${moved_error}")
endif()

endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS "Port-CLI-Fall ${KATANA_PORT_CLI_CASE} erfolgreich")
