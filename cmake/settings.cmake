#vcpkg
set(VCPKG_ROOT "$ENV{HOME}/vcpkg/scripts/buildsystems/vcpkg.cmake")
set(CMAKE_TOOLCHAIN_FILE ${VCPKG_ROOT} CACHE STRING "Vcpkg toolchain file")

if (APPLE)
    set(VCPKG_INCLUDE "$ENV{HOME}/vcpkg/installed/arm64-osx/include")
    set(CMAKE_OSX_SYSROOT "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
            CACHE STRING "SDK_PATH" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.5")
    set(CMAKE_OSX_ARCHITECTURES "arm64")

elseif (WIN32)
    set(VCPKG_INCLUDE "$ENV{HOME}/vcpkg/installed/x64-windows/include")
elseif (UNIX)
    set(VCPKG_INCLUDE "$ENV{HOME}/vcpkg/installed/x64-linux/include")

endif ()


# aquila
set(AQUILA_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR})
message(STATUS "AQUILA_INCLUDE: " ${AQUILA_INCLUDE})



