# SPDX-License-Identifier: GPL-2.0-or-later
#
# Standalone build for Core::Interpreter, independent of the full shadPS4 build (which this
# repo does not vendor - see ../../README.md). It exists to prove the interpreter is correct
# against hand-written machine-code test cases using an ordinary native compiler, before any of
# the much larger work of wiring it into shadPS4 itself (Core::Memory, Core::Linker, ...) is
# attempted. Point SHADPS4_SRC_DIR at a shadPS4 checkout that has the externals/zydis and
# externals/zydis/dependencies/zycore submodules initialized.

cmake_minimum_required(VERSION 3.24)
project(shadps4_interpreter_tests CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED SHADPS4_SRC_DIR)
    set(SHADPS4_SRC_DIR "$ENV{HOME}/shadps4_src" CACHE PATH "Path to a shadPS4 checkout")
endif()

if(NOT EXISTS "${SHADPS4_SRC_DIR}/externals/zydis/CMakeLists.txt")
    message(FATAL_ERROR
        "Zydis not found under SHADPS4_SRC_DIR=${SHADPS4_SRC_DIR}. Clone "
        "https://github.com/shadps4-emu/shadps4 and run "
        "`git submodule update --init --depth 1 externals/zydis` and "
        "`git -C externals/zydis submodule update --init --depth 1 dependencies/zycore` in it, "
        "then pass -DSHADPS4_SRC_DIR=<that checkout> here.")
endif()

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
add_subdirectory("${SHADPS4_SRC_DIR}/externals/zydis" zydis EXCLUDE_FROM_ALL)

set(INTERPRETER_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../patches/new_files")

add_library(shadps4_interpreter STATIC
    "${INTERPRETER_SRC_DIR}/core/interpreter/x86_register_file.cpp"
    "${INTERPRETER_SRC_DIR}/core/interpreter/host_thunk.cpp"
    "${INTERPRETER_SRC_DIR}/core/interpreter/x86_interpreter.cpp"
)
target_include_directories(shadps4_interpreter PUBLIC "${INTERPRETER_SRC_DIR}")
target_link_libraries(shadps4_interpreter PUBLIC Zydis)

add_executable(interpreter_tests test_main.cpp)
target_link_libraries(interpreter_tests PRIVATE shadps4_interpreter)

add_executable(disasm_check disasm_check.cpp)
target_link_libraries(disasm_check PRIVATE Zydis)

enable_testing()
add_test(NAME interpreter_tests COMMAND interpreter_tests)
