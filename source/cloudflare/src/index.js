import {
  ensureInit,
  computeHmac,
  generateCurveKey,
  loadCurveKey,
  exportCurveKey,
  getCurvePublicKey,
  createE2eeChannel,
  unwrapE2eeKeyChain,
  unwrapGroupSharedKey,
  wrapGroupSharedKey,
  getE2eeKeyId,
  exportE2eeKey,
  loadE2eeKey,
  encryptE2eeChannelV2,
  decryptE2eeChannelV1,
  decryptE2eeChannelV2,
  getLtsmStateSummary,
  resetLtsmState,
} from "./sandbox.js";

const MAX_PAYLOAD_SIZE = 9 * 1024 * 1024; // 9MB

function badRequest(message) {
  return Response.json({ error: message }, { status: 400 });
}

function checkPayloadSize(request) {
  const contentLength = request.headers.get("Content-Length");
  if (contentLength) {
    const size = parseInt(contentLength, 10);
    if (size > MAX_PAYLOAD_SIZE) {
      return `payload exceeds maximum size (${size} > ${MAX_PAYLOAD_SIZE} bytes)`;
    }
  }
  return null;
}

function buildSecretSet(env) {
  const raw = env?.WORKER_SECRETS || "";
  return new Set(raw.split(",").map(s => s.trim()).filter(Boolean));
}

function checkAuth(request, env) {
  const secrets = buildSecretSet(env);
  if (secrets.size === 0) return "WORKER_SECRETS is not configured";
  const header = request.headers.get("X-Worker-Secret");
  if (!header || !secrets.has(header)) return "unauthorized";
  return null;
}

function isObject(value) {
  return !!value && typeof value === "object" && !Array.isArray(value);
}

function requireString(value, name) {
  if (typeof value !== "string" || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`);
  }
  return value;
}

function requireInteger(value, name) {
  if (!Number.isInteger(value)) {
    throw new TypeError(`${name} must be an integer`);
  }
  return value;
}

function requireUint64(value, name) {
  const integer = requireInteger(value, name);
  if (integer < 0 || !Number.isSafeInteger(integer)) {
    throw new TypeError(`${name} must be a safe non-negative integer`);
  }
  return BigInt(integer);
}

function bytesToBase64(bytes) {
  if (typeof bytes === "string") return bytes;
  if (Array.isArray(bytes)) return btoa(String.fromCharCode(...bytes));
  if (bytes instanceof Uint8Array) return btoa(String.fromCharCode(...bytes));
  throw new TypeError("bytes must be a string or byte array");
}

function base64ToBytes(value, name) {
  requireString(value, name);
  try {
    return Uint8Array.from(atob(value), (ch) => ch.charCodeAt(0));
  } catch (_) {
    throw new TypeError(`${name} must be base64`);
  }
}

function blankRestoreState() {
  return { version: 1, keys: [], qrKeys: [], peerPublicKeys: {}, groupSharedKeys: {}, channels: [] };
}

function normalizeRestoreState(value) {
  if (value === undefined || value === null) return blankRestoreState();
  if (!isObject(value)) throw new TypeError("workerRestoreState must be an object");
  const state = blankRestoreState();
  
  // TODO: Delete if state restoration is not working properly.
  const validateObjectArray = (arr, name) => {
    if (!Array.isArray(arr)) throw new TypeError(`workerRestoreState.${name} must be an array`);
    for (let i = 0; i < arr.length; i++) {
      if (!isObject(arr[i])) throw new TypeError(`workerRestoreState.${name}[${i}] must be an object`);
    }
  };

  if (value.keys !== undefined) {
    validateObjectArray(value.keys, "keys");
    state.keys = value.keys;
  }

  if (value.qrKeys !== undefined) {
    validateObjectArray(value.qrKeys, "qrKeys");
    state.qrKeys = value.qrKeys;
  }

  if (value.channels !== undefined) {
    validateObjectArray(value.channels, "channels");
    state.channels = value.channels;
  }

  if (value.peerPublicKeys !== undefined) {
    if (!isObject(value.peerPublicKeys)) throw new TypeError("workerRestoreState.peerPublicKeys must be an object");
    state.peerPublicKeys = value.peerPublicKeys;
  }

  if (value.groupSharedKeys !== undefined) {
    if (!isObject(value.groupSharedKeys)) throw new TypeError("workerRestoreState.groupSharedKeys must be an object");
    state.groupSharedKeys = value.groupSharedKeys;
  }

  console.log(`[worker.normalizeRestoreState] restored: qrKeys=${state.qrKeys.length} channels=${state.channels.length}`);
  return state;
}

function restoreHandles() {
  globalThis.__ENIL_WORKER_RESTORE_HANDLES__ ||= {
    keys: new Map(),
    qrKeys: new Map(),
    channels: new Map(),
  };
  return globalThis.__ENIL_WORKER_RESTORE_HANDLES__;
}

export function resetRestoreHandles() {
  globalThis.__ENIL_WORKER_RESTORE_HANDLES__ = {
    keys: new Map(),
    qrKeys: new Map(),
    channels: new Map(),
  };
  console.log("[worker.resetRestoreHandles] cleared isolate-local state");
}

function keyLogicalId(keyId) {
  return `my-e2ee-${keyId}`;
}

function qrKeyLogicalId(keyId) {
  return `qr-login-${keyId}`;
}

function channelLogicalId(privateKeyId, peerMid, peerKeyId) {
  const peer = typeof peerMid === "string" && peerMid.length > 0 ? peerMid : "unknown";
  return `1to1:my-${privateKeyId}:peer-${peer}-${peerKeyId}`;
}

function upsertKeyState(state, keyId, exportedKey) {
  const logicalId = keyLogicalId(keyId);
  const found = state.keys.find((key) => key.logicalId === logicalId);
  if (found) return logicalId;
  state.keys.push({ logicalId, kind: "e2ee", keyId, exportedKey });
  return logicalId;
}

function upsertQrKeyState(state, keyId, publicKey, exportedKey) {
  const logicalId = qrKeyLogicalId(keyId);
  const found = state.qrKeys.find((key) => key.logicalId === logicalId);
  if (found) {
    found.publicKey = publicKey;
    found.exportedKey = exportedKey;
    return logicalId;
  }
  state.qrKeys.push({ logicalId, kind: "qr-e2ee", keyId, publicKey, exportedKey });
  return logicalId;
}

function upsertPeerKeyState(state, peerMid, keyId, publicKey) {
  const key = typeof peerMid === "string" && peerMid.length > 0 ? peerMid : `key:${keyId}`;
  state.peerPublicKeys[key] = { keyId, publicKey };
}

function upsertChannelState(state, privateKeyId, peerMid, peerKeyId) {
  const logicalId = channelLogicalId(privateKeyId, peerMid, peerKeyId);
  const privateKeyLogicalId = keyLogicalId(privateKeyId);
  const found = state.channels.find((channel) => channel.logicalId === logicalId);
  if (found) return logicalId;
  state.channels.push({
    logicalId,
    kind: "user-e2ee",
    privateKeyLogicalId,
    peerMid,
    peerKeyId,
  });
  return logicalId;
}

function upsertGroupKeyState(state, groupMid, groupKey) {
  requireString(groupMid, "groupMid");
  if (!isObject(groupKey)) throw new TypeError("groupKey must be an object");
  const groupKeyId = requireInteger(groupKey.groupKeyId, "groupKey.groupKeyId");
  state.groupSharedKeys[groupMid] ||= {};
  state.groupSharedKeys[groupMid][groupKeyId] = {
    keyVersion: groupKey.keyVersion,
    groupKeyId,
    creator: requireString(groupKey.creator, "groupKey.creator"),
    creatorKeyId: requireInteger(groupKey.creatorKeyId, "groupKey.creatorKeyId"),
    receiver: requireString(groupKey.receiver, "groupKey.receiver"),
    receiverKeyId: requireInteger(groupKey.receiverKeyId, "groupKey.receiverKeyId"),
    encryptedSharedKey: requireString(groupKey.encryptedSharedKey, "groupKey.encryptedSharedKey"),
    allowedTypes: Array.isArray(groupKey.allowedTypes) ? groupKey.allowedTypes : [],
    specVersion: Number.isInteger(groupKey.specVersion) ? groupKey.specVersion : 1,
  };
}

async function loadRestoredPrivateKey(state, privateKeyId, privateKey) {
  const logicalId = upsertKeyState(state, privateKeyId, privateKey);
  const handles = restoreHandles();
  if (handles.keys.has(logicalId)) return handles.keys.get(logicalId);
  const keyHandle = await loadPrivateKey(privateKey);
  handles.keys.set(logicalId, keyHandle);
  return keyHandle;
}

async function loadRestoredQrKey(state, qrKeyId) {
  const qrKey = state.qrKeys.find((key) => key?.keyId === qrKeyId);
  if (!qrKey || typeof qrKey.exportedKey !== "string") {
    return qrKeyId;
  }
  const logicalId = typeof qrKey.logicalId === "string" ? qrKey.logicalId : qrKeyLogicalId(qrKeyId);
  const handles = restoreHandles();
  if (handles.qrKeys.has(logicalId)) return handles.qrKeys.get(logicalId);
  const keyHandle = await loadCurveKey(base64ToBytes(qrKey.exportedKey, "qrKey.exportedKey"));
  handles.qrKeys.set(logicalId, keyHandle);
  return keyHandle;
}

async function createQrLoginChannel(state, qrKeyId, peerPublicKey) {
  try {
    return await createE2eeChannel(qrKeyId, peerPublicKey);
  } catch (err) {
    const qrKeyMissing = err?.code === "ltsm_e2ee_key_not_exists"
      || String(err?.message || "").includes("ltsm_e2ee_key_not_exists");
    if (!qrKeyMissing) throw err;
  }
  try {
    const qrKeyHandle = await loadRestoredQrKey(state, qrKeyId);
    return await createE2eeChannel(qrKeyHandle, peerPublicKey);
  } catch (err) {
    throw new Error(`[unwrapKeychain] QR login key was lost after Worker reset and cannot be restored by LTSM: ${err.message}`);
  }
}

async function createRestoredUserChannel(state, params) {
  const logicalId = upsertChannelState(
    state,
    params.privateKeyId,
    params.peerMid,
    params.peerKeyId
  );
  const handles = restoreHandles();
  if (handles.channels.has(logicalId)) return handles.channels.get(logicalId);
  const keyHandle = await loadRestoredPrivateKey(state, params.privateKeyId, params.privateKey);
  const channelId = await createE2eeChannel(keyHandle, params.peerPublicKey);
  handles.channels.set(logicalId, channelId);
  return channelId;
}

function findPeerPublicKey(state, peerMid, peerKeyId) {
  if (typeof peerMid === "string" && isObject(state.peerPublicKeys[peerMid])) {
    return state.peerPublicKeys[peerMid].publicKey;
  }
  const found = Object.values(state.peerPublicKeys).find((entry) => {
    return isObject(entry) && entry.keyId === peerKeyId && typeof entry.publicKey === "string";
  });
  return found?.publicKey || "";
}

async function resetWorkerState() {
  resetRestoreHandles();
  return resetLtsmState();
}

async function setWorkerState(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  await resetWorkerState();

  const keysByLogicalId = new Map();
  for (const key of workerRestoreState.keys) {
    if (!Number.isInteger(key.keyId) || typeof key.exportedKey !== "string") continue;
    const logicalId = typeof key.logicalId === "string" ? key.logicalId : keyLogicalId(key.keyId);
    keysByLogicalId.set(logicalId, key);
    await loadRestoredPrivateKey(workerRestoreState, key.keyId, key.exportedKey);
  }

  for (const key of workerRestoreState.qrKeys) {
    if (!Number.isInteger(key.keyId) || typeof key.exportedKey !== "string") continue;
    await loadRestoredQrKey(workerRestoreState, key.keyId);
  }

  for (const channel of workerRestoreState.channels) {
    if (!isObject(channel)) continue;
    const key = keysByLogicalId.get(channel.privateKeyLogicalId);
    if (!key) continue;
    if (!Number.isInteger(channel.peerKeyId)) continue;
    const peerMid = typeof channel.peerMid === "string" ? channel.peerMid : "";
    const peerPublicKeyValue = findPeerPublicKey(workerRestoreState, peerMid, channel.peerKeyId);
    if (typeof peerPublicKeyValue !== "string" || peerPublicKeyValue.length === 0) continue;
    await createRestoredUserChannel(workerRestoreState, {
      privateKey: key.exportedKey,
      privateKeyId: key.keyId,
      peerMid,
      peerKeyId: channel.peerKeyId,
      peerPublicKey: base64ToBytes(peerPublicKeyValue, "peerPublicKey"),
    });
  }

  return {
    workerRestoreState,
    state: await getLtsmStateSummary(),
  };
}

async function generatePortableCurveKey(body) {
  if (body !== undefined && body !== null && !isObject(body)) {
    throw new TypeError("request body must be an object");
  }
  const workerRestoreState = normalizeRestoreState(body?.workerRestoreState);
  const keyId = await generateCurveKey();
  const pubKeyBytes = await getCurvePublicKey(keyId);
  const publicKey = bytesToBase64(pubKeyBytes);
  const exported = await exportCurveKey(keyId);
  const exportedKey = bytesToBase64(exported);
  upsertQrKeyState(workerRestoreState, keyId, publicKey, exportedKey);
  return { keyId, publicKey, workerRestoreState };
}

async function loadPrivateKey(privateKey) {
  requireString(privateKey, "privateKey");
  try {
    return await loadE2eeKey(privateKey);
  } catch (_) {
    return loadE2eeKey(base64ToBytes(privateKey, "privateKey"));
  }
}

function parseDecryptedPayload(decrypted) {
  const bytes = Array.isArray(decrypted) ? new Uint8Array(decrypted) : decrypted;
  if (!(bytes instanceof Uint8Array)) {
    throw new TypeError("decrypted payload must be bytes");
  }
  const text = new TextDecoder().decode(bytes);
  if (text.includes('\uFFFD')) {
    throw new TypeError("decrypted payload contains invalid UTF-8 (possible corruption)");
  }
  const escaped = text.replace(/[\u0000-\u001f\u0080-\u00ff]/g, (ch) => {
    return `\\u00${ch.charCodeAt(0).toString(16)}`;
  });
  return JSON.parse(escaped);
}

async function unwrapKeychain(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const qrKeyId = requireInteger(body.qrKeyId, "qrKeyId");
  const peerPublicKey = base64ToBytes(body.peerPublicKey, "peerPublicKey");
  const encryptedKeyChain = base64ToBytes(body.encryptedKeyChain, "encryptedKeyChain");
  const channelId = await createQrLoginChannel(workerRestoreState, qrKeyId, peerPublicKey);
  const keyHandles = await unwrapE2eeKeyChain(channelId, encryptedKeyChain);
  if (!Array.isArray(keyHandles)) {
    throw new TypeError("unwrapped keychain must be an array");
  }
  const keys = [];
  for (const handle of keyHandles) {
    const keyId = await getE2eeKeyId(handle);
    const exported = await exportE2eeKey(handle);
    const exportedKey = bytesToBase64(exported);
    upsertKeyState(workerRestoreState, keyId, exportedKey);
    keys.push({ keyId, exportedKey });
  }
  workerRestoreState.qrKeys = workerRestoreState.qrKeys.filter((key) => key?.keyId !== qrKeyId);
  return { keys, workerRestoreState };
}

async function decryptUserV2(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const privateKey = requireString(body.privateKey, "privateKey");
  const privateKeyId = requireInteger(body.privateKeyId, "privateKeyId");
  const peerKeyId = requireInteger(body.peerKeyId, "peerKeyId");
  const peerMid = typeof body.peerMid === "string" ? body.peerMid : "";
  const peerPublicKeyValue = requireString(body.peerPublicKey, "peerPublicKey");
  const peerPublicKey = base64ToBytes(body.peerPublicKey, "peerPublicKey");
  const payload = {
    to: requireString(body.to, "to"),
    from: requireString(body.from, "from"),
    senderKeyId: requireInteger(body.senderKeyId, "senderKeyId"),
    receiverKeyId: requireInteger(body.receiverKeyId, "receiverKeyId"),
    contentType: requireInteger(body.contentType, "contentType"),
    ciphertext: base64ToBytes(body.ciphertext, "ciphertext"),
  };
  upsertPeerKeyState(workerRestoreState, peerMid, peerKeyId, peerPublicKeyValue);
  const channelId = await createRestoredUserChannel(workerRestoreState, {
    privateKey,
    privateKeyId,
    peerMid,
    peerKeyId,
    peerPublicKey,
  });
  const decrypted = await decryptE2eeChannelV2(channelId, payload);
  return { payload: parseDecryptedPayload(decrypted), workerRestoreState };
}

async function decryptUserV1(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const privateKey = requireString(body.privateKey, "privateKey");
  const privateKeyId = requireInteger(body.privateKeyId, "privateKeyId");
  const peerKeyId = requireInteger(body.peerKeyId, "peerKeyId");
  const peerMid = typeof body.peerMid === "string" ? body.peerMid : "";
  const peerPublicKeyValue = requireString(body.peerPublicKey, "peerPublicKey");
  const peerPublicKey = base64ToBytes(body.peerPublicKey, "peerPublicKey");
  const ciphertext = base64ToBytes(body.ciphertext, "ciphertext");
  upsertPeerKeyState(workerRestoreState, peerMid, peerKeyId, peerPublicKeyValue);
  const channelId = await createRestoredUserChannel(workerRestoreState, {
    privateKey,
    privateKeyId,
    peerMid,
    peerKeyId,
    peerPublicKey,
  });
  const decrypted = await decryptE2eeChannelV1(channelId, ciphertext);
  return { payload: parseDecryptedPayload(decrypted), workerRestoreState };
}

async function createGroupChannel(state, params) {
  const myKeyHandle = await loadRestoredPrivateKey(state, params.myPrivateKeyId, params.myPrivateKey);
  const creatorChannelId = await createE2eeChannel(myKeyHandle, params.creatorPublicKey);
  const groupKeyHandle = await unwrapGroupSharedKey(creatorChannelId, params.encryptedSharedKey);
  return createE2eeChannel(groupKeyHandle, params.senderPublicKey);
}

function readGroupDecryptParams(body) {
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const groupMid = requireString(body.groupMid, "groupMid");
  const groupKey = body.groupKey;
  if (!isObject(groupKey)) throw new TypeError("groupKey must be an object");
  const myPrivateKey = requireString(body.myPrivateKey, "myPrivateKey");
  const myPrivateKeyId = requireInteger(body.myPrivateKeyId, "myPrivateKeyId");
  const senderMid = requireString(body.senderMid, "senderMid");
  const senderKeyId = requireInteger(body.senderKeyId, "senderKeyId");
  const groupKeyId = requireInteger(body.groupKeyId, "groupKeyId");
  const creatorPublicKeyValue = requireString(body.creatorPublicKey, "creatorPublicKey");
  const senderPublicKeyValue = requireString(body.senderPublicKey, "senderPublicKey");
  upsertGroupKeyState(workerRestoreState, groupMid, groupKey);
  upsertPeerKeyState(workerRestoreState, groupKey.creator, groupKey.creatorKeyId, creatorPublicKeyValue);
  upsertPeerKeyState(workerRestoreState, senderMid, senderKeyId, senderPublicKeyValue);
  return {
    workerRestoreState,
    groupMid,
    myPrivateKey,
    myPrivateKeyId,
    senderMid,
    senderKeyId,
    groupKeyId,
    creatorPublicKey: base64ToBytes(creatorPublicKeyValue, "creatorPublicKey"),
    senderPublicKey: base64ToBytes(senderPublicKeyValue, "senderPublicKey"),
    encryptedSharedKey: base64ToBytes(groupKey.encryptedSharedKey, "groupKey.encryptedSharedKey"),
  };
}

async function decryptGroupV2(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const params = readGroupDecryptParams(body);
  const channelId = await createGroupChannel(params.workerRestoreState, params);
  const payload = {
    to: requireString(body.to, "to"),
    from: requireString(body.from, "from"),
    senderKeyId: params.senderKeyId,
    receiverKeyId: params.groupKeyId,
    contentType: requireInteger(body.contentType, "contentType"),
    ciphertext: base64ToBytes(body.ciphertext, "ciphertext"),
  };
  const decrypted = await decryptE2eeChannelV2(channelId, payload);
  return { payload: parseDecryptedPayload(decrypted), workerRestoreState: params.workerRestoreState };
}

async function decryptGroupV1(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const params = readGroupDecryptParams(body);
  const channelId = await createGroupChannel(params.workerRestoreState, params);
  const ciphertext = base64ToBytes(body.ciphertext, "ciphertext");
  const decrypted = await decryptE2eeChannelV1(channelId, ciphertext);
  return { payload: parseDecryptedPayload(decrypted), workerRestoreState: params.workerRestoreState };
}

async function encryptGroupV2(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const params = readGroupDecryptParams(body);
  const channelId = await createGroupChannel(params.workerRestoreState, params);
  const payload = {
    to: requireString(body.to, "to"),
    from: requireString(body.from, "from"),
    senderKeyId: params.senderKeyId,
    receiverKeyId: params.groupKeyId,
    contentType: requireInteger(body.contentType, "contentType"),
    sequenceNumber: requireUint64(body.sequenceNumber, "sequenceNumber"),
    plaintext: base64ToBytes(body.plaintext, "plaintext"),
  };
  const encrypted = await encryptE2eeChannelV2(channelId, payload);
  return { ciphertext: bytesToBase64(encrypted), workerRestoreState: params.workerRestoreState };
}

async function encryptUserV2(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const privateKey = requireString(body.privateKey, "privateKey");
  const privateKeyId = requireInteger(body.privateKeyId, "privateKeyId");
  const peerKeyId = requireInteger(body.peerKeyId, "peerKeyId");
  const peerMid = typeof body.peerMid === "string" ? body.peerMid : "";
  const peerPublicKeyValue = requireString(body.peerPublicKey, "peerPublicKey");
  const peerPublicKey = base64ToBytes(body.peerPublicKey, "peerPublicKey");
  const payload = {
    to: requireString(body.to, "to"),
    from: requireString(body.from, "from"),
    senderKeyId: requireInteger(body.senderKeyId, "senderKeyId"),
    receiverKeyId: requireInteger(body.receiverKeyId, "receiverKeyId"),
    contentType: requireInteger(body.contentType, "contentType"),
    sequenceNumber: requireUint64(body.sequenceNumber, "sequenceNumber"),
    plaintext: base64ToBytes(body.plaintext, "plaintext"),
  };
  upsertPeerKeyState(workerRestoreState, peerMid, peerKeyId, peerPublicKeyValue);
  const channelId = await createRestoredUserChannel(workerRestoreState, {
    privateKey,
    privateKeyId,
    peerMid,
    peerKeyId,
    peerPublicKey,
  });
  const encrypted = await encryptE2eeChannelV2(channelId, payload);
  return { ciphertext: bytesToBase64(encrypted), workerRestoreState };
}

async function createGroupKey(body) {
  if (!isObject(body)) throw new TypeError("request body must be an object");
  const workerRestoreState = normalizeRestoreState(body.workerRestoreState);
  const myPrivateKey = requireString(body.myPrivateKey, "myPrivateKey");
  const myPrivateKeyId = requireInteger(body.myPrivateKeyId, "myPrivateKeyId");
  const members = body.members;
  if (!Array.isArray(members) || members.length === 0)
    throw new TypeError("members must be a non-empty array");

  const sharedKeyId = await generateCurveKey();
  const myKeyHandle = await loadRestoredPrivateKey(workerRestoreState, myPrivateKeyId, myPrivateKey);

  const encryptedKeys = [];
  for (const member of members) {
    const memberPublicKey = base64ToBytes(
      requireString(member.publicKey, "member.publicKey"), "member.publicKey");
    const channelId = await createE2eeChannel(myKeyHandle, memberPublicKey);
    const encrypted = await wrapGroupSharedKey(channelId, sharedKeyId);
    encryptedKeys.push({
      mid: requireString(member.mid, "member.mid"),
      keyId: requireInteger(member.keyId, "member.keyId"),
      encryptedKey: bytesToBase64(encrypted),
    });
  }

  return { workerRestoreState, encryptedKeys };
}

export default {
  async fetch(request, env) {
    try {
      const { pathname } = new URL(request.url);

      // Check payload size for any request with a body
      const sizeError = checkPayloadSize(request);
      if (sizeError) return badRequest(sizeError);

      const authError = checkAuth(request, env);
      if (authError) return new Response(authError, { status: 401 });

      if (pathname === "/sign" && request.method === "POST") {
        await ensureInit();
        const { path, body, accessToken = "" } = await request.json();
        if (typeof path !== "string" || typeof body !== "string") {
          return Response.json({ error: "path and body are required strings" }, { status: 400 });
        }
        const hmac = await computeHmac(path, body, accessToken);
        return Response.json({ hmac });
      }

      if (pathname === "/keygen" && request.method === "POST") {
        try {
          return Response.json(await generatePortableCurveKey(await request.json().catch(() => ({}))));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/debug/ltsm-state" && request.method === "GET") {
        return Response.json(await getLtsmStateSummary());
      }

      if (pathname === "/debug/ltsm-state/set" && request.method === "POST") {
        try {
          return Response.json(await setWorkerState(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/debug/ltsm-state/reset" && request.method === "POST") {
        return Response.json(await resetWorkerState());
      }

      if (pathname === "/e2ee/unwrap-keychain" && request.method === "POST") {
        try {
          return Response.json(await unwrapKeychain(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/decrypt-user-v2" && request.method === "POST") {
        try {
          return Response.json(await decryptUserV2(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/decrypt-user-v1" && request.method === "POST") {
        try {
          return Response.json(await decryptUserV1(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/decrypt-group-v2" && request.method === "POST") {
        try {
          return Response.json(await decryptGroupV2(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/decrypt-group-v1" && request.method === "POST") {
        try {
          return Response.json(await decryptGroupV1(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/encrypt-user-v2" && request.method === "POST") {
        try {
          return Response.json(await encryptUserV2(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/encrypt-group-v2" && request.method === "POST") {
        try {
          return Response.json(await encryptGroupV2(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      if (pathname === "/e2ee/create-group-key" && request.method === "POST") {
        try {
          return Response.json(await createGroupKey(await request.json()));
        } catch (err) {
          if (err instanceof TypeError) return badRequest(err.message);
          throw err;
        }
      }

      return new Response("Not Found", { status: 404 });
    } catch (err) {
      console.error("[worker.fetch]", err.message);
      return Response.json({ error: err.message }, { status: 500 });
    } finally {
      // TODO: Delete if state restoration is not working properly.
      // Clear isolate-local state at end of request. The __ENIL_WORKER_RESTORE_HANDLES__
      // map persists across warm isolate reuse in Cloudflare Workers; we clear it to ensure
      // the next request starts fresh and forces restoration from workerRestoreState.
      resetRestoreHandles();
    }
  },
};
