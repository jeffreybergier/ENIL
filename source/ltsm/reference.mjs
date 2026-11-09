// Offline reference only. Execute the verified generated Emscripten wrapper in
// Node; never shipped in the native library. No account data or network calls.
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';
import { createHash, webcrypto } from 'node:crypto';

export async function reference() {
  const root = new URL('../cloudflare/', import.meta.url);
  const config = JSON.parse(await readFile(new URL('scripts/extension.json', root)));
  const source = await readFile(new URL('src/ltsm-worker.js', root), 'utf8');
  const marker = 'let uS, cS, lS;';
  if (source.split(marker).length !== 2) throw new Error('Review reference loader');
  globalThis.window = globalThis;
  globalThis.document = { addEventListener() {} };
  globalThis.location = { origin: `chrome-extension://${config.id}` };
  if (!globalThis.crypto) Object.defineProperty(globalThis, 'crypto', { value: webcrypto });
  globalThis.fetch = () => { throw new Error('Reference network access denied'); };
  vm.runInThisContext(source.replace(marker,
    `globalThis.__ENIL_TEST_FACTORY__ = Q(); ${marker}`), { filename: 'verified-ltsm-reference.js' });
  const module = new WebAssembly.Module(await readFile(new URL('src/ltsm.wasm', root)));
  const api = await globalThis.__ENIL_TEST_FACTORY__({
    instantiateWasm(imports, receive) {
      const instance = new WebAssembly.Instance(module, imports);
      receive(instance); return instance.exports;
    },
  });
  delete globalThis.__ENIL_TEST_FACTORY__;
  const origin = JSON.stringify(globalThis.location.origin).replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const token = source.match(new RegExp(origin + ': "([A-Za-z0-9_-]+)"'))?.[1];
  if (!token) throw new Error('Missing reference signing identity');
  const digest = s => new Uint8Array(createHash('sha256').update(s).digest());
  return {
    api,
    sign(path, body, accessToken) {
      const key = api.SecureKey.loadToken(token);
      const derived = key.deriveKey(digest(config.version), digest(accessToken));
      const hmac = new api.Hmac(derived);
      try { return Buffer.from(hmac.digest(Buffer.from(path + body))).toString('base64'); }
      finally { hmac.delete(); derived.delete(); key.delete(); }
    },
  };
}
