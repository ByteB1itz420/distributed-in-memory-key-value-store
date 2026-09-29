import net from "node:net";

const MAX_RESPONSE_BYTES = 2 * 1024 * 1024;

export function encodeRequest(argv) {
  const parts = [u32(argv.length)];
  for (const value of argv) {
    const bytes = Buffer.from(value, "utf8");
    parts.push(u32(bytes.length), bytes);
  }
  return Buffer.concat(parts);
}

function u32(value) {
  const out = Buffer.allocUnsafe(4);
  out.writeUInt32LE(value);
  return out;
}

export function decodeResponse(frame) {
  if (frame.length < 5) {
    throw new Error("incomplete response frame");
  }
  const size = frame.readUInt32LE(0);
  if (size !== frame.length - 4 || size < 1) {
    throw new Error("invalid response frame length");
  }
  const type = frame[4];
  const payload = frame.subarray(5).toString("utf8");
  if (type === 0) return null;
  if (type === 1) throw new Error(payload || "key-value server error");
  if (type === 2) return payload;
  if (type === 3) {
    const value = Number(payload);
    if (!Number.isSafeInteger(value)) throw new Error("invalid integer response");
    return value;
  }
  if (type === 4) return payload.length === 0 ? [] : payload.split("\n");
  throw new Error(`unknown response type: ${type}`);
}

export function sendCommand(argv, host = process.env.KV_HOST ?? "127.0.0.1", port = Number(process.env.KV_PORT ?? 6380)) {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host, port });
    let data = Buffer.alloc(0);
    let settled = false;

    const fail = (error) => {
      if (settled) return;
      settled = true;
      socket.destroy();
      reject(error);
    };

    socket.setTimeout(5000, () => fail(new Error("key-value server timed out")));
    socket.once("error", fail);
    socket.on("data", (chunk) => {
      data = Buffer.concat([data, chunk]);
      if (data.length > MAX_RESPONSE_BYTES) {
        fail(new Error("key-value server response exceeded the size limit"));
        return;
      }
      if (data.length < 4) return;
      const expected = data.readUInt32LE(0) + 4;
      if (expected > MAX_RESPONSE_BYTES) {
        fail(new Error("key-value server response exceeded the size limit"));
        return;
      }
      if (data.length < expected) return;
      settled = true;
      socket.end();
      try {
        resolve(decodeResponse(data.subarray(0, expected)));
      } catch (error) {
        reject(error);
      }
    });
    socket.once("connect", () => socket.write(encodeRequest(argv)));
    socket.once("close", () => {
      if (!settled) reject(new Error("key-value server closed before replying"));
    });
  });
}
