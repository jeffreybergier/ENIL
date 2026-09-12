// Unit tests for the Worker routing logic.
// Mocks sandbox so no WASM needed.

import { describe, it, expect, vi } from "vitest";

vi.mock("../src/sandbox.js", () => ({
  ensureInit: vi.fn().mockResolvedValue(undefined),
  computeHmac: vi.fn().mockResolvedValue("mock-hmac"),
  generateCurveKey: vi.fn().mockResolvedValue(42),
  loadCurveKey: vi.fn().mockResolvedValue(43),
  exportCurveKey: vi.fn().mockResolvedValue(new Uint8Array([12, 13, 14])),
  getCurvePublicKey: vi.fn().mockResolvedValue(new Uint8Array([1, 2, 3, 4])),
  createE2eeChannel: vi.fn().mockResolvedValue(7),
  unwrapE2eeKeyChain: vi.fn().mockResolvedValue([8]),
  unwrapGroupSharedKey: vi.fn().mockResolvedValue(10),
  wrapGroupSharedKey: vi.fn().mockResolvedValue(new Uint8Array([12, 13, 14])),
  getE2eeKeyId: vi.fn().mockResolvedValue(123),
  exportE2eeKey: vi.fn().mockResolvedValue(new Uint8Array([9, 10, 11])),
  loadE2eeKey: vi.fn().mockResolvedValue(9),
  decryptE2eeChannelV1: vi.fn().mockResolvedValue(
    new TextEncoder().encode(JSON.stringify({ text: "old hello" }))
  ),
  decryptE2eeChannelV2: vi.fn().mockResolvedValue(
    new TextEncoder().encode(JSON.stringify({ text: "hello" }))
  ),
  encryptE2eeChannelV2: vi.fn().mockResolvedValue(new Uint8Array([1, 2, 3])),
  getLtsmStateSummary: vi.fn().mockResolvedValue({
    hasStorageKey: false,
    keyCount: 2,
    channelCount: 1,
    nextObjectId: 3,
  }),
  resetLtsmState: vi.fn().mockResolvedValue({
    hasStorageKey: false,
    keyCount: 0,
    channelCount: 0,
    nextObjectId: 0,
  }),
}));

const { default: worker } = await import("../src/index.js");

const TEST_SECRET = "test-secret";
const TEST_ENV = { WORKER_SECRETS: TEST_SECRET };

function req(url, method = "GET", body) {
  return new Request(url, {
    method,
    headers: {
      "X-Worker-Secret": TEST_SECRET,
      ...(body ? { "Content-Type": "application/json" } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
}

describe("POST /sign", () => {
  it("returns hmac for valid input", async () => {
    const resp = await worker.fetch(req("https://example.com/sign", "POST", {
      path: "/api/foo",
      body: "[{}]",
      accessToken: "",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.hmac).toBe("mock-hmac");
  });

  it("returns 400 when path or body missing", async () => {
    const resp = await worker.fetch(req("https://example.com/sign", "POST", { path: "/api/foo" }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /keygen", () => {
  it("returns keyId, base64 publicKey, and portable QR key state", async () => {
    const resp = await worker.fetch(req("https://example.com/keygen", "POST", {}), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.keyId).toBe(42);
    expect(body.publicKey).toBe("AQIDBA==");
    expect(body.workerRestoreState.qrKeys).toEqual([{
      logicalId: "qr-login-42",
      kind: "qr-e2ee",
      keyId: 42,
      publicKey: "AQIDBA==",
      exportedKey: "DA0O",
    }]);
  });
});

describe("GET /debug/ltsm-state", () => {
  it("returns the LTSM state summary", async () => {
    const resp = await worker.fetch(req("https://example.com/debug/ltsm-state"), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body).toEqual({
      hasStorageKey: false,
      keyCount: 2,
      channelCount: 1,
      nextObjectId: 3,
    });
  });
});

describe("POST /debug/ltsm-state/reset", () => {
  it("resets the LTSM state summary", async () => {
    const resp = await worker.fetch(req("https://example.com/debug/ltsm-state/reset", "POST", {}), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body).toEqual({
      hasStorageKey: false,
      keyCount: 0,
      channelCount: 0,
      nextObjectId: 0,
    });
  });
});

describe("POST /debug/ltsm-state/set", () => {
  it("restores handles from portable state", async () => {
    const resp = await worker.fetch(req("https://example.com/debug/ltsm-state/set", "POST", {
      workerRestoreState: {
        version: 1,
        keys: [{
          logicalId: "my-e2ee-2",
          kind: "e2ee",
          keyId: 2,
          exportedKey: "exported-key",
        }],
        peerPublicKeys: {
          "u-peer": { keyId: 1, publicKey: "AQID" },
        },
        channels: [{
          logicalId: "1to1:my-2:peer-u-peer-1",
          kind: "user-e2ee",
          privateKeyLogicalId: "my-e2ee-2",
          peerMid: "u-peer",
          peerKeyId: 1,
        }],
      },
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.workerRestoreState.keys).toHaveLength(1);
    expect(body.state).toEqual({
      hasStorageKey: false,
      keyCount: 2,
      channelCount: 1,
      nextObjectId: 3,
    });
  });
});

describe("POST /e2ee/unwrap-keychain", () => {
  it("returns exported keys for valid input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/unwrap-keychain", "POST", {
      qrKeyId: 42,
      peerPublicKey: "AQID",
      encryptedKeyChain: "BAUG",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.keys).toEqual([{ keyId: 123, exportedKey: "CQoL" }]);
    expect(body.workerRestoreState.keys).toEqual([{
      logicalId: "my-e2ee-123",
      kind: "e2ee",
      keyId: 123,
      exportedKey: "CQoL",
    }]);
  });

  it("reloads the QR key from portable state", async () => {
    const sandbox = await import("../src/sandbox.js");
    vi.clearAllMocks();
    sandbox.loadCurveKey.mockResolvedValueOnce(99);
    const missingKey = new Error("missing");
    missingKey.code = "ltsm_e2ee_key_not_exists";
    sandbox.createE2eeChannel
      .mockRejectedValueOnce(missingKey)
      .mockResolvedValueOnce(7);
    const resp = await worker.fetch(req("https://example.com/e2ee/unwrap-keychain", "POST", {
      qrKeyId: 42,
      peerPublicKey: "AQID",
      encryptedKeyChain: "BAUG",
      workerRestoreState: {
        version: 1,
        qrKeys: [{
          logicalId: "qr-login-42",
          kind: "qr-e2ee",
          keyId: 42,
          publicKey: "AQIDBA==",
          exportedKey: "DA0O",
        }],
      },
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    expect(sandbox.loadCurveKey).toHaveBeenCalledWith(new Uint8Array([12, 13, 14]));
    expect(sandbox.createE2eeChannel).toHaveBeenCalledWith(99, new Uint8Array([1, 2, 3]));
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/unwrap-keychain", "POST", {
      qrKeyId: "42",
      peerPublicKey: "AQID",
      encryptedKeyChain: "BAUG",
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/decrypt-user-v2", () => {
  it("returns parsed plaintext payload for valid input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-user-v2", "POST", {
      privateKey: "exported-key",
      privateKeyId: 2,
      peerPublicKey: "AQID",
      peerMid: "u-peer",
      peerKeyId: 1,
      to: "u-to",
      from: "u-from",
      senderKeyId: 1,
      receiverKeyId: 2,
      contentType: 0,
      ciphertext: "BAUG",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.payload).toEqual({ text: "hello" });
    expect(body.workerRestoreState.channels[0]).toEqual(expect.objectContaining({
      logicalId: "1to1:my-2:peer-u-peer-1",
      privateKeyLogicalId: "my-e2ee-2",
    }));
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-user-v2", "POST", {
      privateKey: "exported-key",
      privateKeyId: 2,
      peerPublicKey: "AQID",
      peerMid: "u-peer",
      peerKeyId: 1,
      to: "u-to",
      from: "u-from",
      senderKeyId: 1,
      receiverKeyId: 2,
      contentType: "0",
      ciphertext: "BAUG",
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/decrypt-user-v1", () => {
  it("returns parsed plaintext payload for valid input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-user-v1", "POST", {
      privateKey: "exported-key",
      privateKeyId: 2,
      peerPublicKey: "AQID",
      peerMid: "u-peer",
      peerKeyId: 1,
      ciphertext: "BAUG",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.payload).toEqual({ text: "old hello" });
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-user-v1", "POST", {
      privateKey: "exported-key",
      peerPublicKey: "AQID",
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/decrypt-group-v2", () => {
  it("unwraps the group key and returns parsed plaintext", async () => {
    const { unwrapGroupSharedKey, decryptE2eeChannelV2 } = await import("../src/sandbox.js");
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-group-v2", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      groupMid: "c-group",
      groupKeyId: 77,
      groupKey: {
        keyVersion: 1,
        groupKeyId: 77,
        creator: "u-creator",
        creatorKeyId: 3,
        receiver: "u-me",
        receiverKeyId: 2,
        encryptedSharedKey: "BAUG",
        allowedTypes: [0],
        specVersion: 2,
      },
      creatorPublicKey: "AQID",
      senderPublicKey: "AgME",
      senderMid: "u-sender",
      senderKeyId: 4,
      to: "c-group",
      from: "u-sender",
      contentType: 0,
      ciphertext: "CQoL",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.payload).toEqual({ text: "hello" });
    expect(unwrapGroupSharedKey).toHaveBeenCalledWith(7, new Uint8Array([4, 5, 6]));
    expect(decryptE2eeChannelV2).toHaveBeenCalledWith(7, expect.objectContaining({
      senderKeyId: 4,
      receiverKeyId: 77,
    }));
    expect(body.workerRestoreState.groupSharedKeys["c-group"]["77"]).toEqual(expect.objectContaining({
      creator: "u-creator",
      receiverKeyId: 2,
    }));
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-group-v2", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      groupMid: "c-group",
      groupKeyId: 77,
      groupKey: {},
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/decrypt-group-v1", () => {
  it("returns parsed plaintext payload for valid input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/decrypt-group-v1", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      groupMid: "c-group",
      groupKeyId: 77,
      groupKey: {
        keyVersion: 1,
        groupKeyId: 77,
        creator: "u-creator",
        creatorKeyId: 3,
        receiver: "u-me",
        receiverKeyId: 2,
        encryptedSharedKey: "BAUG",
        allowedTypes: [0],
        specVersion: 1,
      },
      creatorPublicKey: "AQID",
      senderPublicKey: "AgME",
      senderMid: "u-sender",
      senderKeyId: 4,
      ciphertext: "CQoL",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.payload).toEqual({ text: "old hello" });
  });
});

describe("POST /e2ee/encrypt-user-v2", () => {
  it("returns ciphertext for valid input", async () => {
    const { encryptE2eeChannelV2 } = await import("../src/sandbox.js");
    const resp = await worker.fetch(req("https://example.com/e2ee/encrypt-user-v2", "POST", {
      privateKey: "exported-key",
      privateKeyId: 1,
      peerPublicKey: "AQID",
      peerMid: "u-peer",
      peerKeyId: 2,
      to: "u-to",
      from: "u-from",
      senderKeyId: 1,
      receiverKeyId: 2,
      contentType: 0,
      sequenceNumber: 0,
      plaintext: "eyJ0ZXh0IjoiaGkifQ==",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.ciphertext).toBe("AQID");
    expect(body.workerRestoreState.peerPublicKeys["u-peer"]).toEqual({
      keyId: 2,
      publicKey: "AQID",
    });
    expect(encryptE2eeChannelV2).toHaveBeenCalledWith(7, expect.objectContaining({
      sequenceNumber: 0n,
    }));
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/encrypt-user-v2", "POST", {
      privateKey: "exported-key",
      peerPublicKey: "AQID",
      to: "u-to",
      from: "u-from",
      senderKeyId: 1,
      receiverKeyId: 2,
      contentType: 0,
      sequenceNumber: "0",
      plaintext: "eyJ0ZXh0IjoiaGkifQ==",
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/encrypt-group-v2", () => {
  it("unwraps the group key and returns ciphertext", async () => {
    const { encryptE2eeChannelV2, unwrapGroupSharedKey } = await import("../src/sandbox.js");
    const resp = await worker.fetch(req("https://example.com/e2ee/encrypt-group-v2", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      groupMid: "c-group",
      groupKeyId: 77,
      groupKey: {
        keyVersion: 1,
        groupKeyId: 77,
        creator: "u-creator",
        creatorKeyId: 3,
        receiver: "u-me",
        receiverKeyId: 2,
        encryptedSharedKey: "BAUG",
        allowedTypes: [0],
        specVersion: 2,
      },
      creatorPublicKey: "AQID",
      senderPublicKey: "AgME",
      senderMid: "u-me",
      senderKeyId: 4,
      to: "c-group",
      from: "u-me",
      contentType: 0,
      sequenceNumber: 0,
      plaintext: "eyJ0ZXh0IjoiaGkifQ==",
    }), TEST_ENV);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.ciphertext).toBe("AQID");
    expect(unwrapGroupSharedKey).toHaveBeenCalledWith(7, new Uint8Array([4, 5, 6]));
    expect(encryptE2eeChannelV2).toHaveBeenLastCalledWith(7, expect.objectContaining({
      to: "c-group",
      from: "u-me",
      senderKeyId: 4,
      receiverKeyId: 77,
      sequenceNumber: 0n,
    }));
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/encrypt-group-v2", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      groupMid: "c-group",
      groupKeyId: 77,
      groupKey: {},
      sequenceNumber: "0",
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("POST /e2ee/create-group-key", () => {
  it("generates a group key and wraps it for every member", async () => {
    const sandbox = await import("../src/sandbox.js");
    vi.clearAllMocks();
    sandbox.generateCurveKey.mockResolvedValueOnce(55);
    sandbox.createE2eeChannel
      .mockResolvedValueOnce(71)
      .mockResolvedValueOnce(72);
    sandbox.wrapGroupSharedKey
      .mockResolvedValueOnce(new Uint8Array([10, 11]))
      .mockResolvedValueOnce(new Uint8Array([12, 13]));

    const resp = await worker.fetch(req("https://example.com/e2ee/create-group-key", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      members: [{
        mid: "u-a",
        keyId: 3,
        publicKey: "AQID",
      }, {
        mid: "u-b",
        keyId: 4,
        publicKey: "BAUG",
      }],
    }), TEST_ENV);

    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.encryptedKeys).toEqual([{
      mid: "u-a",
      keyId: 3,
      encryptedKey: "Cgs=",
    }, {
      mid: "u-b",
      keyId: 4,
      encryptedKey: "DA0=",
    }]);
    expect(sandbox.generateCurveKey).toHaveBeenCalledTimes(1);
    expect(sandbox.createE2eeChannel).toHaveBeenNthCalledWith(1, 9, new Uint8Array([1, 2, 3]));
    expect(sandbox.createE2eeChannel).toHaveBeenNthCalledWith(2, 9, new Uint8Array([4, 5, 6]));
    expect(sandbox.wrapGroupSharedKey).toHaveBeenNthCalledWith(1, 71, 55);
    expect(sandbox.wrapGroupSharedKey).toHaveBeenNthCalledWith(2, 72, 55);
    expect(body.workerRestoreState.keys).toEqual([{
      logicalId: "my-e2ee-2",
      kind: "e2ee",
      keyId: 2,
      exportedKey: "exported-key",
    }]);
  });

  it("returns 400 for malformed input", async () => {
    const resp = await worker.fetch(req("https://example.com/e2ee/create-group-key", "POST", {
      myPrivateKey: "exported-key",
      myPrivateKeyId: 2,
      members: [],
    }), TEST_ENV);
    expect(resp.status).toBe(400);
  });
});

describe("auth", () => {
  it("returns 401 with no secret header", async () => {
    const r = new Request("https://example.com/sign", { method: "POST" });
    const resp = await worker.fetch(r, TEST_ENV);
    expect(resp.status).toBe(401);
  });

  it("returns 401 with wrong secret", async () => {
    const r = new Request("https://example.com/sign", {
      method: "POST",
      headers: { "X-Worker-Secret": "wrong" },
    });
    const resp = await worker.fetch(r, TEST_ENV);
    expect(resp.status).toBe(401);
  });

  it("returns 401 when WORKER_SECRETS not configured", async () => {
    const r = new Request("https://example.com/sign", {
      method: "POST",
      headers: { "X-Worker-Secret": TEST_SECRET },
    });
    const resp = await worker.fetch(r, {});
    expect(resp.status).toBe(401);
  });
});

describe("unknown routes", () => {
  it("returns 404 for unknown path", async () => {
    const resp = await worker.fetch(req("https://example.com/unknown"), TEST_ENV);
    expect(resp.status).toBe(404);
  });

  it("returns 404 for GET /qr (endpoint removed)", async () => {
    const resp = await worker.fetch(req("https://example.com/qr"), TEST_ENV);
    expect(resp.status).toBe(404);
  });
});

describe("error handling", () => {
  it("returns 500 JSON on unexpected error", async () => {
    const { computeHmac } = await import("../src/sandbox.js");
    computeHmac.mockRejectedValueOnce(new Error("boom"));

    const resp = await worker.fetch(req("https://example.com/sign", "POST", {
      path: "/api/foo",
      body: "[{}]",
    }), TEST_ENV);
    expect(resp.status).toBe(500);
    const body = await resp.json();
    expect(body.error).toBe("boom");
  });
});
