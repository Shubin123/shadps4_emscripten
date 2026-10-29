# SPDX-License-Identifier: GPL-2.0-or-later
#
# WebAssembly (Emscripten, wasm64 + pthreads) build of shadPS4.
#
# Compiles upstream shadPS4's own sources (build/patched/src: upstream src/ at UPSTREAM_COMMIT
# with patches/*.patch applied - see scripts/setup_tree.sh) plus this repo's new files
# (patches/new_files), against the externals of an ordinary shadPS4 checkout (SHADPS4_SRC_DIR).
# It does not use upstream's CMakeLists.txt: most of the 45 externals that file configures have no
# meaning in a browser (libusb, miniupnpc, discord-rpc, openal-soft's native backends, the Vulkan
# loader, ...), and the ones that do are either header-only or have an Emscripten port.
#
#   emcmake cmake -S emscripten -B build/wasm -G Ninja -DSHADPS4_SRC_DIR=~/shadps4_src
#   cmake --build build/wasm

cmake_minimum_required(VERSION 3.24)
project(shadps4_wasm C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "configure this with emcmake")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SHADPS4_SRC_DIR "$ENV{HOME}/shadps4_src" CACHE PATH "shadPS4 checkout with submodules")
set(PATCHED_SRC "${REPO_ROOT}/build/patched/src" CACHE PATH "Patched upstream src/")
set(NEW_FILES "${REPO_ROOT}/patches/new_files")
set(EXT "${SHADPS4_SRC_DIR}/externals")

if(NOT EXISTS "${PATCHED_SRC}/core/linker.cpp")
    message(FATAL_ERROR "${PATCHED_SRC} missing - run scripts/setup_tree.sh first")
endif()

# Flags every translation unit (ours and the externals') must agree on: wasm64 changes the
# target triple, and pthreads/wasm-exceptions change the ABI.
set(WASM_FLAGS -sMEMORY64=1 -pthread -fwasm-exceptions -msimd128)
add_compile_options(${WASM_FLAGS} -Wno-ignored-attributes -Wno-unknown-attributes
                    -Wno-deprecated-declarations -Wno-unused-command-line-argument)
add_link_options(${WASM_FLAGS})

# --- externals ----------------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)

add_subdirectory("${EXT}/fmt" fmt EXCLUDE_FROM_ALL SYSTEM)

set(ZYDIS_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZYDIS_BUILD_DOXYGEN OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXT}/zydis" zydis EXCLUDE_FROM_ALL SYSTEM)
# Zycore recognizes Emscripten (ZYAN_EMSCRIPTEN) but only implements its thread/sync/memory APIs
# for ZYAN_WINDOWS/ZYAN_POSIX; Emscripten's libc is POSIX, so select that implementation.
target_compile_definitions(Zycore PUBLIC ZYAN_POSIX)

set(SPDLOG_NO_EXCEPTIONS ON CACHE BOOL "" FORCE)
set(SPDLOG_DISABLE_GLOBAL_LOGGER ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)
set(fmt_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake" CACHE PATH "" FORCE)
add_subdirectory("${EXT}/spdlog" spdlog EXCLUDE_FROM_ALL SYSTEM)
target_compile_definitions(spdlog PUBLIC SPDLOG_FUNCTION=__func__)

add_subdirectory("${EXT}/toml11" toml11 EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory("${EXT}/magic_enum" magic_enum EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory("${EXT}/robin-map" robin_map EXCLUDE_FROM_ALL SYSTEM)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory("${EXT}/json" json EXCLUDE_FROM_ALL SYSTEM)

option(PUGIXML_NO_EXCEPTIONS "" ON)
add_subdirectory("${EXT}/pugixml" pugixml EXCLUDE_FROM_ALL SYSTEM)

add_subdirectory("${EXT}/miniz" miniz EXCLUDE_FROM_ALL SYSTEM)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${EXT}/zstd/build/cmake" zstd EXCLUDE_FROM_ALL SYSTEM)

add_library(zarchive STATIC "${EXT}/zarchive/src/zarchivewriter.cpp"
            "${EXT}/zarchive/src/zarchivereader.cpp" "${EXT}/zarchive/src/sha_256.c")
target_include_directories(zarchive SYSTEM PUBLIC "${EXT}/zarchive/include")
target_link_libraries(zarchive PRIVATE libzstd_static)

add_library(xxhash STATIC "${EXT}/xxhash/xxhash.c")
target_include_directories(xxhash SYSTEM PUBLIC "${EXT}/xxhash")

file(GLOB LIBATRAC9_SOURCES "${EXT}/LibAtrac9/C/src/*.c")
add_library(LibAtrac9 STATIC ${LIBATRAC9_SOURCES})
target_include_directories(LibAtrac9 SYSTEM INTERFACE "${EXT}/LibAtrac9/C/src")

# Dear ImGui itself: the system dialogs (msgdialog, savedata, ime, ...) call into it. Nothing is
# rendered in the wasm build (the overlay renderer is Vulkan), but the calls must link.
add_library(imgui STATIC "${EXT}/imgui/imgui.cpp" "${EXT}/imgui/imgui_draw.cpp"
            "${EXT}/imgui/imgui_tables.cpp" "${EXT}/imgui/imgui_widgets.cpp")
target_include_directories(imgui SYSTEM PUBLIC "${EXT}/imgui")
target_compile_definitions(imgui PRIVATE IMGUI_USER_CONFIG="${PATCHED_SRC}/imgui/imgui_config.h")
target_include_directories(imgui PRIVATE "${PATCHED_SRC}")

add_library(ext_headers INTERFACE)
target_include_directories(ext_headers SYSTEM INTERFACE
    "${EXT}/ext-boost"
    "${EXT}/half/include"
    "${EXT}/stb"
    "${EXT}/minimp3"
    "${EXT}/dr_flac"
    "${EXT}/renderdoc"
    "${EXT}/tracy/public"
    "${EXT}/vulkan-headers/include"
    "${EXT}/CLI11/include"
    "${EXT}/date/include"
    "${EXT}/imgui"
    "${EXT}/xbyak")

# --- shadPS4 sources ----------------------------------------------------------------------------
file(GLOB_RECURSE UPSTREAM_SOURCES CONFIGURE_DEPENDS
    "${PATCHED_SRC}/common/*.cpp"
    "${PATCHED_SRC}/core/*.cpp"
    "${PATCHED_SRC}/input/*.cpp"
    "${PATCHED_SRC}/video_core/amdgpu/*.cpp"
    "${PATCHED_SRC}/shader_recompiler/*.cpp")
list(APPEND UPSTREAM_SOURCES "${PATCHED_SRC}/emulator.cpp")

# Excluded: things with no browser equivalent, or whose only implementation is native-only
# (x86 code patching/signals, Xbyak stubs), or that need externals not built here yet
# (FFmpeg, libusb, networking/TLS, ImGui overlays, OpenAL). Libraries removed here are also
# removed from Libraries::InitHLELibs by the patch, so their NIDs fall back to the generic
# unresolved-import stub, exactly like any other HLE function shadPS4 hasn't written yet.
set(EXCLUDE_REGEX
    "/shader_recompiler/backend/spirv/"
    "/core/cpu_patches\\.cpp$"
    "/core/aerolib/stubs\\.cpp$"
    "/core/devtools/"
    "/core/debugger\\.cpp$"
    "/core/ipc/"
    "/common/discord_rpc_handler\\.cpp$"
    "/common/ntapi\\.cpp$"
    "/core/libraries/kernel/threads/stack_asm\\.cpp$"
    "/core/libraries/fiber/fiber_context\\.cpp$"
    "/core/libraries/ajm/"
    "/core/libraries/avplayer/"
    "/core/libraries/videodec/"
    "/core/libraries/music/"
    "/core/libraries/usbd/"
    "/core/libraries/net/"
    "/core/libraries/network/"
    "/core/libraries/np/"
    "/core/libraries/rudp/"
    "/core/libraries/invitation_dialog/"
    "/core/libraries/net/net_upnp\\.cpp$"
    "/core/libraries/np/np_handler/"
    "/core/libraries/np/np_matching2/np_matching2_internal\\.cpp$"
    "/core/libraries/np/np_matching2/np_matching2_mm\\.cpp$"
    "/core/libraries/np/trophy_ui\\.cpp$"
    "/core/net/"
    "/core/libraries/audio/openal_audio_out\\.cpp$"
    "/core/libraries/audio3d/audio3d_openal\\.cpp$"
)
foreach(re IN LISTS EXCLUDE_REGEX)
    list(FILTER UPSTREAM_SOURCES EXCLUDE REGEX "${re}")
endforeach()

file(GLOB_RECURSE NEW_SOURCES CONFIGURE_DEPENDS "${NEW_FILES}/*.cpp")
list(FILTER NEW_SOURCES EXCLUDE REGEX "/web/main_web\\.cpp$")

set(SCM_REV_IN "${PATCHED_SRC}/common/scm_rev.cpp.in")
file(READ "${REPO_ROOT}/UPSTREAM_COMMIT" GIT_REV)
string(STRIP "${GIT_REV}" GIT_REV)
string(SUBSTRING "${GIT_REV}" 0 7 GIT_DESC)
set(APP_VERSION "0.20.1 WIP (wasm)")
set(APP_IS_RELEASE false)
set(GIT_BRANCH "main")
set(GIT_REMOTE_NAME "origin")
set(GIT_REMOTE_URL "https://github.com/shadps4-emu/shadPS4")
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")
configure_file("${SCM_REV_IN}" "${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp" @ONLY)

add_library(shadps4_core STATIC ${UPSTREAM_SOURCES} ${NEW_SOURCES}
            "${CMAKE_CURRENT_BINARY_DIR}/scm_rev.cpp")
target_include_directories(shadps4_core PUBLIC "${NEW_FILES}" "${PATCHED_SRC}" "${PATCHED_SRC}/..")
target_compile_definitions(shadps4_core PUBLIC
    BOOST_ASIO_STANDALONE
    IMGUI_USER_CONFIG="imgui/imgui_config.h"
    ARCH_WASM=1)
# Xbyak picks 32- or 64-bit code generation from the host target; the only Xbyak user built here
# (the shader SRT walker) emits x86-64 that the interpreter runs.
set_source_files_properties("${PATCHED_SRC}/shader_recompiler/ir/passes/flatten_extended_userdata_pass.cpp"
    PROPERTIES COMPILE_DEFINITIONS "XBYAK64=1;XBYAK64_GCC=1")
target_compile_options(shadps4_core PRIVATE -sUSE_SDL=3 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_FREETYPE=1)
target_link_libraries(shadps4_core PUBLIC
    ext_headers fmt::fmt Zydis toml11::toml11 magic_enum::magic_enum tsl::robin_map
    nlohmann_json::nlohmann_json pugixml::pugixml miniz libzstd_static xxhash LibAtrac9
    spdlog::spdlog zarchive imgui)

# --- executables --------------------------------------------------------------------------------
# Memory layout (see patches/new_files/core/wasm_layout.h): guest memory occupies linear memory
# below 12GB at its own addresses, so the emulator's static data, stacks and heap start at 12GB.
set(SHADPS4_LINK_FLAGS
    -sGLOBAL_BASE=12GB
    -sINITIAL_MEMORY=13GB
    -sMAXIMUM_MEMORY=16GB
    -sALLOW_MEMORY_GROWTH=1
    -sPROXY_TO_PTHREAD=1
    -sPTHREAD_POOL_SIZE=24
    -sSTACK_SIZE=2MB
    -sDEFAULT_PTHREAD_STACK_SIZE=2MB
    -sUSE_SDL=3 -sUSE_ZLIB=1 -sUSE_LIBPNG=1 -sUSE_FREETYPE=1
    -sEXPORTED_RUNTIME_METHODS=callMain,FS,UTF8ToString,HEAPU8
    -sEXPORTED_FUNCTIONS=_main,_shadps4_display_release,_shadps4_pad_button,_shadps4_pad_axis,_shadps4_request_quit
    -sMALLOC=mimalloc
    --profiling-funcs
    -Wl,--error-limit=0)

# Node build, for headless runs and tests: the host filesystem is visible directly.
add_executable(shadps4_node "${NEW_FILES}/web/main_web.cpp")
target_link_libraries(shadps4_node PRIVATE shadps4_core)
target_link_options(shadps4_node PRIVATE ${SHADPS4_LINK_FLAGS} -sNODERAWFS=1 -sEXIT_RUNTIME=1
                    -sENVIRONMENT=node "--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js")

# Browser build: the page mounts the game folder and drives main (see shadps4_web).
add_executable(shadps4 "${NEW_FILES}/web/main_web.cpp")
target_link_libraries(shadps4 PRIVATE shadps4_core)
# webgpu.js gives the page's WebGPU device to the renderer (video_core/renderer_web/web_gpu.h).
set(WEBGPU_LINK_FLAGS "--pre-js=${CMAKE_CURRENT_LIST_DIR}/webgpu.js"
    "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$addRunDependency,$removeRunDependency")
target_link_options(shadps4 PRIVATE ${SHADPS4_LINK_FLAGS} -sINVOKE_RUN=0 -sMODULARIZE=1
                    -sEXPORT_NAME=createShadPS4 -sENVIRONMENT=web,worker
                    "--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js" ${WEBGPU_LINK_FLAGS})

# Interpreter tests (tests/interpreter): `node build/wasm/interpreter_tests.js`.
add_executable(interpreter_tests "${REPO_ROOT}/tests/interpreter/test_main.cpp")
target_link_libraries(interpreter_tests PRIVATE shadps4_core)
target_include_directories(interpreter_tests PRIVATE "${REPO_ROOT}/tests/interpreter")
target_link_options(interpreter_tests PRIVATE ${SHADPS4_LINK_FLAGS} -sNODERAWFS=1 -sEXIT_RUNTIME=1
                    -sENVIRONMENT=node "--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js")

# Shader translation check (tests/shaders): GCN shaders from a game -> recompiler -> WGSL.
add_executable(translate_shaders "${REPO_ROOT}/tests/shaders/translate_main.cpp")
target_link_libraries(translate_shaders PRIVATE shadps4_core)
target_link_options(translate_shaders PRIVATE ${SHADPS4_LINK_FLAGS} -sNODERAWFS=1 -sEXIT_RUNTIME=1
                    -sENVIRONMENT=node "--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js")

# WebGPU compute path (tests/gpu): GCN programs dispatched through Web::ComputePipelines in a
# browser; `node tests/gpu/run_gpu_tests.mjs` serves the page and drives headless Chromium.
add_executable(gpu_tests "${REPO_ROOT}/tests/gpu/compute_test.cpp")
target_link_libraries(gpu_tests PRIVATE shadps4_core)
target_link_options(gpu_tests PRIVATE ${SHADPS4_LINK_FLAGS} -sEXIT_RUNTIME=1
                    -sENVIRONMENT=web,worker "--pre-js=${CMAKE_CURRENT_LIST_DIR}/pre.js"
                    ${WEBGPU_LINK_FLAGS})

# --pre-js files aren't link inputs as far as CMake knows; relink when they change.
foreach(target shadps4_node shadps4 interpreter_tests translate_shaders gpu_tests)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/pre.js")
endforeach()
foreach(target shadps4 gpu_tests)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/webgpu.js")
endforeach()
