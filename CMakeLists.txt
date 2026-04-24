cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL
    "Generate compile_commands.json for clangd" FORCE)

include(cmake/settings.cmake)
project(aquila)
include(cmake/compile_options.cmake)
include(cmake/third_party.cmake)
include(cmake/packages.cmake)

add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(test)
add_subdirectory(benchmark)
