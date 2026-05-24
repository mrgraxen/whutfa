import { describe, it } from "node:test";
import assert from "node:assert";
import {
  splitAnnexB,
  extractSpsPps,
  buildAvcDescription,
  isKeyframeAccessUnit,
} from "./h264.js";

describe("h264", () => {
  const sample = new Uint8Array([
    0, 0, 0, 1, 0x67, 0x64, 0, 0x28,
    0, 0, 0, 1, 0x68, 0xeb, 0xe3,
    0, 0, 0, 1, 0x65, 0x88,
  ]);

  it("splits annex B", () => {
    const units = splitAnnexB(sample.buffer);
    assert.equal(units.length, 3);
  });

  it("extracts sps/pps", () => {
    const units = splitAnnexB(sample.buffer);
    const { sps, pps } = extractSpsPps(units);
    assert.ok(sps);
    assert.ok(pps);
    assert.ok(buildAvcDescription(sps, pps));
  });

  it("detects keyframe", () => {
    const units = splitAnnexB(sample.buffer);
    assert.equal(isKeyframeAccessUnit(units), true);
  });
});
