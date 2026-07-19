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
  RESULT_VARIABLE game_result
  OUTPUT_VARIABLE game_output
  ERROR_VARIABLE game_error
)
if(NOT game_result EQUAL 0 OR
   NOT game_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT game_output MATCHES "KR_GUEST_PROGRAM_ENTERED")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Porttarget startet nicht aus dem Standard-Disc-Pack: ${game_output} ${game_error}")
endif()

execute_process(
  COMMAND "${game}" --content "${fixture}/port/content/game.katana-disc"
  RESULT_VARIABLE generated_result
  OUTPUT_VARIABLE generated_output
  ERROR_VARIABLE generated_error
)
if(NOT generated_result EQUAL 0 OR
   NOT generated_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT generated_output MATCHES "indirect_dispatches=1" OR
   NOT generated_output MATCHES "frames=0" OR
   NOT generated_output MATCHES "audio_buffers=1")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Eigenstaendiger PackedDiscSource-Runtimepfad ist nicht lauffaehig (${generated_result}): "
    "${generated_output} ${generated_error}")
endif()

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

file(REMOVE_RECURSE "${fixture}/disc")
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
if(NOT moved_result EQUAL 0 OR NOT moved_output MATCHES "KR_GUEST_PROGRAM_ENTERED")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Verschobener Port startet nach Entfernen der GDI nicht: ${moved_output} ${moved_error}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS "Port-CLI, PackedDiscSource und GDI-unabhaengiger Hostbuild erfolgreich")
