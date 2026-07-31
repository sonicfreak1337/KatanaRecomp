if(NOT DEFINED KATANA_CLI)
  message(FATAL_ERROR "Runtime-Buildtree-Profiltest braucht die Katana-CLI")
endif()

if(DEFINED ENV{TEMP} AND NOT "$ENV{TEMP}" STREQUAL "")
  file(TO_CMAKE_PATH
       "$ENV{TEMP}/katana-runtime-buildtree-profile-fixture"
       fixture)
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
  file(TO_CMAKE_PATH
       "$ENV{TMPDIR}/katana-runtime-buildtree-profile-fixture"
       fixture)
else()
  set(fixture
      "${CMAKE_CURRENT_BINARY_DIR}/katana-runtime-buildtree-profile-fixture")
endif()
file(REMOVE_RECURSE "${fixture}")
file(MAKE_DIRECTORY "${fixture}/source")
file(WRITE "${fixture}/source/runtime.cpp"
     "int katana_runtime_buildtree_profile_fixture() { return 0; }\n")
file(WRITE "${fixture}/source/CMakeLists.txt"
     "cmake_minimum_required(VERSION 3.25)\n"
     "project(KatanaRuntimeBuildtreeProfileFixture LANGUAGES CXX)\n"
     "add_library(katana_runtime_core STATIC runtime.cpp)\n"
     "set_target_properties(katana_runtime_core PROPERTIES "
       "EXPORT_NAME runtime_core)\n"
     "get_property(runtime_multi_config GLOBAL PROPERTY "
       "GENERATOR_IS_MULTI_CONFIG)\n"
     "if(runtime_multi_config)\n"
     "  set(runtime_multi_config 1)\n"
     "else()\n"
     "  set(runtime_multi_config 0)\n"
     "endif()\n"
     "file(WRITE \"\${CMAKE_BINARY_DIR}/KatanaRuntimeBuildProfile.txt\" "
       "\"schema=katana-runtime-build-profile-v1\\n"
       "multi_config=\${runtime_multi_config}\\n\")\n"
     "export(TARGETS katana_runtime_core "
       "FILE \"\${CMAKE_BINARY_DIR}/KatanaRuntimeBuildTargets.cmake\" "
       "NAMESPACE KatanaRecomp::)\n")

function(configure_single_config_runtime build_type output_root)
  set(build_root "${fixture}/${output_root}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${fixture}/source"
            -B "${build_root}"
            -G Ninja
            "-DCMAKE_BUILD_TYPE=${build_type}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
    TIMEOUT 30
  )
  set(targets "${build_root}/KatanaRuntimeBuildTargets.cmake")
  if(NOT configure_result EQUAL 0 OR
     NOT EXISTS "${build_root}/CMakeCache.txt" OR
     NOT EXISTS "${targets}")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Synthetischer ${build_type}-Runtimeexport konnte nicht erzeugt "
      "werden: ${configure_output} ${configure_error}")
  endif()
  file(READ "${build_root}/CMakeCache.txt" cache)
  file(READ "${targets}" target_export)
  string(TOUPPER "${build_type}" build_type_upper)
  if(NOT cache MATCHES "CMAKE_BUILD_TYPE:STRING=${build_type}" OR
     NOT target_export MATCHES
         "IMPORTED_CONFIGURATIONS ${build_type_upper}")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Synthetischer ${build_type}-Baum besitzt keinen echten "
      "Single-Config-Cache/Targetexport")
  endif()
  set("${output_root}" "${build_root}" PARENT_SCOPE)
endfunction()

function(configure_multi_config_runtime configuration output_root)
  set(build_root "${fixture}/${output_root}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${fixture}/source"
            -B "${build_root}"
            -G "Ninja Multi-Config"
            "-DCMAKE_CONFIGURATION_TYPES=${configuration}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
    TIMEOUT 30
  )
  set(targets "${build_root}/KatanaRuntimeBuildTargets.cmake")
  if(NOT configure_result EQUAL 0 OR
     NOT EXISTS "${build_root}/CMakeCache.txt" OR
     NOT EXISTS "${targets}" OR
     NOT EXISTS "${build_root}/KatanaRuntimeBuildProfile.txt")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Synthetischer Multi-Config-${configuration}-Runtimeexport konnte "
      "nicht erzeugt werden: ${configure_output} ${configure_error}")
  endif()
  file(READ "${build_root}/CMakeCache.txt" cache)
  file(READ "${build_root}/KatanaRuntimeBuildProfile.txt" profile)
  if(NOT cache MATCHES
         "CMAKE_CONFIGURATION_TYPES:[^=]+=${configuration}" OR
     NOT profile MATCHES "multi_config=1")
    file(REMOVE_RECURSE "${fixture}")
    message(FATAL_ERROR
      "Synthetischer Multi-Config-${configuration}-Baum besitzt kein "
      "passendes Generatorprofil")
  endif()
  set("${output_root}" "${build_root}" PARENT_SCOPE)
endfunction()

configure_single_config_runtime("Debug" debug_runtime)
# Eine vom Nutzer injizierte Multi-Config-Cachevariable darf den von CMake
# selbst gemeldeten Single-Config-Generatortyp nicht ueberstimmen.
file(APPEND "${debug_runtime}/CMakeCache.txt"
     "\nCMAKE_CONFIGURATION_TYPES:STRING=Release\n")
set(ENV{KATANA_RUNTIME_ROOT} "")
set(ENV{KATANA_RUNTIME_PREFIX} "")
set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
    "${debug_runtime}/KatanaRuntimeBuildTargets.cmake")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/missing.gdi"
          --output "${fixture}/debug-output"
          --target-name debug_runtime_game
  RESULT_VARIABLE debug_result
  OUTPUT_VARIABLE debug_output
  ERROR_VARIABLE debug_error
  TIMEOUT 30
)
if(debug_result EQUAL 0 OR
   NOT debug_error MATCHES
       "Single-Config-Buildtree ist nicht optimiert.*CMAKE_BUILD_TYPE=Debug" OR
   EXISTS "${fixture}/debug-output")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Echter Single-Config-Debug-Export wurde nicht vor Analyse und Ausgabe "
    "abgelehnt: ${debug_output} ${debug_error}")
endif()

# Ein optimierter Single-Config-Baum muss denselben Discovery-Pfad weiterhin
# passieren. Die absichtlich fehlende GDI scheitert erst nach der Runtimewahl;
# jede Buildtree-Profilmeldung waere daher eine Regression.
configure_single_config_runtime("Release" release_runtime)
set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
    "${release_runtime}/KatanaRuntimeBuildTargets.cmake")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/missing.gdi"
          --output "${fixture}/release-output"
          --target-name release_runtime_game
  RESULT_VARIABLE release_result
  OUTPUT_VARIABLE release_output
  ERROR_VARIABLE release_error
  TIMEOUT 30
)
unset(ENV{KATANA_RUNTIME_BUILD_TARGETS})
if(release_result EQUAL 0 OR
   release_error MATCHES
       "(Single-Config-Buildtree|Multi-Config-Buildtree|CMAKE_BUILD_TYPE)" OR
   EXISTS "${fixture}/release-output")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Optimierter Single-Config-Release-Export wurde nicht akzeptiert: "
    "${release_output} ${release_error}")
endif()

configure_multi_config_runtime("Debug" multi_debug_runtime)
set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
    "${multi_debug_runtime}/KatanaRuntimeBuildTargets.cmake")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/missing.gdi"
          --output "${fixture}/multi-debug-output"
          --target-name multi_debug_runtime_game
  RESULT_VARIABLE multi_debug_result
  OUTPUT_VARIABLE multi_debug_output
  ERROR_VARIABLE multi_debug_error
  TIMEOUT 30
)
if(multi_debug_result EQUAL 0 OR
   NOT multi_debug_error MATCHES
       "Multi-Config-Buildtree besitzt keine optimierte Konfiguration" OR
   EXISTS "${fixture}/multi-debug-output")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Multi-Config-Baum ohne optimierte Konfiguration wurde nicht abgelehnt: "
    "${multi_debug_output} ${multi_debug_error}")
endif()

configure_multi_config_runtime("MinSizeRel" multi_optimized_runtime)
set(ENV{KATANA_RUNTIME_BUILD_TARGETS}
    "${multi_optimized_runtime}/KatanaRuntimeBuildTargets.cmake")
execute_process(
  COMMAND "${KATANA_CLI}" port "${fixture}/missing.gdi"
          --output "${fixture}/multi-optimized-output"
          --target-name multi_optimized_runtime_game
  RESULT_VARIABLE multi_optimized_result
  OUTPUT_VARIABLE multi_optimized_output
  ERROR_VARIABLE multi_optimized_error
  TIMEOUT 30
)
unset(ENV{KATANA_RUNTIME_BUILD_TARGETS})
if(multi_optimized_result EQUAL 0 OR
   multi_optimized_error MATCHES
       "(Single-Config-Buildtree|Multi-Config-Buildtree|CMAKE_BUILD_TYPE)" OR
   EXISTS "${fixture}/multi-optimized-output")
  file(REMOVE_RECURSE "${fixture}")
  message(FATAL_ERROR
    "Optimierter Multi-Config-MinSizeRel-Export wurde nicht akzeptiert: "
    "${multi_optimized_output} ${multi_optimized_error}")
endif()

file(REMOVE_RECURSE "${fixture}")
