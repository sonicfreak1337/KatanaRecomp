if(NOT DEFINED KATANA_PORT_CLI_CASE)
  message(FATAL_ERROR "Port-CLI-Dispatcher braucht KATANA_PORT_CLI_CASE")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/verify_port_cli_common.cmake")
