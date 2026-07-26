cmake_minimum_required(VERSION 3.25)

foreach(KATANA_REQUIRED_VARIABLE
        KATANA_SOURCE_IDENTITY_GIT
        KATANA_SOURCE_IDENTITY_ROOT
        KATANA_SOURCE_IDENTITY_EXPECTED_COMMIT)
    if(NOT DEFINED ${KATANA_REQUIRED_VARIABLE} OR
       "${${KATANA_REQUIRED_VARIABLE}}" STREQUAL "")
        message(FATAL_ERROR "${KATANA_REQUIRED_VARIABLE} is required")
    endif()
endforeach()

if(NOT EXISTS "${KATANA_SOURCE_IDENTITY_GIT}")
    message(
        FATAL_ERROR
        "Git executable does not exist: ${KATANA_SOURCE_IDENTITY_GIT}"
    )
endif()
if(NOT EXISTS "${KATANA_SOURCE_IDENTITY_ROOT}/.git")
    message(
        FATAL_ERROR
        "External SH-4 evidence requires a Git checkout: "
        "${KATANA_SOURCE_IDENTITY_ROOT}"
    )
endif()

execute_process(
    COMMAND
        "${KATANA_SOURCE_IDENTITY_GIT}"
        -c "safe.directory=${KATANA_SOURCE_IDENTITY_ROOT}"
        -C "${KATANA_SOURCE_IDENTITY_ROOT}"
        rev-parse HEAD
    RESULT_VARIABLE KATANA_SOURCE_IDENTITY_RESULT
    OUTPUT_VARIABLE KATANA_SOURCE_IDENTITY_HEAD
    ERROR_VARIABLE KATANA_SOURCE_IDENTITY_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT KATANA_SOURCE_IDENTITY_RESULT EQUAL 0)
    message(
        FATAL_ERROR
        "Cannot determine Katana HEAD: ${KATANA_SOURCE_IDENTITY_ERROR}"
    )
endif()
string(TOLOWER "${KATANA_SOURCE_IDENTITY_HEAD}" KATANA_SOURCE_IDENTITY_HEAD)
if(NOT "${KATANA_SOURCE_IDENTITY_HEAD}" STREQUAL
   "${KATANA_SOURCE_IDENTITY_EXPECTED_COMMIT}")
    message(
        FATAL_ERROR
        "Katana HEAD changed after configuration: expected "
        "${KATANA_SOURCE_IDENTITY_EXPECTED_COMMIT}, got "
        "${KATANA_SOURCE_IDENTITY_HEAD}. Reconfigure before producing evidence."
    )
endif()

execute_process(
    COMMAND
        "${KATANA_SOURCE_IDENTITY_GIT}"
        -c "safe.directory=${KATANA_SOURCE_IDENTITY_ROOT}"
        -C "${KATANA_SOURCE_IDENTITY_ROOT}"
        status --porcelain=v1 --untracked-files=all
    RESULT_VARIABLE KATANA_SOURCE_IDENTITY_RESULT
    OUTPUT_VARIABLE KATANA_SOURCE_IDENTITY_STATUS
    ERROR_VARIABLE KATANA_SOURCE_IDENTITY_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT KATANA_SOURCE_IDENTITY_RESULT EQUAL 0)
    message(
        FATAL_ERROR
        "Cannot inspect Katana worktree: ${KATANA_SOURCE_IDENTITY_ERROR}"
    )
endif()
if(NOT "${KATANA_SOURCE_IDENTITY_STATUS}" STREQUAL "")
    message(
        FATAL_ERROR
        "External SH-4 evidence requires a clean Katana index and worktree. "
        "Commit or remove all source changes before building the SST runner."
    )
endif()

message(
    STATUS
    "Katana source identity verified: ${KATANA_SOURCE_IDENTITY_HEAD}"
)
