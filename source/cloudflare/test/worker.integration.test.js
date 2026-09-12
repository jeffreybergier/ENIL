// Integration tests — real WASM via wrangler unstable_dev, no mocks.
// Uses synthetic requests and keys generated at test time; no captured account data.
// Requires: npm run build (ltsm-worker.js + ltsm.wasm must exist in src/).

import { unstable_dev } from "wrangler";
import { describe, it, expect, beforeAll, afterAll } from "vitest";

let worker;

const TEST_SECRET = "test-secret";

beforeAll(async () => {
  worker = await unstable_dev("src/index.js", {
    experimental: { disableExperimentalWarning: true },
    local: true,
    logLevel: "error",
    vars: { WORKER_SECRETS: TEST_SECRET },
  });
}, 60000);

afterAll(async () => {
  await worker?.stop();
});

function url(path) {
  return `http://${worker.address}:${worker.port}${path}`;
}

function post(path, body) {
  return worker.fetch(url(path), {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "X-Worker-Secret": TEST_SECRET,
    },
    body: JSON.stringify(body),
  });
}

function get(path) {
  return worker.fetch(url(path), {
    headers: { "X-Worker-Secret": TEST_SECRET },
  });
}

describe("POST /sign", () => {
  it("returns hmac for valid input", async () => {
    const resp = await post("/sign", { path: "/api/foo", body: "[{}]", accessToken: "" });
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(typeof body.hmac).toBe("string");
    expect(body.hmac.length).toBeGreaterThan(0);
    expect(body.hmac).not.toBe("mock-hmac");
  });

  it("is deterministic for the same inputs", async () => {
    const args = { path: "/api/foo", body: "[{}]", accessToken: "" };
    const [r1, r2] = await Promise.all([post("/sign", args), post("/sign", args)]);
    const [b1, b2] = await Promise.all([r1.json(), r2.json()]);
    expect(b1.hmac).toBe(b2.hmac);
  });

  it("changes for a different path", async () => {
    const [r1, r2] = await Promise.all([
      post("/sign", { path: "/api/foo", body: "[{}]", accessToken: "" }),
      post("/sign", { path: "/api/bar", body: "[{}]", accessToken: "" }),
    ]);
    const [b1, b2] = await Promise.all([r1.json(), r2.json()]);
    expect(b1.hmac).not.toBe(b2.hmac);
  });

  it("returns 400 when path or body missing", async () => {
    const resp = await post("/sign", { path: "/api/foo" });
    expect(resp.status).toBe(400);
  });
});

describe("POST /keygen", () => {
  it("returns keyId, base64 publicKey, and portable QR key state", async () => {
    const resp = await post("/keygen", {});
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(typeof body.keyId).toBe("number");
    // 32-byte Curve25519 public key encodes to exactly 44 base64 chars
    expect(body.publicKey.length).toBe(44);
    const [qrKey] = body.workerRestoreState.qrKeys;
    expect(qrKey.kind).toBe("qr-e2ee");
    expect(qrKey.keyId).toBe(body.keyId);
    expect(qrKey.publicKey).toBe(body.publicKey);
    expect(typeof qrKey.exportedKey).toBe("string");
  });

  it("generates a different key on each call", async () => {
    const [r1, r2] = await Promise.all([post("/keygen", {}), post("/keygen", {})]);
    const [b1, b2] = await Promise.all([r1.json(), r2.json()]);
    expect(b1.publicKey).not.toBe(b2.publicKey);
  });
});

describe("GET /debug/ltsm-state", () => {
  it("returns the LTSM state summary", async () => {
    const resp = await get("/debug/ltsm-state");
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(typeof body.hasStorageKey).toBe("boolean");
    expect(typeof body.keyCount).toBe("number");
    expect(typeof body.channelCount).toBe("number");
    expect(typeof body.nextObjectId).toBe("number");
  });

  it("keyCount increases after keygen", async () => {
    await post("/debug/ltsm-state/reset", {});
    await post("/keygen", {});
    const body = await (await get("/debug/ltsm-state")).json();
    expect(body.keyCount).toBe(1);
  });
});

describe("POST /debug/ltsm-state/reset", () => {
  it("resets the LTSM state summary", async () => {
    await post("/keygen", {});
    const resp = await post("/debug/ltsm-state/reset", {});
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.hasStorageKey).toBe(false);
    expect(body.keyCount).toBe(0);
    expect(body.channelCount).toBe(0);
    expect(body.nextObjectId).toBe(0);
  });
});

describe("POST /debug/ltsm-state/set", () => {
  it("restores handles from portable state", async () => {
    // Use a real keygen-derived QR key as restore material.
    const kg = await (await post("/keygen", {})).json();
    await post("/debug/ltsm-state/reset", {});

    const resp = await post("/debug/ltsm-state/set", {
      workerRestoreState: {
        version: 1,
        qrKeys: [kg.workerRestoreState.qrKeys[0]],
        keys: [],
        channels: [],
        peerPublicKeys: {},
        groupSharedKeys: {},
      },
    });
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(body.workerRestoreState.qrKeys).toHaveLength(1);
    expect(body.state.keyCount).toBe(1);
  });
});

