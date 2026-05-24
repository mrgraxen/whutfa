import { describe, it } from "node:test";
import assert from "node:assert";
import { parseVideoPayload, parseAudioPacket } from "./ipc-framing.js";

describe("ipc-framing", () => {
  it("parses video payload", () => {
    const h264 = Buffer.from([0, 0, 0, 1, 0x67, 0x64]);
    const payload = Buffer.alloc(8 + h264.length);
    payload.writeBigUInt64BE(1000n, 0);
    h264.copy(payload, 8);
    const frame = parseVideoPayload(payload);
    assert.ok(frame);
    assert.equal(frame!.timestampUs, 1000n);
    assert.ok(frame!.keyframe);
  });

  it("parses audio packet", () => {
    const pcm = Buffer.alloc(4);
    const buf = Buffer.alloc(20 + 4);
    buf.writeUInt8(1, 0);
    buf.writeUInt32BE(48000, 1);
    buf.writeUInt8(2, 5);
    buf.writeUInt16BE(16, 6);
    buf.writeBigUInt64BE(500n, 8);
    buf.writeUInt32BE(4, 16);
    pcm.copy(buf, 20);
    const frame = parseAudioPacket(buf);
    assert.ok(frame);
    assert.equal(frame!.sampleRate, 48000);
  });
});
