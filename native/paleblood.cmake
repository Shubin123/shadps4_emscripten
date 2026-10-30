# SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds Paleblood's game function replacements (third_party/paleblood) into upstream shadPS4's
# own x86-64 build, from an unmodified shadPS4 checkout (scripts/build_native.sh):
#
#   cmake -S <shadPS4 checkout> -B build/native -DCMAKE_PROJECT_shadPS4_INCLUDE=<this file> ...
#
# CMake includes this right after shadPS4's project(); the work is deferred to the end of its
# CMakeLists.txt, once the shadps4 target exists. The checkout isn't touched: src/core/linker.cpp
# is compiled from a copy in the build directory that installs the replacements before the game's
# entry runs, the same call the wasm build's patch makes (core/paleblood/paleblood.h).

set(PALEBLOOD_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
cmake_path(NORMAL_PATH PALEBLOOD_NATIVE_ROOT)

function(paleblood_native)
    set(root "${PALEBLOOD_NATIVE_ROOT}")
    set(new_files "${root}/patches/new_files")
    set(paleblood_dir "${root}/third_party/paleblood")
    set(out "${CMAKE_BINARY_DIR}/paleblood")

    # The replacements, with only the thread pointer read changed (tools/paleblood_port.py
    # --native), and their hook table.
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(GLOB_RECURSE files RELATIVE "${paleblood_dir}" CONFIGURE_DEPENDS
         "${paleblood_dir}/game/*.cpp" "${paleblood_dir}/game/*.c" "${paleblood_dir}/game/*.h")
    set(inputs "${paleblood_dir}/game/hooks.csv" "${paleblood_dir}/symbols/functions.csv")
    set(outputs "${out}/paleblood_hooks.cpp")
    set(game_sources)
    foreach(f IN LISTS files)
        list(APPEND inputs "${paleblood_dir}/${f}")
        list(APPEND outputs "${out}/${f}")
        if(f MATCHES "\\.cpp$")
            list(APPEND game_sources "${out}/${f}")
        endif()
    endforeach()
    add_custom_command(
        OUTPUT ${outputs}
        COMMAND "${Python3_EXECUTABLE}" "${root}/tools/paleblood_port.py" --native
                "${paleblood_dir}" "${out}"
        DEPENDS "${root}/tools/paleblood_port.py" ${inputs}
        COMMENT "Preparing Paleblood's game replacements")
    # Paleblood's own runtime interface, and its floating-point rules: no contraction, no
    # fast-math.
    set_source_files_properties(${game_sources} PROPERTIES
        INCLUDE_DIRECTORIES "${paleblood_dir}/runtime/include"
        COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")

    # src/core/linker.cpp with the call that installs them.
    set(linker_src "${CMAKE_SOURCE_DIR}/src/core/linker.cpp")
    set(linker_out "${out}/linker.cpp")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${linker_src}")
    file(READ "${linker_src}" text)
    set(include_anchor "#include \"core/linker.h\"\n")
    set(call_anchor "        // Run the game's entry function\n")
    foreach(anchor IN ITEMS "${include_anchor}" "${call_anchor}")
        string(FIND "${text}" "${anchor}" first)
        string(FIND "${text}" "${anchor}" last REVERSE)
        if(first EQUAL -1 OR NOT first EQUAL last)
            message(FATAL_ERROR "paleblood.cmake: ${linker_src} doesn't have one `${anchor}`")
        endif()
    endforeach()
    string(REPLACE "${include_anchor}"
           "${include_anchor}#include \"core/paleblood/paleblood.h\"\n" text "${text}")
    string(REPLACE "${call_anchor}"
"        // Paleblood's game function replacements (native/paleblood.cmake), now that every module
        // is loaded and relocated and before any of the game's own code runs.
        Core::Paleblood::OnGameLoaded(module->GetBaseAddress(), module->aligned_base_size,
                                      Common::ElfInfo::Instance().GameSerial(),
                                      Common::ElfInfo::Instance().AppVer());

${call_anchor}" text "${text}")
    file(WRITE "${linker_out}.new" "${text}")
    file(COPY_FILE "${linker_out}.new" "${linker_out}" ONLY_IF_DIFFERENT)

    get_target_property(sources shadps4 SOURCES)
    list(FIND sources "src/core/linker.cpp" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "paleblood.cmake: shadps4 doesn't build src/core/linker.cpp")
    endif()
    list(REMOVE_AT sources ${at})
    list(INSERT sources ${at} "${linker_out}")
    set_property(TARGET shadps4 PROPERTY SOURCES ${sources})

    set(hook_sources "${new_files}/core/paleblood/paleblood.cpp" "${out}/paleblood_hooks.cpp"
                     "${linker_out}")
    set_source_files_properties(${hook_sources} PROPERTIES INCLUDE_DIRECTORIES "${new_files}")
    # The copy's includes relative to its original's directory.
    set_property(SOURCE "${linker_out}" APPEND PROPERTY INCLUDE_DIRECTORIES
                 "${CMAKE_SOURCE_DIR}/src/core")
    target_sources(shadps4 PRIVATE ${hook_sources} ${game_sources})
    message(STATUS "Paleblood: game function replacements built into shadps4")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL paleblood_native)
