# patches/

`new_files/` mirrors the path new interpreter source will have inside a shadPS4 checkout
(`new_files/core/interpreter/x86_interpreter.cpp` -> `src/core/interpreter/x86_interpreter.cpp`).
Nothing here has been applied to a real shadPS4 checkout yet - there is no CMake integration
(see PROJECT.md, M6) and no diff-style patches against existing shadPS4 files yet either. Right
now this only builds standalone via `../tests/interpreter/CMakeLists.txt`.

Once M6 (Emscripten/CMake build integration) starts, expect this directory to also gain unified
diffs against files shadPS4 already has (`CMakeLists.txt`, `src/core/linker.cpp`, ...), applied
by a build script the way azahar_web's `patches/*.patch` are - not committed as a second
first-class copy of shadPS4's own build files.
