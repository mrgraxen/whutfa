#!/usr/bin/env node
/** Generate minimal H.264 Annex-B + PCM fixtures for stub/CI */
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dir = path.join(path.dirname(fileURLToPath(import.meta.url)), "fixtures");
fs.mkdirSync(dir, { recursive: true });

// Minimal Baseline SPS/PPS/IDR (valid-ish for decoder tests; synthetic)
const sps = Buffer.from([
  0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x28, 0xac, 0x1b, 0x1a, 0x80, 0xf2, 0x84, 0xdf, 0x00,
  0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x78, 0xa2, 0x65, 0x30, 0xb5, 0x04,
]);
const pps = Buffer.from([0x00, 0x00, 0x00, 0x01, 0x68, 0xeb, 0xe3, 0xcb, 0x22, 0xc0]);
const idr = Buffer.from([
  0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21, 0x09, 0x45, 0x3b, 0x50, 0x10, 0x10, 0x10, 0x10,
]);

fs.writeFileSync(path.join(dir, "sample.h264"), Buffer.concat([sps, pps, idr]));

// 48kHz stereo 20ms silence
const samples = 48000 * 0.02 * 2;
const pcm = Buffer.alloc(samples * 2);
fs.writeFileSync(path.join(dir, "sample-media.pcm"), pcm);

console.log("Wrote scripts/fixtures/sample.h264 and sample-media.pcm");
