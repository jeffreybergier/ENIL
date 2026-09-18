// Independent Node crypto + a synthetic Compact Thrift keychain exercise the
// real Worker/WASM. No account credentials, captured messages, or LINE traffic.
import { createCipheriv, createDecipheriv, createHash, createPublicKey,
  diffieHellman, generateKeyPairSync, randomBytes } from 'node:crypto';
import { unstable_dev } from 'wrangler';
import { beforeAll, afterAll, beforeEach, describe, expect, it } from 'vitest';

const secret = 'synthetic-e2ee-test';
const selfMid = 'u' + '1a'.repeat(16), peerMid = 'u' + '2b'.repeat(16);
const selfId = 7, peerId = 17;
let worker, key, state;
const self = pair(), peer = pair();

function pair() {
  const { privateKey, publicKey } = generateKeyPairSync('x25519');
  return { privateKey, public: publicKey.export({ type: 'spki', format: 'der' }).subarray(-32),
    private: privateKey.export({ type: 'pkcs8', format: 'der' }).subarray(-32) };
}
function shared(privateKey, publicBytes) {
  return diffieHellman({ privateKey, publicKey: createPublicKey({ type: 'spki', format: 'der',
    key: Buffer.concat([Buffer.from('302a300506032b656e032100', 'hex'), publicBytes]) }) });
}
const hash = (...parts) => createHash('sha256').update(Buffer.concat(parts.map(x => Buffer.from(x)))).digest();
const b64 = bytes => Buffer.from(bytes).toString('base64');
function int(n) { const b = Buffer.alloc(4); b.writeInt32BE(n); return b; }
function aad(p) { return Buffer.concat([Buffer.from(p.to), Buffer.from(p.from),
  int(p.senderKeyId), int(p.receiverKeyId), int(2), int(p.contentType)]); }
function seal(p, dh, plaintext) {
  const salt = randomBytes(16), nonce = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', hash(dh, salt, 'Key'), nonce);
  cipher.setAAD(aad(p));
  return Buffer.concat([salt, nonce, cipher.update(plaintext), cipher.final(), cipher.getAuthTag()]);
}
function open(p, dh, wire) {
  const decipher = createDecipheriv('aes-256-gcm', hash(dh, wire.subarray(0, 16), 'Key'), wire.subarray(16, 28));
  decipher.setAAD(aad(p)); decipher.setAuthTag(wire.subarray(-16));
  return Buffer.concat([decipher.update(wire.subarray(28, -16)), decipher.final()]);
}
function wrap(dh, plain) {
  const h = hash(dh, 'IV'), iv = h.subarray(0, 16).map((x, i) => x ^ h[i + 16]);
  const cipher = createCipheriv('aes-256-cbc', hash(dh, 'Key'), iv);
  return Buffer.concat([cipher.update(plain), cipher.final()]);
}
async function post(path, body) {
  return worker.fetch(`http://${worker.address}:${worker.port}${path}`, { method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-Worker-Secret': secret }, body: JSON.stringify(body) });
}
function incoming() {
  return { privateKey: key, privateKeyId: selfId, peerPublicKey: b64(peer.public), peerMid, peerKeyId: peerId,
    to: selfMid, from: peerMid, senderKeyId: peerId, receiverKeyId: selfId, contentType: 0,
    workerRestoreState: structuredClone(state) };
}

beforeAll(async () => {
  worker = await unstable_dev('src/index.js', { experimental: { disableExperimentalWarning: true },
    local: true, persist: false, logLevel: 'error', vars: { WORKER_SECRETS: secret } });
  const qr = await (await post('/keygen', {})).json();
  // Compact Thrift: field 1 list<struct>, one E2EE key with fields
  // version=1, keyId=7, publicKey, privateKey, timestamp=0.
  const chain = Buffer.concat([Buffer.from([0x19, 0x1c, 0x15, 2, 0x15, selfId * 2, 0x28, 32]),
    self.public, Buffer.from([0x18, 32]), self.private, Buffer.from([0x16, 0, 0, 0])]);
  const wrappingPeer = pair();
  const wrapped = wrap(shared(wrappingPeer.privateKey, Buffer.from(qr.publicKey, 'base64')), chain);
  await post('/debug/ltsm-state/reset', {});
  const response = await post('/e2ee/unwrap-keychain', { qrKeyId: qr.keyId,
    peerPublicKey: b64(wrappingPeer.public), encryptedKeyChain: b64(wrapped), workerRestoreState: qr.workerRestoreState });
  expect(response.status).toBe(200);
  const unwrapped = await response.json();
  expect(unwrapped.keys.map(k => k.keyId)).toEqual([selfId]);
  key = unwrapped.keys[0].exportedKey; state = unwrapped.workerRestoreState;
}, 60000);
beforeEach(async () => { await post('/debug/ltsm-state/reset', {}); });
afterAll(async () => { await worker?.stop(); });

describe('native MID E2EE with restored keys', () => {
  it('decrypts an independently encrypted native message after a Worker reset', async () => {
    const p = incoming(), expected = { text: 'Native message ✓' };
    p.ciphertext = b64(seal(p, shared(peer.privateKey, self.public), JSON.stringify(expected)));
    const response = await post('/e2ee/decrypt-user-v2', p);
    expect(response.status).toBe(200);
    expect((await response.json()).payload).toEqual(expected);
  });

  it('encrypts native messages that independent AES-GCM can authenticate', async () => {
    const p = { ...incoming(), to: peerMid, from: selfMid, senderKeyId: selfId, receiverKeyId: peerId,
      sequenceNumber: 42, plaintext: b64(Buffer.from('{"text":"outgoing"}')) };
    const response = await post('/e2ee/encrypt-user-v2', p);
    expect(response.status).toBe(200);
    const wire = Buffer.from((await response.json()).ciphertext, 'base64');
    expect(wire.readBigUInt64BE(16)).toBe(42n);
    expect(open(p, shared(peer.privateKey, self.public), wire).toString()).toBe('{"text":"outgoing"}');
  });

  for (const prefix of ['c', 'r']) {
    it(`decrypts a ${prefix === 'c' ? 'group' : 'room'} message with native IDs`, async () => {
      const group = pair(), groupId = 29, groupMid = prefix + '3c'.repeat(16);
      const p = { myPrivateKey: key, myPrivateKeyId: selfId, creatorPublicKey: b64(peer.public),
        senderPublicKey: b64(peer.public), senderMid: peerMid, senderKeyId: peerId,
        groupMid, groupKeyId: groupId, groupKey: { keyVersion: 1, groupKeyId: groupId,
          creator: peerMid, creatorKeyId: peerId, receiver: selfMid, receiverKeyId: selfId,
          encryptedSharedKey: b64(wrap(shared(peer.privateKey, self.public), group.private)), allowedTypes: [], specVersion: 2 },
        to: groupMid, from: peerMid, receiverKeyId: groupId, contentType: 0, workerRestoreState: state };
      p.ciphertext = b64(seal(p, shared(peer.privateKey, group.public), '{"text":"group"}'));
      const response = await post('/e2ee/decrypt-group-v2', p);
      expect(response.status).toBe(200);
      expect((await response.json()).payload.text).toBe('group');
    });
  }

  for (const corrupt of ['ciphertext', 'recipient', 'keyId']) {
    it(`rejects tampered ${corrupt}`, async () => {
      const p = incoming(), wire = seal(p, shared(peer.privateKey, self.public), '{"text":"authentic"}');
      if (corrupt === 'ciphertext') wire[wire.length - 1] ^= 1;
      if (corrupt === 'recipient') p.to = 'u' + '4d'.repeat(16);
      if (corrupt === 'keyId') p.senderKeyId++;
      p.ciphertext = b64(wire);
      const response = await post('/e2ee/decrypt-user-v2', p);
      expect(response.status).toBe(500);
      expect((await response.json()).payload).toBeUndefined();
    });
  }

  it('retains authentication of protected Chrome IDs', async () => {
    const p = { ...incoming(), to: 'u' + 'ab'.repeat(32) };
    p.ciphertext = b64(seal(p, shared(peer.privateKey, self.public), '{"text":"not accepted"}'));
    const response = await post('/e2ee/decrypt-user-v2', p);
    expect(response.status).toBe(500);
    expect((await response.json()).error).toContain('Ciphertext MAC is invalid');
  });

  for (const mid of ['x' + 'ab'.repeat(16), 'u' + 'gh'.repeat(16), 'u' + 'AB'.repeat(16)]) {
    it(`does not treat malformed native IDs as authenticated literal IDs (${mid.slice(0, 3)})`, async () => {
      const p = { ...incoming(), to: mid, sequenceNumber: 1, plaintext: b64(Buffer.from('{}')) };
      const response = await post('/e2ee/encrypt-user-v2', p);
      expect(response.status).toBe(500);
      expect((await response.json()).ciphertext).toBeUndefined();
    });
  }
});
