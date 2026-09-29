import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { createHmac, randomUUID } from "node:crypto";
import net from "node:net";
import { once } from "node:events";
import { setTimeout as delay } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const serverBinary = path.join(root, "build", "kvserver");

async function freePort() {
  const server = net.createServer();
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  const { port } = server.address();
  await new Promise((resolve, reject) => server.close((error) => error ? reject(error) : resolve()));
  return port;
}

async function waitForGateway(baseUrl, process) {
  const end = Date.now() + 10_000;
  while (Date.now() < end) {
    if (process.exitCode !== null) throw new Error(`Gateway exited with code ${process.exitCode}`);
    try {
      const response = await fetch(`${baseUrl}/health`);
      if (response.ok) return;
    } catch {}
    await delay(100);
  }
  throw new Error("Gateway did not become healthy");
}

async function api(baseUrl, token, route, init = {}) {
  const response = await fetch(`${baseUrl}/api${route}`, {
    ...init,
    headers: {
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      ...(init.body ? { "Content-Type": "application/json" } : {}),
      ...init.headers,
    },
  });
  return { response, body: await response.json() };
}

test("signed demo sessions operate on isolated keys in the C++ store", { timeout: 30_000 }, async (t) => {
  const storePort = await freePort();
  const gatewayPort = await freePort();
  const store = spawn(serverBinary, ["--host", "127.0.0.1", "--port", String(storePort), "--threads", "2"], {
    cwd: root,
    stdio: "ignore",
  });
  const gateway = spawn(process.execPath, ["index.js"], {
    cwd: path.dirname(fileURLToPath(import.meta.url)),
    env: {
      ...process.env,
      PORT: String(gatewayPort),
      KV_HOST: "127.0.0.1",
      KV_PORT: String(storePort),
      START_KV_SERVER: "false",
      WEB_ORIGINS: "http://localhost:5173",
      DEMO_SECRET: "integration-test-secret-at-least-32-bytes-long",
    },
    stdio: "ignore",
  });
  t.after(() => {
    gateway.kill("SIGTERM");
    store.kill("SIGTERM");
  });

  const baseUrl = `http://127.0.0.1:${gatewayPort}`;
  await waitForGateway(baseUrl, gateway);

  const firstSession = await api(baseUrl, null, "/demo/session", { method: "POST" });
  assert.equal(firstSession.response.status, 201);
  const firstToken = firstSession.body.accessToken;
  const validSession = await api(baseUrl, firstToken, "/demo/session");
  assert.ok(validSession.body.expiresAt > Math.floor(Date.now() / 1000));

  const initialKeys = await api(baseUrl, firstToken, "/keys");
  assert.deepEqual(initialKeys.body.keys, ["sample:json", "welcome"]);

  const created = await api(baseUrl, firstToken, "/keys", {
    method: "POST",
    body: JSON.stringify({ key: "integration:key", value: "hello", ttlSeconds: 30 }),
  });
  assert.equal(created.response.status, 201);

  const fetched = await api(baseUrl, firstToken, "/keys/integration%3Akey");
  assert.deepEqual(fetched.body, { key: "integration:key", value: "hello", ttlSeconds: 30 });

  const updated = await api(baseUrl, firstToken, "/keys", {
    method: "POST",
    body: JSON.stringify({ key: "integration:key", value: "updated", ttlSeconds: null }),
  });
  assert.equal(updated.response.status, 201);
  const fetchedAfterUpdate = await api(baseUrl, firstToken, "/keys/integration%3Akey");
  assert.ok(fetchedAfterUpdate.body.ttlSeconds > 0 && fetchedAfterUpdate.body.ttlSeconds <= 30);

  const persistentRequest = await api(baseUrl, firstToken, "/keys", {
    method: "POST",
    body: JSON.stringify({ key: "session:bounded", value: "bounded by session lifetime" }),
  });
  assert.equal(persistentRequest.response.status, 201);
  const bounded = await api(baseUrl, firstToken, "/keys/session%3Abounded");
  assert.ok(bounded.body.ttlSeconds > 0 && bounded.body.ttlSeconds <= 3600);

  const secondSession = await api(baseUrl, null, "/demo/session", { method: "POST" });
  const secondKeys = await api(baseUrl, secondSession.body.accessToken, "/keys");
  assert.deepEqual(secondKeys.body.keys, ["sample:json", "welcome"]);

  const invalid = await api(baseUrl, "forged-token", "/keys");
  assert.equal(invalid.response.status, 401);

  const expiredId = randomUUID();
  const expiredAt = Math.floor(Date.now() / 1000) - 1;
  const expiredSignature = createHmac("sha256", "integration-test-secret-at-least-32-bytes-long")
    .update(`${expiredId}.${expiredAt}`)
    .digest("base64url");
  const expired = await api(baseUrl, `${expiredId}.${expiredAt}.${expiredSignature}`, "/keys");
  assert.equal(expired.response.status, 401);

  const deleted = await api(baseUrl, firstToken, "/keys/integration%3Akey", { method: "DELETE" });
  assert.deepEqual(deleted.body, { deleted: true });
});
