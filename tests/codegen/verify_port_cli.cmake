if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI OR
   NOT DEFINED KATANA_RUNTIME_ROOT)
  message(FATAL_ERROR "Port-CLI-Test braucht Fixture-Writer, CLI und Runtime-SDK")
endif()

set(ENV{KATANA_RUNTIME_ROOT} "${KATANA_RUNTIME_ROOT}")

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR "Port-CLI-Test braucht ein temporaeres Verzeichnis ausserhalb des Quellbaums")
endif()
file(TO_CMAKE_PATH "$ENV{TEMP}/katana-port-cli-fixture" fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")
set(ENV{KATANA_USER_DATA_ROOT} "${fixture}/user-data")

execute_process(
  COMMAND "${KATANA_FIXTURE_WRITER}" --write-fixture "${fixture}/disc"
  RESULT_VARIABLE writer_result
  OUTPUT_VARIABLE writer_output
  ERROR_VARIABLE writer_error
)
if(NOT writer_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Synthetische Portfixture fehlgeschlagen: ${writer_error}")
endif()

execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
  RESULT_VARIABLE port_result
  OUTPUT_VARIABLE port_output
  ERROR_VARIABLE port_error
)
if(NOT port_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Port-CLI/Hostbuild fehlgeschlagen (${port_result}):\n${port_output}\n${port_error}")
endif()

if(WIN32)
  set(game "${fixture}/port/cli_game.exe")
else()
  set(game "${fixture}/port/cli_game")
endif()
if(NOT EXISTS "${game}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Port-CLI hat kein ausfuehrbares Hosttarget erzeugt")
endif()

execute_process(
  COMMAND "${game}"
  RESULT_VARIABLE missing_cache_result
  OUTPUT_VARIABLE missing_cache_output
  ERROR_VARIABLE missing_cache_error
)
if(missing_cache_result EQUAL 0 OR NOT EXISTS "${fixture}/port/content/game.katana-install" OR
   EXISTS "${fixture}/port/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Distributionsport startet ohne Originaldisc-Installation oder enthaelt Retailsektoren")
endif()

execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0 OR NOT install_output MATCHES "KATANA_DISC_INSTALL_OK" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Originaldisc-Installation fehlgeschlagen: ${install_output} ${install_error}")
endif()

execute_process(
  COMMAND "${game}"
  RESULT_VARIABLE game_result
  OUTPUT_VARIABLE game_output
  ERROR_VARIABLE game_error
)
if(NOT game_result EQUAL 0 OR NOT game_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT game_output MATCHES "KR_GUEST_PROGRAM_ENTERED")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Porttarget startet nicht aus dem lokal installierten Cache: ${game_output} ${game_error}")
endif()

execute_process(
  COMMAND "${game}" --content "${fixture}/port/user-data/content/game.katana-disc"
  RESULT_VARIABLE generated_result
  OUTPUT_VARIABLE generated_output
  ERROR_VARIABLE generated_error
)
if(NOT generated_result EQUAL 0 OR
   NOT generated_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT generated_output MATCHES "indirect_dispatches=2" OR
   NOT generated_output MATCHES "frames=0" OR
   NOT generated_output MATCHES "audio_buffers=0")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Eigenstaendiger PackedDiscSource-Runtimepfad ist nicht lauffaehig (${generated_result}): "
    "${generated_output} ${generated_error}")
endif()

file(WRITE "${fixture}/port/build/katana-incremental-marker" "keep-build-cache\n")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/disc/disc.gdi"
          --output "${fixture}/port" --target-name cli_game
  RESULT_VARIABLE incremental_port_result
  OUTPUT_VARIABLE incremental_port_output
  ERROR_VARIABLE incremental_port_error
)
if(NOT incremental_port_result EQUAL 0 OR
   NOT EXISTS "${fixture}/port/build/katana-incremental-marker" OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Inkrementeller Portbuild verliert Buildcache oder lokalen Nutzercache: "
    "${incremental_port_output} ${incremental_port_error}")
endif()
execute_process(
  COMMAND "${game}" --install-disc "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE reinstall_result
  OUTPUT_VARIABLE reinstall_output
  ERROR_VARIABLE reinstall_error
)
if(NOT reinstall_result EQUAL 0 OR
   NOT EXISTS "${fixture}/port/user-data/content/game.katana-disc")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Originaldisc wurde nach inkrementellem Publish nicht lokal reinstalliert: "
    "${reinstall_output} ${reinstall_error}")
endif()

foreach(lifecycle_case IN ITEMS running-close focus-resume-close paused-close)
  set(ENV{KATANA_PORT_LIFECYCLE_TEST} "${lifecycle_case}")
  execute_process(
    COMMAND "${game}"
    RESULT_VARIABLE lifecycle_result
    OUTPUT_VARIABLE lifecycle_output
    ERROR_VARIABLE lifecycle_error
  )
  if(NOT lifecycle_result EQUAL 0 OR
     NOT lifecycle_output MATCHES "KR_HOST_SHUTDOWN guest_dispatch_stopped=1")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Lifecycle ${lifecycle_case} beendet nativen Gastdispatch nicht: "
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
)
if(NOT trap_writer_result EQUAL 0)
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Trap-Portfixture fehlgeschlagen: ${trap_writer_error}")
endif()
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/trap-disc/disc.gdi"
          --output "${fixture}/trap-port" --target-name trap_game
  RESULT_VARIABLE trap_port_result
  OUTPUT_VARIABLE trap_port_output
  ERROR_VARIABLE trap_port_error
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
)
if(trap_result EQUAL 0 OR trap_output MATCHES "KR_GUEST_PROGRAM_ENTERED|silent_failures=0")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Trap-Einstieg erzeugte einen falschen Hauptprogrammnachweis: ${trap_output} ${trap_error}")
endif()

execute_process(
  COMMAND "${game}" --run-generated
  RESULT_VARIABLE missing_generated_result
  OUTPUT_VARIABLE missing_generated_output
  ERROR_VARIABLE missing_generated_error
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
)
if(NOT moved_result EQUAL 0 OR NOT moved_output MATCHES "KR_GUEST_PROGRAM_ENTERED" OR
   NOT EXISTS "${fixture}/disc/disc.gdi")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Verschobener lokaler Port startet nicht oder Original-GDI ging verloren: "
    "${moved_output} ${moved_error}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS "Port-CLI, PackedDiscSource, Lifecycle und unveraenderte GDI erfolgreich")
