// Offline differential suite. All identities, keys and messages are synthetic.
// Usage: node source/ltsm/test.mjs /path/to/ltsm-test [arguments...]
// Remote: node .../test.mjs ssh host Desktop/owned-stage/ltsm-test
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { createInterface } from 'node:readline';
import { createCipheriv, createDecipheriv, createHash, createPublicKey,
  diffieHellman, generateKeyPairSync, randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { reference } from './reference.mjs';
import { legyDecode, legyEncode, legyMac, encodeHeaders } from '../cloudflare/src/legy.js';

const [command, ...args] = process.argv.slice(2);
if (!command) throw new Error('Supply an explicit native harness command');
const child = spawn(command, args, { stdio: ['pipe', 'pipe', 'inherit'] });
const lines = createInterface({ input: child.stdout });
const responses = [], waiters = [];
lines.on('line', line => {
  const response = JSON.parse(line);
  if (waiters.length) waiters.shift().resolve(response); else responses.push(response);
});
child.on('exit', code => {
  while (waiters.length) waiters.shift().reject(new Error(`Native harness exited: ${code}`));
});
let checks = 0;
async function call(operation, request, success = true) {
  const result = responses.length ? Promise.resolve(responses.shift()) :
    new Promise((resolve, reject) => waiters.push({ resolve, reject }));
  child.stdin.write(JSON.stringify({ operation, requestJson: JSON.stringify(request) }) + '\n');
  let timer;
  const answer = await Promise.race([result, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('Native request timed out')), 120000);
  })]).finally(() => clearTimeout(timer));
  assert.equal(answer.ok, success, `${operation}: ${answer.error ?? 'unexpected success'}`);
  ++checks;
  if (!success) { assert.equal(answer.response, undefined); return answer; }
  return answer.response;
}
const b64 = b => Buffer.from(b).toString('base64');
const hash = (...parts) => createHash('sha256').update(Buffer.concat(parts.map(p => Buffer.from(p)))).digest();
function pair() {
  const { privateKey, publicKey } = generateKeyPairSync('x25519');
  return { privateKey, public: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32),
    private: privateKey.export({ type: 'pkcs8', format: 'der' }).subarray(-32) };
}
function shared(privateKey, pub) {
  return diffieHellman({ privateKey, publicKey: createPublicKey({ type: 'spki', format: 'der',
    key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), pub]) }) });
}
function wrap(dh, bytes) {
  const h = hash(dh, 'IV'), iv = h.subarray(0, 16).map((x, i) => x ^ h[i + 16]);
  const c = createCipheriv('aes-256-cbc', hash(dh, 'Key'), iv);
  return Buffer.concat([c.update(bytes), c.final()]);
}
function unWrap(dh, bytes) {
  const h = hash(dh, 'IV'), iv = h.subarray(0, 16).map((x, i) => x ^ h[i + 16]);
  const c = createDecipheriv('aes-256-cbc', hash(dh, 'Key'), iv);
  return Buffer.concat([c.update(bytes), c.final()]);
}
function int(n) { const b = Buffer.alloc(4); b.writeUInt32BE(n); return b; }
const aad = p => Buffer.concat([Buffer.from(p.to), Buffer.from(p.from),
  int(p.senderKeyId), int(p.receiverKeyId), int(2), int(p.contentType)]);
function seal(p, dh, text) {
  const salt = randomBytes(16), nonce = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', hash(dh, salt, 'Key'), nonce);
  cipher.setAAD(aad(p));
  return Buffer.concat([salt, nonce, cipher.update(text), cipher.final(), cipher.getAuthTag()]);
}
function open(p, dh, wire) {
  const cipher = createDecipheriv('aes-256-gcm', hash(dh, wire.subarray(0, 16), 'Key'), wire.subarray(16, 28));
  cipher.setAAD(aad(p)); cipher.setAuthTag(wire.subarray(-16));
  return Buffer.concat([cipher.update(wire.subarray(28, -16)), cipher.final()]);
}
const own = pair(), peer = pair(), selfId = 7, peerId = 17;
const selfMid = 'u' + '1a'.repeat(16), peerMid = 'u' + '2b'.repeat(16);
function keychain() {
  return Buffer.concat([Buffer.from([0x19, 0x1c, 0x15, 2, 0x15, selfId * 2, 0x28, 32]),
    own.public, Buffer.from([0x18, 32]), own.private, Buffer.from([0x16, 0, 0, 0])]);
}

try {
  const ref = await reference();
  for (const token of ['', 'synthetic-token', '日本語🔐']) {
    for (const [path, body] of [['/S4', '{}'], ['/api/test', '{"text":"こんにちは"}'], ['/S4', '']]) {
      const actual = await call('/sign', { path, body, accessToken: token });
      assert.equal(actual.hmac, ref.sign(path, body, token));
    }
  }
  const largeBody = 'literal \\u0000 ' + 'x'.repeat(32768);
  assert.equal((await call('/sign', { path: '/S4', body: largeBody })).hmac,
    ref.sign('/S4', largeBody, ''));
  console.log('PASS signing: native versus original WASM');

  const qr = await call('/keygen', {});
  const restored = ref.api.Curve25519Key.loadKey(Buffer.from(qr.workerRestoreState.qrKeys[0].exportedKey, 'base64'));
  assert.equal(b64(restored.getPublicKey()), qr.publicKey); restored.delete();
  const second = await call('/keygen', { workerRestoreState: qr.workerRestoreState });
  assert.notEqual(second.keyId, qr.keyId);
  assert.equal(second.workerRestoreState.qrKeys.length, 2);
  const unwrapped = await call('/e2ee/unwrap-keychain', { qrKeyId: qr.keyId,
    peerPublicKey: b64(peer.public),
    encryptedKeyChain: b64(wrap(shared(peer.privateKey, Buffer.from(qr.publicKey, 'base64')), keychain())),
    workerRestoreState: qr.workerRestoreState });
  assert.equal(unwrapped.keys[0].keyId, selfId);
  assert.equal(unwrapped.workerRestoreState.qrKeys.length, 0);
  const key = unwrapped.keys[0].exportedKey, state = unwrapped.workerRestoreState;
  const referenceKey = ref.api.E2EEKey.loadKey(Buffer.from(key, 'base64'));
  assert.equal(b64(referenceKey.getPublicKey()), b64(own.public));
  assert.equal(referenceKey.getKeyId(), selfId);
  const referenceChannel = referenceKey.createChannel(peer.public);

  // Import a QR key produced by the original JS/WASM, not by our native API.
  const referenceQr = ref.api.Curve25519Key.generate(true);
  const referenceState = { version: 1, qrKeys: [{ logicalId: 'qr-login-93', kind: 'qr-e2ee', keyId: 93,
    exportedKey: b64(referenceQr.exportKey()), publicKey: b64(referenceQr.getPublicKey()) }] };
  const imported = await call('/e2ee/unwrap-keychain', { qrKeyId: 93, workerRestoreState: referenceState,
    peerPublicKey: b64(peer.public), encryptedKeyChain: b64(wrap(shared(peer.privateKey,
      Buffer.from(referenceQr.getPublicKey())), keychain())) });
  assert.equal(imported.keys[0].keyId, selfId); referenceQr.delete();
  console.log('PASS key generation, QR restoration, keychain unwrap and reference key interoperability');

  const incoming = () => ({ privateKey: key, privateKeyId: selfId, peerPublicKey: b64(peer.public),
    peerMid, peerKeyId: peerId, to: selfMid, from: peerMid, senderKeyId: peerId,
    receiverKeyId: selfId, contentType: 0, workerRestoreState: state });
  const dh = shared(peer.privateKey, own.public);
  for (const text of ['hello', '日本語 ✓', 'é à 😀', 'x'.repeat(4096)]) {
    const p = incoming(), expected = { text };
    p.ciphertext = b64(seal(p, dh, JSON.stringify(expected)));
    assert.deepEqual((await call('/e2ee/decrypt-user-v2', p)).payload, expected);
    const outgoing = { ...p, from: selfMid, to: peerMid, senderKeyId: selfId, receiverKeyId: peerId,
      sequenceNumber: 4294967299, plaintext: b64(Buffer.from(JSON.stringify(expected))) };
    const encrypted = await call('/e2ee/encrypt-user-v2', outgoing);
    const wire = Buffer.from(encrypted.ciphertext, 'base64');
    assert.equal(wire.readBigUInt64BE(16), 4294967299n);
    assert.deepEqual(JSON.parse(open(outgoing, dh, wire)), expected);
    assert.deepEqual(JSON.parse(Buffer.from(referenceChannel.decryptV2(outgoing.to, outgoing.from,
      selfId, peerId, 0, wire))), expected);
  }
  const v1 = b64(referenceChannel.encryptV1(Buffer.from('{"text":"legacy v1"}')));
  assert.equal((await call('/e2ee/decrypt-user-v1', { ...incoming(), ciphertext: v1 })).payload.text, 'legacy v1');
  console.log('PASS user v1/v2 crypto: independent Node X25519/AES and original WASM');

  for (const prefix of ['c', 'r']) {
    const group = pair(), groupId = 29, groupMid = prefix + '3c'.repeat(16);
    const groupDh = shared(peer.privateKey, group.public);
    const p = { myPrivateKey: key, myPrivateKeyId: selfId, creatorPublicKey: b64(peer.public),
      senderPublicKey: b64(peer.public), senderMid: peerMid, senderKeyId: peerId, groupMid, groupKeyId: groupId,
      groupKey: { keyVersion: 1, groupKeyId: groupId, creator: peerMid, creatorKeyId: peerId,
        receiver: selfMid, receiverKeyId: selfId, encryptedSharedKey: b64(wrap(dh, group.private)),
        allowedTypes: [], specVersion: 2 }, to: groupMid, from: peerMid, receiverKeyId: groupId,
      contentType: 0, workerRestoreState: state };
    p.ciphertext = b64(seal(p, groupDh, '{"text":"group"}'));
    assert.equal((await call('/e2ee/decrypt-group-v2', p)).payload.text, 'group');
    const out = await call('/e2ee/encrypt-group-v2', { ...p, sequenceNumber: 42,
      plaintext: b64(Buffer.from('{"text":"outgoing group"}')) });
    assert.equal(JSON.parse(open(p, groupDh, Buffer.from(out.ciphertext, 'base64'))).text, 'outgoing group');
    const groupKey = referenceChannel.unwrapGroupSharedKey(Buffer.from(p.groupKey.encryptedSharedKey, 'base64'));
    const groupChannel = groupKey.createChannel(peer.public);
    const ciphertext = b64(groupChannel.encryptV1(Buffer.from('{"text":"legacy group"}')));
    assert.equal((await call('/e2ee/decrypt-group-v1', { ...p, ciphertext })).payload.text, 'legacy group');
    groupChannel.delete(); groupKey.delete();
  }
  const anotherPeer = pair();
  const created = await call('/e2ee/create-group-key', { myPrivateKey: key, myPrivateKeyId: selfId,
    workerRestoreState: state, members: [{ mid: peerMid, keyId: peerId, publicKey: b64(peer.public) },
      { mid: 'u' + '4d'.repeat(16), keyId: 24, publicKey: b64(anotherPeer.public) }] });
  const rawGroupKey = unWrap(dh, Buffer.from(created.encryptedKeys[0].encryptedKey, 'base64'));
  assert.equal(rawGroupKey.length, 32);
  assert.deepEqual(rawGroupKey, unWrap(shared(anotherPeer.privateKey, own.public),
    Buffer.from(created.encryptedKeys[1].encryptedKey, 'base64')));
  console.log('PASS group/room v1/v2 and group-key distribution');

  for (const corruption of ['ciphertext', 'recipient', 'sender', 'keyId', 'contentType', 'publicKey', 'privateKey']) {
    const p = incoming(), wire = seal(p, dh, '{"text":"authenticated"}');
    if (corruption === 'ciphertext') wire[wire.length - 1] ^= 1;
    if (corruption === 'recipient') p.to = 'u' + '4d'.repeat(16);
    if (corruption === 'sender') p.from = 'u' + '4d'.repeat(16);
    if (corruption === 'keyId') p.senderKeyId++;
    if (corruption === 'contentType') p.contentType++;
    if (corruption === 'publicKey') p.peerPublicKey = b64(anotherPeer.public);
    if (corruption === 'privateKey') p.privateKey = b64(Buffer.alloc(193));
    p.ciphertext = b64(wire);
    await call('/e2ee/decrypt-user-v2', p, false);
    assert.equal((await call('/sign', { path: '/S4', body: '{}', accessToken: 'synthetic-token' })).hmac,
      'k2qU/iyTBsbcBVr82398TrK7jFI1zC5yADh7OyGmb30=');
  }
  for (const mid of ['x' + 'ab'.repeat(16), 'u' + 'gh'.repeat(16), 'u' + 'AB'.repeat(16), 'u' + 'ab'.repeat(32)]) {
    await call('/e2ee/encrypt-user-v2', { ...incoming(), to: mid, sequenceNumber: 1,
      plaintext: b64(Buffer.from('{}')) }, false);
  }
  for (const bad of [{}, { path: 42 }, { path: '/S4' },
    { path: '/S4', body: null }, { path: '/S4', body: 'a' + String.fromCharCode(0) + 'b' },
    { path: '/S4', accessToken: {} }]) await call('/sign', bad, false);
  for (const text of [Buffer.from([255]), Buffer.from('{"text":"\\u0000"}')]) {
    const p = incoming(); p.ciphertext = b64(seal(p, dh, text));
    await call('/e2ee/decrypt-user-v2', p, false);
  }
  for (const bad of [null, [], 1, 'not an object']) await call('/keygen', { workerRestoreState: bad }, bad === null);
  for (const bad of [-1, 1.5, 9007199254740992]) await call('/e2ee/encrypt-user-v2',
    { ...incoming(), plaintext: b64(Buffer.from('{}')), sequenceNumber: bad }, false);
  await call('/e2ee/unwrap-keychain', { qrKeyId: 1, peerPublicKey: b64(peer.public), encryptedKeyChain: 'AAAA' }, false);
  await call('/unknown', {}, false);
  console.log('PASS tamper rejection, malformed inputs and recovery after failures');

  for (const token of ['', 'synthetic-token', 'uid:abc.def.ghi']) {
    const request = { path: '/S4', accessToken: token, body: b64(Buffer.from([0x82, 0x21, 0, 255])) };
    const native = await call('/transport/legy/encode', request);
    assert.equal(native.xLcs.slice(0, 4), '0008');
    assert.equal(Buffer.from(native.xLcs.slice(4), 'base64').length, 256);
    const decoded = await legyDecode(native);
    assert.equal(decoded.body, request.body);
    assert.equal(decoded.headers['x-lpqs'], '/S4');
    assert.equal(decoded.headers['x-lt'] ?? '', token.replace(/^uid:/, ''));
    const js = await legyEncode(request);
    assert.deepEqual(await call('/transport/legy/decode', js), JSON.parse(JSON.stringify(await legyDecode(js))));
    const damaged = Buffer.from(native.body, 'base64'); damaged[3] ^= 1;
    await call('/transport/legy/decode', { key: native.key, body: b64(damaged) }, false);
    await call('/transport/legy/decode', { key: native.key, body: b64(damaged.subarray(0, -1)) }, false);
  }
  const emptyLegy = await call('/transport/legy/encode', { path: '/S4', body: '' });
  assert.equal((await legyDecode(emptyLegy)).body, '');
  const iv = Buffer.from([78,9,72,62,56,245,255,114,128,18,123,158,251,92,45,51]);
  const aeskey = Buffer.alloc(16, 37);
  for (const status of ['200', '401', '599']) {
    const cipher = createCipheriv('aes-128-cbc', aeskey, iv);
    const plain = Buffer.concat([Buffer.from([7]), encodeHeaders({ 'x-lc': status }), Buffer.from([0, 255, 42])]);
    const ct = Buffer.concat([cipher.update(plain), cipher.final()]);
    for (const wire of [ct, Buffer.concat([ct, legyMac(aeskey, ct)])]) {
      const decoded = await call('/transport/legy/decode', { key: b64(aeskey), body: b64(wire) });
      assert.equal(decoded.status, Number(status)); assert.equal(decoded.body, b64([0, 255, 42]));
    }
  }
  console.log('PASS LEGY: independent framing/AES, original Worker JS, checksum rejection');
  referenceChannel.delete(); referenceKey.delete();
  for (let i = 0; i < 20; i++) {
    await call('/keygen', {});
    await call('/e2ee/decrypt-user-v2', { ...incoming(), ciphertext: b64(Buffer.alloc(48)) }, false);
  }
  console.log(`PASS ${checks} native requests; no credentials or LINE traffic`);
  child.stdin.end();
  const [code] = await once(child, 'exit'); assert.equal(code, 0);
} catch (error) {
  child.kill('SIGTERM'); throw error;
} finally { lines.close(); }
