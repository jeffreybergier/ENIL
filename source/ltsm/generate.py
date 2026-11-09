#!/usr/bin/env python3
"""Reproduce native LTSM from verified extension inputs and a pinned translator.

No LINE-derived bytes are checked in. Compilation/linking of the library itself
belongs to Altivec's product graph, not this generator.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REVISION = '8b05ef3d60788642d3dad348781e18942232c3c9'
# Import names, ASM_CONST callbacks and Embind marshalling are private ABI.
# Updating the extension or MID adapter requires reviewing this host bridge.
WASM_HASH = '0f023ea348adb7c374aa56fd4b2291058516199e053aaf9e969fb1eb32517acc'


def run(*args, cwd=None):
    subprocess.run(list(map(str, args)), cwd=cwd, check=True)


def types(data):
    pos = 8
    def uint():
        nonlocal pos
        value = shift = 0
        while True:
            byte = data[pos]; pos += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
    while pos < len(data):
        section = data[pos]; pos += 1
        size = uint(); end = pos + size
        if section == 1:
            result = []
            for _ in range(uint()):
                assert data[pos] == 0x60
                pos += 1
                count = uint(); params = list(data[pos:pos + count]); pos += count
                count = uint(); returns = list(data[pos:pos + count]); pos += count
                if len(returns) > 1 or any(t not in (0x7f, 0x7e, 0x7d, 0x7c) for t in params + returns):
                    raise ValueError('Unsupported LTSM function signature')
                result.append((params, returns))
            return result
        pos = end
    raise ValueError('Missing WASM type section')


def generate(output, translator):
    cloud = ROOT / 'source/cloudflare'
    # This verifies version + hashes before extraction and applies the MID fix.
    run('node', 'scripts/build.mjs', cwd=cloud)
    wasm = (cloud / 'src/ltsm.wasm').read_bytes()
    if hashlib.sha256(wasm).hexdigest() != WASM_HASH:
        raise ValueError('Native LTSM ABI changed; review the bridge before rebuilding')
    js = (cloud / 'src/ltsm-worker.js').read_text()
    config = json.loads((cloud / 'scripts/extension.json').read_text())
    origin = 'chrome-extension://' + config['id']
    tokens = re.findall(re.escape(json.dumps(origin)) + r': "([A-Za-z0-9_-]+)"', js)
    if len(tokens) != 1:
        raise ValueError('Review LTSM signing token extraction')
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output.parent) as temporary:
        stage = Path(temporary)
        run(translator, '-m', '-f', '100', cloud / 'src/ltsm.wasm', stage / 'ltsm.c')
        # The upstream allocator aborts on OOM. Replace only the pinned module's
        # two initialization sites with fallible, request-owned allocations.
        main = (stage / 'ltsm.c').read_text()
        replacements = {
            'i->m0 = wasmMemoryAllocate(256, 256, false);': 'enil_ltsm_allocate_memory(i);',
            'wasmTableAllocate(&i->t0, 429U, 429U);': 'enil_ltsm_allocate_table(i);',
        }
        for before, after in replacements.items():
            if main.count(before) != 1:
                raise ValueError('Review translated LTSM allocation ABI')
            main = main.replace(before, after)
        (stage / 'ltsm.c').write_text(main)
        # Cooperative cancellation, including CPU-only crypto and lock waits.
        # All functions still use the checked w2c2 memory/table operations.
        function = r'((?:void|U32|U64|F32|F64) m4_ltsmFunction\d+\(wasmModuleInstance\* instance[^\n]*\) \{)'
        count = 0
        for part in stage.glob('*.c'):
            text, changed = re.subn(function, r'\1\nenil_ltsm_checkpoint(instance);', part.read_text())
            count += changed
            part.write_text('#include "ltsm_internal.h"\n' + text)
        if count != 678:
            raise ValueError('Review translated LTSM checkpoint coverage')
        # Only the generated invoker bridge casts function pointers, using the
        # WASM's actual signatures and checked runtime table type descriptors.
        scalar = {0x7f: ('U32', 'i'), 0x7e: ('U64', 'j'), 0x7d: ('F32', 'f'), 0x7c: ('F64', 'd')}
        lines = ['/* Generated; do not commit. */',
                 'static Wire call_wasm(Context *c, U32 index, const Wire *a, size_t n) {',
                 '  Wire result; wasmTableEntry e;',
                 '  memset(&result, 0, sizeof(result));',
                 '  e = wasmTableGetRef(&c->wasm.t0, index);',
                 '  if (!e.func || !e.type || e.type->parameterCount != n) fail(c, "invalid WASM call");']
        for index, (params, returns) in enumerate(types(wasm)):
            ret = scalar[returns[0]][0] if returns else 'void'
            signature = ','.join(['wasmModuleInstance*'] + [scalar[t][0] for t in params])
            arguments = ','.join(['e.instance'] + [f'a[{i}].{scalar[t][1]}' for i, t in enumerate(params)])
            expression = f'(({ret}(*)({signature}))e.func)({arguments});'
            if returns:
                expression = f'result.{scalar[returns[0]][1]} = ' + expression
            lines += [f'  if (e.type == &m4_ltsmFuncType{index}) {{', f'    {expression}', '    return result;', '  }']
        lines += ['  fail(c, "unsupported WASM call signature");', '  return result;', '}']
        (stage / 'invoke.inc').write_text('\n'.join(lines) + '\n')
        (stage / 'identity.h').write_text('/* Generated from verified LINE inputs; do not commit. */\n' +
            '#define ENIL_LTSM_ORIGIN ' + json.dumps(origin) + '\n' +
            '#define ENIL_LTSM_TOKEN ' + json.dumps(tokens[0]) + '\n' +
            '#define ENIL_LTSM_VERSION ' + json.dumps(config['version']) + '\n')
        metadata = {'extension': config['version'], 'wasm_sha256': hashlib.sha256(wasm).hexdigest(),
                    'translator': REVISION, 'verified_inputs': config['files'],
                    'generated_sha256': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                         for p in sorted(stage.iterdir())}}
        (stage / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
        # Publish the primary generator target last. A failed generation must
        # never make a partially generated tree appear current to Make.
        files = sorted(stage.iterdir(), key=lambda p: p.name == 'ltsm.c')
        for path in files:
            temporary = output / (path.name + '.tmp')
            shutil.copyfile(path, temporary)
            temporary.replace(output / path.name)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = ROOT / 'source/deps/w2c2'
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION or subprocess.check_output(['git', '-C', str(source), 'status', '--porcelain'], text=True).strip():
        raise SystemExit('Native LTSM requires the clean, pinned w2c2 submodule')
    build = args.output.resolve().parent / 'w2c2-tool'
    run('cmake', '-S', source / 'w2c2', '-B', build,
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5')
    run('cmake', '--build', build, '--target', 'w2c2', '-j', '4')
    generate(args.output.resolve(), (build / 'w2c2').resolve())


if __name__ == '__main__':
    main()
