cmake_minimum_required(VERSION 3.20)

project(ush
    VERSION 0.1.0
    DESCRIPTION "ush - unnamed shell, a POSIX-compatible shell"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
)

option(USH_BUILD_TESTS "Build ush unit/integration tests" ON)

add_subdirectory(src)

if(USH_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.6.0
    )
    FetchContent_MakeAvailable(Catch2)

    add_subdirectory(tests)
endif()
