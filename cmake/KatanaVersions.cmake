file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" KATANA_PROJECT_VERSION)
string(STRIP "${KATANA_PROJECT_VERSION}" KATANA_PROJECT_VERSION)
if(NOT KATANA_PROJECT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain one semantic x.y.z version")
endif()

# Compatibility values are maintained here and emitted into the public SDK.
# Increment the relevant value for every incompatible contract change.
set(KATANA_RUNTIME_ABI_VERSION 24)
set(KATANA_BLOCK_ABI_VERSION 2)
set(KATANA_PLATFORM_SERVICES_ABI_VERSION 7)
set(KATANA_BACKEND_INTERFACE_ABI_VERSION 2)
set(KATANA_PORT_PROJECT_CONTRACT_VERSION 12)
