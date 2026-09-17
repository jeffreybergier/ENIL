// Downloaded LINE code and generated artifacts stay outside version control.
import { readFile, writeFile, mkdir, rm } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { parse } from 'acorn';
import { generate } from 'astring';
import { unzipSync } from 'fflate';
import { adaptNativeMids } from './native-mids.mjs';

const root = new URL('../', import.meta.url);
const config = JSON.parse(await readFile(new URL('extension.json', import.meta.url), 'utf8'));
const cache = new URL('.cache/line.crx', root);
const output = new URL('src/', root);
const origin = `chrome-extension://${config.id}`;
const ast = source => parse(source, { ecmaVersion: 'latest' });
const sha256 = bytes => createHash('sha256').update(bytes).digest('hex');

function unpack(bytes) {
  let offset = 0;
  if (bytes.subarray(0, 4).toString() === 'Cr24') {
    const version = bytes.readUInt32LE(4);
    if (version === 3) offset = 12 + bytes.readUInt32LE(8);
    else if (version === 2) offset = 16 + bytes.readUInt32LE(8) + bytes.readUInt32LE(12);
    else throw new Error(`Unsupported CRX format ${version}`);
  }
  const wanted = new Set(['manifest.json', ...Object.keys(config.files)]);
  const files = unzipSync(bytes.subarray(offset), { filter: entry => wanted.has(entry.name) });
  const manifest = JSON.parse(new TextDecoder().decode(files['manifest.json']));
  if (manifest.version !== config.version) {
    throw new Error(`LINE ${manifest.version} downloaded; extractor supports ${config.version}. Review the new extension before updating scripts/extension.json.`);
  }
  for (const [name, hash] of Object.entries(config.files)) {
    if (!files[name] || sha256(files[name]) !== hash) throw new Error(`LINE integrity check failed: ${name}`);
  }
  return files;
}

async function download() {
  const url = new URL('https://clients2.google.com/service/update2/crx');
  url.search = new URLSearchParams({
    response: 'redirect', prodversion: config.chromeVersion,
    acceptformat: 'crx2,crx3', x: `id=${config.id}&installsource=ondemand&uc`,
  });
  console.log(`[build] Downloading LINE ${config.id}`);
  const response = await fetch(url, { signal: AbortSignal.timeout(90000) });
  if (!response.ok) throw new Error(`Extension download failed: HTTP ${response.status}`);
  const bytes = Buffer.from(await response.arrayBuffer());
  const files = unpack(bytes);
  await mkdir(new URL('.cache/', root), { recursive: true });
  await writeFile(cache, bytes);
  return files;
}

async function inputs() {
  if (!process.argv.includes('--refresh')) {
    try {
      const files = unpack(await readFile(cache));
      console.log(`[build] Using verified LINE ${config.version} cache`);
      return files;
    } catch (error) {
      if (error.code !== 'ENOENT') console.log(`[build] Discarding invalid cache: ${error.message}`);
    }
  }
  return download();
}

function visit(node, fn) {
  if (!node || typeof node !== 'object') return;
  fn(node);
  for (const value of Object.values(node)) {
    if (Array.isArray(value)) value.forEach(child => visit(child, fn));
    else if (value && typeof value === 'object') visit(value, fn);
  }
}

function exactlyOne(nodes, label) {
  if (nodes.length !== 1) throw new Error(`Expected one ${label}, found ${nodes.length}`);
  return nodes[0];
}

function extract(source) {
  const tree = ast(source);
  const modules = new Map([[1426, []], [75511, []]]);
  const scopes = [];
  visit(tree, node => {
    if (node.type === 'Property' && modules.has(node.key.value)) modules.get(node.key.value).push(node.value);
    if (node.type === 'BlockStatement' && node.body.some(s => s.type === 'FunctionDeclaration' && s.id.name === 'dS')) scopes.push(node);
  });
  const processModule = exactlyOne(modules.get(1426), 'process module');
  const wasmModule = exactlyOne(modules.get(75511), 'WASM module');
  let origins = 0;
  visit(wasmModule, node => {
    if (node.type === 'MemberExpression' && !node.computed && node.property.name === 'origin' &&
        node.object.type === 'MemberExpression' && !node.object.computed &&
        node.object.object.name === 'window' && node.object.property.name === 'location') {
      for (const key of Object.keys(node)) delete node[key];
      Object.assign(node, { type: 'Literal', value: origin });
      origins++;
    }
  });
  if (origins !== 1) throw new Error(`Expected one WASM origin patch, found ${origins}`);

  const scope = exactlyOne(scopes, 'LTSM entry scope');
  const names = new Set(['l', 'p', 'm', 'tm', 'cg', 'ev', 'Pg', 'Dg', 'yw']);
  const support = [];
  for (const statement of scope.body) {
    if (statement.type === 'VariableDeclaration') {
      for (const declaration of statement.declarations) {
        if (names.delete(declaration.id.name)) support.push({ ...statement, declarations: [declaration] });
      }
    } else if (names.delete(statement.id?.name)) support.push(statement);
  }
  if (names.size) throw new Error(`Missing LTSM declarations: ${[...names].join(', ')}`);
  const start = scope.body.findIndex(s => s.type === 'VariableDeclaration' && s.declarations.some(d => d.id.name === 'uS'));
  if (start < 0) throw new Error('Missing LTSM runtime');
  const runtime = scope.body.slice(start);
  const stateNames = new Set(['hS', 'pS', 'mS', 'gS']);
  const state = exactlyOne(runtime.filter(s => s.type === 'VariableDeclaration' && s.declarations.some(d => d.id.name === 'hS')), 'state declaration');
  if (state.declarations.length !== 4 || state.declarations.some(d => !stateNames.has(d.id.name))) throw new Error('Unexpected LTSM state layout');
  runtime.splice(runtime.indexOf(state), 1);
  // These four bindings belong to the LTSM entry scope. Member property names
  // are not bindings; leave them intact when redirecting state references.
  function redirect(node, parent, key) {
    if (!node || typeof node !== 'object') return;
    if (node.type === 'Identifier' && stateNames.has(node.name) &&
        !(parent?.type === 'MemberExpression' && key === 'property' && !parent.computed) &&
        !(parent?.type === 'Property' && key === 'key' && !parent.computed)) {
      const name = node.name;
      Object.assign(node, { type: 'MemberExpression', object: { type: 'Identifier', name: '__enilState' }, property: { type: 'Identifier', name }, computed: false, optional: false });
      delete node.name;
      return;
    }
    for (const [k, value] of Object.entries(node)) {
      if (Array.isArray(value)) value.forEach(child => redirect(child, node, k));
      else if (value && typeof value === 'object') redirect(value, node, k);
    }
  }
  runtime.forEach(node => redirect(node));
  const dispatch = exactlyOne(runtime.filter(s => s.type === 'FunctionDeclaration' && s.id.name === 'SS'), 'dispatcher');
  const switches = [];
  visit(dispatch, node => { if (node.type === 'SwitchStatement' && node.discriminant.object?.name === 'n' && node.discriminant.property?.name === 'command') switches.push(node); });
  const commandSwitch = exactlyOne(switches, 'command switch');
  // ENIL additions: portable QR key import/export across Worker isolates.
  const portable = ast(`switch (n.command) {
    case m.CURVEKEY_LOAD_KEY:
      try { AS({ sandboxId: r, type: p.RESPONSE, data: bS(fS().Curve25519Key.loadKey(n.payload)) }); }
      catch (error) { AS({ sandboxId: r, type: p.ERROR, data: error }); }
      break;
    case m.CURVEKEY_EXPORT_KEY:
      try { AS({ sandboxId: r, type: p.RESPONSE, data: yS(n.ltsmKeyId).exportKey() }); }
      catch (error) { AS({ sandboxId: r, type: p.ERROR, data: error }); }
      break;
  }`).body[0].cases;
  commandSwitch.cases.push(...portable);
  const render = nodes => nodes.map(node => generate(node)).join('\n');
  return `// GENERATED from downloaded LINE ${config.version}; do not commit.\n(function () {
    "use strict";
    const processModule = { exports: {} };
    (${generate(processModule)})(processModule);
    const wasmModule = { exports: {} };
    (${generate(wasmModule)})(wasmModule, wasmModule.exports, id => {
      if (id !== 1426) throw new Error('Unexpected WASM dependency: ' + id);
      return processModule.exports;
    });
    const Q = () => wasmModule.exports;
    const __enilState = globalThis.__ENIL_LTSM_STATE__ ||= { hS: undefined, pS: {}, mS: {}, gS: 0 };
    globalThis.__ENIL_LTSM_DEBUG__ = {
      getStateSummary: () => ({ hasStorageKey: !!__enilState.hS, keyCount: Object.keys(__enilState.pS).length, channelCount: Object.keys(__enilState.mS).length, nextObjectId: __enilState.gS }),
      resetState() {
        Object.assign(__enilState, { hS: undefined, pS: {}, mS: {}, gS: 0 });
        return this.getStateSummary();
      }
    };
    ${render(support)}
    m.CURVEKEY_LOAD_KEY = 'curvekey_load_key';
    m.CURVEKEY_EXPORT_KEY = 'curvekey_export_key';
    ${render(runtime)}
  })();\n`;
}

// A failed download/extraction must not leave deployable stale output behind.
await mkdir(output, { recursive: true });
await Promise.all(['ltsm-worker.js', 'ltsm.wasm'].map(name => rm(new URL(name, output), { force: true })));
const files = await inputs();
const generated = extract(new TextDecoder().decode(files['static/js/ltsmSandbox.js']));
ast(generated);
const wasm = adaptNativeMids(files['static/js/ltsm.wasm']);
await writeFile(new URL('ltsm-worker.js', output), generated);
await writeFile(new URL('ltsm.wasm', output), wasm);
console.log(`[build] Generated Worker assets in ${fileURLToPath(output)}`);
