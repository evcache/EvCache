cmake_minimum_required(VERSION 3.10)
project(vev C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "-O1 -Wall -pthread")

include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_SOURCE_DIR}/vm_tools)

file(GLOB COMMON_SOURCES
    "src/*.c"
    "vm_tools/*.c"
)

list(FILTER COMMON_SOURCES EXCLUDE REGEX "src/vevict.c$|src/vset.c$|src/vpoisoner.c$|src/vcolor.c$|src/vtest.c$|src/polluter.c$")

add_executable(vev src/vevict.c ${COMMON_SOURCES})
add_executable(vset src/vset.c ${COMMON_SOURCES})
add_executable(vpo src/vpoisoner.c ${COMMON_SOURCES})
add_executable(vcolor src/vcolor.c ${COMMON_SOURCES})
add_executable(vtest src/vtest.c ${COMMON_SOURCES})
add_executable(polluter src/polluter.c ${COMMON_SOURCES})

find_library(LIBBPF bpf REQUIRED)
find_library(LIBELF elf REQUIRED)
find_library(LIBZ z REQUIRED)
find_library(LIBZSTD zstd REQUIRED)
find_library(LIBM m REQUIRED)

foreach(tgt IN ITEMS vev vset vpo vcolor vtest polluter)
    target_link_libraries(${tgt} PRIVATE ${LIBBPF} ${LIBELF} ${LIBZ} ${LIBZSTD} ${LIBM})
endforeach()
