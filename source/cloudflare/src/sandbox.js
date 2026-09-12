// Runs the LINE LTSM WASM sandbox inside the Worker's V8 global.
// Globals are set up first, then the patched bundle is imported as a module
// (avoiding new Function / eval which are blocked in workerd).

import ltsmWasm from "./ltsm.wasm";

const EXTENSION_ORIGIN = "chrome-extension://ophjlpahpchlmihnnnihgmmeilfjmjjc";
const SANDBOX_ID = "1";

let initPromise = null;
let _dispatch = null;

async function init() {
  const pending = new Map();
  const domHandlers = [];
  let requestIdCounter = 0;

  globalThis.window = globalThis;

  try {
    Object.defineProperty(globalThis, "origin", {
      value: EXTENSION_ORIGIN,
      configurable: true,
      writable: true,
    });
  } catch (_) {}

  // Collect DOMContentLoaded handlers — fired manually after the bundle loads.
  globalThis.document = {
    addEventListener(event, fn) {
      if (event === "DOMContentLoaded") domHandlers.push(fn);
    },
  };

  // location.search must contain sandboxId so the sandbox registers SS().
  globalThis.location = new URL(
    `https://ltsm.invalid/ltsmSandbox.html?sandboxId=${SANDBOX_ID}`
  );

  // Use the bundled WebAssembly.Module instead of streaming from a URL.
  WebAssembly.instantiateStreaming = async (_, imports) => {
    const instance = await WebAssembly.instantiate(ltsmWasm, imports);
    return { instance, module: ltsmWasm };
  };

  // Deny-all network policy: the sandbox must be a pure crypto oracle.
  // Only .wasm fetches (intercepted by the Emscripten loader) are allowed,
  // and they return empty — the real module is bundled and instantiated via
  // the WebAssembly.instantiateStreaming override above.
  globalThis.fetch = (url) => {
    const u = typeof url === "string" ? url : (url?.url ?? "");
    if (u.endsWith(".wasm")) return Promise.resolve(new Response(""));
    throw new Error(`[sandbox] outbound fetch blocked: ${u}`);
  };
  if (typeof globalThis.WebSocket === "function") {
    globalThis.WebSocket = function () {
      throw new Error("[sandbox] WebSocket blocked");
    };
  }

  // Capture sandbox responses sent via window.parent.postMessage.
  globalThis.parent = {
    postMessage(msg) {
      if (msg?.type !== "response" && msg?.type !== "error") return;
      const p = pending.get(msg.sandboxId);
      if (!p) return;
      pending.delete(msg.sandboxId);
      clearTimeout(p.timer);
      if (msg.type === "error") {
        const d = msg.data;
        const text = d?.message || d?.code || d?.toString?.() || JSON.stringify(d);
        p.reject(new Error(`[ltsm] ${text}`));
      } else p.resolve(msg.data);
    },
  };

  // Import the minimal worker script AFTER globals are ready — no eval needed.
  await import("./ltsm-worker.js");

  // Fire DOMContentLoaded so the sandbox registers its message handler (SS).
  for (const h of domHandlers) {
    try { h(); } catch (_) {}
  }

  const startTime = Date.now();
  await new Promise((resolve, reject) => {
    const checkReady = () => {
      if (typeof globalThis.__ENIL_LTSM_DEBUG__?.getStateSummary === "function") {
        resolve();
      } else if (Date.now() - startTime >= 5000) {
        reject(new Error("[sandbox.init] timed out waiting for LTSM debug bridge"));
      } else {
        setTimeout(checkReady, 5);
      }
    };
    checkReady();
  });
  const elapsed = Date.now() - startTime;
  console.log(`[sandbox.init] WASM sandbox ready in ${elapsed}ms`);

  // extra is spread directly into the message data object.
  // For get_hmac pass { payload: {...} }; for e2ee commands pass top-level fields directly.
  _dispatch = (command, extra = {}) =>
    new Promise((resolve, reject) => {
      const sandboxId = String(++requestIdCounter);
      console.log(`[sandbox.dispatch] ${command} (id=${sandboxId})`);
      const timer = setTimeout(() => {
        pending.delete(sandboxId);
        reject(new Error(`[sandbox] timeout: ${command}`));
      }, 30000);
      pending.set(sandboxId, { resolve, reject, timer });
      globalThis.dispatchEvent(
        new MessageEvent("message", {
          data: {
            type: "request",
            sandboxId,
            data: { command, ...extra },
          },
        })
      );
    });

  await _dispatch("init");
  console.log("[sandbox.init] WASM ready");
}

export function ensureInit() {
  if (!initPromise) initPromise = init();
  return initPromise;
}

function getLtsmDebug() {
  const debug = globalThis.__ENIL_LTSM_DEBUG__;
  if (!debug || typeof debug.getStateSummary !== "function") {
    throw new Error("[sandbox.getLtsmDebug] LTSM debug bridge is not initialized");
  }
  return debug;
}

export async function getLtsmStateSummary() {
  await ensureInit();
  return getLtsmDebug().getStateSummary();
}

export async function resetLtsmState() {
  await ensureInit();
  const debug = getLtsmDebug();
  if (typeof debug.resetState !== "function") {
    throw new Error("[sandbox.resetLtsmState] LTSM reset bridge is missing");
  }
  return debug.resetState();
}

export async function computeHmac(path, body, accessToken = "") {
  await ensureInit();
  return _dispatch("get_hmac", { payload: { path, body, accessToken } });
}

export async function generateCurveKey() {
  await ensureInit();
  return _dispatch("curvekey_generate");
}

export async function loadCurveKey(exportedKey) {
  await ensureInit();
  return _dispatch("curvekey_load_key", { payload: exportedKey });
}

export async function exportCurveKey(ltsmKeyId) {
  await ensureInit();
  return _dispatch("curvekey_export_key", { ltsmKeyId });
}

export async function getCurvePublicKey(ltsmKeyId) {
  await ensureInit();
  return _dispatch("e2eekey_get_public_key", { ltsmKeyId });
}

export async function createE2eeChannel(ltsmKeyId, publicKey) {
  await ensureInit();
  return _dispatch("e2eekey_create_channel", { ltsmKeyId, payload: publicKey });
}

export async function unwrapE2eeKeyChain(channelId, encryptedKeyChain) {
  await ensureInit();
  return _dispatch("e2eechannel_unwrap_e2ee_key_chain", {
    ltsmKeyId: channelId,
    payload: encryptedKeyChain,
  });
}

export async function unwrapGroupSharedKey(channelId, encryptedSharedKey) {
  await ensureInit();
  return _dispatch("e2eechannel_unwrap_group_shared_key", {
    ltsmKeyId: channelId,
    payload: encryptedSharedKey,
  });
}

export async function getE2eeKeyId(ltsmKeyId) {
  await ensureInit();
  return _dispatch("e2eekey_get_key_id", { ltsmKeyId });
}

export async function exportE2eeKey(ltsmKeyId) {
  await ensureInit();
  return _dispatch("e2eekey_export_key", { ltsmKeyId });
}

export async function loadE2eeKey(exportedKey) {
  await ensureInit();
  return _dispatch("e2eekey_load_key", { payload: exportedKey });
}

export async function decryptE2eeChannelV2(channelId, payload) {
  await ensureInit();
  return _dispatch("e2eechannel_decrypt_v2", {
    ltsmKeyId: channelId,
    payload,
  });
}

export async function encryptE2eeChannelV2(channelId, payload) {
  await ensureInit();
  return _dispatch("e2eechannel_encrypt_v2", {
    ltsmKeyId: channelId,
    payload,
  });
}

export async function wrapGroupSharedKey(channelId, sharedKey) {
  await ensureInit();
  return _dispatch("e2eechannel_wrap_group_shared_key", {
    ltsmKeyId: channelId,
    payload: sharedKey,
  });
}

export async function decryptE2eeChannelV1(channelId, ciphertext) {
  await ensureInit();
  return _dispatch("e2eechannel_decrypt_v1", {
    ltsmKeyId: channelId,
    payload: ciphertext,
  });
}
