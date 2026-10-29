# patches/

- `0001-emscripten.patch` - every change to an upstream shadPS4 file, as one diff against the
  commit in `../UPSTREAM_COMMIT`. All of it is behind `ARCH_WASM` (defined for Emscripten in
  `common/arch.h`), so native builds are unaffected.
- `new_files/` - new sources, laid out like upstream's `src/`
  (`new_files/core/interpreter/cpu.cpp` is compiled as if it were `src/core/interpreter/cpu.cpp`).

`../scripts/setup_tree.sh` applies the patch to a copy of upstream's `src/` in `../build/patched`
(its own git repo, tagged `upstream`); after editing there, `../scripts/export_patches.sh` writes
the diff back here. The build compiles `build/patched/src` and `new_files/` together.
