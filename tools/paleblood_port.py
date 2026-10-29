#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare Paleblood's game replacements (third_party/paleblood) for the wasm build.

usage: paleblood_port.py PALEBLOOD_DIR OUT_DIR

Writes OUT_DIR/game/... (a copy of every game source and header, with the changes below) and
OUT_DIR/paleblood_hooks.cpp (the hook table core/paleblood/paleblood.h declares).

Paleblood's replacements run natively in the same process as the game's x86 code; here that code
is interpreted, so two things in them can't stay as written:

  * vtable slots, `R (*name)(Args);`, are called as native function pointers. They become
    `::rt::guest_fn<R(Args)> name;` (core/paleblood/port/runtime/original.h): the same 8 bytes,
    called through the interpreter.
  * engine::thread_pointer() reads gs:[0] with inline x86 asm. It becomes a call to
    paleblood_guest_thread_pointer(), which reads the calling guest thread's fs:[0].

Every rewrite must apply where expected, and no function-pointer declaration may be left in
another form, so a change upstream stops the build instead of producing a replacement that
traps when it runs. Each replacement named in game/hooks.csv also gets an exported getter for its
HLE thunk address (core/interpreter/hle.h), appended to the source that defines it.
"""
from __future__ import annotations

import csv
import pathlib
import re
import sys

FN_POINTER_MEMBER = re.compile(
    r'^(?P<indent>\s*)(?P<ret>[^;()/]*?)\s*\(\*\s*(?P<name>\w+)\s*\)\s*\((?P<params>[^;]*)\)\s*;'
    r'(?P<rest>.*)$')
ANY_FN_POINTER = re.compile(r'\(\*\s*\w+\s*\)\s*\(')
REPLACEMENT_DEF = re.compile(r'^extern\s+"C"\s+[^;{]*?\b(?P<name>bb_\w+)\s*\(', re.MULTILINE)

THREAD_POINTER_ASM = '    asm volatile("movq %%gs:0, %0" : "=r"(p));\n'
THREAD_POINTER_CALL = '    p = paleblood_guest_thread_pointer();\n'
THREAD_POINTER_DECL = 'extern "C" unsigned char *paleblood_guest_thread_pointer();\n\n'


def fail(msg: str) -> None:
    print(f'paleblood_port.py: {msg}', file=sys.stderr)
    sys.exit(1)


def add_include(text: str, line: str) -> str:
    """Insert `line` before the file's first #include (after #pragma once if it has none)."""
    m = re.search(r'^#include', text, re.MULTILINE)
    if m is None:
        m = re.search(r'^#pragma once\n', text, re.MULTILINE)
        if m is None:
            fail('no #include or #pragma once to anchor an include on')
        return text[:m.end()] + line + text[m.end():]
    return text[:m.start()] + line + text[m.start():]


def port_source(rel: str, text: str) -> tuple[str, int]:
    rewritten = 0
    out = []
    for line in text.splitlines(keepends=True):
        m = FN_POINTER_MEMBER.match(line.rstrip('\n'))
        if m and m['ret'].strip():
            ret = m['ret'].strip()
            out.append(f"{m['indent']}::rt::guest_fn<{ret}({m['params']})> {m['name']};{m['rest']}\n")
            rewritten += 1
        else:
            out.append(line)
    text = ''.join(out)
    for leftover in ANY_FN_POINTER.finditer(text):
        lineno = text.count('\n', 0, leftover.start()) + 1
        fail(f'{rel}:{lineno}: function pointer in a form the port does not handle')
    if rewritten and '"runtime/original.h"' not in text:
        text = add_include(text, '#include "runtime/original.h"\n')

    if 'asm' in text:
        if rel != 'game/engine/thread.h' or text.count(THREAD_POINTER_ASM) != 1:
            fail(f'{rel}: inline asm the port does not handle')
        text = text.replace(THREAD_POINTER_ASM, THREAD_POINTER_CALL)
        anchor = 'namespace engine {'
        if text.count(anchor) != 1:
            fail(f'{rel}: no single `{anchor}` to declare the thread pointer before')
        text = text.replace(anchor, THREAD_POINTER_DECL + anchor)
    return text, rewritten


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src = pathlib.Path(sys.argv[1])
    out = pathlib.Path(sys.argv[2])
    game = src / 'game'

    with open(game / 'hooks.csv', newline='') as f:
        hooks = sorted(csv.DictReader(f), key=lambda r: int(r['address'], 16))
    with open(src / 'symbols' / 'functions.csv', newline='') as f:
        sizes = {int(r['address'], 16): int(r['size']) for r in csv.DictReader(f)}
    wanted = {r['replacement'] for r in hooks}

    defined: dict[str, str] = {}
    total_rewritten = 0
    for path in sorted(game.rglob('*')):
        if path.suffix not in ('.cpp', '.c', '.h'):
            continue
        rel = path.relative_to(src).as_posix()
        text, rewritten = port_source(rel, path.read_text())
        total_rewritten += rewritten
        names = [m['name'] for m in REPLACEMENT_DEF.finditer(text) if m['name'] in wanted]
        if names and path.suffix != '.cpp':
            fail(f'{rel}: hooked replacements are only exported from C++ sources')
        if names:
            text += ('\n// Added by tools/paleblood_port.py: the HLE thunks the hook table '
                     'points the originals at.\n#include "core/interpreter/hle.h"\n')
            for name in names:
                if name in defined:
                    fail(f'{name} is defined in both {defined[name]} and {rel}')
                defined[name] = rel
                text += (f'extern "C" unsigned long long paleblood_thunk_{name}() {{\n'
                         f'    return ::Core::Interpreter::HleThunk<&{name}>::Address();\n}}\n')
        dest = out / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists() or dest.read_text() != text:
            dest.write_text(text)

    missing = sorted(wanted - defined.keys())
    if missing:
        fail(f'hooks.csv names replacements no game source defines: {", ".join(missing)}')
    table = ['// SPDX-License-Identifier: GPL-2.0-or-later',
             '// Generated by tools/paleblood_port.py from third_party/paleblood/game/hooks.csv.',
             '#include "core/paleblood/paleblood.h"', '']
    for r in hooks:
        table.append(f'extern "C" unsigned long long paleblood_thunk_{r["replacement"]}();')
    table += ['', 'namespace Core::Paleblood {', '', 'namespace {', 'const Hook kHooks[] = {']
    for r in hooks:
        address = int(r['address'], 16)
        if address not in sizes:
            fail(f'{r["address"]} ({r["replacement"]}) has no size in symbols/functions.csv')
        table.append(f'    {{{address:#010x}, {sizes[address]}, "{r["replacement"]}", '
                     f'&paleblood_thunk_{r["replacement"]}}},')
    table += ['};', '} // namespace', '',
              'std::span<const Hook> GameHooks() {', '    return kHooks;', '}', '',
              '} // namespace Core::Paleblood', '']
    table_text = '\n'.join(table)
    dest = out / 'paleblood_hooks.cpp'
    if not dest.exists() or dest.read_text() != table_text:
        dest.write_text(table_text)
    print(f'paleblood_port.py: {len(hooks)} hooks, {total_rewritten} vtable slots routed through '
          f'the interpreter')
    return 0


if __name__ == '__main__':
    sys.exit(main())
