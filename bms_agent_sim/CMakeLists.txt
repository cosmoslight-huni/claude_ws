cmake_minimum_required(VERSION 3.13)
project(bms_agent_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Portable core: the same sources are meant to build for ESP32 / Opta.
add_library(bms_core STATIC
  src/core/util.cpp
  src/core/config_store.cpp
  src/core/alarm_engine.cpp
  src/core/ntp_client.cpp
  src/core/snmp_ber.cpp
  src/core/snmp_agent.cpp
  src/core/bms_mib.cpp
  src/core/app.cpp
  src/core/web_api.cpp
)
target_compile_options(bms_core PRIVATE -Wall -Wextra)

add_executable(bms_agent_sim
  src/pc/main.cpp
  src/pc/pc_platform.cpp
  src/pc/sim_bms.cpp
)
target_link_libraries(bms_agent_sim bms_core)
target_compile_options(bms_agent_sim PRIVATE -Wall -Wextra)

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests bms_core)
enable_testing()
add_test(NAME core_tests COMMAND core_tests)
