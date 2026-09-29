import { spawn } from "node:child_process";
import { createServer } from "node:http";
import { createHmac, randomBytes, randomUUID, timingSafeEqual } from "node:crypto";
import express from "express";
import { rateLimit } from "express-rate-limit";
import helmet from "helmet";
import { sendCommand } from "./protocol.js";

const port = Number(process.env.PORT ?? 8080);
const kvPort = Number(process.env.KV_PORT ?? 6380);
const allowedOrigins = new Set(
  (process.env.WEB_ORIGINS ?? "http://localhost:5173")
    .split(",")
    .map((origin) => origin.trim())
    .filter(Boolean),
);
const demoSecret = process.env.DEMO_SECRET ?? randomBytes(32).toString("hex");
const demoSessionTtlSeconds = 60 * 60;
if (!process.env.DEMO_SECRET) {
  console.warn("DEMO_SECRET is unset; demo sessions will be invalidated when this process restarts");
}
const app = express();
app.set("trust proxy", 1);

const kvServer = process.env.START_KV_SERVER === "false"
  ? null
  : spawn("kvserver", ["--host", "127.0.0.1", "--port", String(kvPort), "--threads", process.env.KV_THREADS ?? "4", "--maxmemory", process.env.KV_MAXMEMORY ?? "536870912", "--maxmemory-policy", "allkeys-lru"], { stdio: "inherit" });

if (kvServer) {
  kvServer.once("error", (error) => {
    console.error("Could not start kvserver:", error.message);
    process.exit(1);
  });
  kvServer.once("exit", (code, signal) => {
    if (!shuttingDown) {
      console.error(`kvserver exited unexpectedly (code=${code}, signal=${signal})`);
      process.exit(1);
    }
  });
}

let shuttingDown = false;

app.disable("x-powered-by");
app.use(helmet({ crossOriginResourcePolicy: { policy: "cross-origin" } }));
app.use((req, res, next) => {
  const origin = req.get("origin");
  if (!origin) return next();
  if (!allowedOrigins.has(origin)) {
    res.status(403).json({ error: "Origin is not allowed" });
    return;
  }
  res.setHeader("Access-Control-Allow-Origin", origin);
  res.setHeader("Vary", "Origin");
  res.setHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
  res.setHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
  if (req.method === "OPTIONS") {
    res.sendStatus(204);
    return;
  }
  next();
});
app.use(express.json({ limit: "1mb", strict: true }));
app.use("/api", rateLimit({ windowMs: 60_000, limit: 60, standardHeaders: "draft-8", legacyHeaders: false }));

app.get("/health", async (_req, res) => {
  try {
    await sendCommand(["PING"]);
    res.json({ status: "ok" });
  } catch {
    res.status(503).json({ status: "unavailable" });
  }
});

function signDemoSession(id, expiresAt) {
  return createHmac("sha256", demoSecret).update(`${id}.${expiresAt}`).digest("base64url");
}

function isValidDemoToken(token) {
  const match = /^([0-9a-f-]{36})\.(\d{10})\.([A-Za-z0-9_-]{43})$/.exec(token);
  if (!match) return null;
  const expiresAt = Number(match[2]);
  if (expiresAt <= Math.floor(Date.now() / 1000)) return null;
  const expected = Buffer.from(signDemoSession(match[1], expiresAt));
  const supplied = Buffer.from(match[3]);
  if (expected.length !== supplied.length || !timingSafeEqual(expected, supplied)) return null;
  return { id: match[1], expiresAt };
}

const demoSessionLimiter = rateLimit({
  windowMs: 60 * 60_000,
  limit: 10,
  standardHeaders: "draft-8",
  legacyHeaders: false,
});

app.post("/api/demo/session", demoSessionLimiter, async (_req, res) => {
  const id = randomUUID();
  const expiresAt = Math.floor(Date.now() / 1000) + demoSessionTtlSeconds;
  const starterTtl = Math.floor((expiresAt * 1000 - Date.now()) / 1000);
  const prefix = `user:demo-${id}:`;
  await sendCommand(["SET", `${prefix}welcome`, "Your demo is live. Edit or delete this key to try the store.", String(starterTtl)]);
  await sendCommand(["SET", `${prefix}sample:json`, "{\"project\":\"kv.store\",\"mode\":\"in-memory\"}", String(starterTtl)]);
  res.status(201).json({
    accessToken: `${id}.${expiresAt}.${signDemoSession(id, expiresAt)}`,
    expiresAt,
  });
});

app.use("/api", async (req, res, next) => {
  const authorization = req.get("authorization") ?? "";
  const match = /^Bearer ([^\s]+)$/.exec(authorization);
  if (!match) {
    res.status(401).json({ error: "A valid demo session is required" });
    return;
  }
  const demoSession = isValidDemoToken(match[1]);
  if (!demoSession) {
    res.status(401).json({ error: "Demo session is invalid; start a new demo session" });
    return;
  }
  req.userId = `demo-${demoSession.id}`;
  req.demoExpiresAt = demoSession.expiresAt;
  next();
});

const physicalKey = (userId, key) => `user:${userId}:${key}`;
const sessionWriteQueues = new Map();

async function withSessionWriteLock(sessionId, operation) {
  const previous = sessionWriteQueues.get(sessionId) ?? Promise.resolve();
  let release;
  const current = new Promise((resolve) => { release = resolve; });
  const queued = previous.then(() => current);
  sessionWriteQueues.set(sessionId, queued);
  await previous;
  try {
    return await operation();
  } finally {
    release();
    if (sessionWriteQueues.get(sessionId) === queued) {
      sessionWriteQueues.delete(sessionId);
    }
  }
}

app.get("/api/demo/session", (req, res) => {
  res.json({ expiresAt: req.demoExpiresAt });
});

function validateKey(key) {
  if (typeof key !== "string" || key.length === 0 || Buffer.byteLength(key, "utf8") > 256 || /[\r\n\0]/.test(key)) {
    const error = new Error("Keys must be 1–256 bytes and cannot contain line breaks or NUL");
    error.status = 400;
    throw error;
  }
}

function validateValue(value) {
  if (typeof value !== "string" || Buffer.byteLength(value, "utf8") > 16_384) {
    const error = new Error("Values must be text no larger than 16 KB");
    error.status = 400;
    throw error;
  }
}

app.get("/api/keys", async (req, res) => {
  const prefix = `user:${req.userId}:`;
  const keys = await sendCommand(["KEYS", prefix]);
  res.json({ keys: keys.map((key) => key.slice(prefix.length)).sort() });
});

app.get("/api/keys/:key", async (req, res) => {
  validateKey(req.params.key);
  const key = physicalKey(req.userId, req.params.key);
  const [value, ttlSeconds] = await Promise.all([
    sendCommand(["GET", key]),
    sendCommand(["TTL", key]),
  ]);
  if (value === null) {
    res.status(404).json({ error: "Key not found" });
    return;
  }
  res.json({ key: req.params.key, value, ttlSeconds });
});

app.post("/api/keys", async (req, res) => {
  const { key, value, ttlSeconds } = req.body ?? {};
  validateKey(key);
  validateValue(value);
  if (ttlSeconds !== undefined && ttlSeconds !== null &&
      (!Number.isSafeInteger(ttlSeconds) || ttlSeconds < 1 || ttlSeconds > 31_536_000)) {
    res.status(400).json({ error: "TTL must be blank or a whole number between 1 and 31,536,000 seconds" });
    return;
  }
  const prefix = `user:${req.userId}:`;
  const writeResult = await withSessionWriteLock(req.userId, async () => {
    const remainingTtl = Math.floor((req.demoExpiresAt * 1000 - Date.now()) / 1000);
    if (remainingTtl < 1) return "expired";
    const existingKeys = await sendCommand(["KEYS", prefix]);
    if (!existingKeys.includes(key) && existingKeys.length >= 50) {
      return "limit";
    }
    const existingTtl = await sendCommand(["TTL", physicalKey(req.userId, key)]);
    const requestedTtl = Number.isSafeInteger(ttlSeconds) ? ttlSeconds : existingTtl;
    const effectiveTtl = requestedTtl > 0 ? Math.min(requestedTtl, remainingTtl) : remainingTtl;
    await sendCommand(["SET", physicalKey(req.userId, key), value, String(effectiveTtl)]);
    return "created";
  });
  if (writeResult === "expired") {
    res.status(401).json({ error: "Demo session expired; start a new demo session" });
    return;
  }
  if (writeResult === "limit") {
    res.status(429).json({ error: "This demo session is limited to 50 keys" });
    return;
  }
  res.status(201).json({ key });
});

app.delete("/api/keys/:key", async (req, res) => {
  validateKey(req.params.key);
  const deleted = await sendCommand(["DEL", physicalKey(req.userId, req.params.key)]);
  res.json({ deleted: deleted === 1 });
});

app.post("/api/keys/:key/persist", async (req, res) => {
  validateKey(req.params.key);
  const persisted = await sendCommand(["PERSIST", physicalKey(req.userId, req.params.key)]);
  res.json({ persisted: persisted === 1 });
});

app.get("/api/status", async (req, res) => {
  const prefix = `user:${req.userId}:`;
  const keys = await sendCommand(["KEYS", prefix]);
  await sendCommand(["PING"]);
  res.json({ status: "online", keyCount: keys.length });
});

app.use((error, _req, res, _next) => {
  if (error.status) {
    res.status(error.status).json({ error: error.message });
    return;
  }
  if (error instanceof SyntaxError && "body" in error) {
    res.status(400).json({ error: "Invalid JSON request body" });
    return;
  }
  console.error("Request failed:", error.message);
  res.status(503).json({ error: "The key-value store is temporarily unavailable" });
});

const server = createServer(app);
server.listen(port, "0.0.0.0", () => console.log(`HTTP gateway listening on ${port}`));

function shutdown(signal) {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log(`Received ${signal}; shutting down`);
  server.close(() => {
    if (kvServer && !kvServer.killed) kvServer.kill("SIGTERM");
    process.exitCode = 0;
  });
  setTimeout(() => {
    if (kvServer && !kvServer.killed) kvServer.kill("SIGKILL");
    process.exit(1);
  }, 10_000).unref();
}

process.once("SIGTERM", () => shutdown("SIGTERM"));
process.once("SIGINT", () => shutdown("SIGINT"));
