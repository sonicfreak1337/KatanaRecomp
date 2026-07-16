if(NOT DEFINED KATANA_FIXTURE_WRITER OR NOT DEFINED KATANA_CLI)
  message(FATAL_ERROR "Port-CLI-Test braucht Fixture-Writer und CLI")
endif()

if(NOT DEFINED ENV{TEMP} OR "$ENV{TEMP}" STREQUAL "")
  message(FATAL_ERROR "Port-CLI-Test braucht ein temporaeres Verzeichnis ausserhalb des Quellbaums")
endif()
file(TO_CMAKE_PATH "$ENV{TEMP}/katana-port-cli-fixture" fixture)
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/disc")

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
  set(game "${fixture}/port/build/cli_game.exe")
else()
  set(game "${fixture}/port/build/cli_game")
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
if(game_result EQUAL 0 OR NOT game_error MATCHES "Aufruf: game")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR "Porttarget akzeptiert eine fehlende GDI: ${game_output} ${game_error}")
endif()

execute_process(
  COMMAND "${game}" "${fixture}/disc/disc.gdi"
  RESULT_VARIABLE generated_result
  OUTPUT_VARIABLE generated_output
  ERROR_VARIABLE generated_error
)
if(NOT generated_result EQUAL 0 OR
   NOT generated_output MATCHES "KR_GENERATED_RUNTIME_STARTED" OR
   NOT generated_output MATCHES "indirect_dispatches=1")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Eigenstaendiger GDI-/Runtimepfad ist nicht lauffaehig: ${generated_error}")
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
  COMMAND "${game}" --gdi "${fixture}/missing/disc.gdi"
  RESULT_VARIABLE missing_source_result
  OUTPUT_VARIABLE missing_source_output
  ERROR_VARIABLE missing_source_error
)
if(missing_source_result EQUAL 0 OR missing_source_error MATCHES "${fixture}")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Fehlende GDI liefert Erfolg oder einen unredigierten Hostpfad: ${missing_source_error}")
endif()

file(REMOVE_RECURSE "${fixture}")
message(STATUS "KR-3507/KR-4508 Port-CLI, GDI-Runtime und Hostbuild erfolgreich")
