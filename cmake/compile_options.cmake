message(STATUS "C++ Compiler ID: ${CMAKE_CXX_COMPILER_ID}")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wno-interference-size -Wno-register -Wno-volatile)
endif()