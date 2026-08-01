if(NOT DEFINED REQUIRED_PREREQUISITE OR
   "${REQUIRED_PREREQUISITE}" STREQUAL "")
    message(FATAL_ERROR "Missing-prerequisite gate was invoked without a prerequisite name")
endif()
if(NOT DEFINED REQUIRED_TEST OR "${REQUIRED_TEST}" STREQUAL "")
    message(FATAL_ERROR "Missing-prerequisite gate was invoked without a test name")
endif()

message(FATAL_ERROR
    "Required CTest prerequisite is unavailable for ${REQUIRED_TEST}: ${REQUIRED_PREREQUISITE}")
