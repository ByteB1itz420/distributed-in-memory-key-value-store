import test from "node:test";
import assert from "node:assert/strict";
import { decodeResponse, encodeRequest } from "./protocol.js";

test("encodes binary requests with little-endian argument lengths", () => {
  const frame = encodeRequest(["SET", "hello", "world"]);
  assert.equal(frame.readUInt32LE(0), 3);
  assert.equal(frame.readUInt32LE(4), 3);
  assert.equal(frame.subarray(8, 11).toString(), "SET");
  assert.equal(frame.readUInt32LE(11), 5);
  assert.equal(frame.subarray(15, 20).toString(), "hello");
});

test("decodes supported response types", () => {
  assert.equal(decodeResponse(Buffer.from([1, 0, 0, 0, 0])), null);
  assert.equal(decodeResponse(Buffer.from([3, 0, 0, 0, 2, 79, 75])), "OK");
  assert.equal(decodeResponse(Buffer.from([4, 0, 0, 0, 3, 49, 50, 51])), 123);
  assert.deepEqual(
    decodeResponse(Buffer.from([15, 0, 0, 0, 4, 2, 0, 0, 0, 1, 0, 0, 0, 97, 1, 0, 0, 0, 98])),
    ["a", "b"],
  );
  assert.deepEqual(
    decodeResponse(Buffer.from([14, 0, 0, 0, 4, 2, 0, 0, 0, 1, 0, 0, 0, 10, 0, 0, 0, 0])),
    ["\n", ""],
  );
});

test("rejects malformed response frames and server errors", () => {
  assert.throws(() => decodeResponse(Buffer.from([2, 0, 0, 0, 2])), /invalid response frame length/);
  assert.throws(() => decodeResponse(Buffer.from([2, 0, 0, 0, 1, 88])), /X/);
  assert.throws(() => decodeResponse(Buffer.from([2, 0, 0, 0, 0, 88])), /invalid nil response/);
  assert.throws(() => decodeResponse(Buffer.from([1, 0, 0, 0, 3])), /invalid integer response/);
  assert.throws(() => decodeResponse(Buffer.from([5, 0, 0, 0, 4, 1, 0, 0, 0])), /invalid response array/);
});
