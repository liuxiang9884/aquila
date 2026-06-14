cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Generate compile_commands.json for clangd" FORCE)

include(cmake/settings.cmake)
project(aquila)
include(cmake/compile_options.cmake)
include(cmake/third_party.cmake)
include(cmake/packages.cmake)

option(AQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV
       "Enable LeadLag market calculation diagnostic CSV output" OFF)

set(AQUILA_ORDER_ACK_DIAG_LEVEL_DEFAULT 4)
if(DEFINED AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
   AND NOT DEFINED AQUILA_ORDER_ACK_DIAG_LEVEL)
    if(AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION)
        set(AQUILA_ORDER_ACK_DIAG_LEVEL_DEFAULT 4)
    else()
        set(AQUILA_ORDER_ACK_DIAG_LEVEL_DEFAULT 3)
    endif()
endif()

set(AQUILA_ORDER_ACK_DIAG_LEVEL "${AQUILA_ORDER_ACK_DIAG_LEVEL_DEFAULT}"
    CACHE STRING
    "Build Order Ack outlier diagnostic attribution level: 0=off, 1=correlation, 2=runtime/write, 3=TCP_INFO, 4=socket timestamping, 5=pcap/Gate header support")
set_property(CACHE AQUILA_ORDER_ACK_DIAG_LEVEL PROPERTY STRINGS
             0 1 2 3 4 5)
if(NOT AQUILA_ORDER_ACK_DIAG_LEVEL MATCHES "^[0-5]$")
    message(FATAL_ERROR
            "AQUILA_ORDER_ACK_DIAG_LEVEL must be an integer in [0, 5]")
endif()

set(AQUILA_DATA_SESSION_DIAG_LEVEL "0"
    CACHE STRING
    "Build data session latency diagnostic level: 0=off, 1=outlier correlation, 2=user path, 3=TCP_INFO, 4=socket RX timestamping")
set_property(CACHE AQUILA_DATA_SESSION_DIAG_LEVEL PROPERTY STRINGS
             0 1 2 3 4)
if(NOT AQUILA_DATA_SESSION_DIAG_LEVEL MATCHES "^[0-4]$")
    message(FATAL_ERROR
            "AQUILA_DATA_SESSION_DIAG_LEVEL must be an integer in [0, 4]")
endif()

set(AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION OFF)
if(AQUILA_ORDER_ACK_DIAG_LEVEL GREATER_EQUAL 4
   OR AQUILA_DATA_SESSION_DIAG_LEVEL GREATER_EQUAL 4)
    set(AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION ON)
endif()
set(AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
    "${AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION}" CACHE BOOL
    "Deprecated compatibility cache value derived from Order Ack/Data Session diagnostic levels"
    FORCE)

enable_testing()

add_subdirectory(core)
add_subdirectory(exchange)
add_subdirectory(strategy)
add_subdirectory(evaluation)
add_subdirectory(monitor)
add_subdirectory(tools)
add_subdirectory(test)
add_subdirectory(benchmark)
