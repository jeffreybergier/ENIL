// OPT-IN live comparison. Uses synthetic keys/messages only; never calls LINE.
// Credentials are read from the ignored repository .env, never from argv/logs.
// Usage: node source/ltsm/compare-worker.mjs /explicit/path/to/ltsm-test
import assert from 'node:assert/strict';
import { readFile, stat } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { once } from 'node:events';
import { createCipheriv, createHash, createPublicKey, diffieHellman,
  generateKeyPairSync, randomInt } from 'node:crypto';

const envPath = new URL('../../.env', import.meta.url);
if ((await stat(envPath)).mode & 0o077) throw new Error('.env must be owner-only (chmod 600)');
const env = {};
for (const line of (await readFile(envPath, 'utf8')).split(/\r?\n/)) {
  const match = line.match(/^(ENIL_WORKER_URL|ENIL_WORKER_SECRET)=(.*)$/);
  if (match) env[match[1]] = match[2];
}
const url = new URL(env.ENIL_WORKER_URL);
if (url.protocol !== 'https:' || url.username || url.password || !env.ENIL_WORKER_SECRET)
  throw new Error('Expected HTTPS Worker URL and secret in .env');
const [command, ...args] = process.argv.slice(2);
if (!command) throw new Error('Supply a native harness command');
const child = spawn(command, args, { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = createInterface({ input: child.stdout });
const pending = [];
lines.on('line', line => pending.shift()?.resolve(JSON.parse(line)));
child.on('exit', () => { while (pending.length) pending.shift().reject(new Error('Harness exited')); });
let liveCalls = 0;
async function native(operation, request) {
  let timer;
  const result = new Promise((resolve, reject) => {
    pending.push({ resolve, reject });
    timer = setTimeout(() => reject(new Error('Native comparison timed out')), 120000);
  });
  child.stdin.write(JSON.stringify({ operation, requestJson: JSON.stringify(request) }) + '\n');
  const answer = await result.finally(() => clearTimeout(timer));
  assert.equal(answer.ok, true, answer.error); return answer.response;
}
async function remote(operation, request) {
  const response = await fetch(new URL(operation, url), { method: 'POST', redirect: 'error',
    headers: { 'Content-Type': 'application/json', 'X-Worker-Secret': env.ENIL_WORKER_SECRET },
    body: JSON.stringify(request), signal: AbortSignal.timeout(30000) });
  // Never echo response bodies or credentials on errors.
  if (!response.ok) throw new Error(`Worker comparison failed: HTTP ${response.status}, ${operation}`);
  ++liveCalls; return response.json();
}
const b64 = x => Buffer.from(x).toString('base64');
function pair() {
  const { privateKey, publicKey } = generateKeyPairSync('x25519');
  return { privateKey, public: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32),
    private: privateKey.export({ type: 'pkcs8', format: 'der' }).subarray(-32) };
}
const hash = (...xs) => createHash('sha256').update(Buffer.concat(xs.map(x => Buffer.from(x)))).digest();
function wrapped(peer, publicKey, raw) {
  const dh = diffieHellman({ privateKey: peer.privateKey, publicKey: createPublicKey({ type: 'spki', format: 'der',
    key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), Buffer.from(publicKey, 'base64')]) }) });
  const h = hash(dh, 'IV'), iv = h.subarray(0, 16).map((x, i) => x ^ h[i + 16]);
  const aes = createCipheriv('aes-256-cbc', hash(dh, 'Key'), iv);
  return b64(Buffer.concat([aes.update(raw), aes.final()]));
}
try {
  for (const accessToken of ['', 'synthetic-token', '日本語']) {
    const request = { path: '/S4', body: '{"synthetic":true}', accessToken };
    assert.equal((await native('/sign', request)).hmac, (await remote('/sign', request)).hmac);
  }
  const own = pair(), peer = pair(), id = 7;
  const chain = Buffer.concat([Buffer.from([0x19, 0x1c, 0x15, 2, 0x15, id * 2, 0x28, 32]),
    own.public, Buffer.from([0x18, 32]), own.private, Buffer.from([0x16, 0, 0, 0])]);
  const localQr = await native('/keygen', {});
  // The production Worker first tries an isolate-local numeric handle. Use an
  // unused logical ID to force portable restoration, without resetting or
  // otherwise disturbing another user's Worker state.
  const logical = randomInt(1000000000, 1100000000);
  localQr.workerRestoreState.qrKeys[0].keyId = logical;
  localQr.workerRestoreState.qrKeys[0].logicalId = `qr-login-${logical}`;
  const workerKey = await remote('/e2ee/unwrap-keychain', { qrKeyId: logical,
    workerRestoreState: localQr.workerRestoreState, peerPublicKey: b64(peer.public),
    encryptedKeyChain: wrapped(peer, localQr.publicKey, chain) });
  assert.equal(workerKey.keys[0].keyId, id);
  const workerQr = await remote('/keygen', {});
  const localKey = await native('/e2ee/unwrap-keychain', { qrKeyId: workerQr.keyId,
    workerRestoreState: workerQr.workerRestoreState, peerPublicKey: b64(peer.public),
    encryptedKeyChain: wrapped(peer, workerQr.publicKey, chain) });
  assert.equal(localKey.keys[0].keyId, id);
  const params = { privateKey: localKey.keys[0].exportedKey, privateKeyId: id,
    peerPublicKey: b64(peer.public), peerKeyId: 17, peerMid: 'u' + '2b'.repeat(16),
    from: 'u' + '1a'.repeat(16), to: 'u' + '2b'.repeat(16), senderKeyId: id,
    receiverKeyId: 17, contentType: 0, sequenceNumber: 42,
    plaintext: b64(Buffer.from('{"text":"synthetic native comparison"}')),
    workerRestoreState: localKey.workerRestoreState };
  for (const [encode, decode] of [[native, remote], [remote, native]]) {
    const encrypted = await encode('/e2ee/encrypt-user-v2', params);
    const decrypted = await decode('/e2ee/decrypt-user-v2', { ...params, ciphertext: encrypted.ciphertext });
    assert.equal(decrypted.payload.text, 'synthetic native comparison');
  }
  for (const [encode, decode] of [[native, remote], [remote, native]]) {
    const wire = await encode('/transport/legy/encode', { path: '/S4', accessToken: 'synthetic-token', body: b64([0x82, 0, 255]) });
    const clear = await decode('/transport/legy/decode', wire);
    assert.equal(clear.body, b64([0x82, 0, 255]));
  }
  console.log(`PASS live Worker comparison: ${liveCalls} synthetic calls; signatures, bidirectional portable keys, E2EE and LEGY; no LINE traffic`);
  child.stdin.end(); const [code] = await once(child, 'exit'); assert.equal(code, 0);
} catch (error) {
  child.kill('SIGTERM');
  // fetch errors can contain nested connection diagnostics; never dump requests.
  console.error(error.message); process.exitCode = 1;
} finally { lines.close(); }
