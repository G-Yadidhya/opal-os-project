cmake_minimum_required(VERSION 3.16)

project(OPAL_OS_Project LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_executable(
    environment_check
    src/environment_check.c
)

target_link_libraries(
    environment_check
    PRIVATE Threads::Threads
)
