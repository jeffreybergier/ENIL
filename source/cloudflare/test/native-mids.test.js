import { readFile } from 'node:fs/promises';
import { unzipSync } from 'fflate';
import { describe, it, expect } from 'vitest';
import { adaptNativeMids } from '../scripts/native-mids.mjs';

function reader(bytes, pos = 0) {
  return { pos, uint() { let n = 0, shift = 0, b;
    do { b = bytes[this.pos++]; n += (b & 127) * 2 ** shift; shift += 7; } while (b & 128);
    return n;
  } };
}
function sections(wasm) {
  const r = reader(wasm, 8), result = [];
  while (r.pos < wasm.length) {
    const id = wasm[r.pos++], size = r.uint();
    result.push({ id, offset: r.pos, bytes: wasm.subarray(r.pos, r.pos + size) }); r.pos += size;
  }
  return result;
}
function bodies(section) {
  const r = reader(section.bytes), count = r.uint(), result = [];
  for (let i = 0; i < count; i++) {
    const size = r.uint();
    result.push({ offset: section.offset + r.pos, bytes: section.bytes.subarray(r.pos, r.pos + size) }); r.pos += size;
  }
  return result;
}
const crx = await readFile(new URL('../.cache/line.crx', import.meta.url));
const archive = unzipSync(crx.subarray(12 + crx.readUInt32LE(8)), {
  filter: entry => entry.name === 'static/js/ltsm.wasm',
});
const original = Buffer.from(archive['static/js/ltsm.wasm']);

describe('pinned native MID adapter', () => {
  it('only extends the MID helper and retains every original Chrome instruction', () => {
    const patched = adaptNativeMids(original);
    expect(WebAssembly.validate(patched)).toBe(true);
    const oldSections = sections(original), newSections = sections(patched);
    expect(newSections.map(x => x.id)).toEqual(oldSections.map(x => x.id));
    for (let i = 0; i < oldSections.length; i++) {
      if (oldSections[i].id !== 10) expect(newSections[i].bytes.equals(oldSections[i].bytes)).toBe(true);
      else {
        const oldBodies = bodies(oldSections[i]), newBodies = bodies(newSections[i]);
        expect(newBodies.length).toBe(oldBodies.length);
        for (let j = 0; j < oldBodies.length; j++) {
          if (j !== 250) expect(newBodies[j].bytes.equals(oldBodies[j].bytes)).toBe(true);
          else {
            const r = reader(oldBodies[j].bytes), n = r.uint();
            for (let k = 0; k < n; k++) { r.uint(); r.pos++; }
            const instructions = oldBodies[j].bytes.subarray(r.pos);
            expect(newBodies[j].bytes.subarray(-instructions.length).equals(instructions)).toBe(true);
          }
        }
      }
    }
  });

  it('fails closed when the helper changes or the adapter is applied twice', () => {
    const changed = Buffer.from(original), body = bodies(sections(original).find(x => x.id === 10))[250];
    changed[body.offset + 20] ^= 1;
    expect(() => adaptNativeMids(changed)).toThrow('Unsupported LTSM MID helper');
    expect(() => adaptNativeMids(adaptNativeMids(original))).toThrow('Unsupported LTSM MID helper');
  });

  it('rejects malformed input', () => {
    expect(() => adaptNativeMids(Buffer.alloc(8))).toThrow('Invalid WASM header');
    expect(() => adaptNativeMids(original.subarray(0, -1))).toThrow('Truncated WASM section');
  });
});
