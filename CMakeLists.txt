cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)

# Enable testing
enable_testing()

include(cmake/settings.cmake)
project(aquila)
include(cmake/compile_options.cmake)
include(cmake/third_party.cmake)
include(cmake/packages.cmake)

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(exchange/gate)
    add_subdirectory(tools/gate)
    add_subdirectory(benchmark)
    add_subdirectory(test)
endif ()

