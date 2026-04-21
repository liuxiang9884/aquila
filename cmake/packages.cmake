include(FetchContent)
FetchContent_Declare(
        nova
        GIT_REPOSITORY git@github.com:dcfintech/nova.git
        GIT_TAG main
)
FetchContent_MakeAvailable(nova)

set(NOVA_INCLUDE ${nova_SOURCE_DIR}/include)
message(STATUS "NOVA_INCLUDE: " ${NOVA_INCLUDE})


