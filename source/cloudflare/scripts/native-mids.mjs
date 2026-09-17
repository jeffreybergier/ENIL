import { createHash } from 'node:crypto';

// LINE 3.7.2's E2EE V2 AAD builder calls function 290 to decrypt Chrome's
// protected MIDs. Native Talk supplies literal u/c/r + 32 lowercase hex digits.
// Those are already the authenticated identifier bytes: feeding them through
// Chrome's MID decryption throws a MAC error BEFORE message crypto runs.
//
// Add a narrowly validated native-MID return to that helper. Keep the original
// Chrome body byte-for-byte. No message authentication check is removed. The
// build pins the entire upstream WASM hash; also pin this private ABI/body here
// so an extension update cannot silently apply this adaptation to another body.
const BODY_SHA256 = 'e14b073dd95c6ffe57b3b754961b96b8e88842bac4813770044e62d8b083a9ff';
const CODE_BODY_INDEX = 250; // function 290 minus 40 imported functions

function uint(n) {
  const out = [];
  do { const b = n & 127; n = Math.floor(n / 128); out.push(b | (n ? 128 : 0)); } while (n);
  return out;
}

function reader(bytes) {
  return { pos: 0, uint() {
    let n = 0, shift = 0, b;
    do {
      if (this.pos >= bytes.length || shift >= 35) throw new Error('Malformed WASM integer');
      b = bytes[this.pos++]; n += (b & 127) * 2 ** shift; shift += 7;
    } while (b & 128);
    return n;
  } };
}

function adaptBody(body) {
  if (createHash('sha256').update(body).digest('hex') !== BODY_SHA256)
    throw new Error('Unsupported LTSM MID helper; review the native MID adaptation');
  const r = reader(body), groups = r.uint(), localsStart = r.pos;
  let localCount = 0;
  for (let i = 0; i < groups; i++) { localCount += r.uint(); r.pos++; }
  const instructionsStart = r.pos;
  // Three new scratch locals; the original helper's locals remain zero even
  // when native validation fails and execution falls through to Chrome.
  const ptr = 3 + localCount, index = ptr + 1, ch = ptr + 2;
  const code = [];
  const emit = (...bytes) => code.push(...bytes);
  const get = n => emit(0x20, ...uint(n));
  const set = n => emit(0x21, ...uint(n));
  const constant = n => { // signed LEB128 i32.const for nonnegative values
    const b = uint(n); if (b[b.length - 1] & 64) { b[b.length - 1] |= 128; b.push(0); }
    emit(0x41, ...b);
  };
  const load = offset => emit(0x28, 2, ...uint(offset));
  const byte = offset => emit(0x2d, 0, ...uint(offset));
  const store = offset => emit(0x36, 2, ...uint(offset));
  const call = n => emit(0x10, ...uint(n));
  const reject = depth => emit(0x0d, depth);

  emit(0x02, 0x40); // block: invalid/Chrome IDs fall through to original code
  // libc++ std::string: a 33-byte string must use the long layout.
  get(2); byte(11); constant(128); emit(0x71, 0x45); reject(0);
  get(2); load(4); constant(33); emit(0x47); reject(0);
  get(2); load(0); set(ptr);
  get(ptr); byte(0); set(ch);
  get(ch); constant(117); emit(0x46); // u
  get(ch); constant(99); emit(0x46, 0x72); // c
  get(ch); constant(114); emit(0x46, 0x72, 0x45); reject(0); // r
  constant(1); set(index);
  emit(0x03, 0x40); // loop over exactly 32 lowercase hexadecimal digits
  get(ptr); get(index); emit(0x6a); byte(0); set(ch);
  get(ch); constant(48); emit(0x6b); constant(9); emit(0x4d);
  get(ch); constant(97); emit(0x6b); constant(5); emit(0x4d, 0x72, 0x45); reject(1);
  get(index); constant(1); emit(0x6a); set(index);
  get(index); constant(33); emit(0x49); reject(0);
  emit(0x0b);
  constant(33); call(48); set(ch); // pinned malloc
  get(ch); get(ptr); constant(33); call(50); emit(0x1a); // pinned memcpy
  // Return the same owning std::vector<uint8_t> layout as the original helper.
  get(0); get(ch); store(0);
  get(0); get(ch); constant(33); emit(0x6a); store(4);
  get(0); get(ch); constant(33); emit(0x6a); store(8);
  emit(0x0f, 0x0b); // return; end validation block
  return Buffer.concat([
    Buffer.from(uint(groups + 1)), body.subarray(localsStart, instructionsStart),
    Buffer.from([3, 0x7f, ...code]), body.subarray(instructionsStart),
  ]);
}

export function adaptNativeMids(wasm) {
  const input = Buffer.from(wasm), r = reader(input);
  if (!input.subarray(0, 8).equals(Buffer.from([0, 97, 115, 109, 1, 0, 0, 0])))
    throw new Error('Invalid WASM header');
  r.pos = 8;
  const output = [input.subarray(0, 8)];
  let adapted = false;
  while (r.pos < input.length) {
    const start = r.pos, id = input[r.pos++], size = r.uint(), end = r.pos + size;
    if (end > input.length) throw new Error('Truncated WASM section');
    if (id !== 10) { output.push(input.subarray(start, end)); r.pos = end; continue; }
    if (adapted) throw new Error('Duplicate WASM code section');
    const count = r.uint(), bodies = [Buffer.from(uint(count))];
    for (let i = 0; i < count; i++) {
      const length = r.uint(), bodyEnd = r.pos + length;
      if (bodyEnd > end) throw new Error('Truncated WASM body');
      let body = input.subarray(r.pos, bodyEnd); r.pos = bodyEnd;
      if (i === CODE_BODY_INDEX) { body = adaptBody(body); adapted = true; }
      bodies.push(Buffer.from(uint(body.length)), body);
    }
    if (r.pos !== end) throw new Error('Invalid WASM code section length');
    const code = Buffer.concat(bodies);
    output.push(Buffer.from([10, ...uint(code.length)]), code);
  }
  if (!adapted) throw new Error('Missing LTSM MID helper');
  const result = Buffer.concat(output);
  if (!WebAssembly.validate(result)) throw new Error('Invalid adapted LTSM WASM');
  return result;
}
