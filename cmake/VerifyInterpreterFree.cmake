# Verify that a native runner and its core runtime archive do not contain
# decoder or diagnostic-interpreter code.
#
# Invoke this file with `cmake -P` and the following definitions:
#
#   KATANA_AUDIT_BACKEND       MSVC or GNU
#   KATANA_AUDIT_RUNNER        Final executable to inspect
#   KATANA_AUDIT_CORE_ARCHIVE  katana_runtime_core archive to inspect
#
# MSVC additionally requires:
#
#   KATANA_AUDIT_AR            lib.exe
#   KATANA_AUDIT_DUMPBIN       dumpbin.exe
#   KATANA_AUDIT_MAP           Linker map for the final executable
#
# GNU/Clang additionally requires:
#
#   KATANA_AUDIT_AR            ar or llvm-ar
#   KATANA_AUDIT_NM            nm or llvm-nm
#
# Every backend requires:
#
#   KATANA_AUDIT_MAP           Linker map for the final executable
#
# KATANA_AUDIT_EXPECT_FAILURE may be set to ON for a negative control. In that
# mode the audit succeeds only when at least one forbidden member or symbol is
# detected in the deliberately contaminated final executable or its link map.

cmake_minimum_required(VERSION 3.25)

foreach(KATANA_REQUIRED_VARIABLE
        KATANA_AUDIT_BACKEND
        KATANA_AUDIT_RUNNER
        KATANA_AUDIT_CORE_ARCHIVE
        KATANA_AUDIT_AR
        KATANA_AUDIT_MAP)
    if(NOT DEFINED ${KATANA_REQUIRED_VARIABLE} OR
       "${${KATANA_REQUIRED_VARIABLE}}" STREQUAL "")
        message(FATAL_ERROR "${KATANA_REQUIRED_VARIABLE} is required")
    endif()
endforeach()

if(NOT EXISTS "${KATANA_AUDIT_RUNNER}")
    message(FATAL_ERROR "runner does not exist: ${KATANA_AUDIT_RUNNER}")
endif()
if(NOT EXISTS "${KATANA_AUDIT_CORE_ARCHIVE}")
    message(FATAL_ERROR "core archive does not exist: ${KATANA_AUDIT_CORE_ARCHIVE}")
endif()
if(NOT EXISTS "${KATANA_AUDIT_MAP}")
    message(FATAL_ERROR "linker map does not exist: ${KATANA_AUDIT_MAP}")
endif()

string(TOUPPER "${KATANA_AUDIT_BACKEND}" KATANA_AUDIT_BACKEND)
if(NOT DEFINED KATANA_AUDIT_EXPECT_FAILURE)
    set(KATANA_AUDIT_EXPECT_FAILURE OFF)
endif()

set(KATANA_MEMBER_REPORT "")
set(KATANA_CORE_SYMBOL_REPORT "")
set(KATANA_RUNNER_SYMBOL_REPORT "")
set(KATANA_MAP_REPORT "")

if(KATANA_AUDIT_BACKEND STREQUAL "MSVC")
    if(NOT DEFINED KATANA_AUDIT_DUMPBIN OR
       "${KATANA_AUDIT_DUMPBIN}" STREQUAL "")
        message(FATAL_ERROR "KATANA_AUDIT_DUMPBIN is required for the MSVC audit")
    endif()
    execute_process(
        COMMAND "${KATANA_AUDIT_AR}" /NOLOGO /LIST "${KATANA_AUDIT_CORE_ARCHIVE}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_MEMBER_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "lib.exe archive inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    execute_process(
        COMMAND
            "${KATANA_AUDIT_DUMPBIN}"
            /NOLOGO
            /LINKERMEMBER:1
            "${KATANA_AUDIT_CORE_ARCHIVE}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_CORE_SYMBOL_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "dumpbin archive inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    execute_process(
        COMMAND
            "${KATANA_AUDIT_DUMPBIN}"
            /NOLOGO
            /SYMBOLS
            "${KATANA_AUDIT_RUNNER}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_RUNNER_SYMBOL_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "dumpbin runner inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    file(READ "${KATANA_AUDIT_MAP}" KATANA_MAP_REPORT)
elseif(KATANA_AUDIT_BACKEND STREQUAL "GNU")
    if(NOT DEFINED KATANA_AUDIT_NM OR "${KATANA_AUDIT_NM}" STREQUAL "")
        message(FATAL_ERROR "KATANA_AUDIT_NM is required for the GNU/Clang audit")
    endif()

    execute_process(
        COMMAND "${KATANA_AUDIT_AR}" t "${KATANA_AUDIT_CORE_ARCHIVE}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_MEMBER_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "archive member inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    execute_process(
        COMMAND
            "${KATANA_AUDIT_NM}"
            -C
            --defined-only
            "${KATANA_AUDIT_CORE_ARCHIVE}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_CORE_SYMBOL_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "core archive symbol inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    execute_process(
        COMMAND
            "${KATANA_AUDIT_NM}"
            -C
            --defined-only
            "${KATANA_AUDIT_RUNNER}"
        RESULT_VARIABLE KATANA_AUDIT_RESULT
        OUTPUT_VARIABLE KATANA_RUNNER_SYMBOL_REPORT
        ERROR_VARIABLE KATANA_AUDIT_ERROR
    )
    if(NOT KATANA_AUDIT_RESULT EQUAL 0)
        message(FATAL_ERROR "runner symbol inspection failed: ${KATANA_AUDIT_ERROR}")
    endif()

    file(READ "${KATANA_AUDIT_MAP}" KATANA_MAP_REPORT)
else()
    message(
        FATAL_ERROR
        "unsupported KATANA_AUDIT_BACKEND: ${KATANA_AUDIT_BACKEND}"
    )
endif()

set(
    KATANA_FORBIDDEN_MEMBERS
    dynamic_interpreter.cpp.obj
    dynamic_interpreter.cpp.o
    interpreter_boundary.cpp.obj
    interpreter_boundary.cpp.o
    controlled_fallback.cpp.obj
    controlled_fallback.cpp.o
    decoder.cpp.obj
    decoder.cpp.o
    instruction_metadata.cpp.obj
    instruction_metadata.cpp.o
)

# These fragments are deliberately narrower than the word "interpreter".
# Native dispatch diagnostics and executable-module metadata legitimately carry
# names such as DispatchFallbackAction::Interpreter and interpreter_backed.
set(
    KATANA_FORBIDDEN_SYMBOLS
    execute_dynamic_sh4_block
    PreciseInterpreterBoundary
    "?decode@sh4@katana@@"
    "katana::sh4::decode("
    "?instruction_metadata@sh4@katana@@"
    "katana::sh4::instruction_metadata("
    "ControlledFallback@runtime@katana@@"
    "ControlledFallbackError@runtime@katana@@"
    "katana::runtime::ControlledFallback::"
    "katana::runtime::ControlledFallbackError::"
    InterpreterFallback
)

if(KATANA_AUDIT_EXPECT_FAILURE)
    # A negative control is valid only when the deliberately contaminated final
    # executable or its map is caught. A separate core-archive regression must
    # never be able to make this control pass accidentally.
    set(
        KATANA_ALL_AUDIT_REPORTS
        "${KATANA_RUNNER_SYMBOL_REPORT}\n"
        "${KATANA_MAP_REPORT}"
    )
    set(
        KATANA_MEMBER_AUDIT_REPORTS
        "${KATANA_RUNNER_SYMBOL_REPORT}\n"
        "${KATANA_MAP_REPORT}"
    )
else()
    set(
        KATANA_ALL_AUDIT_REPORTS
        "${KATANA_MEMBER_REPORT}\n"
        "${KATANA_CORE_SYMBOL_REPORT}\n"
        "${KATANA_RUNNER_SYMBOL_REPORT}\n"
        "${KATANA_MAP_REPORT}"
    )
    set(
        KATANA_MEMBER_AUDIT_REPORTS
        "${KATANA_MEMBER_REPORT}\n"
        "${KATANA_RUNNER_SYMBOL_REPORT}\n"
        "${KATANA_MAP_REPORT}"
    )
endif()
string(
    TOLOWER
    "${KATANA_MEMBER_AUDIT_REPORTS}"
    KATANA_LOWER_MEMBER_AUDIT_REPORTS
)

set(KATANA_AUDIT_VIOLATIONS)
foreach(KATANA_FORBIDDEN_MEMBER IN LISTS KATANA_FORBIDDEN_MEMBERS)
    string(
        TOLOWER
        "${KATANA_FORBIDDEN_MEMBER}"
        KATANA_LOWER_FORBIDDEN_MEMBER
    )
    string(
        REPLACE
        "."
        "\\."
        KATANA_FORBIDDEN_MEMBER_PATTERN
        "${KATANA_LOWER_FORBIDDEN_MEMBER}"
    )
    string(
        REGEX MATCH
        "(^|[^A-Za-z0-9_.])${KATANA_FORBIDDEN_MEMBER_PATTERN}([^A-Za-z0-9_.]|$)"
        KATANA_FORBIDDEN_MATCH
        "${KATANA_LOWER_MEMBER_AUDIT_REPORTS}"
    )
    if(NOT "${KATANA_FORBIDDEN_MATCH}" STREQUAL "")
        list(APPEND KATANA_AUDIT_VIOLATIONS "member:${KATANA_FORBIDDEN_MEMBER}")
    endif()
endforeach()

foreach(KATANA_FORBIDDEN_SYMBOL IN LISTS KATANA_FORBIDDEN_SYMBOLS)
    string(
        FIND
        "${KATANA_ALL_AUDIT_REPORTS}"
        "${KATANA_FORBIDDEN_SYMBOL}"
        KATANA_FORBIDDEN_OFFSET
    )
    if(NOT KATANA_FORBIDDEN_OFFSET EQUAL -1)
        list(APPEND KATANA_AUDIT_VIOLATIONS "symbol:${KATANA_FORBIDDEN_SYMBOL}")
    endif()
endforeach()

if(KATANA_AUDIT_VIOLATIONS)
    list(REMOVE_DUPLICATES KATANA_AUDIT_VIOLATIONS)
    list(JOIN KATANA_AUDIT_VIOLATIONS ", " KATANA_AUDIT_VIOLATION_REPORT)
    if(KATANA_AUDIT_EXPECT_FAILURE)
        message(
            STATUS
            "interpreter-free negative control detected: "
            "${KATANA_AUDIT_VIOLATION_REPORT}"
        )
        return()
    endif()
    message(
        FATAL_ERROR
        "interpreter-free link audit failed: ${KATANA_AUDIT_VIOLATION_REPORT}"
    )
endif()

if(KATANA_AUDIT_EXPECT_FAILURE)
    message(
        FATAL_ERROR
        "interpreter-free negative control did not detect forbidden linkage"
    )
endif()

message(
    STATUS
    "interpreter-free link audit passed for ${KATANA_AUDIT_RUNNER}"
)
