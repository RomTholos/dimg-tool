cmake_minimum_required(VERSION 3.13)
project(dimg-tool C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Treat warnings as errors
add_compile_options(-Wall -Wextra -Werror -pedantic)

# libaaruformat — built from sibling directory
set(AARUFORMAT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libaaru-ext")
add_subdirectory("${AARUFORMAT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libaaru-ext")

# dimg-tool executable
add_executable(dimg-tool
    src/main.c
    src/cmd_info.c
    src/cmd_convert.c
    src/cmd_verify.c
)

target_include_directories(dimg-tool PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${AARUFORMAT_SOURCE_DIR}/include
)

target_link_libraries(dimg-tool PRIVATE aaruformat)

# Install
install(TARGETS dimg-tool RUNTIME DESTINATION bin)
