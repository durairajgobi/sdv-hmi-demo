cmake_minimum_required(VERSION 3.22)
project(sdv-hmi-demo VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SDV_BUILD_HMI       "Build the Qt Quick cluster HMI"  ON)
option(SDV_BUILD_OTA_AGENT "Build the OTA agent"             ON)
option(SDV_BUILD_SIM       "Build the vehicle ECU simulator" ON)
option(SDV_BUILD_TESTS     "Build unit tests"                ON)
option(SDV_BUILD_COCKPIT   "Build hypervisor cockpit (IVI guest, partition manager)" ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Shared, Qt-free SOME/IP codec (also usable on non-Qt ECUs)
add_subdirectory(someip)

if(SDV_BUILD_SIM)
    add_subdirectory(vehicle-sim)
endif()

if(SDV_BUILD_HMI OR SDV_BUILD_OTA_AGENT OR SDV_BUILD_TESTS)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)
    qt_standard_project_setup()
endif()

if(SDV_BUILD_COCKPIT OR SDV_BUILD_HMI)
    add_subdirectory(cockpit)   # ivshm is also linked by the HMI (cluster partition)
endif()

if(SDV_BUILD_HMI)
    add_subdirectory(hmi)
endif()

if(SDV_BUILD_OTA_AGENT)
    add_subdirectory(ota-agent)
endif()

if(SDV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
