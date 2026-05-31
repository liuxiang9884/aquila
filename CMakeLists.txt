cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Generate compile_commands.json for clangd" FORCE)

include(cmake/settings.cmake)
project(aquila)
include(cmake/compile_options.cmake)
include(cmake/third_party.cmake)
include(cmake/packages.cmake)

option(AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION
       "Build Linux socket timestamping attribution support" ON)

enable_testing()

add_subdirectory(core)
add_subdirectory(exchange)
add_subdirectory(strategy)
add_subdirectory(evaluation)
add_subdirectory(monitor)
add_subdirectory(tools)
add_subdirectory(test)
add_subdirectory(benchmark)
